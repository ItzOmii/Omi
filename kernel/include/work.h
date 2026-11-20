/* work.h - a parallel-for across the machine's cores (see work.c). */
#ifndef OMI_WORK_H
#define OMI_WORK_H

typedef void (*work_fn)(void *arg, int index);

/* Start the pinned worker threads. Safe to call more than once; work_run() calls
 * it on first use, so a caller that has nothing to parallelise never pays for it. */
void work_init(void);
/* How many cores a job will use, including the caller: 1 when there are no
 * workers, so a caller can decide whether tiles are worth the bookkeeping. */
int  work_threads(void);
/* fn(arg, 0..ntasks-1) across every core; returns when all of them have run. */
void work_run(work_fn fn, void *arg, int ntasks);

#endif