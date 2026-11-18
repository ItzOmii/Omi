/* Pipes: the last descriptor type that POSIX-shaped programs expect to find.
 *
 * A pipe is a byte ring with two ends and a count of open ends on each side.
 * That is the whole object - the interesting part is not the buffer, it is what
 * happens when it is empty or full:
 *
 *   read, empty, writer alive    -> wait (a program expecting data must not spin)
 *   read, empty, no writers      -> 0, EOF (this is how a pipeline ends)
 *   write, no readers            -> -EPIPE (a program writing into a dead pipe
 *                                  should learn it, not block for ever)
 *   write, full                  -> wait, then a partial write returns at once
 *
 * Waiting is level-triggered with a one-tick sleep rather than a registration
 * table. That is a deliberate trade: a waiter table is four more failure modes
 * (stale task, reused slot, lost wake, wake with nothing in it) and this kernel
 * already has one of those for sockets. Pipes do not need sub-tick latency, so
 * they get the version that cannot lose a wake, and pay for it with at most one
 * tick of delay per idle check. If something later needs better, this becomes a
 * waitqueue with the socket's two-sided generation check - not before.
 */

#include "pipe.h"

#include <stdint.h>
#include "types.h"
#include "string.h"
#include "errno.h"
#include "kmalloc.h"
#include "task.h"

extern void serial_puts(const char *s);

typedef struct pipe {
  uint8_t  buf[PIPE_CAP];
  uint32_t head;      /* next byte to read */
  uint32_t tail;      /* next byte to write */
  uint32_t count;     /* bytes held */
  int      readers;   /* open read ends (each ofile counts once) */
  int      writers;   /* open write ends */
  int      used;
} pipe_t;

static pipe_t pipes[PIPE_MAX];

static void zero_pipe(pipe_t *p) {
  uint8_t *b = (uint8_t *)p;
  for (uint64_t i = 0; i < sizeof *p; i++) b[i] = 0;
}

struct pipe *pipe_new(void) {
  for (int i = 0; i < PIPE_MAX; i++) {
    if (pipes[i].used) continue;
    zero_pipe(&pipes[i]);
    pipes[i].used = 1;
    pipes[i].readers = 1;
    pipes[i].writers = 1;
    return &pipes[i];
  }
  return 0;
}

/* Register one more holder of one end. An ofile that is dup()'d does not call
 * this: dup shares the description, and therefore shares its single count. */
void pipe_ref(struct pipe *p, int write_end) {
  if (!p) return;
  if (write_end) p->writers++;
  else           p->readers++;
}

/* Drop a holder. The slot is released only when both sides are gone, which is
 * why a writer closing cannot free a pipe a reader still points into. */
void pipe_unref(struct pipe *p, int write_end) {
  if (!p) return;
  if (write_end) { if (p->writers > 0) p->writers--; }
  else           { if (p->readers > 0) p->readers--; }
  if (p->readers == 0 && p->writers == 0) zero_pipe(p);
}

/* Sleep a tick so producers can run, without giving up the deadline. */
static void pipe_pause(void) {
  if (task_current() > 0) task_sleep_ticks(1);
  else                    task_yield();
}

static uint32_t pipe_take(pipe_t *p, uint8_t *dst, uint32_t n) {
  uint32_t took = 0;
  while (took < n) {
    dst[took++] = p->buf[p->head];
    p->head = (p->head + 1) % PIPE_CAP;
    p->count--;
  }
  return took;
}

static uint32_t pipe_put(pipe_t *p, const uint8_t *src, uint32_t n) {
  uint32_t took = 0;
  while (took < n) {
    p->buf[p->tail] = src[took++];
    p->tail = (p->tail + 1) % PIPE_CAP;
    p->count++;
  }
  return took;
}

long pipe_read(struct pipe *p, void *kbuf, uint64_t n, uint64_t ticks) {
  if (!p || !p->used) return -EBADF;
  if (n == 0) return 0;

  uint64_t end = task_total_ticks() + ticks;
  for (;;) {
    if (p->count > 0) {
      uint32_t want = (n < p->count) ? (uint32_t)n : p->count;
      return (long)pipe_take(p, (uint8_t *)kbuf, want);
    }
    /* Empty and unwritable for ever: this is EOF, and it is not an error. */
    if (p->writers == 0) return 0;
    if (task_total_ticks() >= end) return -EAGAIN;
    pipe_pause();
  }
}

long pipe_write(struct pipe *p, const void *kbuf, uint64_t n, uint64_t ticks) {
  if (!p || !p->used) return -EBADF;
  if (n == 0) return 0;

  uint64_t end = task_total_ticks() + ticks;
  for (;;) {
    /* Nobody will ever read this. Waiting would be waiting for a condition that
     * cannot change; a program that has lost its reader wants to hear about it. */
    if (p->readers == 0) return -EPIPE;
    if (p->count < PIPE_CAP) {
      uint32_t room = PIPE_CAP - p->count;
      uint32_t want = (n < room) ? (uint32_t)n : room;
      return (long)pipe_put(p, (const uint8_t *)kbuf, want);
    }
    if (task_total_ticks() >= end) return -EAGAIN;
    pipe_pause();
  }
}

unsigned pipe_poll(struct pipe *p, int write_end) {
  if (!p || !p->used) return OMI_POLLERR;
  if (write_end) {
    unsigned r = 0;
    if (p->readers == 0)      r |= OMI_POLLERR | OMI_POLLHUP;
    else if (p->count < PIPE_CAP) r |= OMI_POLLOUT;
    return r;
  }
  unsigned r = 0;
  if (p->count > 0)   r |= OMI_POLLIN;
  /* A closed writer makes the read end readable: read() returns 0 immediately,
   * and a poller that did not learn this would wait for data that never comes. */
  if (p->writers == 0) r |= OMI_POLLIN | OMI_POLLHUP;
  return r;
}

/* ---------------------------------------------------------------- self-test */

static int p_ok, p_n;
static char p_bad[40];
static int  p_nbad;

static void pchk(int cond) {
  p_n++;
  if (cond) { p_ok++; return; }
  if (p_nbad < (int)sizeof p_bad - 1) p_bad[p_nbad++] = (char)('a' + p_n % 26);
}

static void pnum(char *m, int *o, int v) {
  char d[8]; int k = 0;
  if (!v) d[k++] = '0';
  while (v) { d[k++] = (char)('0' + v % 10); v /= 10; }
  while (k) m[(*o)++] = d[--k];
}

/* A writer that shows up late, so the blocked read has to actually block and be
 * woken by somebody else rather than by its own first look. */
static struct pipe *wr_pipe;
static void pipewr(void) {
  task_sleep_ticks(3);
  if (wr_pipe) pipe_write(wr_pipe, "abc", 3, 50);
}

void pipe_selftest(void) {
  p_ok = 0; p_n = 0;

  struct pipe *a = pipe_new();
  pchk(a != 0);

  /* Empty, writer alive: nothing to read yet, and no EOF lie. */
  char b[64];
  pchk(pipe_read(a, b, 8, 0) == -EAGAIN);
  pchk(pipe_poll(a, 0) == 0);

  /* Round trip, including the byte order. */
  pchk(pipe_write(a, "hello", 5, 10) == 5);
  pchk(pipe_poll(a, 0) & OMI_POLLIN);
  pchk(pipe_read(a, b, 8, 10) == 5);
  pchk(b[0] == 'h' && b[4] == 'o');

  /* Partial read is bounded by what is there, not by what was asked for. */
  pchk(pipe_write(a, "0123456789", 10, 10) == 10);
  pchk(pipe_read(a, b, 4, 10) == 4);
  pchk(b[0] == '0' && b[3] == '3');
  pchk(pipe_read(a, b, 16, 10) == 6);
  pchk(b[0] == '4' && b[5] == '9');

  /* Full pipe: the next write must wait, and the wait must expire, not lie. */
  static uint8_t big[PIPE_CAP];
  for (int i = 0; i < PIPE_CAP; i++) big[i] = (uint8_t)(i & 0xFF);
  pchk(pipe_write(a, big, PIPE_CAP, 20) == PIPE_CAP);
  pchk(pipe_write(a, "x", 1, 0) == -EAGAIN);
  pchk(!(pipe_poll(a, 1) & OMI_POLLOUT));

  /* Drain the full pipe from the front, in order. */
  pchk(pipe_read(a, b, 30, 10) == 30);
  pchk(b[0] == 0 && b[29] == 29);

  /* Wraparound on a pipe of its own, driven to the one point that matters: put
   * both pointers at CAP-8, then write 12 bytes so the last 4 land at the start
   * of the buffer. A ring that loses its place only there passes everything
   * above, so this is the check that is worth its own pipe. */
  static char sink[PIPE_CAP];
  struct pipe *wr = pipe_new();
  pchk(wr != 0);
  pchk(pipe_write(wr, big, PIPE_CAP - 8, 20) == PIPE_CAP - 8);
  pchk(pipe_read(wr, sink, PIPE_CAP - 8, 20) == PIPE_CAP - 8);
  pchk(pipe_write(wr, "ABCDEFGHIJKL", 12, 20) == 12);
  pchk(pipe_read(wr, b, 12, 20) == 12);
  pchk(b[0] == 'A' && b[11] == 'L');
  pipe_unref(wr, 0);
  pipe_unref(wr, 1);

  /* EOF: the write end goes away, the read end still drains then reports 0. */
  struct pipe *e = pipe_new();
  pchk(pipe_write(e, "xy", 2, 10) == 2);
  pipe_unref(e, 1);                       /* last writer closed */
  pchk(pipe_poll(e, 0) & OMI_POLLIN);     /* readable: data first */
  pchk(pipe_read(e, b, 4, 10) == 2);
  pchk(pipe_poll(e, 0) & OMI_POLLHUP);
  pchk(pipe_read(e, b, 4, 10) == 0);      /* and now EOF, immediately */
  pipe_unref(e, 0);
  pchk(!e->used);                         /* both ends gone: slot released */

  /* EPIPE: writing into a pipe whose reader has gone. */
  struct pipe *w = pipe_new();
  pipe_unref(w, 0);
  pchk(pipe_write(w, "z", 1, 10) == -EPIPE);
  pchk(pipe_poll(w, 1) & OMI_POLLERR);
  pipe_unref(w, 1);
  pchk(!w->used);

  /* The blocking path itself, with a real second task doing the writing. */
  wr_pipe = pipe_new();
  int wid = task_spawn("pipewr", pipewr);
  pchk(wid >= 0);
  long got = pipe_read(wr_pipe, b, 8, 200);
  pchk(got == 3);
  pchk(b[0] == 'a' && b[2] == 'c');
  pipe_unref(wr_pipe, 0);
  pipe_unref(wr_pipe, 1);
  wr_pipe = 0;

  pipe_unref(a, 0);
  pipe_unref(a, 1);
  pchk(!a->used);

  /* Everything that was created above must be back in the pool. */
  int live = 0;
  for (int i = 0; i < PIPE_MAX; i++) if (pipes[i].used) live++;
  pchk(live == 0);

  char m[40]; int o = 0;
  const char *pre = "[pipe] ";
  while (*pre) m[o++] = *pre++;
  pnum(m, &o, p_ok);
  const char *mid = "/";
  while (*mid) m[o++] = *mid++;
  pnum(m, &o, p_n);
  const char *post = " ok, failing: ";
  while (*post) m[o++] = *post++;
  if (!p_nbad) m[o++] = '-';
  for (int i = 0; i < p_nbad; i++) m[o++] = p_bad[i];
  m[o++] = '\n';
  m[o] = 0;
  serial_puts(m);
}