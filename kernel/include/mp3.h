/*
 * MeepOS - MP3 decoder interface (see media/mp3.c)
 *
 * The handle is opaque and heap-backed: mp3dec_ex_t alone is ~11 KB, which
 * is far too much to sit on the kernel stack.
 */

#ifndef _MEEPOS_MP3_H
#define _MEEPOS_MP3_H

#include "types.h"

typedef struct meep_mp3 meep_mp3_t;

/* NULL on failure (not an MP3 / out of memory). */
meep_mp3_t *meep_mp3_open(const void *data, size_t size);
uint32_t meep_mp3_rate(meep_mp3_t *m);
int meep_mp3_channels(meep_mp3_t *m);
uint64_t meep_mp3_total(meep_mp3_t *m); /* samples per channel */
uint64_t meep_mp3_pos(meep_mp3_t *m);
/* Reads up to `samples` interleaved int16 values; returns count read. */
int meep_mp3_read(meep_mp3_t *m, int16_t *dst, int samples);
void meep_mp3_seek(meep_mp3_t *m, uint64_t sample);
void meep_mp3_close(meep_mp3_t *m);

/* Plain sequential player (no seek table, no realloc). */
void *meep_mp3_stream_open(const void *data, size_t size);
int meep_mp3_stream_read(void *h, int16_t *dst, int samples);
uint32_t meep_mp3_stream_rate(void *h);
int meep_mp3_stream_permille(void *h);
int meep_mp3_stream_left(void *h);
int meep_mp3_stream_ch(void *h);
void meep_mp3_stream_close(void *h);

void meep_mp3_selftest(void);

#endif /* _MEEPOS_MP3_H */