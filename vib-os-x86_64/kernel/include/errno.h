/* errno values, numbered to match Linux so a compatibility layer is a lookup
 * table rather than a translation. Syscalls return -errno on failure and a
 * non-negative value on success; nothing returns a bare -1. */

#ifndef OMI_ERRNO_H
#define OMI_ERRNO_H

#define EPERM        1
#define ENOENT       2
#define ESRCH        3
#define EINTR        4
#define EIO          5
#define ENOEXEC      8
#define EAFNOSUPPORT 97
#define EPROTOTYPE 91
#define EISCONN 106
#define ENOTCONN 107
#define ECONNREFUSED 111
#define ENOTSOCK 88
#define EADDRINUSE 98
#define ENOTDIR 20
#define EBADF        9
#define EAGAIN      11
#define ENOMEM      12
#define EACCES      13
#define EFAULT      14
#define EBUSY       16
#define EEXIST      17
#define ENODEV      19
#define EINVAL      22
#define EMFILE 24
#define EIO 5
#define EPIPE 32
#define ENOSPC      28
#define ESPIPE      29
#define EDEADLK     35
#define ENOSYS      38
#define EOVERFLOW   75
#define ETIMEDOUT  110
#define ENOBUFS    105   /* no buffer space: a waiter table that cannot take one */
#define ECONNRESET 104   /* the peer or the last descriptor ended the stream */
#define EOPNOTSUPP  95

#endif