/*
 * Omi - TrueType text.
 *
 * stb_truetype renders real outlines, so the UI stops being locked to the
 * 8x16 bitmap face. Two faces ship as boot modules: Bricolage Grotesque for
 * UI text and JetBrains Mono for anything that needs columns.
 */

#ifndef _OMI_TTF_H
#define _OMI_TTF_H

#include "types.h"

#define TTF_DISPLAY 0
#define TTF_MONO    1

/* Returns 1 when at least the display face loaded. Safe to call repeatedly. */
int ttf_init(void);

int ttf_ready(void);
int ttf_line_h(int face, int px);                       /* ascent+descent+gap */
int ttf_ascent(int face, int px);
int ttf_width(int face, int px, const char *s);         /* painted advance  */
void ttf_draw(int face, int px, int x, int baseline, const char *s, color_t color);

#endif
