/* Descriptor tables, open-file descriptions, and a RAM filesystem.
 * See kernel/include/fd.h for the layering and why the offset lives where it
 * does. Nothing here touches page tables: a bad user pointer is caught in
 * ring3.c before the bytes get this far. */

#include "../include/fd.h"
#include "../include/epoll.h"
#include "../include/shfile.h"
#include "../include/errno.h"
#include "../include/kmalloc.h"
#include "../include/omi_net.h"
#include "../include/socket.h"
#include "../include/net.h"
#include "../include/task.h"
#include "../include/readiness.h"
#include "../include/pipe.h"
#include "../include/diskfs.h"

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
  /* With a disk attached a file is not limited to the RAM filesystem's 4 KB:
   * it has somewhere to live, and the cap is what write() is allowed to reach. */
  uint32_t cap = diskfs_present() ? DISK_MAX_FILE : RFILE_CAP;
  uint8_t *data = (uint8_t *)kmalloc(cap);
  if (!data) return 0;
  zero(data, cap);
  zero(f, sizeof *f);
  strcpy32(f->name, name);
  f->data = data;
  f->cap = cap;
  f->used = 1;
  if (body) {
    uint32_t n = 0;
    while (body[n] && n < cap) n++;
    for (uint32_t i = 0; i < n; i++) f->data[i] = (uint8_t)body[i];
    f->size = n;
  }
  return f;
}

/* A boot module, addressed by a path the program can name. No copy and no
 * heap cap: `data` points straight into the module blob, `cap` equals its size
 * so read() stops at the real end of the file rather than at 4 KB. */
int rfs_expose(const char *path, uint8_t *blob, uint32_t size) {
  if (!path || !path[0] || !blob || !size) return -EINVAL;
  if (file_find(path)) return -EEXIST;
  int slot = -1;
  for (int i = 0; i < RFILE_MAX; i++) if (!files[i].used) { slot = i; break; }
  if (slot < 0) return -ENOSPC;
  rfile_t *f = &files[slot];
  zero(f, sizeof *f);
  strcpy32(f->name, path);
  f->data = blob;
  f->size = size;
  f->cap  = size;
  f->used = 1;
  f->ro   = 1;
  return 0;
}

int rfs_count(void) {
  int n = 0;
  for (int i = 0; i < RFILE_MAX; i++) if (files[i].used) n++;
  return n;
}

int rfs_rom_count(void) {
  int n = 0;
  for (int i = 0; i < RFILE_MAX; i++) if (files[i].used && files[i].ro) n++;
  return n;
}

rfile_t *fd_file(fdtable_t *t, int fd) {
  ofile_t *o = fd_ofile(t, fd);
  if (!o || o->kind != FK_FILE || !o->file || !o->file->used) return 0;
  return o->file;
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
  rfs_load_disk();       /* -1 when there is no disk: the RAM fs stays as it was */
}

/* Mount the disk into this table: every record becomes a file, at its recorded
 * length, loaded into the same heap copy the RAM files use. The disk is the
 * durable copy, not a second namespace - a program cannot tell the difference
 * between a file that was seeded at boot and one that survived the last one. */
int rfs_load_disk(void) {
  if (diskfs_init() != 0) return -1;
  int n = diskfs_count();
  for (int i = 0; i < n; i++) {
    const char *name = 0;
    int len = 0;
    if (diskfs_entry(i, &name, &len) != 0 || !name) continue;
    if (file_find(name)) continue;             /* seeded content wins */
    int slot = -1;
    for (int k = 0; k < RFILE_MAX; k++) if (!files[k].used) { slot = k; break; }
    if (slot < 0) return -1;                   /* table full: the disk cannot be mounted whole */
    rfile_t *f = &files[slot];
    uint8_t *data = (uint8_t *)kmalloc(DISK_MAX_FILE);
    if (!data) return -1;
    zero(data, DISK_MAX_FILE);
    zero(f, sizeof *f);
    strcpy32(f->name, name);
    f->data = data;
    f->cap = DISK_MAX_FILE;
    f->used = 1;
    f->on_disk = 1;
    int got = diskfs_read(name, data, (int)DISK_MAX_FILE);
    if (got < 0) { f->used = 0; kfree(data); return -1; }
    f->size = (uint32_t)got;
  }
  /* A program can see whether it has a disk without a new syscall: if this file
   * is there, the disk answered IDENTIFY and its contents were mounted. */
  char info[96];
  int k = 0;
  const char *m = diskfs_model();
  for (int i = 0; m[i] && k < 60; i++) info[k++] = m[i];
  info[k++] = '\n';
  uint32_t mb = diskfs_sectors() / 2048;
  char num[12];
  int d = 0;
  if (!mb) num[d++] = '0';
  while (mb) { num[d++] = (char)('0' + mb % 10); mb /= 10; }
  while (d) info[k++] = num[--d];
  info[k++] = '\n';
  info[k] = 0;
  file_create("/disk.info", info);
  return 0;
}

static void file_flush(rfile_t *f) {
  if (!f || !f->used || f->ro || !f->dirty) return;
  if (!diskfs_present()) return;
  if (diskfs_write(f->name, f->data, (int)f->size) == 0) { f->dirty = 0; f->on_disk = 1; }
}

void rfs_sync(void) {
  for (int i = 0; i < RFILE_MAX; i++) if (files[i].used) file_flush(&files[i]);
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
  /* One ofile is one holder of one end. Two dup'd descriptors are still one
   * ofile, so the pipe's counts already say the right thing. */
  if (o->kind == FK_PIPE && o->pipe) pipe_unref(o->pipe, (o->flags & 3) == O_WRONLY);
  if (o->kind == FK_EPOLL && o->ep) epoll_free(o->ep);
  if (o->kind == FK_SHFILE && o->shf) shfile_unref(o->shf);
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
  if (o->kind == FK_FILE) file_flush(o->file);   /* close() is where a write lands */
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

/* Grow the table so it can hold descriptor `want`. Doubling keeps the cost of
 * opening files amortised and the table's size a power of two, which is also
 * what makes it cheap to clone: the copy is one kmalloc and a memcpy. */
int fd_grow(fdtable_t *t, int want) {
  if (!t || want < 0) return -EINVAL;
  if (want < t->cap) return 0;
  if (want >= FD_MAX) return -EMFILE;
  int cap = t->cap ? t->cap : FD_FIRST;
  while (cap <= want) cap *= 2;
  if (cap > FD_MAX) cap = FD_MAX;
  ofile_t **n = (ofile_t **)kzalloc((size_t)cap * sizeof(ofile_t *));
  if (!n) return -ENOMEM;
  for (int i = 0; i < t->cap; i++) n[i] = t->fd[i];
  kfree(t->fd);
  t->fd = n;
  t->cap = cap;
  return 0;
}

ofile_t *fd_at(fdtable_t *t, int fd) {
  if (!t || !t->fd || fd < 0 || fd >= t->cap) return 0;
  return t->fd[fd];
}

fdtable_t *fdtable_new(void) {
  fdtable_t *t = (fdtable_t *)kmalloc(sizeof(fdtable_t));
  if (!t) return 0;
  zero(t, sizeof *t);
  if (fd_grow(t, 2) != 0) { kfree(t); return 0; }   /* starts with 16 slots */
  t->fd[0] = &console_in;
  t->fd[1] = &console_out;
  t->fd[2] = &console_out;
  return t;
}

/* The descriptor table a forked child is born with: the same descriptors, in the
 * same slots, pointing at the same open files - a child that writes to a pipe its
 * parent made has to reach the parent's reader, and a child that closes one must
 * not close the parent's. That is a reference, not a copy of the file. Consoles
 * are static objects with no reference to take, the same way fd_dup() treats
 * them. */
fdtable_t *fdtable_clone(fdtable_t *src) {
  if (!src) return 0;
  fdtable_t *t = (fdtable_t *)kmalloc(sizeof(fdtable_t));
  if (!t) return 0;
  zero(t, sizeof *t);
  if (src->cap && fd_grow(t, src->cap - 1) != 0) { kfree(t); return 0; }
  for (int i = 0; i < src->cap; i++) {
    ofile_t *o = src->fd[i];
    if (!o) continue;
    t->fd[i] = o;
    if (o->kind != FK_CONSOLE) o->refs++;
  }
  return t;
}

void fdtable_free(fdtable_t *t) {
  if (!t) return;
  for (int i = 0; i < t->cap; i++)
    if (t->fd[i]) ofile_put(t->fd[i]);
  kfree(t->fd);
  kfree(t);
}

static int fd_alloc(fdtable_t *t, ofile_t *o) {
  if (!t) return -EBADF;
  for (int i = 0; i < t->cap; i++)
    if (!t->fd[i]) { t->fd[i] = o; return i; }
  /* Full: grow rather than fail, unless the ceiling is what was hit. */
  int r = fd_grow(t, t->cap);
  if (r != 0) return r == -EMFILE ? -EMFILE : r;
  for (int i = 0; i < t->cap; i++)
    if (!t->fd[i]) { t->fd[i] = o; return i; }
  return -EMFILE;
}

rfile_t *rfs_find(const char *name) { return file_find(name); }

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
  /* A boot module cannot be written: its bytes are the bootloader's image, and
   * truncating it would corrupt a file other processes are running from. */
  if (f->ro && (mode != O_RDONLY || (flags & O_TRUNC))) return -EROFS;

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
  if (!fd_at(t, fd)) return -EBADF;
  ofile_t *o = fd_at(t, fd);
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

/* Same as fd_socket, but for an endpoint that was created before the descriptor:
 * a socketpair is made as a pair and only then handed to two fds. */
int fd_socket_from(fdtable_t *t, struct socket *s) {
  if (!t || !s) return -EINVAL;
  ofile_t *o = ofile_get();
  if (!o) return -EMFILE;
  o->kind = FK_SOCKET;
  o->flags = O_RDWR;
  o->file = 0;
  o->sock = s;
  int fd = fd_alloc(t, o);
  if (fd < 0) { ofile_put(o); return fd; }
  return fd;
}

int fd_shfile(fdtable_t *t) {
  if (!t) return -EMFILE;
  ofile_t *o = ofile_get();
  if (!o) return -EMFILE;
  o->kind = FK_SHFILE;
  o->flags = O_RDWR;
  o->shf = shfile_new();
  if (!o->shf) { ofile_put(o); return -EMFILE; }
  int fd = fd_alloc(t, o);
  if (fd < 0) { ofile_put(o); return fd; }
  return fd;
}

struct shfile *fd_shfile_of(fdtable_t *t, int fd) {
  if (!fd_at(t, fd)) return 0;
  ofile_t *o = fd_at(t, fd);
  return o->kind == FK_SHFILE ? o->shf : 0;
}

int fd_epoll(fdtable_t *t) {
  if (!t) return -EMFILE;
  ofile_t *o = ofile_get();
  if (!o) return -EMFILE;
  o->kind = FK_EPOLL;
  o->flags = O_RDWR;
  o->ep = epoll_new();
  if (!o->ep) { ofile_put(o); return -EMFILE; }
  int fd = fd_alloc(t, o);
  if (fd < 0) { ofile_put(o); return fd; }
  return fd;
}

/* Both ends at once, because a pipe with one end is not a thing a program can
 * use: the pair is the object. pipe_new() hands them over already held (1/1), so
 * the two ofiles below inherit those counts rather than adding to them - and if
 * the second slot cannot be found, both are released and the ring goes back. */
int fd_pipe(fdtable_t *t, int out[2]) {
  if (!t || !out) return -EINVAL;
  struct pipe *p = pipe_new();
  if (!p) return -EMFILE;

  ofile_t *r = ofile_get();
  if (!r) { pipe_unref(p, 0); pipe_unref(p, 1); return -EMFILE; }
  r->kind = FK_PIPE; r->flags = O_RDONLY; r->pipe = p;
  int rfd = fd_alloc(t, r);
  if (rfd < 0) { ofile_put(r); pipe_unref(p, 1); return rfd; }

  ofile_t *w = ofile_get();
  if (!w) { fd_close(t, rfd); pipe_unref(p, 1); return -EMFILE; }
  w->kind = FK_PIPE; w->flags = O_WRONLY; w->pipe = p;
  int wfd = fd_alloc(t, w);
  if (wfd < 0) { ofile_put(w); fd_close(t, rfd); return wfd; }

  out[0] = rfd;
  out[1] = wfd;
  return 0;
}

/* How many descriptors of one kind a table holds. Used by teardown accounting:
 * "did this space own network endpoints" is a question about the table, and it
 * has to be asked before the table is freed. */
int fd_count_kind(fdtable_t *t, int kind) {
  int n = 0;
  if (!t) return 0;
  for (int i = 0; i < t->cap; i++)
    if (t->fd[i] && t->fd[i]->kind == kind) n++;
  return n;
}

/* Readiness, computed. A regular file is always ready: it cannot block. A
 * socket answers through its backend. */
unsigned fd_poll(fdtable_t *t, int fd) {
  if (!fd_at(t, fd)) return OMI_POLLERR;
  ofile_t *o = fd_at(t, fd);
  if (o->kind == FK_SOCKET)  return sock_poll(o->sock);
  if (o->kind == FK_PIPE)    return pipe_poll(o->pipe, (o->flags & 3) == O_WRONLY);
  if (o->kind == FK_FILE)    return OMI_POLLIN | OMI_POLLOUT;
  if (o->kind == FK_EPOLL)   return epoll_any_ready(o->ep, t) ? OMI_POLLIN : 0;
  if (o->kind == FK_SHFILE)  return OMI_POLLIN | OMI_POLLOUT;   /* memory: always ready */
  if (o->kind == FK_CONSOLE) return 0;      /* no keyboard path yet */
  return OMI_POLLERR;
}

int fd_connect(fdtable_t *t, int fd, uint32_t ip, uint16_t port) {
  if (!fd_at(t, fd)) return -EBADF;
  ofile_t *o = fd_at(t, fd);
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

/* shutdown(2). Two halves, two answers: SHUT_WR sends our FIN through the
 * transport and leaves the socket readable, so the reply to what was already
 * sent can still be collected; SHUT_RD drops the receive side, buffered bytes
 * included. sock_shutdown() - the last-descriptor-gone path - is deliberately
 * not used here: it closes both directions at once and would throw away the
 * answer the caller is waiting for. */
long fd_shutdown(fdtable_t *t, int fd, int how) {
  if (!fd_at(t, fd)) return -EBADF;
  ofile_t *o = fd_at(t, fd);
  if (o->kind != FK_SOCKET || !o->sock) return -ENOTSOCK;
  if (how != 0 && how != 1 && how != 2) return -EINVAL;
  if (how == 0 || how == 2) sock_shutdown_rd(o->sock);   /* SHUT_RD, SHUT_RDWR */
  if (how == 1 || how == 2) sock_shutdown_wr(o->sock);   /* SHUT_WR, SHUT_RDWR */
  return 0;
}

int fd_close(fdtable_t *t, int fd) {
  if (!fd_at(t, fd)) return -EBADF;
  ofile_put(fd_at(t, fd));
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

  /* One reader at a time. Without this, two tasks can park on the same socket,
   * both be woken for the same bytes, and split a stream in scheduler order. */
  int owner = sock_read_begin(o->sock);
  if (owner != 0) return owner;          /* -EBUSY, or -EBADF if it is gone */

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
  sock_read_end(o->sock);                /* released on every path out */
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
  if (!fd_at(t, fd)) return -EBADF;
  ofile_t *o = fd_at(t, fd);
  if ((o->flags & 3) == O_WRONLY) return -EBADF;
  if (o->kind == FK_SOCKET) return sock_read_wait(o, kbuf, n, ticks);
  if (o->kind == FK_PIPE)   return pipe_read(o->pipe, kbuf, n, ticks);
  if (o->kind == FK_CONSOLE) return 0;        /* no keyboard path yet: EOF */
  if (o->kind == FK_SHFILE) {
    /* Read from the same pages a mapping sees, at the descriptor's offset: the
     * point of a shared file is that there is one copy of the bytes. */
    long r = (long)shfile_read(o->shf, o->off, kbuf, n);
    if (r > 0) o->off += (uint64_t)r;
    return r;
  }
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
  if (!fd_at(t, fd)) return -EBADF;
  ofile_t *o = fd_at(t, fd);
  if ((o->flags & 3) == O_RDONLY) return -EBADF;

  if (o->kind == FK_SOCKET) return sock_write_wait(o, kbuf, n, ticks);
  if (o->kind == FK_PIPE)   return pipe_write(o->pipe, kbuf, n, ticks);

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

  if (o->kind == FK_SHFILE) {
    long r = (long)shfile_write(o->shf, o->off, kbuf, n);
    if (r > 0) o->off += (uint64_t)r;
    return r;
  }

  rfile_t *f = o->file;
  if (!f || !f->used) return -EBADF;
  if (f->ro) return -EROFS;
  if (o->off >= f->cap) return -ENOSPC;
  uint64_t room = f->cap - o->off;
  uint64_t took = n < room ? n : room;
  const uint8_t *s = (const uint8_t *)kbuf;
  for (uint64_t i = 0; i < took; i++) f->data[o->off + i] = s[i];
  o->off += took;
  if (o->off > f->size) f->size = (uint32_t)o->off;
  f->dirty = 1;                       /* flushed on close() or sync() */
  return (long)took;
}

ofile_t *fd_ofile(fdtable_t *t, int fd) {
  return fd_at(t, fd);
}

/* Descriptor passing needs the description itself, not just the number: the
 * number means nothing in another process's table, and the object must survive
 * the sender closing its copy. */
ofile_t *fd_ref_ofile(fdtable_t *t, int fd) {
  ofile_t *o = fd_at(t, fd);
  if (!o) return 0;
  if (o->kind != FK_CONSOLE) o->refs++;
  return o;
}

int fd_install_ofile(fdtable_t *t, ofile_t *o) {
  if (!t || !o) return -EINVAL;
  int fd = fd_alloc(t, o);
  if (fd < 0 && o->kind != FK_CONSOLE) ofile_put(o);   /* the ref does not leak */
  return fd;
}

void fd_drop_ofile(ofile_t *o) {
  if (o) ofile_put(o);
}

long fd_seek(fdtable_t *t, int fd, int64_t off, int whence) {
  if (!fd_at(t, fd)) return -EBADF;
  ofile_t *o = fd_at(t, fd);
  if (o->kind == FK_CONSOLE) return -ESPIPE;   /* a terminal has no offset */

  int64_t base;
  /* A shared file's length is the file's own, not the boot module's: a program
   * that ftruncates a memfd and seeks to its end is asking about the object it
   * made, and answering from the wrong one would report a size no read agrees
   * with. */
  uint64_t size = o->kind == FK_SHFILE ? shfile_size(o->shf)
                : (o->file ? o->file->size : 0);
  if (whence == SEEK_SET)      base = 0;
  else if (whence == SEEK_CUR) base = (int64_t)o->off;
  else if (whence == SEEK_END) base = (int64_t)size;
  else return -EINVAL;

  int64_t target = base + off;
  if (target < 0) return -EINVAL;
  int64_t limit = o->kind == FK_SHFILE ? (int64_t)shfile_size(o->shf)
                : (int64_t)(o->file ? o->file->cap : 0);
  if (target > limit) return -EINVAL;
  o->off = (uint64_t)target;
  return (long)target;
}
