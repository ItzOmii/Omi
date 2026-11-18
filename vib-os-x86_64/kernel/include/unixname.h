/*
 * AF_UNIX paths: the names two processes find each other by.
 *
 * A socketpair needs both processes present at the same moment - one forks and
 * hands over an end. A name removes that requirement: one process binds a path
 * and listens, another looks the path up later and connects, and neither was
 * there when the other was built.
 *
 * The namespace is deliberately as small as the filesystem it imitates: flat
 * names, no directories, no permissions, no inode. A name exists exactly as long
 * as the listening socket is open, and disappears when it closes - there is no
 * socket file to unlink, because there is nowhere to put one. Saying that in the
 * README is better than pretending a path is a file.
 */

#ifndef OMI_UNIXNAME_H
#define OMI_UNIXNAME_H

#include "types.h"
#include "socket.h"

#define UNIX_NAME_MAX    16   /* names that can exist at once */
#define UNIX_PATH_MAX    64   /* longest path, terminator included */
#define UNIX_BACKLOG_MAX  8   /* connections a listener can hold unanswered */

/* The socket table entry a bound socket carries: poll() answers "a connection is
 * waiting", read/write answer -ENOTCONN, and close removes the name. */
const struct socket_ops *unix_listener_ops(void);

/* Claim a name. -> 0 and the listener handle, or -EADDRINUSE / -ENOSPC /
 * -EINVAL / -ENAMETOOLONG. A name is usable only after unix_name_listen(). */
int unix_name_bind(const char *path, void **out_impl);

/* Start accepting on a bound name. Backlog is clamped to UNIX_BACKLOG_MAX. */
int unix_name_listen(void *impl, int backlog);

/* One waiting connection as a socket object, or -EAGAIN when none is waiting,
 * or -EINVAL when this handle was never told to listen. */
int unix_name_accept(void *impl, struct socket **out);

/* Connect by name: a fresh pair, with one end queued on the listener and the
 * other returned for the caller's own descriptor. -> 0, -ENOENT (no such name),
 * -ECONNREFUSED (named but not listening, or its backlog is full), -EMFILE. */
int unix_name_connect(const char *path, struct socket **out_client);

#endif
