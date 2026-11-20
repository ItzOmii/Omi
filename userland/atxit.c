/* Does exit() finish on Omi, with or without threads having existed?
 *
 * v8hello tears V8 down cleanly, returns from main, and then never logs an
 * exit: no atexit handler runs, and the process is never reaped. This binary
 * exists to find out whether that is about threads at all - it does nothing
 * else. Two threads start and are joined, an atexit handler is registered, and
 * main returns. If "[atxit] atexit ran" does not appear, musl's exit() has a
 * problem that has nothing to do with V8. */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static void bye(void) {
  printf("[atxit] atexit ran\n");
  fflush(stdout);
}

static void *nothing(void *arg) {
  printf("[atxit] thread %ld ran\n", (long)arg);
  fflush(stdout);
  return arg;
}

int main(void) {
  atexit(bye);
  pthread_t a, b;
  pthread_create(&a, 0, nothing, (void *)1L);
  pthread_create(&b, 0, nothing, (void *)2L);
  pthread_join(a, 0);
  pthread_join(b, 0);
  printf("[atxit] threads joined, returning from main\n");
  fflush(stdout);
  return 0;
}