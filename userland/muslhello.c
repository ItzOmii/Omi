#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv) {
  printf("[musl] hello from a real libc (argc=%d)\n", argc);
  for (int i = 0; i < argc; i++) printf("[musl] argv[%d] = %s\n", i, argv[i]);
  char *p = malloc(512);
  if (!p) { printf("[musl] malloc failed\n"); return 1; }
  strcpy(p, "malloc works");
  printf("[musl] %s, pid=%d, sizeof(long)=%zu\n", p, (int)getpid(), sizeof(long));
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  printf("[musl] monotonic clock: %lld.%09ld\n", (long long)ts.tv_sec, ts.tv_nsec);
  char *big = malloc(1 << 20);
  memset(big, 'Z', 1 << 20);
  printf("[musl] 1 MiB touched, byte[%d]=%c\n", (1<<20)-1, big[(1<<20)-1]);
  free(big); free(p);
  printf("[musl] OK\n");
  return 0;
}
