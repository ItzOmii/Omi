/*
 * MeepOS - Intel High Definition Audio (HDA) codec
 *
 * One controller, one codec, one output stream and one input stream. Enough to
 * play PCM and to capture from the mic jack; no mixer graph, no jack
 * detection, no interrupts (every wait is a bounded spin on LPIB).
 *
 * The codec is discovered, not assumed: we walk the audio function group and
 * pick the first audio-output widget, the first audio-input widget and the pin
 * complexes they connect to. QEMU's hda-duplex is nodes 2 (DAC) / 3 (line out)
 * / 4 (ADC) / 5 (line in), but nothing here depends on that.
 */

#ifndef _HDA_H
#define _HDA_H

#include "types.h"

/* Finds the controller, resets it, enumerates the codec. 0 on success. */
int hda_init(void);

/* 1 when init succeeded and both a DAC and an ADC were found. */
int hda_ready(void);

/* Interleaved s16le playback at 48 kHz stereo. Blocks until queued audio has
   been consumed by the codec. Returns bytes played, -1 on error. */
int hda_play(const int16_t *pcm, int bytes);

/* Streaming playback: start once, top up with hda_stream_write() as
   hda_stream_free() reports space, stop when done. */
int hda_stream_start(void);
int hda_stream_free(void);
int hda_stream_write(const int16_t *pcm, int bytes);
void hda_stream_stop(void);
int hda_stream_active(void);

/* Capture interleaved s16le at 48 kHz stereo into buf. Blocks for `bytes`.
   Returns bytes captured, -1 on error. */
int hda_record(int16_t *buf, int bytes);

/* Boot self test: plays a tone-ish burst and captures from the ADC, logging
   sample statistics to COM1. */
void hda_selftest(void);

#endif /* _HDA_H */