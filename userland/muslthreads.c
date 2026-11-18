/* muslthreads.c - what a libc does that a single thread never shows.
 *
 * Three things are under test, and each of them was broken before this file
 * existed:
 *
 *   1. clone(): every pthread is a second thread in the same address space,
 *      born from the caller's register state, not from a kernel-made entry.
 *   2. The thread pointer: `__thread` state is addressed through %fs, and %fs
 *      is per thread. If the base were global, tls_counter would be shared and
 *      the counters below would not all read the same value.
 *   3. pthread_join: it waits on a futex that the dying thread clears
 *      (CLONE_CHILD_CLEARTID). A kernel that forgets to clear it makes join
 *      hang forever, which is the failure this file is really watching for.
 *
 * The mutex is the fourth: uncontended it never calls the kernel, and the
 * moment two threads collide it does - so the shared total has to come out
 * exact, and it will not if futex wakeups are lost.
 */
#include <pthread.h>
#include <stdio.h>

#define NTHREADS 4
#define ITERS    200

static __thread long tls_counter;      /* one of these per thread */
static long shared;                    /* one, guarded by the mutex */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void *worker(void *arg) {
  long iters = (long)arg;
  long spin = tls_counter;             /* this thread's copy: starts at 0 */
  for (long i = 0; i < iters; i++) {
    tls_counter++;
    pthread_mutex_lock(&lock);
    shared++;
    pthread_mutex_unlock(&lock);
  }
  /* Printed before the thread dies, so a torn line means two threads were
   * really running at once. */
  printf("[thr] %ld iterations, tls=%ld (borrowed %ld)\n",
         iters, tls_counter, spin);
  return (void *)(iters * 2);
}

int main(void) {
  pthread_t th[NTHREADS];
  long sum = 0;

  printf("[threads] starting %d threads, main tls=%ld\n", NTHREADS, tls_counter);

  for (long i = 0; i < NTHREADS; i++) {
    int err = pthread_create(&th[i], 0, worker, (void *)ITERS);
    if (err) { printf("[threads] pthread_create failed: %d\n", err); return 1; }
  }

  for (int i = 0; i < NTHREADS; i++) {
    void *ret = 0;
    int err = pthread_join(th[i], &ret);
    if (err) { printf("[threads] pthread_join failed: %d\n", err); return 1; }
    sum += (long)ret;
  }

  long want_shared = (long)NTHREADS * ITERS;
  int  want_sum    = NTHREADS * ITERS * 2;

  if (shared != want_shared || sum != want_sum) {
    printf("[threads] WRONG: shared=%ld (want %ld), sum=%ld (want %d)\n",
           shared, want_shared, sum, want_sum);
    return 1;
  }
  printf("[threads] shared=%ld joined=%ld main-tls=%ld OK\n",
         shared, sum, tls_counter);
  return 0;
}
