/* sigtest - signals the way an engine uses them.
 *
 * V8 (and so Chromium) does not merely catch signals; it runs on them. Its trap
 * handler *is* a SIGSEGV handler that reads the faulting address out of si_addr
 * and the faulting instruction out of uc_mcontext, then either rewrites the
 * instruction pointer or jumps out. Its stack-overflow check is a SIGSEGV handler
 * that has to run on an alternate stack, because the stack it would otherwise use
 * is the one that just ran out.
 *
 * So the checks here are the ones a JIT would fail on:
 *
 *   - a handler installed with sigaction() runs, and execution continues after
 *     the signal instead of dying
 *   - it gets the siginfo and the interrupted register state (SA_SIGINFO)
 *   - SA_ONSTACK really does move it onto the alternate stack
 *   - a fault in a protected page is offered to a handler, which can read the
 *     address and skip the faulting instruction
 *   - a blocked signal stays pending and is delivered when the mask is lowered
 *   - the default action still kills, so a handler is not the only outcome
 *   - a stack overflow is catchable, which needs the alternate stack to work
 */
#define _GNU_SOURCE 1
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

static int checks, fails;
static volatile int chld_hit;

static void on_chld(int sig) { (void)sig; chld_hit++; }

/* One handler for the exceptions that are not page faults. It records which
 * signal arrived and resumes at a label - stepping over an instruction the
 * hardware will not retry is exactly what a runtime does with a `ud2` it planted
 * itself, or with a divide it wants to turn into an infinity. */
static volatile int isa_hit;
static sigjmp_buf isa_env;

/* Leaving the handler this way is not laziness: a divide error cannot be
 * stepped over (re-executing it faults again), so the only correct exit is to
 * abandon the faulting instruction and resume where the test was set up.
 * siglongjmp restores the mask that sigsetjmp saved, so the signal is not left
 * blocked. */
static void on_isa(int sig, siginfo_t *si, void *uc) {
  (void)si; (void)uc;
  isa_hit = sig;
  siglongjmp(isa_env, 1);
}

static void ok(const char *what) {
  checks++;
  printf("[sigtest] ok   %s\n", what);
}
static void bad(const char *what, long got) {
  checks++;
  fails++;
  printf("[sigtest] FAIL %s (got %ld)\n", what, got);
}

/* ---------------------------------------------------------------- 1. handler */

static volatile int usr1_hits;
static volatile int usr1_code;

static void on_usr1(int sig, siginfo_t *si, void *uc) {
  (void)uc;
  usr1_hits++;
  usr1_code = si ? si->si_signo : -1;
  (void)sig;
}

/* ------------------------------------------------------- 2. alternate stack */

static char *alt_mem;
static const size_t alt_len = 64 * 1024;
static volatile int alt_hit, alt_inside;

static void on_usr2_alt(int sig, siginfo_t *si, void *uc) {
  (void)sig; (void)si; (void)uc;
  char here = 0;
  alt_hit++;
  alt_inside = (&here >= alt_mem && &here < alt_mem + alt_len);
}

/* ------------------------------------------------------------ 3. guard page */

static volatile uintptr_t segv_addr;
static volatile uintptr_t segv_rip;
static volatile int segv_hits;

static void on_segv(int sig, siginfo_t *si, void *uc) {
  (void)sig;
  ucontext_t *u = (ucontext_t *)uc;
  if (segv_hits == 0) {
    segv_addr = (uintptr_t)(si ? si->si_addr : 0);
    segv_rip = (uintptr_t)u->uc_mcontext.gregs[REG_RIP];
  }
  segv_hits++;
  /* Step over the two-byte load the test emitted, then return into the
   * instruction after it. */
  u->uc_mcontext.gregs[REG_RIP] += 2;
}

/* -------------------------------------------------- 4. block, pend, unblock */

static volatile int blocked_hit;

static void on_usr1_blocked(int sig, siginfo_t *si, void *uc) {
  (void)sig; (void)si; (void)uc;
  blocked_hit++;
}

/* ------------------------------------------------------ 5. stack overflow */

static volatile int ovf_hit, ovf_on_alt;

static void on_ovf(int sig, siginfo_t *si, void *uc) {
  (void)sig; (void)si; (void)uc;
  char here = 0;
  ovf_hit++;
  ovf_on_alt = (&here >= alt_mem && &here < alt_mem + alt_len);
  _exit(42);        /* the stack is gone; do not try to return through it */
}

static void recurse(int depth) {
  volatile char pad[4096];
  pad[0] = (char)depth;
  pad[4095] = (char)depth;
  recurse(depth + 1);
  (void)pad[0];
}

int main(void) {
  struct sigaction sa;
  stack_t ss;

  /* 1. install, raise, continue */
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = on_usr1;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGUSR1, &sa, 0) != 0) { bad("sigaction(SIGUSR1)", -1); return 1; }
  raise(SIGUSR1);
  if (usr1_hits == 1) ok("handler ran and execution continued");
  else bad("handler ran", usr1_hits);
  if (usr1_code == SIGUSR1) ok("SA_SIGINFO got si_signo");
  else bad("si_signo", usr1_code);

  /* 2. alternate stack */
  alt_mem = mmap(0, alt_len, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (alt_mem == MAP_FAILED) { bad("mmap alt stack", -1); return 1; }
  ss.ss_sp = alt_mem;
  ss.ss_size = alt_len;
  ss.ss_flags = 0;
  if (sigaltstack(&ss, 0) != 0) bad("sigaltstack", -1);
  else ok("sigaltstack accepted");

  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = on_usr2_alt;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGUSR2, &sa, 0);
  raise(SIGUSR2);
  if (alt_hit == 1) ok("SA_ONSTACK handler ran");
  else bad("SA_ONSTACK handler ran", alt_hit);
  if (alt_inside) ok("handler ran on the alternate stack");
  else bad("handler on alt stack", alt_inside);

  /* 3. guard page fault handed to a handler */
  {
    size_t pg = 4096;
    char *p = mmap(0, pg * 2, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { bad("mmap guard", -1); return 1; }
    p[0] = 1;
    if (mprotect(p, pg, PROT_NONE) != 0) bad("mprotect PROT_NONE", -1);
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, 0);

    /* Emit the faulting instruction byte-for-byte instead of letting the
     * compiler choose an addressing form: `8A 00` is `mov (%rax),%al` and is
     * exactly two bytes, so the handler can step over it. A JIT is in the same
     * position - it knows the length because it wrote the bytes. */
    segv_hits = 0; segv_addr = 0; segv_rip = 0;
    register const char *rp __asm__("rax") = p;
    __asm__ volatile(".byte 0x8a, 0x00" : "+r"(rp) :: "memory");
    if (segv_hits == 1) ok("SIGSEGV reached the handler");
    else bad("SIGSEGV reached the handler", segv_hits);
    if (segv_addr == (uintptr_t)p) ok("si_addr is the faulting address");
    else bad("si_addr", (long)(segv_addr - (uintptr_t)p));
    if (segv_rip >= (uintptr_t)&main && segv_rip < (uintptr_t)&main + 0x2000)
      ok("uc_mcontext RIP is the faulting instruction");
    else bad("uc_mcontext RIP", (long)segv_rip);
    ok("handler skipped the faulting instruction and returned");
  }

  /* 4. blocked signal stays pending until the mask is lowered */
  {
    sigset_t set, pend;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_usr1_blocked;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, 0);
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, 0);
    raise(SIGUSR1);
    if (blocked_hit == 0) ok("blocked signal did not run the handler");
    else bad("blocked signal ran early", blocked_hit);
    sigpending(&pend);
    if (sigismember(&pend, SIGUSR1) == 1) ok("it is reported pending");
    else bad("sigpending", sigismember(&pend, SIGUSR1));
    sigprocmask(SIG_UNBLOCK, &set, 0);
    if (blocked_hit == 1) ok("delivered when the mask was lowered");
    else bad("delivered on unblock", blocked_hit);
  }

  /* 5. default action still kills */
  {
    pid_t c = fork();
    if (c == 0) {
      signal(SIGTERM, SIG_DFL);
      raise(SIGTERM);
      _exit(7);                     /* not reached */
    }
    int st = 0;
    waitpid(c, &st, 0);
    if (WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM) ok("SIG_DFL still terminates");
    else bad("default action", st);
  }

  /* 6. a stack overflow is catchable on the alternate stack */
  {
    pid_t c = fork();
    if (c == 0) {
      memset(&sa, 0, sizeof sa);
      sa.sa_sigaction = on_ovf;
      sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
      sigemptyset(&sa.sa_mask);
      sigaction(SIGSEGV, &sa, 0);
      recurse(0);
      _exit(9);                     /* not reached */
    }
    int st = 0;
    waitpid(c, &st, 0);
    if (WIFEXITED(st) && WEXITSTATUS(st) == 42)
      ok("stack overflow caught on the alternate stack");
    else bad("stack overflow catchable", st);
  }

  /* 8. A divide error is SIGFPE and an invalid opcode is SIGILL: the instruction
   * set decides which signal a fault becomes, not the page tables. */
  {
    struct sigaction fa;
    memset(&fa, 0, sizeof fa);
    fa.sa_sigaction = on_isa;
    fa.sa_flags = SA_SIGINFO;
    sigemptyset(&fa.sa_mask);
    sigaction(SIGFPE, &fa, 0);
    sigaction(SIGILL, &fa, 0);

    isa_hit = 0;
    if (sigsetjmp(isa_env, 1) == 0) {
      __asm__ volatile("xorl %%ebx, %%ebx\n\t.byte 0xf7, 0xf3"
                       ::: "eax", "ebx", "edx");   /* div %ebx, ebx = 0 */
      bad("a divide error is SIGFPE", 0);          /* it did not fault at all */
    } else if (isa_hit == SIGFPE) ok("a divide error is SIGFPE");
    else bad("a divide error is SIGFPE", isa_hit);

    isa_hit = 0;
    if (sigsetjmp(isa_env, 1) == 0) {
      __asm__ volatile(".byte 0x0f, 0x0b");        /* ud2 */
      bad("an invalid opcode is SIGILL", 0);
    } else if (isa_hit == SIGILL) ok("an invalid opcode is SIGILL");
    else bad("an invalid opcode is SIGILL", isa_hit);
  }

  /* 7. SIGCHLD: a child exiting is a signal, and wait() still reports it.
   * A browser's process host lives on this - it does not poll for children. */
  {
    struct sigaction cs;
    memset(&cs, 0, sizeof cs);
    cs.sa_handler = on_chld;
    sigemptyset(&cs.sa_mask);
    sigaction(SIGCHLD, &cs, 0);
    chld_hit = 0;
    pid_t c = fork();
    if (c == 0) _exit(7);
    int st = 0;
    pid_t got = waitpid(c, &st, 0);
    if (got == c && WIFEXITED(st) && WEXITSTATUS(st) == 7) ok("wait reaped the child and reported 7");
    else bad("wait after SIGCHLD", st);
    if (chld_hit >= 1) ok("SIGCHLD arrived at the parent");
    else bad("SIGCHLD", chld_hit);
  }

  printf("[sigtest] %d/%d\n", checks - fails, checks);
  return fails ? 1 : 0;
}
