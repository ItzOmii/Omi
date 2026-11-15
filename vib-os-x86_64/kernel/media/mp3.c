/*
 * MeepOS - MP3 playback (minimp3 streaming decoder)
 *
 * Decodes straight from the boot-module blob, so a 6-minute track never needs
 * its full PCM form in memory. The handle lives on the heap (mp3dec_ex_t is
 * ~11 KB; the kernel stack cannot hold it).
 */

#include "../include/types.h"
#include "../include/kmalloc.h"
#include "../include/limine_modules.h"

#define MP3D_MALLOC(sz) kmalloc(sz)
#define MP3D_FREE(p) kfree(p)
#define MP3D_REALLOC(p, sz) krealloc(p, sz)
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_STDIO
#define MINIMP3_NO_SIMD
/* minimp3-ex also calls the C allocator directly in a few helpers. */
#define malloc(sz) kmalloc(sz)
#define realloc(p, sz) krealloc(p, sz)
#define free(p) kfree(p)
#include "minimp3.h"
#include "minimp3_ex.h"

#include "../include/mp3.h"

struct meep_mp3 {
  mp3dec_ex_t dec;
  int open;
};

extern void serial_puts(const char *s);

static void serial_dec(unsigned long v) {
  char b[24];
  int i = 23;
  b[i] = 0;
  if (!v) { serial_puts("0"); return; }
  while (v && i) { b[--i] = '0' + (v % 10); v /= 10; }
  serial_puts(&b[i]);
}

meep_mp3_t *meep_mp3_open(const void *data, size_t size) {
  if (!data || size < 4) return 0;
  meep_mp3_t *m = (meep_mp3_t *)kzalloc(sizeof(meep_mp3_t));
  if (!m) return 0;
  if (mp3dec_ex_open_buf(&m->dec, (const uint8_t *)data, size,
                         MP3D_SEEK_TO_SAMPLE) != 0) {
    kfree(m);
    return 0;
  }
  m->open = 1;
  return m;
}

uint32_t meep_mp3_rate(meep_mp3_t *m) { return m ? (uint32_t)m->dec.info.hz : 0; }
int meep_mp3_channels(meep_mp3_t *m) { return m ? m->dec.info.channels : 0; }
uint64_t meep_mp3_total(meep_mp3_t *m) { return m ? (uint64_t)m->dec.samples : 0; }
uint64_t meep_mp3_pos(meep_mp3_t *m) { return m ? (uint64_t)m->dec.cur_sample : 0; }

int meep_mp3_read(meep_mp3_t *m, int16_t *dst, int samples) {
  if (!m || !m->open || !dst || samples <= 0) return 0;
  return (int)mp3dec_ex_read(&m->dec, dst, (size_t)samples);
}

void meep_mp3_seek(meep_mp3_t *m, uint64_t sample) {
  if (m && m->open) mp3dec_ex_seek(&m->dec, sample);
}

void meep_mp3_close(meep_mp3_t *m) {
  if (!m) return;
  if (m->open) mp3dec_ex_close(&m->dec);
  kfree(m);
}

/*
 * Boot self-test: proves the decoder sees the real track on the boot medium.
 * Prints duration / rate / channels and measures the first second, so a silent
 * or broken decode is obvious in the serial log.
 */
/* Plain-API probe: isolates the core decoder from the mp3dec_ex layer. */
static void plain_probe(void) {
  mp3dec_t *dec = (mp3dec_t *)kzalloc(sizeof(mp3dec_t));
  if (!dec) { serial_puts("[p] alloc fail\n"); return; }
  mp3dec_init(dec);
  const uint8_t *p = (const uint8_t *)meep_media_mp3;
  int left = (int)meep_media_mp3_len;
  static int16_t pcm[2304];
  for (int f = 0; f < 3; f++) {
    mp3dec_frame_info_t info;
    int s = mp3dec_decode_frame(dec, p, left, pcm, &info);
    serial_puts("[p] frame s="); serial_dec((unsigned long)s);
    serial_puts(" bytes="); serial_dec((unsigned long)info.frame_bytes);
    serial_puts(" hz="); serial_dec((unsigned long)info.hz);
    serial_puts(" ch="); serial_dec((unsigned long)info.channels);
    serial_puts(" pcm=");
    for (int i = 0; i < 4; i++) { serial_puts(" "); serial_dec((unsigned long)(pcm[i] < 0 ? -pcm[i] : pcm[i])); }
    serial_puts("\n");
    if (info.frame_bytes <= 0) break;
    p += info.frame_bytes;
    left -= info.frame_bytes;
  }
  kfree(dec);
}

void meep_mp3_selftest(void) {
  plain_probe();
  if (!meep_media_mp3) {
    serial_puts("[mp3] no sample.mp3 module\n");
    return;
  }
  meep_mp3_t *m = meep_mp3_open(meep_media_mp3, meep_media_mp3_len);
  if (!m) {
    serial_puts("[mp3] open failed\n");
    return;
  }
  uint32_t hz = meep_mp3_rate(m);
  int ch = meep_mp3_channels(m);
  uint64_t total = meep_mp3_total(m);

  serial_puts("[mp3] hz="); serial_dec(hz);
  serial_puts(" ch="); serial_dec((unsigned long)ch);
  serial_puts(" secs="); serial_dec(hz ? (unsigned long)(total / hz) : 0);
  serial_puts("\n");

  serial_puts("[k1] about to alloc buf\n");
  int want = (int)hz * ch; /* one second, interleaved */
  int16_t *buf = (int16_t *)kmalloc((size_t)want * sizeof(int16_t));
  if (!buf) {
    serial_puts("[mp3] selftest alloc failed\n");
    meep_mp3_close(m);
    return;
  }

  serial_puts("[k2] buf ok, first frame...\n");
  {
    int n0 = meep_mp3_read(m, buf, 2304);
    serial_puts("[k3] first read n="); serial_dec((unsigned long)n0);
    serial_puts("\n");
  }
  int got = 0;
  while (got < want) {
    int chunk = 4096;
    if (got + chunk > want) chunk = want - got;
    int n = meep_mp3_read(m, buf + got, chunk);
    if (n <= 0) break;
    got += n;
  }

  serial_puts("[k4] first16:");
  for (int i = 0; i < 16; i++) {
    serial_puts(" ");
    int v = buf[i];
    if (v < 0) { serial_puts("-"); v = -v; }
    serial_dec((unsigned long)v);
  }
  serial_puts("\n");
  uint64_t sumsq = 0;
  int peak = 0, nonzero = 0;
  for (int i = 0; i < got; i++) {
    int v = buf[i];
    int a = v < 0 ? -v : v;
    if (a > peak) peak = a;
    if (v) nonzero++;
    sumsq += (uint64_t)(a * a);
  }
  unsigned long rms = 0;
  if (got > 0) {
    uint64_t mean = sumsq / (uint64_t)got;
    while ((uint64_t)(rms + 1) * (rms + 1) <= mean) rms++;
  }
  serial_puts("[mp3] decoded="); serial_dec((unsigned long)got);
  serial_puts(" peak="); serial_dec((unsigned long)peak);
  serial_puts(" rms="); serial_dec(rms);
  serial_puts(" nonzero="); serial_dec((unsigned long)nonzero);
  serial_puts("\n");

  kfree(buf);
  meep_mp3_close(m);
}
/* ---- plain streaming player -------------------------------------------
   The mp3dec_ex layer builds a sample-accurate seek table with krealloc,
   and in a running desktop that table came back corrupted: reads degraded
   to silence and then to EOF. This path allocates once (a kzalloc'd
   decoder, ~7 KB) and walks frames sequentially with no reallocation, so
   nothing can churn underneath it. */
typedef struct {
  mp3dec_t dec;
  const uint8_t *base;   /* start of the blob, for progress */
  int size;
  const uint8_t *p;
  int left;
  int16_t pcm[2304];
  int fill;      /* samples in pcm */
  int pos;       /* read cursor into pcm */
  uint32_t hz;
  int ch;
  int started;
} mp3_stream_t;

static int frame_skip; /* frames decoded, for diagnostics */

void *meep_mp3_stream_open(const void *data, size_t size) {
  if (!data || size < 8) return 0;
  mp3_stream_t *st = (mp3_stream_t *)kzalloc(sizeof(mp3_stream_t));
  if (!st) return 0;
  mp3dec_init(&st->dec);
  st->base = (const uint8_t *)data;
  st->size = (int)size;
  st->p = st->base;
  st->left = (int)size;
  st->fill = 0;
  st->pos = 0;
  st->started = 0;
  frame_skip = 0;
  return st;
}

/* Decodes one frame into st->pcm. Returns samples, 0 at end of stream. */
static int stream_frame(mp3_stream_t *st) {
  for (int tries = 0; tries < 4; tries++) {
    mp3dec_frame_info_t info;
    int s = mp3dec_decode_frame(&st->dec, st->p, st->left, st->pcm, &info);
    if (info.frame_bytes <= 0) return 0;      /* nothing left */
    st->p += info.frame_bytes;
    st->left -= info.frame_bytes;
    st->hz = info.hz;
    st->ch = info.channels ? info.channels : 2;
    frame_skip++;
    if (s > 0) {
      st->fill = s * st->ch;
      st->pos = 0;
      return st->fill;
    }
    /* skipped a tag/metadata frame: keep going */
  }
  return 0;
}

int meep_mp3_stream_read(void *h, int16_t *dst, int samples) {
  mp3_stream_t *st = (mp3_stream_t *)h;
  if (!st || !dst || samples <= 0) return 0;
  int out = 0;
  while (out < samples) {
    if (st->pos >= st->fill) {
      if (stream_frame(st) <= 0) break;
    }
    int n = st->fill - st->pos;
    if (n > samples - out) n = samples - out;
    for (int i = 0; i < n; i++) dst[out + i] = st->pcm[st->pos + i];
    st->pos += n;
    out += n;
  }
  return out;
}

int meep_mp3_stream_left(void *h) {
  mp3_stream_t *st = (mp3_stream_t *)h;
  return st ? st->left : -1;
}

int meep_mp3_stream_permille(void *h) {
  mp3_stream_t *st = (mp3_stream_t *)h;
  if (!st || st->size <= 0) return 0;
  int done = (int)(st->p - st->base);
  if (done < 0) done = 0;
  if (done > st->size) done = st->size;
  return done * 1000 / st->size;
}

uint32_t meep_mp3_stream_rate(void *h) { return h ? ((mp3_stream_t *)h)->hz : 0; }
int meep_mp3_stream_ch(void *h) { return h ? ((mp3_stream_t *)h)->ch : 0; }
void meep_mp3_stream_close(void *h) { if (h) kfree(h); }
