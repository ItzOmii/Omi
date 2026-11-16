/* A descriptor table, open-file descriptions, and a tiny in-memory filesystem.
 *
 * Three layers, deliberately:
 *
 *   fd  ->  ofile_t  ->  rfile_t
 *   |       |            |
 *   |       |            +-- the file itself: name, bytes, capacity
 *   |       +--------------- the open file: flags and the shared offset
 *   +----------------------- the descriptor: a process-owned slot
 *
 * The middle layer is the one worth getting right early. Where the descriptor
 * lives decides who shares it: a table in mm_t means the threads of one process
 * share descriptors and a second process does not. Where the offset lives
 * decides what dup() means: because the offset is in the open-file description
 * and the description is refcounted, dup() gives two descriptors one cursor -
 * which is exactly how a shell redirect and a socket-dup have to behave.
 */

#ifndef OMI_FD_H
#define OMI_FD_H

#include <stdint.h>
#include "readiness.h"

#define FD_MAX      16
#define OFILE_MAX   64
#define RFILE_MAX   8
#define RFILE_NAME  32
#define RFILE_CAP   4096

/* Open flags, Linux's numbering so a compatibility layer stays a lookup. */
#define O_RDONLY  0000
#define O_WRONLY  0001
#define O_RDWR    0002
#define O_CREAT   0100
#define O_TRUNC   01000
#define O_APPEND  02000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* A socket is a fourth kind of descriptor, not a parallel mechanism: the whole
 * point of the ofile layer is that read/write/close on a socket go down the same
 * path as on a file, and the caller cannot tell the difference at the syscall. */
/* A socket is an opaque endpoint object (see socket.h): this header names the
 * pointer and nothing else, so the VFS cannot grow a dependency on a transport. */
struct socket;

enum { FK_NONE = 0, FK_CONSOLE, FK_FILE, FK_SOCKET };

/* The file: shared by every open of the same name. */
typedef struct {
  char     name[RFILE_NAME];
  uint8_t *data;
  uint32_t size;
  uint32_t cap;
  int      used;
} rfile_t;

/* The open file description: owns flags and the offset, shared by dup(). */
typedef struct {
  int      kind;
  int      flags;
  uint64_t off;
  int      refs;           /* descriptor references: dup() adds one, close drops one */
  int      pins;           /* tasks inside a backend call or parked on readiness */
  int      closing;        /* the last descriptor went: no new operations */
  rfile_t *file;
  struct socket *sock;     /* FK_SOCKET: the endpoint, shared by every dup */
  int      used;
} ofile_t;

/* The descriptor table: one per address space. */
typedef struct {
  ofile_t *fd[FD_MAX];      /* FD_CONSOLE slots point at two static ofiles */
} fdtable_t;

void  rfs_init(void);
fdtable_t *fdtable_new(void);
void  fdtable_free(fdtable_t *t);

int  fd_open(fdtable_t *t, const char *path, int flags);
long fd_read(fdtable_t *t, int fd, void *kbuf, uint64_t n);
long fd_write(fdtable_t *t, int fd, const void *kbuf, uint64_t n);
long fd_seek(fdtable_t *t, int fd, int64_t off, int whence);
int  fd_close(fdtable_t *t, int fd);
int  fd_dup(fdtable_t *t, int fd);
int  fd_socket(fdtable_t *t);
unsigned fd_poll(fdtable_t *t, int fd);   /* OMI_POLL* bits, never blocks */
int  fd_count_kind(fdtable_t *t, int kind);

/* Pin a description while a task is inside it. The last descriptor ref triggers
 * shutdown; destruction waits for the last pin, so a backend call cannot run
 * against a freed transport. */
void ofile_pin(ofile_t *o);
void ofile_unpin(ofile_t *o);
int  fd_connect(fdtable_t *t, int fd, uint32_t ip, uint16_t port);

#endif
