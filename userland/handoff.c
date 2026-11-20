/* Third question about exit() on Omi: can a thread that is parked on a
 * semaphore be woken and joined, reliably, many times?
 *
 * ~DefaultPlatform terminates its worker runners and waits for them; v8hello
 * hangs there. This binary does the same handoff with nothing else in the way:
 * a worker parks on sem_wait, main posts, both repeat, and the worker is joined
 * at the end. Any lost wake shows up as a hang inside the loop. */

#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>

#define ROUNDS 2000

static sem_t to_worker, to_main;
static volatile int stop;

static void *worker(void *arg) {
  (void)arg;
  for (;;) {
    sem_wait(&to_worker);
    if (stop) break;
    sem_post(&to_main);
  }
  return 0;
}

int main(void) {
  sem_init(&to_worker, 0, 0);
  sem_init(&to_main, 0, 0);
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  printf("[handoff] start\n");
  fflush(stdout);
  for (int i = 0; i < ROUNDS; i++) {
    sem_post(&to_worker);
    sem_wait(&to_main);
    if (i % 200 == 199) {
      printf("[handoff] %d\n", i + 1);
      fflush(stdout);
    }
  }
  printf("[handoff] %d rounds done\n", ROUNDS);
  fflush(stdout);
  stop = 1;
  sem_post(&to_worker);
  pthread_join(t, 0);
  printf("[handoff] worker joined, exiting\n");
  fflush(stdout);
  return 0;
}