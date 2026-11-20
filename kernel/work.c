/*
 * work.c - run a loop body on every core, for the compositor's row jobs.
 *
 * The raster is embarrassingly parallel: a wallpaper copy is rows, and the
 * supersampled box filter is rows. Both were single-threaded, so a frame cost
 * what one core could do in the time a frame takes. This is the smallest thing
 * that changes that: N-1 pinned kernel threads, the calling thread also working,
 * one job at a time.
 *
 * Why the workers nap instead of blocking. task_block_current() sets the task
 * BLOCKED and yields; task_wake() only flips a task that is already BLOCKED. A
 * wake that lands while the worker is still running - after it decided there was
 * no job and before it parked - is lost, and the worker sleeps forever while the
 * dispatcher waits for it. That is a hang, not a slow frame. So a worker never
 * parks on the job state: it spins with task_yield() while jobs are live, and
 * after half a second of nothing it naps one tick at a time, which it always
 * wakes from on its own. A lost wake then costs at most a tick of latency, and
 * a rendering desktop never naps because it is never idle that long.
 */

#include "include/work.h"
#include "include/task.h"
#include "include/smp.h"
#include "include/string.h"
#include "include/gui.h"
#include "include/lapic.h"   /* IPI: an AP with nothing to run has to be nudged */    /* serial_puts: the log is how this reports */

static volatile int job_gen;              /* bumped per job */
static volatile int job_next;             /* next task index, taken atomically */
static volatile int job_ntasks;
static volatile int job_done;             /* workers that finished this job */
static work_fn job_fn;
static void *job_arg;

static int worker_id[SMP_MAX_CPUS];
static volatile uint32_t worker_last_job[SMP_MAX_CPUS];
static int nworkers;
static int started;

/* the one clock: 100 Hz PIT ticks since boot, for the nap decision */
#define tick_count task_total_ticks()

static void worker_body(void) {
  int me = task_current();
  int slot = -1;
  for (int i = 0; i < nworkers; i++) if (worker_id[i] == me) { slot = i; break; }
  int seen = 0;
  for (;;) {
    int gen = job_gen;
    if (gen == seen) {
      /* No job. Spin while one is recent, nap when the desktop is quiet: a
       * napping worker that is needed anyway loses at most one tick, and a
       * spinning one costs nothing that anything else wanted. */
      uint64_t now = tick_count;
      if (slot >= 0 && now - worker_last_job[slot] > (uint64_t)TASK_TICK_HZ / 2)
        task_sleep_ticks(1);
      else
        task_yield();
      continue;
    }
    seen = gen;
    if (slot >= 0) worker_last_job[slot] = tick_count;
    for (;;) {
      int i = __atomic_fetch_add(&job_next, 1, __ATOMIC_SEQ_CST);
      if (i >= job_ntasks) break;
      job_fn(job_arg, i);
    }
    __atomic_fetch_add(&job_done, 1, __ATOMIC_SEQ_CST);
  }
}

/* Off unless asked for. The application processors schedule during boot - the
 * pinned worker test in smp_start_workers proves that - and then stop: a thread
 * spawned on one after the desktop is up is never picked, with no fault and no
 * log line. Until that is understood, this pool is a switch a developer can turn
 * on, not something a shipped desktop does on every frame. RASTER_MT=1. */
#ifndef OMI_RASTER_MT
#define OMI_RASTER_MT 0
#endif

int work_threads(void) {
#if OMI_RASTER_MT
  if (!started) work_init();      /* a caller asking how many cores it can use
                                   * is a caller about to use them */
  return started ? nworkers + 1 : 1;
#else
  return 1;
#endif
}

void work_init(void) {
  if (started) return;
  started = 1;
  int ncpu = smp_cpu_count();
  if (ncpu < 2) { nworkers = 0; return; }
  if (ncpu > SMP_MAX_CPUS) ncpu = SMP_MAX_CPUS;
  for (int i = 1; i < ncpu; i++) {
    int id = task_spawn_on("raster", worker_body, smp_cpu_apic_id(i));
    if (id >= 0) worker_id[nworkers++] = id;
  }
  if (!nworkers) {
    serial_puts("[work] no raster threads\n");
    return;
  }
  for (int i = 0; i < nworkers; i++) {
    serial_puts("[work] thread ");
    serial_puthex((uint64_t)worker_id[i]);
    serial_puts(" on apic ");
    serial_puthex((uint64_t)smp_cpu_apic_id(i + 1));
    serial_puts("\n");
  }
  serial_puts("[work] raster threads: ");
  serial_puthex((uint64_t)nworkers);
  serial_puts("\n");
}

/* Run fn(arg, i) for i in 0..ntasks-1 across every core, the caller included,
 * and return when they are all done. Tasks should be independent and about equal
 * in cost - the caller takes tasks by the same atomic counter as everyone else,
 * so a long task at index 0 delays the frame exactly as much as it would have
 * single-threaded. */
void work_run(work_fn fn, void *arg, int ntasks) {
  if (ntasks <= 0) return;
  if (!started) work_init();
  if (nworkers == 0 || ntasks == 1) {
    for (int i = 0; i < ntasks; i++) fn(arg, i);
    return;
  }

  job_fn = fn;
  job_arg = arg;
  job_ntasks = ntasks;
  job_next = 0;
  job_done = 0;
  uint64_t now = tick_count;
  for (int i = 0; i < nworkers; i++) worker_last_job[i] = (uint32_t)now;
  __atomic_add_fetch(&job_gen, 1, __ATOMIC_SEQ_CST);
  for (int i = 0; i < nworkers; i++) {
    task_wake(worker_id[i]);
    if (smp_lapic_present()) lapic_send_ipi(smp_cpu_apic_id(i + 1), LAPIC_VECTOR_IPI);
  }

  /* the caller works too: N cores do N shares, not N-1 */
  for (;;) {
    int i = __atomic_fetch_add(&job_next, 1, __ATOMIC_SEQ_CST);
    if (i >= job_ntasks) break;
    fn(arg, i);
  }

  /* Wait, but never forever. If a worker is wedged the frame still has to get
   * drawn, so the caller finishes the work itself after a bounded wait and says
   * so once. A desktop that silently drops frames is bad; one that hangs showing
   * a half-drawn frame is worse. */
  long spins = 0;
  while (job_done < nworkers) {
    if (++spins > 2000000) {
      static int told;
      if (!told) {
        told = 1;
        serial_puts("[work] worker stuck: done=");
        serial_puthex((uint64_t)job_done);
        serial_puts(" of ");
        serial_puthex((uint64_t)nworkers);
        serial_puts("  finishing serially\n");
      }
      for (;;) {
        int i = __atomic_fetch_add(&job_next, 1, __ATOMIC_SEQ_CST);
        if (i >= job_ntasks) break;
        fn(arg, i);
      }
      break;
    }
    task_yield();
  }
}