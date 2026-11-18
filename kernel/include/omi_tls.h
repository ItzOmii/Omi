/*
 * Omi - TLS 1.2 client (BearSSL inside the kernel)
 *
 * There is no socket layer under this: the caller has already opened the TCP
 * connection (tcp_connect) and tls_start() drives the handshake over it. The
 * certificate chain is verified against the roots compiled into roots.c, and
 * the reference date comes from the CMOS clock, so an expired or
 * not-yet-valid certificate is a failure rather than a warning.
 */

#ifndef _OMI_TLS_H
#define _OMI_TLS_H

#include "types.h"

/* Failure codes returned by tls_start / tls_send_all / tls_recv. */
#define TLS_OK          0
#define TLS_E_SETUP    -1   /* engine refused to start (no entropy, buffer) */
#define TLS_E_IO       -2   /* TCP read/write failed or timed out */
#define TLS_E_HANDSHAKE -3  /* peer sent something we could not complete */
#define TLS_E_CERT     -4   /* chain did not validate against our roots */
#define TLS_E_STUCK    -5   /* engine made no progress */

/* Runs the client handshake on an already-connected socket.
 * `host` is used for SNI and for the certificate name check.
 * Returns TLS_OK or a negative code. */
int tls_start(const char *host);

/* Sends the whole buffer as application data. Returns TLS_OK or negative. */
int tls_send_all(const void *buf, int len);

/* Reads decrypted bytes. Returns >0 bytes read, 0 on clean close/timeout,
 * or a negative TLS_E_* code. */
int tls_recv(void *buf, int max, int timeout_ms);

/* Sends close_notify and resets the engine. Does not touch the TCP socket. */
void tls_shutdown(void);

/* BearSSL error code from the last failure (BR_ERR_*), 0 if none. */
int tls_last_error(void);

/* Plain-text reason for a TLS_E_* code. */
const char *tls_error_text(int code);

/* CN of the certificate we validated, or "" if none was captured. */
const char *tls_peer_name(void);

/* Number of trust anchors actually loaded (decoded) from roots.c. */
int tls_root_count(void);

#endif /* _OMI_TLS_H */