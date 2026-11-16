/* Descriptor tables, open-file descriptions, and a RAM filesystem.
 * See kernel/include/fd.h for the layering and why the offset lives where it
 * does. Nothing here touches page tables: a bad user pointer is caught in
 * ring3.c before the bytes get this far. */

#include "../include/fd.h"
#include "../include/errno.h"
#include "../include/kmalloc.h"
#include "../include/omi_net.h"
#include "../include/socket.h"
#include "../include/net.h"
#include "../include/task.h"
#include "../include/readiness.h"

extern void serial_puts(const char *s);

static void zero(void *p, uint64_t n) {
  uint8_t *b = (uint8_t *)p;
  for (uint64_t i = 0; i < n; i++) b[i] = 0;
}

/* ------------------------------------------------------------- the files */

static rfile_t files[RFILE_MAX];

/* Console slots are shared by every process, so they are static rather than
 * allocated: refcounting them would only ever oscillate around "still here". */
static ofile_t console_out = { .kind = FK_CONSOLE, .flags = O_WRONLY, .refs = 1, .used = 1 };
static ofile_t console_in  = { .kind = FK_CONSOLE, .flags = O_RDONLY, .refs = 1, .used = 1 };

static char *strcpy32(char *dst, const char *src) {
  int i = 0;
  for (; src[i] && i < RFILE_NAME - 1; i++) dst[i] = src[i];
  dst[i] = 0;
  return dst;
}

static int str_eq(const char *a, const char *b) {
  int i = 0;
  for (; a[i] && b[i]; i++) if (a[i] != b[i]) return 0;
  return a[i] == b[i];
}

static rfile_t *file_find(const char *name) {
  for (int i = 0; i < RFILE_MAX; i++)
    if (files[i].used && str_eq(files[i].name, name)) return &files[i];
  return 0;
}

static rfile_t *file_create(const char *name, const char *body) {
  int slot = -1;
  for (int i = 0; i < RFILE_MAX; i++) if (!files[i].used) { slot = i; break; }
  if (slot < 0) return 0;
  rfile_t *f = &files[slot];
  uint8_t *data = (uint8_t *)kmalloc(RFILE_CAP);
  if (!data) return 0;
  zero(data, RFILE_CAP);
  zero(f, sizeof *f);
  strcpy32(f->name, name);
  f->data = data;
  f->cap = RFILE_CAP;
  f->used = 1;
  if (body) {
    uint32_t n = 0;
    while (body[n] && n < RFILE_CAP) n++;
    for (uint32_t i = 0; i < n; i++) f->data[i] = (uint8_t)body[i];
    f->size = n;
  }
  return f;
}

/* Seeded at boot so open() has something to find. Deliberately small: this is
 * a landing place for the syscalls, not a filesystem anyone should store work
 * in - it lives in kernel heap and vanishes with the machine. */
void rfs_init(void) {
  zero(files, sizeof files);
  file_create("/hello.txt", "hello from omi's ram fs\n");
  file_create("/readme.txt",
              "omi ram fs\n"
              "------------\n"
              "open, read, write, lseek, close, dup\n"
              "eight files, four kilobytes each\n"
              "offsets live in the open-file description, so dup shares them\n");
  file_create("/motd", "welcome, human\n");
}

/* ------------------------------------------------------ open-file descriptions */

static ofile_t ofiles[OFILE_MAX];

static ofile_t *ofile_get(void) {
  for (int i = 0; i < OFILE_MAX; i++)
    if (!ofiles[i].used) {
      zero(&ofiles[i], sizeof ofiles[i]);
      ofiles[i].used = 1;
      ofiles[i].refs = 1;
      return &ofiles[i];
    }
  return 0;
}

/* Destruction is the only path that tears down a transport, and it runs only
 * when nothing can reach the description any more: no descriptor, and no task
 * inside a backend call. */
static void ofile_destroy(ofile_t *o) {
  if (o->kind == FK_SOCKET && o->sock) sock_close(o->sock);
  zero(o, sizeof *o);
}

static void ofile_maybe_destroy(ofile_t *o) {
  if (!o || o->kind == FK_CONSOLE) return;    /* static, never freed */
  if (o->refs > 0 || o->pins > 0) return;
  ofile_destroy(o);
}

/* The last descriptor is gone. Stop new work and release anyone parked - but
 * leave the object (and its transport) alive while tasks are still inside it. */
static void ofile_shutdown(ofile_t *o) {
  if (!o || o->kind == FK_CONSOLE || o->closing) return;   /* idempotent */
  o->closing = 1;
  if (o->kind == FK_SOCKET && o->sock) sock_shutdown(o->sock);
}

static void ofile_put(ofile_t *o) {
  /* The connection belongs to the description: closing one of two dup'd
   * descriptors must not tear down the socket the other is still using. */
  if (!o || o->kind == FK_CONSOLE) return;    /* static, never freed */
  if (o->refs > 0) o->refs--;
  if (o->refs > 0) return;
  ofile_shutdown(o);
  ofile_maybe_destroy(o);
}

/* A pin is not a descriptor: it keeps the object alive without granting new
 * operations. Taken before the first backend access, released after the last. */
void ofile_pin(ofile_t *o) {
  if (!o || o->kind == FK_CONSOLE) return;
  o->pins++;
}

void ofile_unpin(ofile_t *o) {
  if (!o || o->kind == FK_CONSOLE) return;
  if (o->pins > 0) o->pins--;
  ofile_maybe_destroy(o);
}

/* ------------------------------------------------------------ descriptor tables */

fdtable_t *fdtable_new(void) {
  fdtable_t *t = (fdtable_t *)kmalloc(sizeof(fdtable_t));
  if (!t) return 0;
  zero(t, sizeof *t);
  t->fd[0] = &console_in;
  t->fd[1] = &console_out;
  t->fd[2] = &console_out;
  return t;
}

void fdtable_free(fdtable_t *t) {
  if (!t) return;
  for (int i = 0; i < FD_MAX; i++) {
    if (t->fd[i]) ofile_put(t->fd[i]);
    t->fd[i] = 0;
  }
  kfree(t);
}

static int fd_alloc(fdtable_t *t, ofile_t *o) {
  for (int i = 0; i < FD_MAX; i++)
    if (!t->fd[i]) { t->fd[i] = o; return i; }
  return -EMFILE;
}

int fd_open(fdtable_t *t, const char *path, int flags) {
  if (!t || !path || !path[0]) return -EINVAL;
  int mode = flags & 3;
  if (mode == 3) return -EINVAL;

  rfile_t *f = file_find(path);
  if (!f) {
    if (!(flags & O_CREAT)) return -ENOENT;
    f = file_create(path, 0);
    if (!f) return -ENOSPC;
  }

  ofile_t *o = ofile_get();
  if (!o) return -EMFILE;

  o->kind = FK_FILE;
  o->file = f;
  o->flags = flags;
  if (flags & O_APPEND) o->off = f->size;
  if ((flags & O_TRUNC) && (mode == O_WRONLY || mode == O_RDWR)) f->size = 0;

  int fd = fd_alloc(t, o);
  if (fd < 0) { ofile_put(o); return fd; }
  return fd;
}

int fd_dup(fdtable_t *t, int fd) {
  if (!t || fd < 0 || fd >= FD_MAX || !t->fd[fd]) return -EBADF;
  ofile_t *o = t->fd[fd];
  if (o->kind == FK_CONSOLE) {
    int n = fd_alloc(t, o);
    return n < 0 ? n : n;
  }
  int n = fd_alloc(t, o);
  if (n < 0) return n;
  o->refs++;
  return n;
}

int fd_socket(fdtable_t *t) {
  if (!t) return -EMFILE;
  ofile_t *o = ofile_get();
  if (!o) return -EMFILE;
  o->kind = FK_SOCKET;
  o->flags = O_RDWR;
  o->file = 0;
  o->sock = sock_new();          /* no transport until connect() */
  if (!o->sock) { ofile_put(o); return -EMFILE; }
  int fd = fd_alloc(t, o);
  if (fd < 0) { ofile_put(o); return fd; }
  return fd;
}

/* How many descriptors of one kind a table holds. Used by teardown accounting:
 * "did this space own network endpoints" is a question about the table, and it
 * has to be asked before the table is freed. */
int fd_count_kind(fdtable_t *t, int kind) {
  int n = 0;
  if (!t) return 0;
  for (int i = 0; i < FD_MAX; i++)
    if (t->fd[i] && t->fd[i]->kind == kind) n++;
  return n;
}

/* Readiness, computed. A regular file is always ready: it cannot block. A
 * socket answers through its backend. */
unsigned fd_poll(fdtable_t *t, int fd) {
  if (!t || fd < 0 || fd >= FD_MAX || !t->fd[fd]) return OMI_POLLERR;
  ofile_t *o = t->fd[fd];
  if (o->kind == FK_SOCKET)  return sock_poll(o->sock);
  if (o->kind == FK_FILE)    return OMI_POLLIN | OMI_POLLOUT;
  if (o->kind == FK_CONSOLE) return 0;      /* no keyboard path yet */
  return OMI_POLLERR;
}

int fd_connect(fdtable_t *t, int fd, uint32_t ip, uint16_t port) {
  if (!t || fd < 0 || fd >= FD_MAX || !t->fd[fd]) return -EBADF;
  ofile_t *o = t->fd[fd];
  if (o->kind != FK_SOCKET) return -ENOTSOCK;
  if (!o->sock) return -EBADF;

  uint8_t a[4];
  a[0] = (uint8_t)(ip >> 24); a[1] = (uint8_t)(ip >> 16);
  a[2] = (uint8_t)(ip >> 8);  a[3] = (uint8_t)ip;
  const net_cfg_t *cfg = net_get();
  /* Each socket gets its own connection, so a second program connecting
   * elsewhere no longer disturbs the first. The transport is chosen by the
   * backend behind sock_connect, not by this file. */
  return sock_connect(o->sock, cfg, a, port);
}

int fd_close(fdtable_t *t, int fd) {
  if (!t || fd < 0 || fd >= FD_MAX || !t->fd[fd]) return -EBADF;
  ofile_put(t->fd[fd]);
  t->fd[fd] = 0;
  return 0;
}

/* ------------------------------------------------------------------ io */

/* A socket read waits on the SOCKET OBJECT, never on the descriptor number, and
 * it pins the description for the whole operation - not just while registering.
 * The pin is what makes the last fd_close safe: it stops new work and wakes us
 * with POLLERR, but cannot free the transport while a call is in progress. A pin
 * only for the registration would leave the loop below re-entering a freed
 * socket after every wake.
 *
 * The read loop is level-triggered, so a wake is a hint: look again, and only
 * the readiness bits decide. Every pass has a deadline, so a lost wake costs
 * patience rather than the thread. */
static long sock_read_wait(ofile_t *o, void *kbuf, uint64_t n, uint64_t ticks) {
  if (o->closing) return -EBADF;
  uint64_t end = task_total_ticks() + ticks;
  long r = 0;

  ofile_pin(o);
  for (;;) {
    r = sock_recv(o->sock, kbuf, n, 0);   /* one non-blocking attempt */
    if (r != -EAGAIN) break;              /* bytes, EOF (0), or a hard error */

    uint64_t now = task_total_ticks();
    if (now >= end) { r = -EAGAIN; break; }

    int w = sock_wait(o->sock, OMI_POLLIN | OMI_POLLERR | OMI_POLLHUP, end - now);
    if (w < 0) { r = w; break; }          /* -EAGAIN deadline, -ENOBUFS, -EBADF */
    if (w & OMI_POLLIN) continue;
    if (w & OMI_POLLERR) { r = -ECONNRESET; break; }   /* closed, not finished */
    if (w & OMI_POLLHUP) { r = 0; break; }             /* FIN, nothing buffered */
    /* woken with no readiness bit set: level-triggered, so just look again */
  }
  ofile_unpin(o);
  return r;
}

/* The write side of the same rule. A send that accepts nothing is not an error,
 * it is a full pipe: wait for POLLOUT, look again, and keep the pin across all of
 * it for exactly the reason reads keep it. Partial acceptance returns at once -
 * write() reports what it took, and a caller that wants the rest calls again. */
static long sock_write_wait(ofile_t *o, const void *kbuf, uint64_t n, uint64_t ticks) {
  if (o->closing) return -EBADF;
  uint64_t end = task_total_ticks() + ticks;
  long r = 0;

  ofile_pin(o);
  for (;;) {
    r = sock_send(o->sock, kbuf, n);
    if (r != 0) break;                    /* bytes accepted, or a hard error */

    uint64_t now = task_total_ticks();
    if (now >= end) { r = -EAGAIN; break; }

    int w = sock_wait(o->sock, OMI_POLLOUT | OMI_POLLERR | OMI_POLLHUP, end - now);
    if (w < 0) { r = w; break; }
    if (w & OMI_POLLERR) { r = -EPIPE; break; }
    /* Hangup with nothing accepted means the peer is gone: waiting for room in a
     * connection that no longer exists would just burn the deadline. */
    if (w & OMI_POLLHUP) { r = -EPIPE; break; }
    /* POLLOUT, or a wake with nothing in it: level-triggered, so look again. */
  }
  ofile_unpin(o);
  return r;
}

long fd_read_ticks(fdtable_t *t, int fd, void *kbuf, uint64_t n, uint64_t ticks) {
  if (!t || fd < 0 || fd >= FD_MAX || !t->fd[fd]) return -EBADF;
  ofile_t *o = t->fd[fd];
  if ((o->flags & 3) == O_WRONLY) return -EBADF;
  if (o->kind == FK_SOCKET) return sock_read_wait(o, kbuf, n, ticks);
  if (o->kind == FK_CONSOLE) return 0;        /* no keyboard path yet: EOF */
  rfile_t *f = o->file;
  if (!f || !f->used) return -EBADF;
  if (o->off >= f->size) return 0;
  uint64_t avail = f->size - o->off;
  uint64_t took = n < avail ? n : avail;
  uint8_t *d = (uint8_t *)kbuf;
  for (uint64_t i = 0; i < took; i++) d[i] = f->data[o->off + i];
  o->off += took;
  return (long)took;
}

/* The default patience keeps the old 5 s behaviour of a blocking socket call. */
long fd_read(fdtable_t *t, int fd, void *kbuf, uint64_t n) {
  return fd_read_ticks(t, fd, kbuf, n, 500);
}

long fd_write(fdtable_t *t, int fd, const void *kbuf, uint64_t n) {
  return fd_write_ticks(t, fd, kbuf, n, 500);
}

long fd_write_ticks(fdtable_t *t, int fd, const void *kbuf, uint64_t n, uint64_t ticks) {
  if (!t || fd < 0 || fd >= FD_MAX || !t->fd[fd]) return -EBADF;
  ofile_t *o = t->fd[fd];
  if ((o->flags & 3) == O_RDONLY) return -EBADF;

  if (o->kind == FK_SOCKET) return sock_write_wait(o, kbuf, n, ticks);

  if (o->kind == FK_CONSOLE) {
    /* Write all of it. The first version capped every call at 256 bytes and
     * returned the short count, which a program checking `written == n` reads
     * as an error: httpget printed its body and then reported 0 bytes. */
    const uint8_t *s = (const uint8_t *)kbuf;
    for (uint64_t off = 0; off < n; ) {
      uint64_t k = n - off < 256 ? n - off : 256;
      char tmp[257];
      for (uint64_t i = 0; i < k; i++) tmp[i] = (char)s[off + i];
      tmp[k] = 0;
      serial_puts(tmp);
      off += k;
    }
    return (long)n;
  }

  rfile_t *f = o->file;
  if (!f || !f->used) return -EBADF;
  if (o->off >= f->cap) return -ENOSPC;
  uint64_t room = f->cap - o->off;
  uint64_t took = n < room ? n : room;
  const uint8_t *s = (const uint8_t *)kbuf;
  for (uint64_t i = 0; i < took; i++) f->data[o->off + i] = s[i];
  o->off += took;
  if (o->off > f->size) f->size = (uint32_t)o->off;
  return (long)took;
}

long fd_seek(fdtable_t *t, int fd, int64_t off, int whence) {
  if (!t || fd < 0 || fd >= FD_MAX || !t->fd[fd]) return -EBADF;
  ofile_t *o = t->fd[fd];
  if (o->kind == FK_CONSOLE) return -ESPIPE;   /* a terminal has no offset */

  int64_t base;
  uint32_t size = o->file ? o->file->size : 0;
  if (whence == SEEK_SET)      base = 0;
  else if (whence == SEEK_CUR) base = (int64_t)o->off;
  else if (whence == SEEK_END) base = (int64_t)size;
  else return -EINVAL;

  int64_t target = base + off;
  if (target < 0) return -EINVAL;
  if (target > (int64_t)(o->file ? o->file->cap : 0)) return -EINVAL;
  o->off = (uint64_t)target;
  return (long)target;
}
