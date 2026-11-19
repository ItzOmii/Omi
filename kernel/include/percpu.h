/*
 * Per-CPU state.
 *
 * One struct per core, found by APIC id rather than by an index the caller has
 * to keep straight: any core can ask who it is with one register read, so
 * `percpu_self()` is correct everywhere without threading a cpu number through
 * every call.
 *
 * This is the array a runqueue, an idle stack and a preemption counter will hang
 * off. It starts with what the SMP bring-up needs: identity, and a place for a
 * core to report what only it can see.
 */

#ifndef _PERCPU_H
#define _PERCPU_H

#include "types.h"
#include "smp.h"

typedef struct {
  int      online;
  int      index;                       /* 0..n-1, the MADT's order          */
  uint32_t apic_id;
  uint64_t pages_allocated;             /* pages this core took from the pmm */
  uint64_t ticks;                       /* interrupts taken on this core     */

  /* What this core is doing, for anyone who wants to look. The scheduler writes
   * cur_task, the idle loops set idle before they halt, and the bootstrap
   * processor samples both from the tick - one reader, so a stale value costs
   * one sample of accuracy and never a race in the counters below. */
  volatile int32_t  cur_task;           /* task running here, -1 if none     */
  volatile uint32_t idle;               /* 1 while halted or waiting         */
  uint64_t recent[2];                   /* one bit per tick, 1 = busy        */
  uint64_t busy_ticks;
  uint64_t idle_ticks;
} percpu_t;

void      percpu_init(void);
void      percpu_set_cur(int task);     /* this core is running `task`       */
void      percpu_set_idle(int on);      /* this core has nothing to do       */
/* Share of the last second this core spent with work: 0..100, one point per
 * tick sampled. Before a full second has gone by it reads low, not wrong. */
int       percpu_busy_pct(int index);
int       percpu_cpu_pct(void);         /* every online core, averaged       */
percpu_t *percpu_self(void);            /* this core's own slot              */
percpu_t *percpu_by_index(int i);       /* the i'th enabled processor        */

#endif /* _PERCPU_H */