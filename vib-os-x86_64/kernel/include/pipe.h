/* Pipes: a byte ring with two ends, exposed to the descriptor layer as a fifth
 * kind of open file. See pipe.c for the semantics and the reason waiting is a
 * one-tick sleep rather than a waiter table. */

#ifndef OMI_PIPE_H
#define OMI_PIPE_H

#include <stdint.h>

#define PIPE_MAX 8
#define PIPE_CAP 4096

struct pipe;

struct pipe *pipe_new(void);
void pipe_ref(struct pipe *p, int write_end);
void pipe_unref(struct pipe *p, int write_end);
long pipe_read(struct pipe *p, void *kbuf, uint64_t n, uint64_t ticks);
long pipe_write(struct pipe *p, const void *kbuf, uint64_t n, uint64_t ticks);
unsigned pipe_poll(struct pipe *p, int write_end);

/* Runs from ring3_init: round trip, wraparound, EOF, EPIPE, and one real
 * blocked read woken by a second task. */
void pipe_selftest(void);

#endif