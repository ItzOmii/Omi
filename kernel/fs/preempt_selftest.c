/* preempt_selftest - does the timer actually switch tasks?
 *
 * Invariant 8 says network progress must not depend on what ring 3 is doing.
 * The pump task can only satisfy that if the scheduler can take the CPU away from
 * a ring-3 task that never yields. Those are different claims:
 *
 *   the timer fires        -> task_total_ticks() advances
 *   the scheduler switches -> another task's code runs
 *
 * A system can have the first without the second, and then every "it works" test
 * passes only because the application happened to make a syscall. This test
 * separates them with one counter and one cooperative-looking program that makes
 * no syscalls at all.
 *
 * Method. A kernel probe increments a counter and sleeps a tick, so it can only
 * advance when the scheduler runs it. First measure it in a quiet system (it
 * must advance - that proves the probe works). Then spawn `spin.elf`, which
 * loops on its own arithmetic and never traps, and measure the same two things
 * across that window. Whatever the outcome, the numbers say which layer is
 * responsible - and the ring-3 program is bounded by a count, not by the clock,
 * so a failure cannot hang the boot.
 */

#include "../include/task.h"
#include "../include/elf.h"
#include "../include/omi_net.h"
#include "../include/gui.h"

static volatile uint64_t probe_runs;

static void probe_task(void) {
  for (;;) {
    probe_runs++;
    /* Sleep, not yield: advancing requires the scheduler to pick us back up,
     * which is exactly the thing under test. */
    task_sleep_ticks(1);
  }
}

static int name_is(const char *a, const char *b) {
  while (*a && *b) { if (*a != *b) return 0; a++; b++; }
  return *a == *b;
}

/* A live task by name. TASK_DEAD means it exited and will not run again. */
static int find_live(const char *name) {
  int n = task_count();
  for (int i = 0; i < n; i++) {
    task_t *t = task_at(i);
    if (!t || t->state == TASK_FREE || t->state == TASK_DEAD) continue;
    const char *nm = task_name(i);
    if (nm && name_is(nm, name)) return i;
  }
  return -1;
}

/* task_sleep_ticks() refuses task 0, so the boot task waits by yielding. */
static void wait_ticks(uint64_t n) {
  uint64_t want = task_total_ticks() + n;
  while (task_total_ticks() < want) task_yield();
}

static void say(const char *label, long a, long b, long c) {
  serial_puts("[pre] ");
  serial_puts(label);
  for (int k = 0; k < 3; k++) {
    long v = k == 0 ? a : (k == 1 ? b : c);
    char d[12]; int o = 0; int neg = v < 0;
    if (neg) v = -v;
    if (!v) d[o++] = '0';
    while (v) { d[o++] = (char)('0' + v % 10); v /= 10; }
    serial_puts(" ");
    if (neg) serial_puts("-");
    while (o) { char ch = d[--o]; char s[2]; s[0] = ch; s[1] = 0; serial_puts(s); }
  }
  serial_puts("\n");
}

static int pre_ok, pre_total;
static void pchk(const char *what, int cond) {
  pre_total++;
  if (cond) { pre_ok++; return; }
  serial_puts("[pre] FAILED: ");
  serial_puts(what);
  serial_puts("\n");
}

void preempt_selftest(void) {
  serial_puts("[pre] preemption: timer ticks versus scheduler switches\n");

  int probe = task_spawn("probe", probe_task);
  if (probe < 0) { serial_puts("[pre] FAILED: no probe task\n"); return; }
  wait_ticks(10);                       /* let the probe find its rhythm */

  /* --- control: in a quiet system the probe must advance ------------------ */
  uint64_t t0 = task_total_ticks(), r0 = probe_runs, f0 = tcp_pump_count();
  wait_ticks(30);
  uint64_t quiet_ticks = task_total_ticks() - t0;
  uint64_t quiet_runs = probe_runs - r0;
  uint64_t quiet_frames = tcp_pump_count() - f0;
  say("quiet ticks/probe/pump", (long)quiet_ticks, (long)quiet_runs, (long)quiet_frames);
  pchk("the probe runs when the system is quiet", quiet_runs > 0);
  pchk("the pump runs when the system is quiet", quiet_frames > 0);
  if (quiet_runs == 0) {
    serial_puts("[pre] FAILED: the probe never ran even with nothing else going on\n");
    return;                             /* the measurement below would mean nothing */
  }

  /* --- the experiment: ring 3 spins, and makes no syscalls ----------------- */
  int sid = ring3_spawn_elf("spinner", "spin.elf", 0);
  if (sid < 0) { serial_puts("[pre] FAILED: could not spawn spin.elf\n"); return; }

  uint64_t t1 = task_total_ticks(), r1 = probe_runs, f1 = tcp_pump_count();
  /* This loop cannot run while the spinner holds the CPU. If the scheduler never
   * switches, we resume only after the spinner exits - and the tick delta below
   * is the whole spin. The spinner is bounded by a count, so that always comes. */
  while (find_live("spinner") >= 0) task_yield();
  uint64_t spin_ticks  = task_total_ticks() - t1;
  uint64_t spin_runs   = probe_runs - r1;
  uint64_t spin_frames = tcp_pump_count() - f1;
  say("spin ticks/probe/pump", (long)spin_ticks, (long)spin_runs, (long)spin_frames);

  /* --- recovery: the counter was not broken by any of this ---------------- */
  uint64_t r2 = probe_runs;
  wait_ticks(20);
  uint64_t after_runs = probe_runs - r2;
  say("after ticks/probe", (long)(task_total_ticks() - (t1 + spin_ticks)), (long)after_runs, 0);

  /* The window has to be real before any conclusion counts. If the spinner
   * finished instantly there was nothing to preempt, and "probe advanced" would
   * merely describe an idle machine. */
  pchk("the spinner really held the cpu", spin_ticks > 0);
  pchk("the timer fired during the spin", spin_ticks > 0);
  /* The distinction under test: a tick is not a switch. */
  pchk("the scheduler switched away from the spinner", spin_runs > 0);
  /* Invariant 8's actual claim, in its strongest form: the ring-3 program made
   * no syscalls at all, and the network still made progress. */
  pchk("the pump made progress during a syscall-free ring-3 loop", spin_frames > 0);

  if (spin_ticks == 0) {
    serial_puts("[pre] INCONCLUSIVE: the spinner did not run at all\n");
    return;
  }
  if (spin_runs == 0) {
    /* The timer fired - the clock moved - while no other task got the CPU. */
    serial_puts("[pre] VERDICT: the timer fires, the scheduler does not switch\n");
    serial_puts("[pre]   a ring-3 loop without syscalls stops the kernel dead\n");
    serial_puts("[pre]   invariant 8 is therefore NOT covered\n");
  } else {
    serial_puts("[pre] VERDICT: ring 3 is preempted; kernel tasks keep running\n");
  }

  char nb[16]; int o = 0, v = pre_total;
  if (!v) nb[o++] = '0';
  while (v) { nb[o++] = (char)('0' + v % 10); v /= 10; }
  for (int a = 0, b = o - 1; a < b; a++, b--) { char c = nb[a]; nb[a] = nb[b]; nb[b] = c; }
  nb[o] = 0;
  serial_puts("[pre] ");
  serial_puts(nb);
  serial_puts(pre_ok == pre_total ? " checks, all ok (preemption)\n"
                                  : " checks, some failed (preemption)\n");
}