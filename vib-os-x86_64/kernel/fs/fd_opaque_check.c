/*
 * A compile-time guard, built with the kernel so it cannot rot.
 *
 * The boundary rule is that the descriptor layer does not know what a connection
 * is: fd.h names an opaque struct socket pointer and nothing more. That is easy
 * to violate by accident - one convenience include, and every file that sees an
 * ofile_t is suddenly compiled against the TCP stack, which is exactly the
 * dependency the extraction was meant to remove.
 *
 * So this translation unit includes fd.h and nothing else, and fails the build
 * if either the transport or the backend interface came along for the ride.
 */

#include "fd.h"

#ifdef _OMI_TCP_H
#error "fd.h must not include the TCP stack: the VFS may not know what a connection is"
#endif

#ifdef OMI_NET_BACKEND_H
#error "fd.h must not include the socket backend interface"
#endif

/* Instantiate the type: a pointer-only forward declaration would let a broken
 * ofile_t compile as long as nobody tried to use it. */
void fd_opaque_check(void) {
  ofile_t *o = 0;
  (void)sizeof(ofile_t);
  (void)o;
}