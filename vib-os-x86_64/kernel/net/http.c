/*
 * Omi - HTTP/1.1 GET (see include/omi_http.h)
 */

#include "types.h"
#include "string.h"
#include "net.h"
#include "omi_net.h"
#include "omi_http.h"


#define SCRATCH_MAX 24576

static char raw[SCRATCH_MAX];

static int starts_with(const char *s, const char *p) {
  while (*p) { if (*s++ != *p++) return 0; }
  return 1;
}

static int a_to_i(const char *s) {
  int v = 0;
  while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
  return v;
}

int http_parse_url(const char *url, char *host, int host_max, uint16_t *port,
                   char *path, int path_max) {
  const char *s = url;
  if (starts_with(s, "http://")) s += 7;
  else if (starts_with(s, "https://")) return 0;   /* no TLS in here */
  else if (s[0] == '/') return 0;                  /* relative, caller's job */
  else if (!s[0]) return 0;

  int h = 0;
  while (*s && *s != ':' && *s != '/' && h < host_max - 1) host[h++] = *s++;
  host[h] = 0;
  if (h == 0) return 0;

  *port = 80;
  if (*s == ':') {
    s++;
    int p = a_to_i(s);
    if (p > 0 && p < 65536) *port = (uint16_t)p;
    while (*s >= '0' && *s <= '9') s++;
  }

  int i = 0;
  if (*s == '/') {
    while (*s && i < path_max - 1) path[i++] = *s++;
  } else {
    path[i++] = '/';
  }
  path[i] = 0;
  return 1;
}

static void copy_title(const char *body, char *out, int max) {
  out[0] = 0;
  for (const char *p = body; *p; p++) {
    if ((*p == '<' || *p == '<') && (p[1] == 't' || p[1] == 'T')) {
      const char *q = p + 1;
      if ((q[0] == 't' || q[0] == 'T') && (q[1] == 'i' || q[1] == 'I') &&
          (q[2] == 't' || q[2] == 'T') && (q[3] == 'l' || q[3] == 'L') &&
          (q[4] == 'e' || q[4] == 'E')) {
        const char *v = q + 5;
        while (*v && *v != '>') v++;
        if (*v == '>') v++;
        int n = 0;
        while (*v && *v != '<' && n < max - 1) out[n++] = *v++;
        out[n] = 0;
        return;
      }
    }
  }
}

/* Decodes a chunked body in place-ish: copies de-chunked bytes to dst. */
static int dechunk(const char *src, int len, char *dst, int max) {
  int i = 0, o = 0;
  while (i < len) {
    int size = 0, digits = 0;
    while (i < len && src[i] != '\r' && src[i] != '\n') {
      char c = src[i++];
      int d;
      if (c >= '0' && c <= '9') d = c - '0';
      else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
      else break;
      size = size * 16 + d;
      digits++;
    }
    while (i < len && (src[i] == '\r' || src[i] == '\n')) i++;
    if (!digits || size == 0) break;
    for (int k = 0; k < size && i < len; k++) {
      if (o < max - 1) dst[o++] = src[i];
      i++;
    }
    while (i < len && (src[i] == '\r' || src[i] == '\n')) i++;
  }
  dst[o] = 0;
  return o;
}
int http_get(const net_cfg_t *cfg, const char *url, char *body, int body_max,
             http_result_t *res) {
  char host[128];
  char path[256];
  uint16_t port = 80;
  uint8_t ip[4];

  res->status = 0; res->len = 0; res->truncated = 0;
  res->title[0] = 0;
  if (body_max <= 0) return 0;
  body[0] = 0;

  if (!http_parse_url(url, host, sizeof host, &port, path, sizeof path)) { res->status = -1; return 0; }

  /* dotted quad or name? */
  int dotted = 1, dots = 0;
  for (const char *p = host; *p; p++) {
    if (*p == '.') { dots++; if (dots > 3) { dotted = 0; break; } }
    else if (*p < '0' || *p > '9') { dotted = 0; break; }
  }
  if (dotted && dots == 3) {
    int part = 0, o = 0;
    for (const char *p = host;; p++) {
      if (*p == '.' || !*p) { ip[o++] = (uint8_t)part; part = 0; if (!*p) break; }
      else part = part * 10 + (*p - '0');
    }
  } else if (!net_dns_resolve(cfg, host, ip)) {
    res->status = -1;                 /* no A record for that name */
    return 0;
  }

  if (!tcp_connect(cfg, ip, port)) { res->status = -2; return 0; }

  char req[512];
  int rl = 0;
  const char *hdr1 = "GET ";
  for (const char *p = hdr1; *p; p++) req[rl++] = *p;
  for (const char *p = path; *p && rl < 400; p++) req[rl++] = *p;
  const char *rest = " HTTP/1.1\r\nHost: ";
  for (const char *p = rest; *p; p++) req[rl++] = *p;
  for (const char *p = host; *p && rl < 460; p++) req[rl++] = *p;
  const char *tail =
      "\r\nUser-Agent: Omi/1.0\r\nAccept: text/html,text/plain\r\n"
      "Connection: close\r\n\r\n";
  for (const char *p = tail; *p && rl < 511; p++) req[rl++] = *p;
  req[rl] = 0;

  if (tcp_send(req, rl) != rl) { res->status = -3; tcp_close(); return 0; }

  int total = 0;
  int closed = 0;
  while (!closed && total < SCRATCH_MAX) {
    int n = tcp_recv(raw + total, SCRATCH_MAX - total - 1, 4000);
    if (n > 0) { total += n; continue; }
    if (n < 0) closed = 1;
    else break;                       /* idle timeout */
  }
  raw[total] = 0;
  tcp_close();
  if (total <= 0) return 0;

  /* status line */
  const char *sp = raw;
  while (*sp && *sp != ' ') sp++;
  if (*sp == ' ') res->status = a_to_i(sp + 1);
  if (res->status == 0) { res->status = -4; return 0; }

  /* header/body split */
  int body_off = -1;
  for (int i = 0; i + 3 < total; i++) {
    if (raw[i] == '\r' && raw[i + 1] == '\n' && raw[i + 2] == '\r' && raw[i + 3] == '\n') {
      body_off = i + 4;
      break;
    }
  }
  if (body_off < 0) return 1;         /* headers only */

  int chunked = 0;
  for (int i = 0; i + 7 < body_off; i++) {
    if (raw[i] == 'c' && starts_with(raw + i, "chunked")) { chunked = 1; break; }
  }

  int blen = total - body_off;
  if (chunked) {
    res->len = dechunk(raw + body_off, blen, body, body_max);
    if (res->len >= body_max - 1) res->truncated = 1;
  } else {
    int n = blen;
    if (n > body_max - 1) { n = body_max - 1; res->truncated = 1; }
    for (int i = 0; i < n; i++) body[i] = raw[body_off + i];
    body[n] = 0;
    res->len = n;
  }
  copy_title(body, res->title, sizeof res->title);
  return 1;
}