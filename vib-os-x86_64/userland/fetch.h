/* fetch.h - one HTTPS GET, in ring 3. */
#ifndef OMI_FETCH_H
#define OMI_FETCH_H

#include <stddef.h>

/* Dial, handshake, GET, and leave the body at the front of `out`.
 * Returns the HTTP status code (200, 301, ...) or -1 with *why set. */
long https_get(const char *addr, int port, const char *sni, const char *path,
               char *out, size_t cap, const char **why, long *body_len);

#endif