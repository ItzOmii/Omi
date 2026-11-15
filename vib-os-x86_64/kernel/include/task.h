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

#define TASK_MAX    12
#define TASK_STACK  16384

/* BLOCKED is a task waiting for something (an event, in practice): the
 * scheduler skips it until something calls task_wake(). */
enum { TASK_FREE = 0, TASK_READY = 1, TASK_RUN = 2, TASK_DEAD = 3, TASK_BLOCKED = 4 };

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
} task_t;

void task_init(void);
int  task_spawn(const char *name, void (*fn)(void));
int  task_spawn_user(const char *name, uint64_t entry, uint64_t user_sp,
                     uint64_t cr3, uint64_t arg);
void task_kill_current(uint64_t code);   /* does not return */
void task_block_current(void);           /* sleep until task_wake() */
void task_wake(int id);
uint64_t task_total_ticks(void);
const char *task_name(int i);
void task_yield(void);
void task_tick(void);
int  task_count(void);
task_t *task_at(int i);
int  task_current(void);
const char *task_state_name(int st);
void task_block(int blocked);   /* 1 = don't schedule this task */
void task_system_start(void);
void task_demo_start(void);

#endif