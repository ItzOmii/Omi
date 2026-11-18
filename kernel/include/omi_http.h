/*
 * Omi - HTTP/1.1 GET on top of the TCP client
 *
 * Scope: http:// and https:// (TLS 1.2 via tls.c), one request per connection,
 * and the whole body must fit the caller's buffer. Chunked bodies are
 * de-chunked so hosts that stream work anyway; http_get follows up to four
 * 301/302/303/307/308 hops (this is why google.com lands on www.google.com).
 */

#ifndef _OMI_HTTP_H
#define _OMI_HTTP_H

#include "types.h"
#include "net.h"

typedef struct {
  int status;         /* 200, 301, ...; negative codes are failures */
  int len;            /* body bytes written to the buffer */
  int truncated;      /* 1 when the body did not fit */
  int secure;         /* 1 when the fetch went over TLS */
  char location[192];  /* Location: header of a redirect, resolved by http_get */
  char title[96];     /* <title> from the body, when there is one */
  char err[80];       /* human reason when status < 0 */
} http_result_t;

/* Splits "http://host:port/path" or "https://host/path" (port and path
   optional; https defaults to 443). Returns 1 if this client can fetch it. */
int http_parse_url(const char *url, char *host, int host_max, uint16_t *port,
                   char *path, int path_max);

/* Blocking GET. Returns 1 when a response was written (see res->status). */
int http_get(const net_cfg_t *cfg, const char *url, char *body, int body_max,
             http_result_t *res);

#endif /* _OMI_HTTP_H */