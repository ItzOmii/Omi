/*
 * Omi - HTTP/1.1 GET on top of the TCP client
 *
 * Scope: http:// only (no TLS, no proxy), one request per connection, and the
 * whole body must fit the caller's buffer. Chunked bodies are de-chunked so
 * hosts that stream work anyway; redirects are reported, not followed.
 */

#ifndef _OMI_HTTP_H
#define _OMI_HTTP_H

#include "types.h"
#include "net.h"

typedef struct {
  int status;         /* 200, 301, ...; 0 when no response arrived */
  int len;            /* body bytes written to the buffer */
  int truncated;      /* 1 when the body did not fit */
  char title[96];     /* <title> from the body, when there is one */
} http_result_t;

/* Splits "http://host:port/path" (port and path optional). Returns 1 if the
   URL is one this client can fetch. */
int http_parse_url(const char *url, char *host, int host_max, uint16_t *port,
                   char *path, int path_max);

/* Blocking GET. Returns 1 when a response was written (see res->status). */
int http_get(const net_cfg_t *cfg, const char *url, char *body, int body_max,
             http_result_t *res);

#endif /* _OMI_HTTP_H */