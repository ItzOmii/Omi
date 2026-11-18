/* AF_UNIX socketpair: the channel a multi-process program talks over.
 *
 * Ordered so the cheap facts print before the one that can block: both
 * directions carry bytes, a closed peer reads as end-of-file rather than a hang,
 * the pool gives out and says so, and finally a read that has nothing yet is woken
 * by another process writing into the other end - which is the property that
 * makes a pair a channel instead of a buffer. */
#define _GNU_SOURCE 1
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int checks, failures;

static void check(int ok, const char *what, long got, long want) {
  checks++;
  if (!ok) failures++;
  printf("[unixpair] %s %s (got %ld want %ld)\n", ok ? "ok  " : "FAIL", what, got, want);
}

#define PAIR_MAX 40

int main(void) {
  int sv[2] = { -1, -1 };
  int r = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
  check(r == 0, "socketpair makes a pair", r, 0);
  if (r != 0) {
    printf("[unixpair] %d/%d\n", checks - failures, checks);
    return 1;
  }
  check(sv[0] >= 3 && sv[1] >= 3 && sv[0] != sv[1], "two distinct descriptors", sv[0], sv[1]);

  char buf[64];
  const char *hello = "hello down the pair";
  long w = write(sv[0], hello, strlen(hello));
  check(w == (long)strlen(hello), "a writes to b", w, (long)strlen(hello));
  long n = read(sv[1], buf, sizeof buf);
  check(n == (long)strlen(hello) && memcmp(buf, hello, strlen(hello)) == 0,
        "b reads exactly those bytes", n, (long)strlen(hello));

  const char *back = "and back up it";
  long w2 = write(sv[1], back, strlen(back));
  check(w2 == (long)strlen(back), "b writes to a (it is full duplex)", w2, (long)strlen(back));
  long n2 = read(sv[0], buf, sizeof buf);
  check(n2 == (long)strlen(back) && memcmp(buf, back, strlen(back)) == 0,
        "a reads them back", n2, (long)strlen(back));

  /* A half-close is what a program does when it has finished sending: the other
   * end must read end-of-file, and promptly, not wait for a deadline. */
  close(sv[1]);
  long eof = read(sv[0], buf, sizeof buf);
  check(eof == 0, "a closed peer reads as end-of-file", eof, 0);
  close(sv[0]);

  /* The pool is finite, so running out has to be an errno and not a crash: a
   * program that opens a pair per child must be able to see the wall. */
  int held[PAIR_MAX][2];
  int made = 0, refused = 0, refused_errno = 0;
  for (int i = 0; i < PAIR_MAX; i++) {
    int p[2] = { -1, -1 };
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, p) == 0) {
      held[made][0] = p[0];
      held[made][1] = p[1];
      made++;
    } else {
      refused++;
      refused_errno = errno;
    }
  }
  check(made >= 12, "the pool holds many pairs at once", made, 12);
  check(refused == 0 || refused_errno == EMFILE || refused_errno == ENFILE,
        "and refuses past its end with a real errno", refused_errno, ENFILE);
  for (int i = 0; i < made; i++) { close(held[i][0]); close(held[i][1]); }

  int again[2] = { -1, -1 };
  check(socketpair(AF_UNIX, SOCK_STREAM, 0, again) == 0,
        "closing them gives the slots back", 0, 0);

  /* The one that matters: a blocking read with nothing in it yet, woken by
   * another process. If the write does not wake the reader, this hangs - which
   * is exactly the failure a channel must not have. */
  pid_t pid = fork();
  if (pid == 0) {
    close(again[0]);
    usleep(50000);
    const char *late = "from the child";
    write(again[1], late, strlen(late));
    close(again[1]);
    _exit(0);
  }
  close(again[1]);
  long got = read(again[0], buf, sizeof buf);
  int status = 0;
  waitpid(pid, &status, 0);
  check(got == (long)strlen("from the child") && memcmp(buf, "from the child", 14) == 0,
        "a read blocks until another process writes", got, 14);
  close(again[0]);

  printf("[unixpair] %d/%d\n", checks - failures, checks);
  return failures ? 1 : 0;
}
