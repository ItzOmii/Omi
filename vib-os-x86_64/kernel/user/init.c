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

void _start(long mode) {
  if (mode == 3) ui_demo();
  if (mode == 4) shm_writer();
  if (mode == 5) shm_reader();

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