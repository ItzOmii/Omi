#include "include/task.h"
#include "include/kmalloc.h"
#include "include/idt.h"

/* Save callee-saved state of the current task on its own stack, publish that
 * rsp through `save`, then adopt `next`. The 6 pushed slots plus the return
 * address are exactly what a fresh task stack is primed with below. */
__asm__(
".text\n"
".global task_switch_asm\n"
".type task_switch_asm, @function\n"
"task_switch_asm:\n"
"  pushq %rbp\n"
"  pushq %rbx\n"
"  pushq %r12\n"
"  pushq %r13\n"
"  pushq %r14\n"
"  pushq %r15\n"
"  movq %rsp, (%rdi)\n"
"  movq %rsi, %rsp\n"
"  popq %r15\n"
"  popq %r14\n"
"  popq %r13\n"
"  popq %r12\n"
"  popq %rbx\n"
"  popq %rbp\n"
"  ret\n"
".size task_switch_asm, .-task_switch_asm\n");

extern void task_switch_asm(uint64_t *save, uint64_t next);

static task_t tasks[TASK_MAX];
static int ntasks;
static int cur = -1;
static volatile uint64_t total_ticks;

void task_init(void) {
  for (int i = 0; i < TASK_MAX; i++) {
    tasks[i].state = TASK_FREE;
    tasks[i].rsp = 0;
  }
  ntasks = 1;
  cur = 0;
  tasks[0].id = 0;
  tasks[0].state = TASK_RUN;
  const char *n = "kernel";
  int i = 0;
  while (n[i] && i < 23) { tasks[0].name[i] = n[i]; i++; }
  tasks[0].name[i] = 0;
}

int task_count(void) { return ntasks; }
task_t *task_at(int i) { return (i >= 0 && i < ntasks) ? &tasks[i] : 0; }
int task_current(void) { return cur; }

const char *task_state_name(int st) {
  switch (st) {
    case TASK_RUN:   return "running";
    case TASK_READY: return "ready";
    case TASK_DEAD:  return "done";
  }
  return "free";
}

static void task_trampoline(void) {
  int me = cur;
  if (me >= 0 && tasks[me].fn) tasks[me].fn();
  tasks[me].state = TASK_DEAD;
  for (;;) task_yield();           /* a finished task parks here */
}

int task_spawn(const char *name, void (*fn)(void)) {
  if (ntasks >= TASK_MAX) return -1;
  int id = ntasks;
  task_t *t = &tasks[id];
  uint8_t *mem = (uint8_t *)kmalloc(TASK_STACK);
  if (!mem) return -1;
  t->stack = mem;
  t->fn = fn;
  t->id = id;
  t->ticks = 0;
  t->switches = 0;
  t->work = 0;
  int i = 0;
  while (name[i] && i < 23) { t->name[i] = name[i]; i++; }
  t->name[i] = 0;

  /* Prime the stack so the first switch "returns" into task_trampoline with a
   * 16-byte aligned frame: 6 saved registers, the entry address, one pad. */
  uintptr_t top = ((uintptr_t)mem + TASK_STACK) & ~(uintptr_t)15;
  uint64_t *sp = (uint64_t *)(top - 64);
  for (int k = 0; k < 6; k++) sp[k] = 0;
  sp[6] = (uint64_t)(uintptr_t)task_trampoline;
  sp[7] = 0;
  t->rsp = (uint64_t)(uintptr_t)sp;
  t->state = TASK_READY;
  ntasks++;
  return id;
}

void task_block(int blocked) {
  if (cur <= 0) return;
  tasks[cur].state = blocked ? TASK_READY : TASK_RUN;
}

/* Round robin. Called from ordinary C context only. */
void task_yield(void) {
  if (ntasks < 2 || cur < 0) return;
  int next = -1;
  for (int k = 1; k <= ntasks; k++) {
    int i = (cur + k) % ntasks;
    if (i == cur) continue;
    /* task 0 is the compositor: it stays RUN while parked in task_yield */
    if (i == 0 || tasks[i].state == TASK_READY) { next = i; break; }
  }
  if (next < 0) return;                 /* only the kernel task is runnable */

  { extern void serial_puts(const char *s);
    static int dbg; if (dbg < 12) { dbg++;
      serial_puts("[sw] "); char b[4]={ (char)('0'+cur), '-', (char)('0'+next), 0 };
      serial_puts(b); serial_puts("\n"); } }
  int prev = cur;
  tasks[prev].state = (prev == 0) ? TASK_RUN : TASK_READY;
  tasks[next].state = TASK_RUN;
  tasks[next].switches++;
  cur = next;
  task_switch_asm(&tasks[prev].rsp, tasks[next].rsp);
  cur = prev;                           /* resumed: we are the running task again */
}

/* IRQ0: charge the tick to whoever is running. No switching here. */
void task_tick(void) {
  total_ticks++;
  if (cur >= 0 && cur < ntasks) tasks[cur].ticks++;
}
/* ------------------------------------------------------------ PIT + IRQ0 */

static inline void outb(uint16_t port, uint8_t v) {
  __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

static void irq0_handler(interrupt_frame_t *f) { (void)f; task_tick(); }

/* Bring the scheduler up: a 100 Hz PIT and IRQ0 unmasked. Called with
 * interrupts still off, before the first sti. */
void task_system_start(void) {
  task_init();
  uint16_t div = (uint16_t)(1193182 / 100);
  outb(0x43, 0x36);
  outb(0x40, (uint8_t)(div & 0xFF));
  outb(0x40, (uint8_t)(div >> 8));
  idt_register_handler(0x20, irq0_handler);
  pic_clear_mask(0);
}

/* Two worker tasks, so the scheduler has something to switch to. They only
 * bump their own counter: nothing here touches the framebuffer or the heap. */
/* A little real work, so the scheduler has something to charge time to. */
static void worker_body(unsigned spins) {
  for (;;) {
    int me = cur;
    if (me > 0 && me < ntasks) tasks[me].work++;
    for (volatile unsigned i = 0; i < spins; i++) __asm__ volatile("pause");
    task_yield();
  }
}
static void worker_netd(void) { worker_body(20000); }
static void worker_idle(void) { worker_body(4000); }

void task_demo_start(void) {
  task_spawn("netd", worker_netd);
  task_spawn("idle", worker_idle);
}
