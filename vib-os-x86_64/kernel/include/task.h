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
 * futex rather than a full table. Headroom is cheap: 16 KB of kernel stack each. */
#define TASK_MAX 32
#define TASK_STACK  16384

/* BLOCKED is a task waiting for something (an event, in practice): the
 * scheduler skips it until something calls task_wake(). */
enum { TASK_FREE = 0, TASK_READY = 1, TASK_RUN = 2, TASK_DEAD = 3, TASK_BLOCKED = 4 };

/* A lazily-populated region of a process's address space: SYS_VM_ALLOC records
 * the range, and the first touch of each page fills it in. */
#define TASK_AREAS 8
typedef struct {
  uint64_t base, len;   /* byte range, page-aligned */
  uint64_t flags;       /* PTE flags for the pages that appear */
  int      used;
} vm_area_t;

/* An address space. Threads share one: a page-table root, the lazily-mapped
 * regions, and the allocation hints. Refcounted, so the last thread out frees
 * the tables instead of the first one to exit. */
typedef struct {
  uint64_t  cr3;
  uint64_t  mmap_hint;    /* SYS_VM_ALLOC grows up from RING3_MMAP_BASE */
  uint64_t  stack_hint;   /* new thread stacks grow down from RING3_MMAP_TOP */
  vm_area_t areas[TASK_AREAS];
  fdtable_t *fds;         /* descriptors belong to the process, not the thread */
  int       refcount;
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
int  task_spawn_user(const char *name, uint64_t entry, uint64_t user_sp,
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

#endif