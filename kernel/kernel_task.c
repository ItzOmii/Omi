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
#include "include/smp.h"
#include "include/lapic.h"
#include "include/spinlock.h"
#include "include/percpu.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t v);

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
/* `cur` used to be one global. A scheduler per core cannot share it: two cores
 * picking at the same time would each think they own the other's task, and the
 * first switch would save one core's frame into the other's slot. Indexed by
 * APIC id so that every use below reads the same as it did, and each core only
 * ever touches its own entry. */
static int cur_by_apic[SMP_MAX_CPUS];
static inline int *cur_slot(void) {
  uint32_t id = lapic_this_id();
  return &cur_by_apic[(id < SMP_MAX_CPUS) ? id : 0];
}
#define cur (*cur_slot())
static volatile uint64_t total_ticks;
/* The task table is appended to by whichever core spawns; a task's own fields
 * are only ever touched by the core running it. */
static spinlock_t task_lock = SPINLOCK_INIT;

/* The FXSAVE area of whichever task is running, so the asm stubs can save the
 * incoming xmm state before any C code touches it. */
void *task_fpu_slot;


static inline void outb(uint16_t port, uint8_t v) {
  __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

void task_init(void) {
  for (int i = 0; i < SMP_MAX_CPUS; i++) cur_by_apic[i] = -1;
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
/* May this core run that task? An unpinned task (-1) belongs to the bootstrap
 * processor: every task that existed before SMP did, and moving one now would
 * carry its stack and its %fs to a core that never saved them. */
static int mine(int i) {
  int c = tasks[i].cpu;
  if (c < 0) return lapic_this_id() == 0;
  return (uint32_t)c == lapic_this_id();
}

static int pick_next(int from) {
  if (ntasks < 2) return -1;
  for (int k = 1; k < ntasks; k++) {
    int i = (from + k) % ntasks;
    if (i == from) continue;
    if (!mine(i)) continue;
    if (i == 0 || tasks[i].state == TASK_READY) return i;
  }
  return -1;
}

/* Hand the cpu to `next`. The address space and the ring3 landing stack travel
 * with the task, so they change here, before the frame is restored. */
static void switch_to(int next, ctx_t *frame) {
  int prev = cur;
  /* A slot released one switch ago is safe to hand out now: the frame that was
   * using its stack has long since been switched away from. */
  for (int i = 1; i < ntasks; i++)
    if (tasks[i].reclaim == 2) tasks[i].reclaim = 0;
  tasks[prev].rsp = (uint64_t)(uintptr_t)frame;
  /* a task that killed itself must not come back, and a blocked one is waiting
   * for its wake-up rather than for the cpu: leave both alone */
  if (tasks[prev].state != TASK_DEAD && tasks[prev].state != TASK_BLOCKED)
    tasks[prev].state = (prev == 0) ? TASK_RUN : TASK_READY;
  if (prev > 0 && tasks[prev].state == TASK_DEAD && tasks[prev].reclaim == 1)
    task_release_dead(prev);

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
  /* There is one TSS and it is shared, so only the core that owns it may write
   * its rsp0. An AP runs kernel threads only; when a ring-3 task is allowed to
   * run there it will need a TSS of its own rather than this guard. */
  if (lapic_this_id() == 0 && tasks[next].kstack_top)
    gdt_set_kernel_stack(tasks[next].kstack_top);
  /* cr3 == 0 means "the kernel's own space", and the kernel is mapped in every
   * space, so there is nothing to load: the live tables already reach this core's
   * code and stack. Writing ring3_kernel_cr3() here is only a convenience, and
   * doing it unconditionally cost a boot loop - that value is still 0 until
   * ring3_init() runs, so an AP starting earlier wrote CR3=0 and the next
   * instruction fetch faulted. Never write a zero page table. */
  percpu_set_cur(next);
  uint64_t want = tasks[next].cr3 ? tasks[next].cr3 : ring3_kernel_cr3();
  if (want && ring3_read_cr3() != want) ring3_write_cr3(want);
  /* mm_reap walks the process list, which is still single-core state. */
  if (lapic_this_id() == 0) mm_reap();
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
  __atomic_add_fetch(&total_ticks, 1, __ATOMIC_RELAXED);

  /* Sample every core from the one place that already runs at a known rate.
   * Windows Task Manager's per-core graph is a sampling profiler too; the only
   * difference is it gets a hardware counter and we count ticks. */
  percpu_set_cur(cur);
  int ncpu = smp_cpu_count();
  for (int i = 0; i < ncpu; i++) {
    percpu_t *c = percpu_by_index(i);
    if (!c || !c->online) continue;
    int busy = c->idle ? 0 : 1;
    c->recent[1] = (c->recent[1] << 1) | (c->recent[0] >> 63);
    c->recent[0] = (c->recent[0] << 1) | (uint64_t)busy;
    if (busy) {
      c->busy_ticks++;
      /* Credit the task on that core. Only the core itself ever runs it, so
       * this is the single writer for any one task's count. */
      if (i > 0 && c->cur_task >= 0 && c->cur_task < ntasks)
        tasks[c->cur_task].ticks++;
    } else {
      c->idle_ticks++;
    }
  }
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
    /* A thread, or a task whose parent is already gone, has no reader for its
     * exit status: its slot is freed by the switch that leaves it. A child with a
     * live parent keeps its slot, because wait4 reports the status from it. */
    if (tasks[cur].is_thread || tasks[cur].parent <= 0) tasks[cur].reclaim = 1;
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

/* Give a dead task's slot back. Nothing did this before: task_alloc only ever
 * appended, so every thread and every reaped child held its slot until reboot.
 * Called by wait4 once the exit status has been read, and by the switch that
 * leaves a thread's last frame - never by the dying task itself, because freeing
 * a slot while its own stack is the one executing hands that stack to whichever
 * task asks for one next. reclaim == 2 means "released, one switch from safe". */
void task_release_dead(int id) {
  if (id <= 0 || id >= ntasks) return;
  task_t *t = &tasks[id];
  if (t->state == TASK_RUN || t->state == TASK_READY) return;
  /* The stack stays with the slot and is reused, so a program that makes threads
   * in a loop does not leak a kernel stack per thread. */
  t->state = TASK_FREE;
  t->reclaim = 2;
  t->mm = 0;
  t->cr3 = 0;
  t->ctid = 0;
  t->fs_base = 0;
  t->is_thread = 0;
  t->parent = 0;
  t->wait_obj = 0;
  t->kill_req = 0;
  t->sig_pending = 0;
  t->sig_blocked = 0;
  t->name[0] = 0;
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
  int irq = irq_save();
  spin_lock(&task_lock);
  /* Recycle a slot that has been given back before growing the table. Slots used
   * to be appended only, so every thread and every reaped child held its place
   * for the life of the machine: a program that joins two thousand threads hit
   * the ceiling with its memory long since freed, and a boot that forks a few
   * dozen times was quietly eating the table too. A slot released by the switch
   * that left it (reclaim == 2) is not handed out until one more switch has
   * happened, because until then its kernel stack may still be in use. */
  int slot = -1;
  for (int i = 1; i < ntasks; i++)
    if (tasks[i].state == TASK_FREE && tasks[i].reclaim == 0) { slot = i; break; }
  if (slot < 0) {
    if (ntasks >= TASK_MAX) {
      spin_unlock(&task_lock); irq_restore(irq);
      task_alloc_fail("no free slot (TASK_MAX)", name);
      return 0;
    }
    slot = ntasks;
  }
  task_t *t = &tasks[slot];
  uint8_t *mem = (uint8_t *)t->stack;        /* a recycled slot keeps its stack */
  if (!mem) {
    mem = (uint8_t *)kmalloc(TASK_STACK);
    if (!mem) {
      spin_unlock(&task_lock); irq_restore(irq);
      task_alloc_fail("kernel stack allocation failed", name);
      return 0;
    }
    t->stack = mem;
  }
  uintptr_t top = ((uintptr_t)mem + TASK_STACK) & ~(uintptr_t)15;
  for (int i = 0; i < TASK_STACK; i += 8) *(uint64_t *)(mem + i) = 0;

  /* A recycled slot starts empty apart from its stack: the signal mask, the
   * thread pointer and the fd table all belonged to the task that lived here,
   * and a stale fd table would outlive its owner. */
  {
    uint32_t keep_stack = (uint32_t)(uintptr_t)0;
    (void)keep_stack;
    for (uint32_t b = 0; b < (uint32_t)sizeof *t; b++) ((uint8_t *)t)[b] = 0;
    t->stack = mem;
  }
  t->id = slot;
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
  t->cpu = -1;
  t->parent = (cur > 0 && cur < ntasks) ? tasks[cur].id : 0;
  t->tgen = ++task_gen_counter;   /* a fresh slot is a new identity */
  int i = 0;
  while (name[i] && i < 23) { t->name[i] = name[i]; i++; }
  t->name[i] = 0;
  if (slot == ntasks) ntasks++;
  spin_unlock(&task_lock);
  irq_restore(irq);
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
  t->sig_pending = 0;
  t->sig_alt_sp = 0;
  t->sig_alt_size = 0;
  t->sig_alt_flags = 0;
  t->sig_on_alt = 0;
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

/* ------------------------------------------------------- one core per task
 *
 * The switch itself is not core-specific: int 0x81 pushes a frame, sched_pick
 * chooses, and the stub restores. What an application processor needs of its own
 * is a `cur` slot (per APIC id, above), an idle task to own the cpu when nothing
 * is ready, and the assurance that nothing it picks belongs to another core
 * (mine()). With those three, the same scheduler runs on every core.
 */
int task_spawn_on(const char *name, void (*fn)(void), uint32_t apic_id) {
  task_t *t = task_alloc(name);
  if (!t) return -1;
  t->fn = fn;
  t->cpu = (int)apic_id;
  prime(t, (uint64_t)(uintptr_t)k_task_entry, 0x08, (uint64_t)(uintptr_t)fn, 0, 0, 0);
  t->state = TASK_READY;
  return t->id;
}

int task_spawn_ap_idle(uint32_t apic_id, uint64_t stack_top) {
  task_t *t = task_alloc("ap-idle");
  if (!t) return -1;
  t->cpu = (int)apic_id;
  t->kstack_top = stack_top;
  t->rsp = stack_top;
  t->state = TASK_RUN;     /* the core is already running on that stack */
  return t->id;
}

void sched_ap_enter(int idle_id) {
  *cur_slot() = idle_id;
  task_fpu_slot = tasks[idle_id].fpu;
  unsigned long long spins = 0;
  for (;;) {
    __asm__ volatile("sti");
    task_yield();          /* int 0x81: pick, switch, and come back here */
#if OMI_BENCH
    /* is this core even alive, and does it see ready work? */
    if ((++spins & 0x3FFFFF) == 0) {
      int ready = 0;
      for (int i = 1; i < ntasks; i++) if (tasks[i].state == TASK_READY) ready++;
      serial_puts("[ap] core=");
      serial_puthex((uint64_t)lapic_this_id());
      serial_puts(" idle spins, ready tasks=");
      serial_puthex((uint64_t)ready);
      serial_puts("\n");
    }
#endif
  }
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
  /* The filter comes with the child. A process that confined itself and then
   * forked or made a thread would otherwise hand the new one an unfiltered door,
   * which is the whole escape the mask is meant to close. */
  task_t *par = task_at(task_current());
  if (par) {
    t->sbx_on = par->sbx_on;
    for (int i = 0; i < 4; i++) t->sbx[i] = par->sbx[i];
  }
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