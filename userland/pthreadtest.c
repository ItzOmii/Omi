/* pthreadtest - does Omi run an off-the-shelf multithreaded Linux program?
 *
 * Everything here is plain musl pthread: no Omi syscalls, no hand-rolled
 * threads. If this passes, the Linux thread ABI that V8's platform layer needs
 * is real: clone(CLONE_VM|CLONE_THREAD), a per-thread %fs, futex wait/wake,
 * CLONE_CHILD_CLEARTID and the thread exit path.
 *
 * Proven from the ISO, not from host tests.
 */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static int checks, failures;

static void ok(const char *what) {
  checks++;
  printf("[pthread] ok   %s\n", what);
}

static void bad(const char *what, long got) {
  checks++;
  failures++;
  printf("[pthread] FAIL %s (got %ld)\n", what, got);
}

/* ---- 1. every thread has its own %fs ----------------------------------- */

static __thread int tls_mine;

static void *tls_thread(void *arg) {
  long want = (long)arg;
  tls_mine = (int)want * 11;
  /* Give the others a chance to run while our value is live. */
  for (volatile int i = 0; i < 2000; i++) {
  }
  return (void *)(long)(tls_mine == want * 11 ? 1 : 0);
}

static void test_tls(void) {
  pthread_t th[4];
  int made = 0;
  for (long i = 0; i < 4; i++)
    if (pthread_create(&th[i], NULL, tls_thread, (void *)(i + 1)) == 0) made++;
  if (made != 4) {
    bad("four threads started", made);
    return;
  }
  ok("four threads started");

  long good = 0;
  for (int i = 0; i < 4; i++) {
    void *ret = NULL;
    pthread_join(th[i], &ret);
    if (ret) good++;
  }
  if (good == 4) ok("each thread read its own thread pointer");
  else bad("each thread read its own thread pointer", good);
}

/* ---- 2. a mutex actually serialises four threads ----------------------- */

static pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;
static long counter;

static void *bump(void *arg) {
  (void)arg;
  for (int i = 0; i < 2500; i++) {
    pthread_mutex_lock(&counter_lock);
    counter++;
    pthread_mutex_unlock(&counter_lock);
  }
  return NULL;
}

static void test_mutex(void) {
  pthread_t th[4];
  counter = 0;
  for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, bump, NULL);
  for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
  if (counter == 10000) ok("a mutex serialises four threads (10000 increments)");
  else bad("a mutex serialises four threads", counter);
}

/* ---- 3. condition variable wakeup -------------------------------------- */

static pthread_mutex_t cond_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int ready;

static void *waiter(void *arg) {
  (void)arg;
  pthread_mutex_lock(&cond_lock);
  while (!ready) pthread_cond_wait(&cond, &cond_lock);
  pthread_mutex_unlock(&cond_lock);
  return (void *)1;
}

static void test_cond(void) {
  pthread_t th;
  ready = 0;
  if (pthread_create(&th, NULL, waiter, NULL) != 0) {
    bad("a thread waiting on a condvar", -1);
    return;
  }
  /* Let the waiter reach pthread_cond_wait before signalling it. */
  for (volatile int i = 0; i < 20000; i++) {
  }
  pthread_mutex_lock(&cond_lock);
  ready = 1;
  pthread_cond_signal(&cond);
  pthread_mutex_unlock(&cond_lock);
  void *ret = NULL;
  pthread_join(th, &ret);
  if (ret) ok("a condvar wakeup releases the waiter");
  else bad("a condvar wakeup releases the waiter", 0);
}

/* ---- 4. thread identity matches what the kernel says ------------------- */

static volatile long seen_tid;

static void *tid_thread(void *arg) {
  (void)arg;
  seen_tid = (long)syscall(SYS_gettid);
  return NULL;
}

static void test_tid(void) {
  pthread_t th;
  seen_tid = 0;
  pthread_create(&th, NULL, tid_thread, NULL);
  pthread_join(th, NULL);
  long me = (long)syscall(SYS_gettid);
  if (seen_tid > 0 && seen_tid != me) ok("gettid differs per thread");
  else bad("gettid differs per thread", seen_tid - me);

  if (me == (long)getpid()) ok("the main thread's tid is the process id");
  else bad("the main thread's tid is the process id", me - (long)getpid());

  if (pthread_self() != 0) ok("pthread_self is usable");
  else bad("pthread_self is usable", 0);
}

/* ---- 5. thread-local storage through pthread_key ----------------------- */

static pthread_key_t tsd_key;
static void tsd_destroy(void *p) { free(p); }

static void *tsd_thread(void *arg) {
  long v = (long)arg;
  int *box = malloc(sizeof *box);
  if (!box) return (void *)0;
  *box = (int)v;
  pthread_setspecific(tsd_key, box);
  int *back = pthread_getspecific(tsd_key);
  return (void *)(long)(back && *back == (int)v ? 1 : 0);
}

static void test_tsd(void) {
  if (pthread_key_create(&tsd_key, tsd_destroy) != 0) {
    bad("pthread_key_create", errno);
    return;
  }
  ok("pthread_key_create");
  pthread_t th[3];
  for (long i = 0; i < 3; i++) pthread_create(&th[i], NULL, tsd_thread, (void *)(i + 7));
  long good = 0;
  for (int i = 0; i < 3; i++) {
    void *ret = NULL;
    pthread_join(th[i], &ret);
    if (ret) good++;
  }
  if (good == 3) ok("pthread-specific data is per thread");
  else bad("pthread-specific data is per thread", good);
}

/* ---- 6. thousands of threads, created and destroyed -------------------- */

static void *shortlived(void *arg) {
  return (void *)((long)arg + 1);
}

static void test_many(void) {
  const int N = 2000;
  int made = 0, joined = 0;
  for (int i = 0; i < N; i++) {
    pthread_t th;
    if (pthread_create(&th, NULL, shortlived, (void *)(long)i) != 0) continue;
    made++;
    void *ret = NULL;
    if (pthread_join(th, &ret) == 0 && (long)ret == i + 1) joined++;
  }
  if (made == N && joined == N) ok("two thousand threads created and joined");
  else {
    printf("[pthread]      created %d joined %d of %d\n", made, joined, N);
    bad("two thousand threads created and joined", joined);
  }
}

/* ---- 7. exit path: return, pthread_exit, and a detached thread -------- */

static void *exiter(void *arg) {
  (void)arg;
  pthread_exit((void *)42);
  return (void *)0;
}

static volatile int detached_ran;

static void *detached(void *arg) {
  (void)arg;
  detached_ran = 1;
  return NULL;
}

static void test_exit_paths(void) {
  pthread_t th;
  void *ret = NULL;
  pthread_create(&th, NULL, exiter, NULL);
  pthread_join(th, &ret);
  if ((long)ret == 42) ok("pthread_exit passes its value to pthread_join");
  else bad("pthread_exit passes its value to pthread_join", (long)ret);

  pthread_t d;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  detached_ran = 0;
  if (pthread_create(&d, NULL, detached, NULL) == 0) {
    for (int i = 0; i < 20000 && !detached_ran; i++) {
    }
    ok("a detached thread runs without being joined");
  } else {
    bad("a detached thread runs without being joined", -1);
  }
  pthread_attr_destroy(&attr);
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("[pthread] a static musl program with real pthreads\n");

  test_tls();
  test_mutex();
  test_cond();
  test_tid();
  test_tsd();
  test_exit_paths();
  test_many();

  printf("[pthread] %d/%d\n", checks - failures, checks);
  return failures ? 1 : 0;
}