/* Backtraces and the panic path.
 *
 * A fault used to print one line - vector, error code, rip, cr2 - and stop. That
 * is enough to know *what* happened and useless for knowing *where*: the address
 * is a number, the process is not named, and the frames above it are invisible.
 * This turns the same event into a report someone can act on, which is the
 * difference between debugging a scheduler bug and guessing at one.
 *
 * Three pieces:
 *   - the symbol table, generated at link time (tools/gen_symtab.py) and looked up
 *     by nearest address below, because the kernel image has no size table;
 *   - a frame-pointer walk, which is why the kernel is built
 *     -fno-omit-frame-pointer: the registers are cheap, and without them a
 *     backtrace is a stack scan and a guess;
 *   - panic(), callable from anywhere, that prints the same report and halts.
 *
 * The walk is deliberately suspicious. Every candidate rbp has to be aligned,
 * inside the kernel's own address space, strictly above the frame that pointed at
 * it (the stack grows down, so a caller's frame is at a higher address), and every
 * return address has to land inside the kernel's text. A walk that breaks any of
 * those stops there - a short true backtrace is worth more than a long invented
 * one, and this code runs when something is already wrong.
 */
#include "include/bt.h"
#include "include/gui.h"
#include "include/task.h"

/* From the generated table, and weak on purpose: the build links the kernel once
 * without symbols (that image is what the table is generated from), and that first
 * link has to succeed with no names to look at. The real object overrides these,
 * and an empty table just means every frame prints "?" instead of a name. */
typedef struct {
  uint64_t addr;
  uint32_t name;
} bt_sym_t;
__attribute__((weak)) const char bt_names[] = "";
__attribute__((weak)) const bt_sym_t bt_syms[] = {{0, 0}};
__attribute__((weak)) const unsigned bt_sym_count = 0;

/* From the linker script. */
extern char __text_start[], __text_end[];

static void put_dec(uint64_t v) {
  char buf[21];
  int i = 20;
  buf[i] = '\0';
  if (!v) {
    serial_puts("0");
    return;
  }
  while (v && i > 0) {
    buf[--i] = (char)('0' + (v % 10));
    v /= 10;
  }
  serial_puts(&buf[i]);
}

const char *bt_lookup(uint64_t addr, uint64_t *off) {
  unsigned lo, hi;
  if (!bt_sym_count) return 0;
  lo = 0;
  hi = bt_sym_count;
  while (lo + 1 < hi) {
    unsigned mid = lo + (hi - lo) / 2;
    if (bt_syms[mid].addr <= addr) lo = mid;
    else hi = mid;
  }
  if (bt_syms[lo].addr > addr) return 0;
  if (off) *off = addr - bt_syms[lo].addr;
  return bt_names + bt_syms[lo].name;
}

static int in_text(uint64_t a) {
  return a >= (uint64_t)(uintptr_t)__text_start && a < (uint64_t)(uintptr_t)__text_end;
}

/* The frames, newest first: out[0] is the address that faulted. */
int bt_capture(uint64_t rbp, uint64_t rip, uint64_t *out, int max) {
  int n = 0;
  uint64_t prev = 0;
  if (max <= 0) return 0;
  out[n++] = rip;
  while (n < max) {
    /* Higher half only: the kernel is at -2GB and user space is not, so a
     * value outside that is already not a frame pointer we can follow. */
    if (rbp < 0xFFFF800000000000ULL) break;
    if ((rbp & 7) != 0) break;
    if (prev && rbp <= prev) break;
    uint64_t next_rbp = ((const uint64_t *)(uintptr_t)rbp)[0];
    uint64_t ret = ((const uint64_t *)(uintptr_t)rbp)[1];
    if (!in_text(ret)) break;
    /* The caller's frame pointer points at the return address we were *given* as
     * the starting rip, so the first hop would report the same frame twice. */
    if (ret != out[n - 1]) out[n++] = ret;
    prev = rbp;
    rbp = next_rbp;
  }
  return n;
}

void bt_print(uint64_t rbp, uint64_t rip) {
  uint64_t frames[BT_MAX_FRAMES];
  int n = bt_capture(rbp, rip, frames, BT_MAX_FRAMES);
  for (int i = 0; i < n; i++) {
    uint64_t off = 0;
    const char *name = bt_lookup(frames[i], &off);
    serial_puts("[bt] #");
    put_dec((uint64_t)i);
    serial_puts(" ");
    serial_puthex(frames[i]);
    serial_puts(" ");
    if (name) {
      serial_puts(name);
      serial_puts("+");
      serial_puthex(off);
    } else {
      serial_puts("?");
    }
    serial_puts("\n");
  }
  if (n == 1) serial_puts("[bt] no further frames - frame pointer did not lead anywhere\n");
}

/* Who is running. A fault in a task and a fault in the idle loop are different
 * bugs, and until now the log could not tell them apart. */
void bt_who(void) {
  int cur = task_current();
  if (cur < 0 || cur >= TASK_MAX) {
    serial_puts("[bt] task ? (no current task)\n");
    return;
  }
  serial_puts("[bt] task '");
  serial_puts(task_name(cur));
  serial_puts("' id=");
  put_dec((uint64_t)cur);
  serial_puts("\n");
}

static void halt_here(void) {
  serial_puts("[panic] halted - this cpu will not run again\n");
  for (;;) __asm__ volatile("cli; hlt");
}

void panic_from(uint64_t rbp, uint64_t rip, const char *what) {
  serial_puts("\n[kpanic] ");
  serial_puts(what);
  serial_puts("\n");
  bt_who();
  serial_puts("[panic] at ");
  serial_puthex(rip);
  serial_puts("\n");
  bt_print(rbp, rip);
  halt_here();
}
/* A fault with a chain above it, so the report has something to walk. Reached
 * only via panic_demo(), which is only linked in a PANIC_DEMO=1 build. */
__attribute__((noinline)) static void demo_fault(void) {
  /* Canonical, well above anything this kernel maps, so it faults on the first
   * store instead of quietly landing in physical RAM - 0xDEAD0000 is real memory
   * on a 4 GB VM, which is how the first version of this demo did nothing. */
  volatile uint64_t *nowhere = (volatile uint64_t *)0x0000800000000000ULL;
  *nowhere = 1;
}

__attribute__((noinline)) static void demo_middle(void) {
  demo_fault();
  __asm__ volatile("" ::: "memory");
}

void panic_demo(void) {
  serial_puts("[panic-demo] faulting on purpose three frames deep\n");
  demo_middle();
}

/* The layout r3_isr_fault leaves on the stack, in memory order: the stub pushes
 * rax first and r15 last, so r15 is at the lowest address. */
typedef struct {
  uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
  uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
} stub_regs_t;

static void reg4(const char *n0, uint64_t v0, const char *n1, uint64_t v1,
                 const char *n2, uint64_t v2, const char *n3, uint64_t v3) {
  serial_puts("[fault] ");
  serial_puts(n0); serial_puts("="); serial_puthex(v0); serial_puts(" ");
  serial_puts(n1); serial_puts("="); serial_puthex(v1); serial_puts(" ");
  serial_puts(n2); serial_puts("="); serial_puthex(v2); serial_puts(" ");
  serial_puts(n3); serial_puts("="); serial_puthex(v3); serial_puts("\n");
}

void bt_fault_dump(const uint64_t *saved, uint64_t err, uint64_t rip, uint64_t cs,
                   uint64_t rflags, uint64_t rsp, uint64_t ss) {
  const stub_regs_t *r = (const stub_regs_t *)(const void *)saved;
  serial_puts("[fault] err=");
  serial_puthex(err);
  serial_puts(" rip=");
  serial_puthex(rip);
  serial_puts(" cs=");
  serial_puthex(cs);
  serial_puts(" rflags=");
  serial_puthex(rflags);
  {
    /* Only meaningful for #PF, and harmless to print for #GP: it holds whatever
     * the last page fault left there, which is a hint, not a fact. */
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    serial_puts(" cr2=");
    serial_puthex(cr2);
  }
  serial_puts("\n");
  reg4("rax", r->rax, "rbx", r->rbx, "rcx", r->rcx, "rdx", r->rdx);
  reg4("rsi", r->rsi, "rdi", r->rdi, "rbp", r->rbp, "rsp", rsp);
  reg4("r8", r->r8, "r9", r->r9, "r10", r->r10, "r11", r->r11);
  reg4("r12", r->r12, "r13", r->r13, "r14", r->r14, "r15", r->r15);
  (void)ss;
  bt_who();
  if (cs & 3) {
    serial_puts("[fault] taken from user mode - no kernel backtrace from here\n");
    return;
  }
  bt_print(r->rbp, rip);
}

__attribute__((noinline)) static void panic_demo_middle(void) {
  panic("PANIC_DEMO=2: panic() called from kernel code on purpose");
}

void panic_selftest_demo(void) {
  panic_demo_middle();
}
