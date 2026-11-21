/* fetch.h - one HTTPS GET, in ring 3. */
#ifndef OMI_FETCH_H
#define OMI_FETCH_H

#include <stddef.h>

/* Resolve `host` through the kernel, dial it, handshake with SNI = host, GET
 * `path`, and leave the body at the front of `out`. The address actually used is
 * written to `ip_used` (16 bytes, may be NULL).
 * Returns the HTTP status code (200, 301, ...) or -1 with *why set. */
/* Non-zero when the last fetch stopped because the caller's buffer filled: the
 * body was longer than the space it was given. Set by the reader, not guessed at
 * from the length afterwards - a chunked body's length shrinks when the framing
 * comes off, so a full buffer can look like a short body. */
extern int fetch_clipped;

/* When set, each request opens its own connection and closes it afterwards.
 *
 * Keep-alive assumes the server will hold the socket until the next request; a
 * CDN serving a page's images closes an idle one almost immediately, and the next
 * request then goes out on a dead socket and comes back with no reply at all
 * ("no-headers" for half the thumbnails on a gallery page). Image fetches are a
 * handful per page, so a fresh connection each time is the cheaper mistake. */
extern int https_no_reuse;

/* Called while the fetch is blocked on the network, so the caller's window can
 * keep running. Set it to something that drains your event queue; leave it null
 * and the fetch behaves exactly as it always did. */
extern void (*fetch_pump)(void);

/* The last body's arrival, for the timing line: bytes actually read off the
 * socket and how many read() calls it took. */
extern long fetch_reads, fetch_bytes;

long https_get(const char *host, int port, const char *path,
               char *out, size_t cap, const char **why, long *body_len,
               char *ip_used);

#endif