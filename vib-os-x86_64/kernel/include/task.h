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

#define TASK_MAX    8
#define TASK_STACK  16384

enum { TASK_FREE = 0, TASK_READY = 1, TASK_RUN = 2, TASK_DEAD = 3 };

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
} task_t;

void task_init(void);
int  task_spawn(const char *name, void (*fn)(void));
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