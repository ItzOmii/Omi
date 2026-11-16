/* Kernel tasks: separate stacks, a round-robin scheduler and a tick counter.
 *
 * Every task owns a stack; a switch saves the callee-saved registers on the
 * outgoing stack and pops the incoming one, so tasks resume inside
 * task_yield() exactly where they left off. Switches happen at yield points
 * (the compositor yields once per frame), never from interrupt context, so no
 * register state has to be reconstructed around an interrupt frame. */
#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include "fd.h"

/* 20 was enough while the boot ran four demo processes; the standalone apps and
 * the thread tests together ran past it, and the failure looked like a broken
 * futex rather than a full table. Headroom is cheap: 16 KB of kernel stack each.
 *
 * 32 then ran out once the ring-3 test programs appeared, because a task's slot
 * is never reclaimed: an exited process keeps its entry (and its table's fds)
 * until reboot. So every program spawned at boot is a permanent cost, and the
 * thread tests need a free slot to hand out - when there is none they fail as
 * "the thread inherits the fd table", which reads like a descriptor bug and is
 * really a full table. 40 leaves the thread tests room to work. */
/* Slots are never recycled: a finished task stays in the table so its id keeps
 * pointing at something with a name, which is what the Task Manager and the exit
 * log read. That makes this a budget for the whole life of the boot, not a live
 * count - 40 ran out the moment a libc with four threads joined the demo, and the
 * failure looked like pthread_create returning EAGAIN. 96 x 16 KB of kernel stack
 * is 1.5 MB of an 18 GB machine. */
#define TASK_MAX 96
#define TASK_STACK  16384

/* BLOCKED is a task waiting for something (an event, in practice): the
 * scheduler skips it until something calls task_wake(). */
enum { TASK_FREE = 0, TASK_READY = 1, TASK_RUN = 2, TASK_DEAD = 3, TASK_BLOCKED = 4 };

/* A lazily-populated region of a process's address space: SYS_VM_ALLOC records
 * the range, and the first touch of each page fills it in. */
/* 8 areas was the whole map when the only caller was SYS_VM_ALLOC. A Linux-shaped
 * process spends one on its heap and one per mapping, so the table became the
 * binding constraint rather than memory. 24 bytes a slot. */
#define TASK_AREAS 16
typedef struct {
  uint64_t base, len;   /* byte range, page-aligned */
  uint64_t flags;       /* PTE flags for the pages that appear */
  /* Linux PROT_* bits (1 read, 2 write, 4 exec), 0 meaning no access at all.
   * Kept separately from the PTE flags because a range can be reserved with no
   * permission yet - PROT_NONE has to be a real refusal at fault time, not just
   * an absence of the write bit, or a guard page guards nothing. */
  int      used;
  /* File-backed regions (mmap of a boot module). `src` is the blob and
   * `src_off` is where this region starts inside it; pages are filled from
   * there on first touch instead of being left zero. NULL means anonymous.
   * The blob is bootloader memory owned by nobody, so a file region is copied
   * into frames rather than pointing at it: every frame then belongs to the
   * ordinary allocator and releases through cow_release like any other. */
  uint8_t  prot;
  const uint8_t *src;
  uint32_t src_off;     /* offset of base inside the blob */
  uint32_t src_len;     /* blob bytes from src_off to EOF */
} vm_area_t;

/* An address space. Threads share one: a page-table root, the lazily-mapped
 * regions, and the allocation hints. Refcounted, so the last thread out frees
 * the tables instead of the first one to exit. */
typedef struct {
  uint64_t  cr3;
  uint64_t  mmap_hint;    /* SYS_VM_ALLOC grows up from RING3_MMAP_BASE */
  uint64_t  stack_hint;   /* new thread stacks grow down from RING3_MMAP_TOP */
  /* brk: one lazily-populated area reserved on the first call, then a pointer
   * that moves inside it. Pages appear on touch, so the reservation is free. */
  uint64_t  brk_base, brk_cur, brk_end;
  vm_area_t areas[TASK_AREAS];
  fdtable_t *fds;         /* descriptors belong to the process, not the thread */
  int       refcount;
  /* Signal handlers are per process and the blocked mask is per thread, which is
   * why one lives here and the other in task_t. We store both and deliver
   * neither: there is no signal frame and nothing ever raises one, so a handler
   * installed here is remembered and read back but never called. A libc asks at
   * startup and reads the answer back, so storing beats -ENOSYS; the absence of
   * delivery is the documented gap, not the storage. */
  uint64_t  sig_act[32];  /* handler address per signal 1..31, 0 = default */
  /* Set when this space ever forked or was forked from, so its teardown is worth
   * auditing: the bank's contents are only interesting once a participant goes. */
  int       cow_participant;
} mm_t;

typedef struct {
  int id;
  int state;
  char name[24];
  uint64_t rsp;        /* saved stack pointer while not running */
  uint64_t ticks;      /* PIT ticks spent running */
  uint64_t switches;   /* times this task was scheduled in */
  void *stack;
  void (*fn)(void);
  uint64_t work;       /* free counter for tasks to report progress */
  uint64_t cr3;        /* physical PML4 of a user task, 0 = kernel space */
  uint64_t kstack_top; /* TSS rsp0 while this task is the current one */
  uint64_t user_entry;
  uint64_t user_sp;
  uint64_t user_arg;
  /* FXSAVE area (512 bytes, 16-byte aligned): the SSE/FPU state of this task.
   * The kernel is built with SSE enabled and uses xmm freely, so user state has
   * to be saved around every entry into the kernel and restored on the way out. */
  uint8_t  fpu[512] __attribute__((aligned(16)));
  mm_t    *mm;          /* shared address space, 0 for kernel tasks */
  /* The thread pointer: %fs is per thread, so its base travels with the task
   * and is loaded on the way into it. Without this, a second thread's libc
   * state lands on the first thread's. */
  uint64_t fs_base;
  /* CLONE_CHILD_CLEARTID: the user address to zero on exit so a joiner's futex
   * wakes up, 0 = none. */
  uint64_t ctid;
  int      parent;      /* the task that spawned this one, 0 for the kernel; the
                         * only thing wait4() looks for, and the only reason a
                         * dead child is not simply forgotten */
  /* Copy-on-write instrumentation, per process: how many stores hit a shared
   * page, how many of those needed a copy, and where the last one was - so a
   * store that faults twice at the same address is visible as a failure rather
   * than silently absorbed. */
  uint64_t cow_faults;
  /* Bumped by each fork. A page can legitimately fault again at the same address
   * after a *later* fork re-protects it, so the repeat check compares this too -
   * otherwise the second fork's first store looks like a copy that did not take. */
  uint64_t cow_gen;
  uint64_t last_cow_gen;
  uint64_t cow_copies;
  uint64_t cow_steals;
  uint64_t last_cow_va;
  uint64_t sig_blocked; /* per-thread signal mask, stored but never consulted:
                         * nothing is ever delivered, so it can never stop one */
  uint64_t wait_addr;   /* futex word this thread sleeps on, 0 = not waiting */
  mm_t    *wait_mm;
  uint64_t wake_at;     /* tick at which a timed sleep gives up, 0 = never */
  int      timed_out;
  /* Object this task is parked on (a socket today), 0 = not waiting. Paired with
   * the waiter slot inside that object so a wake must match on both sides: the
   * object alone does not prove the task is still the one that registered. */
  void    *wait_obj;
  /* Bumped when a task slot is reused. Slots are never reused today (task_alloc
   * only appends), so this guards a future change rather than a live bug - see
   * docs/readiness-design.md. */
  uint32_t tgen;
} task_t;

void task_init(void);
int  task_spawn(const char *name, void (*fn)(void));
/* A thread made the way Linux's clone() makes one: the caller's saved registers,
 * the caller's next instruction, a stack of the caller's choosing, rax = 0. The
 * frame is the syscall stub's nineteen-qword image, not a ctx_t. */
int task_spawn_clone(const char *name, mm_t *mm, const uint64_t *frame,
                     uint64_t child_rsp, uint64_t fs_base, uint64_t ctid);
int task_spawn_user(const char *name, uint64_t entry, uint64_t user_sp,
                     uint64_t cr3, uint64_t arg);
/* A new thread in an existing address space: same mm, fresh stack, refcount++. */
int  task_spawn_user_mm(const char *name, uint64_t entry, uint64_t user_sp,
                        mm_t *mm, uint64_t arg);
void task_sleep_ticks(uint64_t ticks);   /* block until the tick passes */
void mm_on_exit(task_t *t);              /* ring3.c: drop the mm reference */
void mm_reap(void);                      /* ring3.c: free dead spaces */
void task_kill_current(uint64_t code);   /* does not return */
void task_block_current(void);           /* sleep until task_wake() */
void task_wake(int id);
uint64_t task_total_ticks(void);

/* The PIT runs at 100 Hz (kernel_task.c programs the divisor). Anything turning
 * ticks into wall time has to say so, because there is exactly one clock here. */
#define TASK_TICK_HZ 100
const char *task_name(int i);
void task_yield(void);
/* the timer wakes expired deadlines itself; there is no task_tick() hook */
int  task_count(void);
task_t *task_at(int i);
int  task_current(void);
const char *task_state_name(int st);
void task_block(int blocked);   /* 1 = don't schedule this task */
void task_system_start(void);
void task_demo_start(void);
/* Does the timer switch tasks, or only tick? Runs spin.elf; see preempt_selftest.c */
void preempt_selftest(void);

#endif