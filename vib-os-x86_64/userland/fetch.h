/* fetch.h - one HTTPS GET, in ring 3. */
#ifndef OMI_FETCH_H
#define OMI_FETCH_H

#include <stddef.h>

/* Resolve `host` through the kernel, dial it, handshake with SNI = host, GET
 * `path`, and leave the body at the front of `out`. The address actually used is
 * written to `ip_used` (16 bytes, may be NULL).
 * Returns the HTTP status code (200, 301, ...) or -1 with *why set. */
long https_get(const char *host, int port, const char *path,
               char *out, size_t cap, const char **why, long *body_len,
               char *ip_used);

#endif