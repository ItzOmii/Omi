/* Omi - freestanding glue for the vendored Gumbo HTML5 parser.
 *
 * Gumbo is plain C99 and expects only malloc/free and a handful of string
 * helpers, so the whole library runs on the kernel heap with no libc.
 */

#include "types.h"
#include "kmalloc.h"
#include "string.h"
#include <stdarg.h>

void *malloc(size_t n) { return kmalloc(n); }
void *calloc(size_t n, size_t sz) { return kzalloc(n * sz); }
void *realloc(void *p, size_t n) { return krealloc(p, n); }
void free(void *p) { kfree(p); }
void abort(void) { for (;;) __asm__ volatile("cli; hlt"); }

int strcasecmp(const char *a, const char *b) {
  while (*a && *b) {
    int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
    int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
    if (ca != cb) return ca - cb;
    a++; b++;
  }
  return (unsigned char)*a - (unsigned char)*b;
}

int strncasecmp(const char *a, const char *b, size_t n) {
  while (n && *a && *b) {
    int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
    int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
    if (ca != cb) return ca - cb;
    a++; b++; n--;
  }
  if (!n) return 0;
  return (unsigned char)*a - (unsigned char)*b;
}

/* Gumbo's diagnostics formatter pulls in printf/vsnprintf. The kernel has no
 * console formatter for these, and a malformed page's error text is not worth
 * carrying, so the sink below is deliberately quiet: it formats into a small
 * scratch buffer and drops it. */

static void put_c(char *b, size_t cap, size_t *o, char c) { if (*o + 1 < cap) b[(*o)++] = c; }

static void put_s(char *b, size_t cap, size_t *o, const char *s, int len) {
  if (len < 0) len = 0x7fffffff;
  for (int i = 0; s[i] && i < len; i++) put_c(b, cap, o, s[i]);
}

static void put_n(char *b, size_t cap, size_t *o, long v, int base) {
  char t[24];
  int n = 0, neg = 0;
  unsigned long u = (unsigned long)v;
  if (v < 0 && base == 10) { neg = 1; u = (unsigned long)(-v); }
  if (!u) t[n++] = '0';
  while (u) { int d = (int)(u % (unsigned long)base); t[n++] = d < 10 ? '0' + d : 'a' + d - 10; u /= (unsigned long)base; }
  if (neg) put_c(b, cap, o, '-');
  while (n) put_c(b, cap, o, t[--n]);
}

int vsnprintf(char *str, size_t size, const char *fmt, va_list ap) {
  size_t o = 0;
  if (!size) return 0;
  for (const char *f = fmt; *f; f++) {
    if (*f != '%') { put_c(str, size, &o, *f); continue; }
    f++;
    int ljust = 0, prec = -1;
    if (*f == '-') { ljust = 1; f++; }
    if (*f == '*') { prec = va_arg(ap, int); f++; }
    if (*f == '.') { f++; if (*f == '*') { prec = va_arg(ap, int); f++; } else { prec = 0; while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0'); } }
    while (*f >= '0' && *f <= '9') f++;
    if (*f == 'l') f++;
    if (!*f) break;
    switch (*f) {
      case 's': put_s(str, size, &o, va_arg(ap, const char *), prec); break;
      case 'd': case 'i': put_n(str, size, &o, (long)va_arg(ap, int), 10); break;
      case 'u': put_n(str, size, &o, (long)va_arg(ap, unsigned), 10); break;
      case 'c': put_c(str, size, &o, (char)va_arg(ap, int)); break;
      case '%': put_c(str, size, &o, '%'); break;
      default: put_c(str, size, &o, *f); break;
    }
    (void)ljust;
  }
  str[o] = 0;
  return (int)o;
}

int printf(const char *fmt, ...) {
  char scratch[192];
  va_list ap;
  __builtin_va_start(ap, fmt);
  int n = vsnprintf(scratch, sizeof scratch, fmt, ap);
  __builtin_va_end(ap);
  (void)n;
  return 0;
}
