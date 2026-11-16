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
#define OFILE_MAX    64
/* Slots, not bytes: each one is ~56 bytes of static table. The boot medium
 * carries 24 modules and three RAM files are seeded on top, so 24 was exactly
 * full the day a shared library and its loader joined the ISO - and the three
 * files that did not fit were the last ones listed, which is how a library
 * becomes silently unopenable. */
#define RFILE_MAX    48
#define RFILE_NAME   32
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
/* A pipe end is a fifth kind: the ring itself gets its own object (pipe.h) and
 * this header names the pointer only, for the same reason it does for sockets -
 * the descriptor layer must not learn how a transport or a buffer works. */
struct pipe;

enum { FK_NONE = 0, FK_CONSOLE, FK_FILE, FK_SOCKET, FK_PIPE };

/* The file: shared by every open of the same name. */
typedef struct {
  char     name[RFILE_NAME];
  uint8_t *data;
  uint32_t size;
  uint32_t cap;
  int      used;
  /* Read-only: the bytes belong to a boot module (a Limine blob in memory the
   * kernel does not own and cannot grow). Opening one for writing is EROFS, not
   * a silent truncation of the boot image. */
  int      ro;
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
  struct pipe   *pipe;     /* FK_PIPE: the ring; the end is in flags (O_WRONLY) */
  int      used;
} ofile_t;

/* The descriptor table: one per address space. */
typedef struct {
  ofile_t *fd[FD_MAX];      /* FD_CONSOLE slots point at two static ofiles */
} fdtable_t;

void  rfs_init(void);
/* The RAM file with this name, or NULL. Read-only for the caller: the entry is
 * owned by the filesystem and can go away when the last descriptor closes. */
rfile_t *rfs_find(const char *name);

/* Register a boot module as a file at `path`. The blob is a Limine module: it
 * lives wherever the bootloader put it and is never copied or freed here, which
 * is why the entry is read-only and why `cap` is the blob's size - a write past
 * it must fail rather than grow memory the kernel does not own.
 * Returns 0, -EEXIST, or -ENOSPC when the table is full. */
int  rfs_expose(const char *path, uint8_t *blob, uint32_t size);
/* How many files the table holds, and how many of those are module-backed.
 * Boot accounting reads it: "exposed 12 module files" is checkable, "it works"
 * is not. */
int  rfs_count(void);
int  rfs_rom_count(void);

/* The file behind a descriptor, or NULL when it is not a file descriptor. Used by
 * mmap, which needs the bytes and the length rather than a read position. */
rfile_t *fd_file(fdtable_t *t, int fd);
fdtable_t *fdtable_new(void);
/* Same descriptors, same open files, one more reference each: what fork() hands
 * the child. Null on exhaustion. */
fdtable_t *fdtable_clone(fdtable_t *src);
void  fdtable_free(fdtable_t *t);

int  fd_open(fdtable_t *t, const char *path, int flags);
long fd_read(fdtable_t *t, int fd, void *kbuf, uint64_t n);
/* Same, with an explicit patience in ticks (100 ticks ~= 1 s). */
long fd_read_ticks(fdtable_t *t, int fd, void *kbuf, uint64_t n, uint64_t ticks);
long fd_write(fdtable_t *t, int fd, const void *kbuf, uint64_t n);
long fd_write_ticks(fdtable_t *t, int fd, const void *kbuf, uint64_t n, uint64_t ticks);
long fd_seek(fdtable_t *t, int fd, int64_t off, int whence);
/* The description behind a descriptor, or NULL. The caller must not use it after
 * the descriptor can be closed by another task; fstat only reads what it needs
 * while the task is pinned in the syscall, which is sufficient for its fields. */
ofile_t *fd_ofile(fdtable_t *t, int fd);
int  fd_close(fdtable_t *t, int fd);
int  fd_dup(fdtable_t *t, int fd);
int  fd_socket(fdtable_t *t);
/* Creates both ends. out[0] is the read end, out[1] the write end. */
int  fd_pipe(fdtable_t *t, int out[2]);
unsigned fd_poll(fdtable_t *t, int fd);   /* OMI_POLL* bits, never blocks */
int  fd_count_kind(fdtable_t *t, int kind);

/* Pin a description while a task is inside it. The last descriptor ref triggers
 * shutdown; destruction waits for the last pin, so a backend call cannot run
 * against a freed transport. */
void ofile_pin(ofile_t *o);
void ofile_unpin(ofile_t *o);
int  fd_connect(fdtable_t *t, int fd, uint32_t ip, uint16_t port);

#endif
