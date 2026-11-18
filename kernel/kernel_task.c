/* Kernel tasks, a preemptive round-robin scheduler, and the PIT tick.
 *
 * Every task's suspended state is an interrupt frame: the 15 general registers
 * this file pushes, followed by rip/cs/rflags (and rsp/ss when the task was in
 * ring 3). That single shape is what makes preemption possible - IRQ0 arrives,
 * the cpu has already pushed a frame, we push the registers, hand the whole
 * thing to the scheduler, and on the way out iretq into whichever task was
 * picked. A voluntary switch (task_yield) takes the same path through `int
 * 0x81`, so there is only one way a context is saved or restored.
 *
 * Preemption is limited to ring 3: IRQ0 only switches when the interrupted CS
 * has RPL 3. Kernel tasks still leave the cpu at explicit yield points, which
 * keeps the kernel single-threaded - no kernel data structure is ever touched
 * by two tasks at once, and no locks are needed. A user program cannot hog the
 * machine: it gets preempted 100 times a second whether it cooperates or not. */

#include "include/task.h"
#include "include/kmalloc.h"
#include "include/idt.h"
#include "include/gdt.h"
#include "include/ring3.h"
#include "include/socket.h"

extern void serial_puts(const char *s);

/* The saved frame. Order must match the push order in the stubs below. */
typedef struct {
  uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
  uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
  uint64_t rip, cs, rflags;
  uint64_t rsp, ss;          /* only meaningful for a ring 3 frame */
} ctx_t;

#define CTX_REGS   15
#define CTX_KERNEL 18          /* regs + rip,cs,rflags          */
#define CTX_USER   20          /* regs + rip,cs,rflags,rsp,ss   */

/* Where a brand new kernel task starts. It arrives by iretq (not by call), so
 * it fixes up the stack alignment before entering C, and parks if the body ever
 * returns. The function to run comes in from the primed frame in rdi. */
__asm__(
".text\n"
".global k_task_entry\n"
".type k_task_entry, @function\n"
"k_task_entry:\n"
"  andq $-16, %rsp\n"
"  callq *%rdi\n"
"1:\n"
"  callq task_yield\n"
"  jmp 1b\n"
".size k_task_entry, .-k_task_entry\n"
);
extern void k_task_entry(void);

/* int 0x81: a task gives up the cpu voluntarily. */
__asm__(
".text\n"
".global sched_yield_entry\n"
".type sched_yield_entry, @function\n"
"sched_yield_entry:\n"
"  pushq %rax\n"
"  pushq %rbx\n"
"  pushq %rcx\n"
"  pushq %rdx\n"
"  pushq %rsi\n"
"  pushq %rdi\n"
"  pushq %rbp\n"
"  pushq %r8\n"
"  pushq %r9\n"
"  pushq %r10\n"
"  pushq %r11\n"
"  pushq %r12\n"
"  pushq %r13\n"
"  pushq %r14\n"
"  pushq %r15\n"
"  movq task_fpu_slot(%rip), %rax\n"
"  fxsave (%rax)\n"
"  movq %rsp, %rdi\n"
"  call sched_pick\n"
"  movq %rax, %rsp\n"
"  jmp sched_restore\n"
".size sched_yield_entry, .-sched_yield_entry\n"
);

/* IRQ0: the tick, the PIC ack, and a switch if the interrupted task was in
 * ring 3 and someone else is runnable. */
__asm__(
".text\n"
".global sched_irq0_entry\n"
".type sched_irq0_entry, @function\n"
"sched_irq0_entry:\n"
"  pushq %rax\n"
"  pushq %rbx\n"
"  pushq %rcx\n"
"  pushq %rdx\n"
"  pushq %rsi\n"
"  pushq %rdi\n"
"  pushq %rbp\n"
"  pushq %r8\n"
"  pushq %r9\n"
"  pushq %r10\n"
"  pushq %r11\n"
"  pushq %r12\n"
"  pushq %r13\n"
"  pushq %r14\n"
"  pushq %r15\n"
"  movq task_fpu_slot(%rip), %rax\n"
"  fxsave (%rax)\n"
"  movq %rsp, %rdi\n"
"  call sched_tick\n"
"  movq %rax, %rsp\n"
"sched_restore:\n"
"  movq task_fpu_slot(%rip), %rax\n"
"  fxrstor (%rax)\n"
"  popq %r15\n"
"  popq %r14\n"
"  popq %r13\n"
"  popq %r12\n"
"  popq %r11\n"
"  popq %r10\n"
"  popq %r9\n"
"  popq %r8\n"
"  popq %rbp\n"
"  popq %rdi\n"
"  popq %rsi\n"
"  popq %rdx\n"
"  popq %rcx\n"
"  popq %rbx\n"
"  popq %rax\n"
"  iretq\n"
".size sched_irq0_entry, .-sched_irq0_entry\n"
);

extern void sched_yield_entry(void);
extern void sched_irq0_entry(void);

static task_t tasks[TASK_MAX];
/* Monotonic, so a tgen is never equal to one handed out before. Only matters if
 * task slots ever get reused; today task_alloc appends and never frees. */
static uint32_t task_gen_counter;
static int ntasks;
static int cur = -1;
static volatile uint64_t total_ticks;

/* The FXSAVE area of whichever task is running, so the asm stubs can save the
 * incoming xmm state before any C code touches it. */
void *task_fpu_slot;


static inline void outb(uint16_t port, uint8_t v) {
  __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

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
  __asm__ volatile("fxsave %0" : "=m"(tasks[0].fpu));
  task_fpu_slot = tasks[0].fpu;
}

int task_count(void) { return ntasks; }
task_t *task_at(int i) { return (i >= 0 && i < ntasks) ? &tasks[i] : 0; }
int task_current(void) { return cur; }
const char *task_name(int i) { return (i >= 0 && i < ntasks) ? tasks[i].name : "?"; }
uint64_t task_total_ticks(void) { return total_ticks; }

const char *task_state_name(int st) {
  switch (st) {
    case TASK_RUN:   return "running";
    case TASK_READY: return "ready";
    case TASK_DEAD:  return "done";
    case TASK_BLOCKED: return "blocked";
  }
  return "free";
}

/* The compositor (0) is always eligible; everything else has to be READY. */
static int pick_next(int from) {
  if (ntasks < 2) return -1;
  for (int k = 1; k < ntasks; k++) {
    int i = (from + k) % ntasks;
    if (i == from) continue;
    if (i == 0 || tasks[i].state == TASK_READY) return i;
  }
  return -1;
}

/* Hand the cpu to `next`. The address space and the ring3 landing stack travel
 * with the task, so they change here, before the frame is restored. */
static void switch_to(int next, ctx_t *frame) {
  int prev = cur;
  tasks[prev].rsp = (uint64_t)(uintptr_t)frame;
  /* a task that killed itself must not come back, and a blocked one is waiting
   * for its wake-up rather than for the cpu: leave both alone */
  if (tasks[prev].state != TASK_DEAD && tasks[prev].state != TASK_BLOCKED)
    tasks[prev].state = (prev == 0) ? TASK_RUN : TASK_READY;

  tasks[next].state = TASK_RUN;
  tasks[next].switches++;
  cur = next;

  task_fpu_slot = tasks[next].fpu;         /* the stub restores this on exit */
  /* %fs is per thread and nothing else switches it, so it is loaded here, with
   * the task's other state, before it can run a single instruction. */
#ifndef NO_FS_SWITCH
  __asm__ volatile("wrmsr" :: "c"(0xC0000100), "a"((uint32_t)tasks[next].fs_base),
                   "d"((uint32_t)(tasks[next].fs_base >> 32)));
#endif
  if (tasks[next].kstack_top) gdt_set_kernel_stack(tasks[next].kstack_top);
  uint64_t want = tasks[next].cr3 ? tasks[next].cr3 : ring3_kernel_cr3();
  if (ring3_read_cr3() != want) ring3_write_cr3(want);
  mm_reap();        /* a space whose last thread died is safe to free now */
}

/* Called from the int 0x81 stub. Returns the frame to iretq into. */
ctx_t *sched_pick(ctx_t *frame) {
  if (cur < 0 || ntasks < 2) return frame;
  int next = pick_next(cur);
  if (next < 0) {
    tasks[cur].rsp = (uint64_t)(uintptr_t)frame;
    return frame;
  }
  switch_to(next, frame);
  return (ctx_t *)(uintptr_t)tasks[next].rsp;
}

static void wake_deadlines(void);

/* Called from the IRQ0 stub. Ticks, acks the PIC, and preempts ring 3 only. */
ctx_t *sched_tick(ctx_t *frame) {
  total_ticks++;
  if (cur >= 0 && cur < ntasks) tasks[cur].ticks++;
  outb(0x20, 0x20);                        /* EOI: PIC1, IRQ0 */
  wake_deadlines();                        /* futex deadlines fire here */

  if (cur < 0) return frame;
  if ((frame->cs & 3) != 3) {              /* kernel task: no preemption */
    tasks[cur].rsp = (uint64_t)(uintptr_t)frame;
    return frame;
  }
  int next = pick_next(cur);
  if (next < 0) {
    tasks[cur].rsp = (uint64_t)(uintptr_t)frame;
    return frame;
  }
  switch_to(next, frame);
  return (ctx_t *)(uintptr_t)tasks[next].rsp;
}

/* Give up the cpu from kernel context. The compiler sees a normal call, so live
 * registers and xmm spills are handled at the call boundary. */
void task_yield(void) {
  __asm__ volatile("int $0x81" ::: "memory");
}

/* Wake anything whose deadline the tick has passed. This used to live in
 * task_tick(), which nothing ever called - the real timer path is sched_tick(),
 * so a futex timeout was decoration and a wake that raced its block hung the
 * thread for good. It runs from the timer now, once per tick. */
static void wake_deadlines(void) {
  /* A blocked thread with a deadline wakes itself when the tick passes. Doing
   * it here, on the timer, is what keeps a missed futex wake from hanging a
   * thread forever. */
  for (int i = 1; i < ntasks; i++) {
    if (tasks[i].state != TASK_BLOCKED) continue;
    if (!tasks[i].wake_at) continue;
    if (total_ticks < tasks[i].wake_at) continue;
    tasks[i].wake_at = 0;
    tasks[i].timed_out = 1;
    tasks[i].state = TASK_READY;
  }
}

void task_sleep_ticks(uint64_t ticks) {
  if (cur <= 0 || cur >= ntasks) return;
  tasks[cur].timed_out = 0;
  tasks[cur].wake_at = total_ticks + ticks;
  tasks[cur].state = TASK_BLOCKED;
  task_yield();
  tasks[cur].wake_at = 0;
}

/* Park the current task forever. No iretq ever comes back here: the frame this
 * saves belongs to a task that the scheduler will not pick again. */
void task_kill_current(uint64_t code) {
  if (cur > 0 && cur < ntasks) {
    /* A dying task must not leave a registration behind in a socket's waiter
     * table: the slot would name a task that can never be woken. */
    tasks[cur].wait_obj = 0;
    sock_drop_waiters(cur);
    mm_on_exit(&tasks[cur]);       /* last thread out frees the address space */
    tasks[cur].state = TASK_DEAD;
    tasks[cur].work = code;
    /* A child's death is the event its parent may be waiting for, and there is
     * no signal to deliver it with, so the parent is woken directly. A parent
     * that was waiting on something else re-checks and sleeps again. */
    if (tasks[cur].parent > 0) task_wake(tasks[cur].parent);
  } else {
    serial_puts("[ring3] refusing to kill the kernel task, halting\n");
    for (;;) __asm__ volatile("cli; hlt");
  }
  for (;;) task_yield();
}

/* Park the running task until someone calls task_wake(id). The frame saved by
 * the switch below is a normal resumption point: waking simply makes the task
 * eligible again and the scheduler picks up where it left off. */
void task_block_current(void) {
  if (cur > 0 && cur < ntasks) tasks[cur].state = TASK_BLOCKED;
  task_yield();
}

void task_wake(int id) {
  if (id <= 0 || id >= ntasks) return;
  if (tasks[id].state == TASK_BLOCKED) tasks[id].state = TASK_READY;
}

/* The compositor's only way to end a process it no longer has a window for. It
 * cannot reach into the task's frame, so it leaves a flag; the syscall door in
 * ring3.c turns the flag into an exit between calls. Waking a blocked task is
 * what makes this work at all: a browser parked in SYS_WIN_WAIT or in a socket
 * read would otherwise never reach a door to be killed at. */
void task_request_kill(int id) {
  if (id <= 0 || id >= ntasks) return;
  if (tasks[id].state == TASK_FREE || tasks[id].state == TASK_DEAD) return;
  tasks[id].kill_req = 1;
  task_wake(id);
}

void task_block(int blocked) {
  if (cur <= 0) return;
  tasks[cur].state = blocked ? TASK_BLOCKED : TASK_RUN;
}

/* ------------------------------------------------------------- task tables */

static void task_alloc_fail(const char *why, const char *name) {
  serial_puts("[task] ");
  serial_puts(why);
  serial_puts(" for ");
  serial_puts(name);
  serial_puts("\n");
}

/* The two ways this can fail mean completely different things - a full table is
 * a policy limit, a failed kmalloc is memory pressure - and they used to be the
 * same silent 0. A failed spawn left a test waiting on a flag that could never
 * change, which reads as a broken synchronisation primitive rather than a full
 * table. Name the resource. */
static task_t *task_alloc(const char *name) {
  if (ntasks >= TASK_MAX) { task_alloc_fail("no free slot (TASK_MAX)", name); return 0; }
  task_t *t = &tasks[ntasks];
  uint8_t *mem = (uint8_t *)kmalloc(TASK_STACK);
  if (!mem) { task_alloc_fail("kernel stack allocation failed", name); return 0; }
  uintptr_t top = ((uintptr_t)mem + TASK_STACK) & ~(uintptr_t)15;
  for (int i = 0; i < TASK_STACK; i += 8) *(uint64_t *)(mem + i) = 0;

  t->stack = mem;
  t->id = ntasks;
  t->ticks = 0;
  t->switches = 0;
  t->work = 0;
  t->cr3 = 0;
  t->kstack_top = (uint64_t)top;
  t->user_entry = 0;
  t->user_sp = 0;
  t->user_arg = 0;
  t->fn = 0;
  t->mm = 0;
  t->wait_addr = 0;
  t->wait_mm = 0;
  t->wake_at = 0;
  t->timed_out = 0;
  t->wait_obj = 0;
  t->kill_req = 0;
  t->parent = (cur > 0 && cur < ntasks) ? tasks[cur].id : 0;
  t->tgen = ++task_gen_counter;   /* a fresh slot is a new identity */
  int i = 0;
  while (name[i] && i < 23) { t->name[i] = name[i]; i++; }
  t->name[i] = 0;
  ntasks++;
  return t;
}

/* Build the frame a task resumes from the first time it is picked. */
static void prime(task_t *t, uint64_t rip, uint64_t cs, uint64_t rdi,
                  uint64_t user_rsp, uint64_t ss, int user) {
  uint64_t top = t->kstack_top;
  /* Always reserve the full 20 slots and always give rsp/ss real values. A
   * kernel frame only needs 18, but if rsp/ss are left zero the iretq that
   * starts the task takes a fault path that leaves the task with no stack at
   * all (it lands on the qwords past the frame, which is heap metadata). */
  int n = CTX_USER;
  ctx_t *c = (ctx_t *)(uintptr_t)(top - (uint64_t)n * 8);
  uint64_t *w = (uint64_t *)c;
  for (int i = 0; i < n; i++) w[i] = 0;
  c->rdi = rdi;
  c->rip = rip;
  c->cs = cs;
  c->rflags = 0x202;                 /* IF set: the task runs interruptible */
  c->rsp = user ? user_rsp : top;    /* only user frames pop these two */
  c->ss = user ? ss : 0x10;
  t->fs_base = 0;
  t->ctid = 0;
  t->sig_blocked = 0;
  /* Start from the FPU state this cpu is in right now, never from zeros: an
   * fxsave image of all zeros carries an invalid MXCSR. */
  __asm__ volatile("fxsave %0" : "=m"(t->fpu));
  t->rsp = (uint64_t)(uintptr_t)c;
}

int task_spawn(const char *name, void (*fn)(void)) {
  task_t *t = task_alloc(name);
  if (!t) return -1;
  t->fn = fn;
  prime(t, (uint64_t)(uintptr_t)k_task_entry, 0x08, (uint64_t)(uintptr_t)fn, 0, 0, 0);
  t->state = TASK_READY;
  return t->id;
}

int task_spawn_user(const char *name, uint64_t entry, uint64_t user_sp,
                    uint64_t cr3, uint64_t arg) {
  task_t *t = task_alloc(name);
  if (!t) return -1;
  t->user_entry = entry;
  t->user_sp = user_sp;
  t->user_arg = arg;
  t->cr3 = cr3;
  prime(t, entry, 0x23, arg, user_sp, 0x1B, 1);
  t->state = TASK_READY;
  return t->id;
}

int task_spawn_user_mm(const char *name, uint64_t entry, uint64_t user_sp,
                       mm_t *mm, uint64_t arg) {
  task_t *t = task_alloc(name);
  if (!t) return -1;
  t->user_entry = entry;
  t->user_sp = user_sp;
  t->user_arg = arg;
  t->cr3 = mm->cr3;
  t->mm = mm;
  mm->refcount++;
  prime(t, entry, 0x23, arg, user_sp, 0x1B, 1);
  t->state = TASK_READY;
  return t->id;
}

/* A second thread born the way Linux's clone() makes one: the caller's saved
 * registers, the caller's next instruction, a stack of the caller's choosing,
 * and rax = 0 so the code after the `syscall` knows it is the child. The frame
 * passed in is the one the syscall stub saved (nineteen qwords), not a ctx_t -
 * the two differ by the rbp slot, so the fields are copied across by name
 * rather than memcpy'd. */
int task_spawn_clone(const char *name, mm_t *mm, const uint64_t *frame,
                     uint64_t child_rsp, uint64_t fs_base, uint64_t ctid) {
  task_t *t = task_alloc(name);
  if (!t) return -1;
  t->user_entry = frame[14];         /* rip: the instruction after the syscall */
  t->user_sp = child_rsp;
  t->user_arg = 0;
  t->cr3 = mm->cr3;
  t->mm = mm;
  mm->refcount++;

  uint64_t top = t->kstack_top;
  ctx_t *c = (ctx_t *)(uintptr_t)(top - (uint64_t)CTX_USER * 8);
  uint64_t *w = (uint64_t *)c;
  for (int i = 0; i < CTX_USER; i++) w[i] = 0;
  c->r15 = frame[0]; c->r14 = frame[1]; c->r13 = frame[2]; c->r12 = frame[3];
  c->r11 = frame[4]; c->r10 = frame[5]; c->r9  = frame[6]; c->r8  = frame[7];
  c->rdi = frame[8]; c->rsi = frame[9]; c->rdx = frame[10];
  c->rcx = frame[11]; c->rbx = frame[12];
  c->rax = 0;                        /* the child's return value */
  c->rip = frame[14];
  c->cs = frame[15];
  c->rflags = 0x202;
  c->rsp = child_rsp;
  c->ss = frame[18];

  __asm__ volatile("fxsave %0" : "=m"(t->fpu));
  t->rsp = (uint64_t)(uintptr_t)c;
  t->fs_base = fs_base;
  t->ctid = ctid;
  t->state = TASK_READY;
  return t->id;
}

/* ------------------------------------------------------------ PIT + IRQ0 */

/* Bring the scheduler up: a 100 Hz PIT, IRQ0 unmasked, and the two gates the
 * scheduler itself runs on. Called with interrupts still off. */
void task_system_start(void) {
  task_init();
  uint16_t div = (uint16_t)(1193182 / 100);
  outb(0x43, 0x36);
  outb(0x40, (uint8_t)(div & 0xFF));
  outb(0x40, (uint8_t)(div >> 8));
  idt_set_gate_raw(0x20, (void *)sched_irq0_entry, 0);
  idt_set_gate_raw(0x81, (void *)sched_yield_entry, 0);
  pic_clear_mask(0);
}

/* Two worker tasks, so the scheduler has something to charge time to. They used
 * to prove it by spinning - twenty thousand pause instructions per pass, forever,
 * in every boot - which pinned a core on an otherwise idle desktop and left
 * whatever was actually on screen with a share of the machine. They only have to
 * exist and be scheduled, so one pass per timer tick is enough: hlt returns on
 * the next interrupt and the 100Hz tick charges each task a slice. */
static void worker_body(unsigned spins) {
  (void)spins;
  for (;;) {
    int me = cur;
    if (me > 0 && me < ntasks) tasks[me].work++;
    __asm__ volatile("hlt");
    task_yield();
  }
}
static void worker_netd(void) { worker_body(20000); }
static void worker_idle(void) { worker_body(4000); }

void task_demo_start(void) {
  task_spawn("netd", worker_netd);
  task_spawn("idle", worker_idle);
}