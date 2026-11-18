/* The first user-mode program, now a real ELF on the ISO rather than a blob
 * baked into the kernel.
 *
 * kernel/elf.c maps its PT_LOAD segments into a fresh address space and enters
 * _start at CPL 3 with rdi = mode. No libc, no relocations: every service is an
 * `int 0x80` with the number in rax and arguments in rdi/rsi/rdx.
 *
 *   mode 0  say hello, touch .bss, yield, say it again, exit(7)
 *   mode 1  read a kernel address, which must fault and kill only this task
 */

typedef unsigned long u64;

/* Mirror of the kernel's win_cmd_u (kernel/include/guiipc.h): 32 bytes. */
typedef struct {
  int kind;
  int x, y, w, h;
  unsigned int color;
  unsigned long text;
} win_cmd_u;

/* Mirror of kernel win_event_t (kernel/include/guiipc.h): 16 bytes. */
typedef struct {
  int kind;
  int x, y;
  int code;
} win_event_t;

#define EV_MOVE  1
#define EV_DOWN  2
#define EV_UP    3
#define EV_KEY   4
#define EV_CLOSE 5

static long sys3(long n, long a, long b, long c) {
  long r;
  __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
  return r;
}

/* Four-argument syscall: the fourth goes in r8 (the kernel reads it from the
 * saved register block). */
static long sys4(long n, long a, long b, long c, long d) {
  long r;
  register long r8v __asm__("r8") = d;
  __asm__ volatile("int $0x80"
                   : "=a"(r)
                   : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r8v)
                   : "memory");
  return r;
}

static long slen(const char *s) {
  long n = 0;
  while (s[n]) n++;
  return n;
}

static void say(const char *s) { sys3(1, 1, (long)s, slen(s)); }

/* .bss: the loader has to hand us zeroed pages for this to read as empty */
static char scratch[64];

/* ---- hand-rolled number to string (there is no printf in ring 3) -------- */
static int put_uint(char *buf, int n, int v) {
  char tmp[8];
  int t = 0;
  if (v == 0) tmp[t++] = '0';
  while (v > 0) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
  while (t > 0) buf[n++] = tmp[--t];
  return n;
}

static int put_str(char *buf, int n, const char *p) {
  while (*p) buf[n++] = *p++;
  return n;
}

/* The compositor-as-a-server demo.
 *
 * This process never touches a pixel buffer and never busy-waits: it describes
 * a window, then sleeps in SYS_WIN_WAIT until the kernel has an event for it.
 * `m` resizes the window from inside the process, and the last line shows what
 * it can see in the shared segment another process wrote to. */
static void ui_demo(void) {
  long wid = sys3(6, (long)"hello, ring 3", 380, 210);
  if (wid < 0) {
    say("no window available\n");
    sys3(2, 1, 0, 0);
  }

  /* shared segment slot 0: one page, also mapped by the writer process */
  sys3(15, 0, 1, 0);
  char *shm = (char *)sys3(16, 0, 0, 0);
  if (!shm) say("shared segment not available\n");

  /* let the writer process run once before the first paint, so the shared
     segment already holds something when the window appears */
  for (int i = 0; i < 200; i++) {
    if (shm && shm[0]) break;
    sys3(3, 0, 0, 0);                    /* yield */
  }

  int lastx = -20, lasty = -20, clicks = 0, keys = 0, big = 0;
  win_cmd_u c;

  for (;;) {
    /* ---- describe the frame ------------------------------------------- */
    sys3(8, wid, 0x1B1C22, 0);

    c.kind = 2; c.x = 12; c.y = 12; c.w = 0; c.h = 0;
    c.color = 0xF2EEE6; c.text = (unsigned long)"a window owned by a user process";
    sys3(7, wid, (long)&c, 0);

    c.kind = 2; c.x = 12; c.y = 30;
    c.color = 0x9AA0A6; c.text = (unsigned long)"click me, type, or press m to resize";
    sys3(7, wid, (long)&c, 0);

    c.kind = 3; c.x = 8; c.y = 8; c.w = 364; c.h = 176;
    c.color = 0x3A3F4A; c.text = 0;
    sys3(7, wid, (long)&c, 0);

    if (clicks > 0) {                       /* crosshair at the last click */
      c.kind = 1; c.x = lastx - 1; c.y = lasty - 10; c.w = 3; c.h = 21;
      c.color = 0xFFC23D; c.text = 0;
      sys3(7, wid, (long)&c, 0);
      c.kind = 1; c.x = lastx - 10; c.y = lasty - 1; c.w = 21; c.h = 3;
      c.color = 0xFFC23D; c.text = 0;
      sys3(7, wid, (long)&c, 0);
    }

    {
      char buf[96];
      int n = 0;
      n = put_str(buf, n, "clicks ");
      n = put_uint(buf, n, clicks);
      n = put_str(buf, n, "  keys ");
      n = put_uint(buf, n, keys);
      n = put_str(buf, n, "  size ");
      n = put_uint(buf, n, big ? 460 : 380);
      buf[n] = 0;
      c.kind = 4; c.x = 12; c.y = 118; c.w = 0; c.h = 0;
      c.color = 0xFFC23D; c.text = (unsigned long)buf;
      sys3(7, wid, (long)&c, 0);
    }

    if (shm && shm[0]) {                    /* written by the other process */
      char buf[80];
      int n = put_str(buf, n = 0, "shared: ");
      for (int i = 0; i < 40 && shm[i]; i++) buf[n++] = shm[i];
      buf[n] = 0;
      c.kind = 2; c.x = 12; c.y = 146; c.w = 0; c.h = 0;
      c.color = 0x7FD1A8; c.text = (unsigned long)buf;
      sys3(7, wid, (long)&c, 0);
    }

    sys3(9, wid, 0, 0);                     /* present */

    /* ---- sleep until the kernel has an event -------------------------- */
    win_event_t ev;
    long r = sys3(12, wid, (long)&ev, 0);
    if (r < 0) {
      say("window closed by the shell\n");
      sys3(2, 0, 0, 0);
    }

    if (ev.kind == EV_DOWN) {
      lastx = ev.x;
      lasty = ev.y;
      clicks++;
    } else if (ev.kind == EV_KEY) {
      keys++;
      if (ev.code == 'm' || ev.code == 'M') {
        big = !big;
        int w = big ? 460 : 380, h = big ? 250 : 210;
        long packed = (long)((unsigned)w << 16 | (unsigned)h);
        /* x and y ride in a1 (x low, y high), the size in a3 */
        sys4(13, wid, 60 | (60L << 32), 0, packed);
      }
    } else if (ev.kind == EV_CLOSE) {
      say("window closed\n");
      sys3(2, 0, 0, 0);
    }
  }
}


/* Threads: two of them in *this* address space, sharing globals, plus a futex
 * so the starter can sleep instead of spinning. The counter is deliberately not
 * atomic: on one CPU with preemption every 10 ms the increments interleave, and
 * the total lands below 2*n. That number is the interesting part. */
static long thr_done;
static long thr_counter;

static void thread_body(long n) {
  for (long i = 0; i < n; i++) thr_counter++;
  thr_done++;
  sys4(22, (long)&thr_done, 1, 1, 0);           /* FUTEX_WAKE, one waiter */
  sys3(2, 0, 0, 0);                              /* exit this thread */
}

static void thread_demo(void) {
  long n = 5000000;   /* long enough to cross several 10 ms ticks */
  thr_done = 0;
  thr_counter = 0;

  long t1 = sys4(21, (long)thread_body, n, 65536, 0);  /* THREAD_CREATE */
  long t2 = sys4(21, (long)thread_body, n, 65536, 0);

  char buf[80];
  int k = 0;
  k = put_str(buf, k = 0, "threads: started ");
  k = put_uint(buf, k, t1);
  k = put_str(buf, k, " and ");
  k = put_uint(buf, k, t2);
  buf[k] = 0;
  say(buf);
  say("\n");

  long guard = 0;
  while (thr_done < 2 && guard < 3000) {
    /* 20-tick deadline: a wake that arrives before we actually sleep costs one
     * tick, not a hang. */
    sys4(22, (long)&thr_done, 0, thr_done, 20);  /* FUTEX_WAIT */
    guard++;
  }

  k = 0;
  k = put_str(buf, k, "threads: done=");
  k = put_uint(buf, k, thr_done);
  k = put_str(buf, k, " counter=");
  k = put_uint(buf, k, thr_counter);
  k = put_str(buf, k, " of ");
  k = put_uint(buf, k, n * 2);
  buf[k] = 0;
  say(buf);
  say("\n");

  sys3(2, thr_done == 2 ? 0 : 1, 0, 0);
}

/* Demand paging: ask for 4 MB, touch one page every 64 KB, prove the pages only
 * appear where they are touched, then hand the range back. */
static void vm_demo(void) {
  unsigned long len = 4 * 1024 * 1024;
  char *p = (char *)sys3(19, (long)len, 2, 0);      /* PROT_READ|PROT_WRITE */
  if ((long)p < 0) {
    say("vm: alloc refused\n");
    sys3(2, 1, 0, 0);
  }

  int touched = 0;
  for (unsigned long off = 0; off < len; off += 65536) {
    p[off] = (char)(1 + (off & 0x7F));              /* first touch faults it in */
    touched++;
  }

  /* read them back: a lazily-mapped page must keep what was written */
  int good = 0;
  for (unsigned long off = 0; off < len; off += 65536) {
    if (p[off] == (char)(1 + (off & 0x7F))) good++;
  }

  char buf[64];
  int n = 0;
  n = put_str(buf, n = 0, "vm: touched ");
  n = put_uint(buf, n, touched);
  n = put_str(buf, n, " pages, read back ");
  n = put_uint(buf, n, good);
  buf[n] = 0;
  say(buf);
  say("\n");

  sys3(20, (long)p, (long)len, 0);                  /* munmap */
  say("vm: range released\n");
  sys3(2, good == touched ? 0 : 1, 0, 0);
}

/* Two processes, one segment: this one writes, the reader below reads it. */
static void shm_writer(void) {
  sys3(15, 0, 1, 0);
  char *p = (char *)sys3(16, 0, 0, 0);
  if (!p) { say("writer: no shared segment\n"); sys3(2, 1, 0, 0); }
  const char *msg = "written by the writer process";
  for (int i = 0; msg[i]; i++) p[i] = msg[i];
  p[slen(msg)] = 0;
  say("writer: wrote into shared slot 0\n");
  sys3(17, (long)msg, slen(msg), 0);          /* also put it on the clipboard */
  say("writer: put it on the clipboard\n");
  sys3(2, 0, 0, 0);
}

static void shm_reader(void) {
  sys3(15, 0, 1, 0);
  char *p = (char *)sys3(16, 0, 0, 0);
  if (!p) { say("reader: no shared segment\n"); sys3(2, 1, 0, 0); }
  for (int spin = 0; spin < 200000; spin++) {     /* wait for the write */
    if (p[0]) break;
    sys3(3, 0, 0, 0);                             /* yield */
  }
  say("reader: sees \"");
  char buf[48];
  int n = 0;
  for (int i = 0; i < 40 && p[i]; i++) buf[n++] = p[i];
  buf[n] = 0;
  say(buf);
  say("\"\n");

  char cb[64];
  long n2 = sys3(18, (long)cb, 63, 0);
  if (n2 > 0) {
    cb[n2] = 0;
    say("reader: clipboard says \"");
    say(cb);
    say("\"\n");
  } else {
    say("reader: clipboard empty\n");
  }
  sys3(2, 0, 0, 0);
}


/* ---------------------------------------------------------------- futexes */

#define F_WAIT 0
#define F_WAKE 1

static long fx_word;
static long fx_done_word;
/* One slot per waiter: a shared counter would need atomics to be trustworthy,
 * and a test that can lose an update is a test that lies. */
static long fx_result[2];
static long fx_ready[2];

static void fx_waiter(long idx) {
  /* Announce before blocking, so the test can inject its wake instead of
   * guessing how long it takes to get to the futex call. */
  fx_ready[idx] = 1;
  /* Expect 0, give up after 300 ticks (3 s) so a lost wake cannot wedge
   * the boot: it fails with -ETIMEDOUT instead of hanging. */
  long r = sys4(22, (long)&fx_word, F_WAIT, 0, 300);
  fx_result[idx] = r;
  fx_done_word++;
  sys4(22, (long)&fx_done_word, F_WAKE, 1, 0);
  sys3(2, 0, 0, 0);
}

static void futex_demo(void) {
  int pass = 0;

  /* 1. The value already moved on: a wait taken with a stale expectation must
   *    return -EAGAIN immediately rather than block forever. */
  fx_word = 5;
  long r = sys4(22, (long)&fx_word, F_WAIT, 0, 0);
  if (r == -11) { pass++; say("[futex] value already moved: -EAGAIN\n"); }
  else { say("[futex] value already moved: FAIL\n"); }

  /* 2. A pointer outside the user window is rejected, not dereferenced. */
  r = sys4(22, 0x1000, F_WAIT, 0, 0);
  if (r == -14) { pass++; say("[futex] bad address: -EFAULT\n"); }
  else { say("[futex] bad address: FAIL\n"); }

  /* 3. Two sleepers, one wake: exactly one comes back with 0, the other runs
   *    to its deadline and reports -ETIMEDOUT. That covers wake-after-block,
   *    selective wake, and timeout expiry in one go. */
  fx_word = 0;
  fx_done_word = 0;
  fx_result[0] = 1;
  fx_result[1] = 1;
  fx_ready[0] = 0;
  fx_ready[1] = 0;

  long t1 = sys4(21, (long)fx_waiter, 0, 65536, 0);
  long t2 = sys4(21, (long)fx_waiter, 1, 65536, 0);

  /* Wait until both have announced, then let their futex calls actually run.
   * A yield hands the cpu to one task for one slice, so a handful is enough -
   * and a fixed yield count is not, because the loop itself costs ticks. */
  for (int guard = 0; guard < 400 && !(fx_ready[0] && fx_ready[1]); guard++)
    sys3(3, 0, 0, 0);
  for (int i = 0; i < 12; i++) sys3(3, 0, 0, 0);

  fx_word = 1;
  long woke = sys4(22, (long)&fx_word, F_WAKE, 1, 0);

  /* Wait for both to report, with a deadline on every wait so a lost wake
   * costs a timeout rather than the boot. */
  for (int guard = 0; guard < 25; guard++) {
    if (fx_result[0] != 1 && fx_result[1] != 1) break;
    sys4(22, (long)&fx_done_word, F_WAIT, fx_done_word, 300);
  }

  int zeros = 0, timeouts = 0;
  for (int i = 0; i < 2; i++) {
    if (fx_result[i] == 0) zeros++;
    if (fx_result[i] == -110) timeouts++;
  }
  if (woke == 1 && zeros == 1 && timeouts == 1) {
    pass++;
    say("[futex] one waiter woke, the other timed out\n");
  } else {
    say("[futex] selective wake: FAIL\n");
  }

  char buf[64];
  int k = 0;
  k = put_str(buf, k, "futex: ");
  k = put_uint(buf, k, (u64)pass);
  k = put_str(buf, k, " of 3 checks ok");
  buf[k] = 0;
  say(buf);
  say("\n");

  (void)t1; (void)t2;
  sys3(2, pass == 3 ? 0 : 1, 0, 0);
}


/* ------------------------------------------------------------------- vfs */

#define SYS_OPEN   23
#define SYS_READ   24
#define SYS_LSEEK  25
#define SYS_CLOSE  26
#define SYS_DUP    27
#define SYS_WRITED 1
#define SYS_TYPE   2
#define SYS_SYNC   40
#define SYS_VM_ALLOC_ 19

#define F_RDWR    2
#define F_CREAT   0100
#define S_SET     0
#define S_END     2

static char vbuf[256];
static char vbuf2[256];
static int vpass, vtotal;

static void vcheck(const char *what, int ok) {
  vtotal++;
  if (ok) vpass++;
  say("[vfs] ");
  say(ok ? "ok   " : "FAIL ");
  say(what);
  say("\n");
}

static int veq(const char *a, const char *b, int n) {
  for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
  return 1;
}

/* A thread does not get a copy of the descriptor table: it shares the
 * process's, so a descriptor opened in main is the same open file here - and
 * because the offset lives in the open-file description, the two see one
 * cursor. This is the test that protects moving the table into mm_t. */
static long vt_flag;
static int vt_ok, vt_closed;

static void vt_body(long arg) {
  int d = (int)sys3(SYS_DUP, arg, 0, 0);
  char t[8];
  long r = d >= 3 ? sys3(SYS_READ, d, (long)t, 3) : -1;
  vt_ok = (d >= 3) && (r == 3) && veq(t, "hel", 3);
  if (d >= 3) sys3(SYS_CLOSE, d, 0, 0);      /* close the duplicate... */
  vt_closed = 1;
  vt_flag = 1;
  sys4(22, (long)&vt_flag, 1, 1, 0);         /* ...then wake main */
  sys3(2, 0, 0, 0);
}

static void vfs_demo(void) {
  const char *hello = "hello from omi's ram fs\n";
  int hlen = (int)slen(hello);

  int fd = sys3(SYS_OPEN, (long)"/hello.txt", 0, 0);
  vcheck("open a seeded file", fd == 3);

  long r = sys3(SYS_READ, fd, (long)vbuf, 5);
  vcheck("read returns bytes, not a copy of the whole file", r == 5 && veq(vbuf, "hello", 5));

  vcheck("lseek back to the start", sys3(SYS_LSEEK, fd, 0, S_SET) == 0);

  r = sys3(SYS_READ, fd, (long)vbuf, sizeof vbuf);
  vcheck("read the whole file", r == hlen && veq(vbuf, hello, hlen));

  vcheck("read at EOF is 0, not an error", sys3(SYS_READ, fd, (long)vbuf, 16) == 0);
  vcheck("SEEK_END lands on the size", sys3(SYS_LSEEK, fd, 0, S_END) == hlen);

  vcheck("a missing file is -ENOENT", sys3(SYS_OPEN, (long)"/nope", 0, 0) == -2);

  /* write, close, reopen: the bytes have to survive the descriptor */
  int wfd = sys3(SYS_OPEN, (long)"/log.txt", F_RDWR | F_CREAT, 0);
  long w = sys3(1, wfd, (long)"omi was here\n", 13);
  sys3(SYS_CLOSE, wfd, 0, 0);
  int rfd = sys3(SYS_OPEN, (long)"/log.txt", 0, 0);
  r = sys3(SYS_READ, rfd, (long)vbuf, sizeof vbuf);
  vcheck("write then read back through a new descriptor",
         w == 13 && r == 13 && veq(vbuf, "omi was here\n", 13));
  sys3(SYS_CLOSE, rfd, 0, 0);

  /* two opens of one file are two independent cursors */
  int a = sys3(SYS_OPEN, (long)"/hello.txt", 0, 0);
  int b = sys3(SYS_OPEN, (long)"/hello.txt", 0, 0);
  sys3(SYS_READ, a, (long)vbuf, 2);
  sys3(SYS_READ, b, (long)vbuf2, 2);
  vcheck("two opens have independent offsets", veq(vbuf, "he", 2) && veq(vbuf2, "he", 2));

  /* dup is not a second cursor: it shares the one it came from */
  int d = sys3(SYS_DUP, a, 0, 0);
  sys3(SYS_READ, a, (long)vbuf, 3);         /* cursor 2 -> 5 */
  r = sys3(SYS_READ, d, (long)vbuf2, 3);    /* same cursor: 5 -> 8 */
  vcheck("dup shares the offset with its source", d >= 3 && veq(vbuf, "llo", 3) && veq(vbuf2, " fr", 3));
  sys3(SYS_CLOSE, d, 0, 0);
  sys3(SYS_CLOSE, a, 0, 0);
  sys3(SYS_CLOSE, b, 0, 0);

  /* read into memory that has never been touched: the kernel must fault it in
   * rather than panic on a not-present page */
  long page = sys3(SYS_VM_ALLOC_, 4096, 2, 0);
  int vf = sys3(SYS_OPEN, (long)"/motd", 0, 0);
  r = sys3(SYS_READ, vf, page, 16);
  vcheck("read into a fresh vm_alloc page",
         page > 0 && r == 15 && veq((const char *)page, "welcome, human\n", 15));
  sys3(SYS_CLOSE, vf, 0, 0);

  /* a thread shares the table, and a dup is one open file, not two */
  int tf = sys3(SYS_OPEN, (long)"/hello.txt", 0, 0);
  vt_ok = 0; vt_closed = 0; vt_flag = 0;
  long vtid = sys4(21, (long)vt_body, tf, 65536, 0);
  if (vtid < 0) say("[vfs] thread creation failed: no free task slot\n");
  for (int g = 0; g < 20 && !vt_flag; g++)
    sys4(22, (long)&vt_flag, 0, 0, 300);
  vcheck("a thread inherits the descriptor table and its offsets", vt_ok);
  long after = sys3(SYS_READ, tf, (long)vbuf, 3);
  vcheck("closing the duplicate leaves the original open at the same cursor",
         vt_closed && after == 3 && veq(vbuf, "lo ", 3));
  sys3(SYS_CLOSE, tf, 0, 0);

  /* the table has to end somewhere, and say so in Linux's word. Everything
   * above has to be closed first, or the count is 16 minus whatever leaked -
   * which is how the first run of this test failed. */
  sys3(SYS_CLOSE, fd, 0, 0);
  int opened = 0;
  long last = 0;
  /* The table starts at 16 slots and doubles to FD_MAX as a program opens more,
   * so the end is 4096 descriptors away, not 13. What has to stay true is that
   * growing is silent and the end is loud: -EMFILE, not a short read or a crash. */
  for (int i = 0; i < 4200; i++) {
    last = sys3(SYS_OPEN, (long)"/motd", 0, 0);
    if (last >= 0) opened++;
    else break;
  }
  vcheck("descriptors run out with -EMFILE", last == -24 && opened > 13 && opened <= 4096);
  for (int i = 3; i < 3 + opened; i++) sys3(SYS_CLOSE, i, 0, 0);

  vcheck("close of a bad descriptor is -EBADF", sys3(SYS_CLOSE, 99, 0, 0) == -9);
  int c = sys3(SYS_OPEN, (long)"/motd", 0, 0);
  sys3(SYS_CLOSE, c, 0, 0);
  vcheck("a closed descriptor cannot be read", sys3(SYS_READ, c, (long)vbuf, 4) == -9);

  char buf[48];
  int k = 0;
  k = put_str(buf, k, "vfs: ");
  k = put_uint(buf, k, (u64)vpass);
  k = put_str(buf, k, " of ");
  k = put_uint(buf, k, (u64)vtotal);
  k = put_str(buf, k, " checks ok");
  buf[k] = 0;
  say(buf);
  say("\n");

  sys3(2, vpass == vtotal ? 0 : 1, 0, 0);
}

/* The disk: a counter that has to survive the machine going away. Boot 1 finds
 * no file and writes 1; boot 2 reads 1 and writes 2. There is no way to fake
 * that from inside one boot, which is why this is the test worth having - every
 * other check in this file would pass with a filesystem that lived in RAM. */
static int dpass, dtotal;

static void dcheck(const char *what, int ok) {
  dtotal++;
  if (ok) dpass++;
  say("[disk] ");
  say(ok ? "ok   " : "FAIL ");
  say(what);
  say("\n");
}

static int parse_u32(const char *s, int n) {
  int v = 0;
  for (int i = 0; i < n && s[i] >= '0' && s[i] <= '9'; i++) v = v * 10 + (s[i] - '0');
  return v;
}

static int read_boots(void) {
  long fd = sys3(SYS_OPEN, (long)"/boots", 0, 0);
  if (fd < 0) return -1;
  char b[32];
  long n = sys3(SYS_READ, fd, (long)b, 31);
  sys3(SYS_CLOSE, fd, 0, 0);
  if (n <= 0) return 0;
  return parse_u32(b, (int)n);
}

static void disk_demo(void) {
  /* If the kernel mounted a disk it publishes this file; without one there is
   * nothing to test and nothing to fail - a machine with no disk is legal. */
  long info = sys3(SYS_OPEN, (long)"/disk.info", 0, 0);
  if (info < 0) {
    say("[disk] no disk attached: the filesystem is ram-only this boot\n");
    sys3(2, 0, 0, 0);
    return;
  }
  char line[96];
  long got = sys3(SYS_READ, info, (long)line, 80);
  sys3(SYS_CLOSE, info, 0, 0);
  if (got > 0) {
    int k = 0;
    char head[96];
    k = put_str(head, k, "[disk] ");
    for (long i = 0; i < got && k < 90; i++) {
      if (line[i] == '\n') break;          /* the model line */
      head[k++] = line[i];
    }
    head[k] = 0;
    say(head);
    say("\n");
  }
  dcheck("the kernel mounted a disk", got > 0);

  int boots = read_boots();
  if (boots < 0) boots = 0;                 /* first boot: no file yet */
  int k = 0;
  k = put_str(line, k, "[disk] this boot read back boots=");
  k = put_uint(line, k, (u64)boots);
  k = put_str(line, k, "\n");
  line[k] = 0;
  say(line);

  /* Write the next number through the same open/write/close path a program uses,
   * so the layer above the disk is tested as well as the disk itself. */
  int next = boots + 1;
  char out[16];
  int m = put_uint(out, 0, (u64)next);
  long fd = sys3(SYS_OPEN, (long)"/boots", F_CREAT | 01000 | F_RDWR, 0);   /* O_TRUNC */
  dcheck("a program can open a file for writing", fd >= 3);
  long w = fd >= 0 ? sys3(1, fd, (long)out, m) : -1;                       /* SYS_WRITE */
  dcheck("and write to it", w == m);
  sys3(SYS_SYNC, 0, 0, 0);
  if (fd >= 0) sys3(SYS_CLOSE, fd, 0, 0);

  int back = read_boots();
  dcheck("what it wrote reads back", back == next);

  /* One byte proves the plumbing, not the sectors. Six sectors of a known
   * pattern prove the last short sector is written and read back as carefully
   * as a full one. */
  static char big[3000];
  for (int i = 0; i < 3000; i++) big[i] = (char)('A' + (i % 26));
  long bfd = sys3(SYS_OPEN, (long)"/blocks", F_CREAT | 01000 | F_RDWR, 0);
  long bw = bfd >= 0 ? sys3(1, bfd, (long)big, 3000) : -1;
  if (bfd >= 0) sys3(SYS_CLOSE, bfd, 0, 0);
  dcheck("a six-sector file writes in one call", bw == 3000);

  long rfd = sys3(SYS_OPEN, (long)"/blocks", 0, 0);
  static char back2[3000];
  long br = rfd >= 0 ? sys3(SYS_READ, rfd, (long)back2, 3000) : -1;
  if (rfd >= 0) sys3(SYS_CLOSE, rfd, 0, 0);
  int same = br == 3000;
  for (int i = 0; same && i < 3000; i++) if (back2[i] != big[i]) same = 0;
  dcheck("and reads back byte for byte", same);

  k = 0;
  k = put_str(line, k, "[disk] ");
  k = put_uint(line, k, (u64)dpass);
  k = put_str(line, k, "/");
  k = put_uint(line, k, (u64)dtotal);
  k = put_str(line, k, "\n");
  line[k] = 0;
  say(line);

  sys3(2, dpass == dtotal ? 0 : 1, 0, 0);
}

void _start(long mode) {
  if (mode == 3) ui_demo();
  if (mode == 4) shm_writer();
  if (mode == 5) shm_reader();
  if (mode == 6) vm_demo();
  if (mode == 7) thread_demo();
  if (mode == 8) futex_demo();
  if (mode == 9) vfs_demo();
  if (mode == 10) disk_demo();

  if (mode == 2) {
    /* Spin forever without a single syscall: only a preemptive scheduler can
     * take the cpu away from this. */
    volatile u64 n = 0;
    for (;;) n++;
  }

  if (mode == 1) {
    /* Kernel text is supervisor-only here; this load must take a #PF. */
    volatile u64 *k = (volatile u64 *)0xFFFFFFFF80000000UL;
    (void)*k;
    sys3(2, 9, 0, 0);          /* only reached if isolation is broken */
    for (;;) {}
  }

  say("hello from ring 3 (elf)\n");

  if (scratch[0] == 0) say("bss reads as zero\n");
  else say("bss was NOT zero\n");

  for (int i = 0; i < 5; i++) scratch[i] = (char)('1' + i);
  scratch[5] = 0;
  say("wrote to bss: ");
  say(scratch);
  say("\n");

  sys3(3, 0, 0, 0);            /* yield: the scheduler has to bring us back */
  say("second syscall, still ring 3\n");

  sys3(2, 7, 0, 0);            /* exit(7) */
  for (;;) {}
}