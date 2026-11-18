/* omifont.h - TrueType faces for the browser's text. */
#ifndef OMI_OMIFONT_H
#define OMI_OMIFONT_H

#define FONT_FACES 2
#define FONT_SANS  0
#define FONT_MONO  1

/* Read the whole file, initialise stb_truetype on face 0. -> 0 | -1 */
int font_load(int slot, const char *path);
int font_ready(int slot);

/* Vertical metrics at a pixel size, rounded once. */
void font_vmetrics(int slot, int px, int *ascent, int *descent, int *line_h);

/* Width of a run in pixels (advances plus kerning). len <= 0 means strlen. */
int font_measure(int slot, int px, const char *s, int len);

/* Blend a run into a 0x00RRGGBB surface of stride x clip_h. */
void font_draw(unsigned *pix, int stride, int clip_w, int clip_h,
               int x, int y, const char *s, int len, int px, unsigned color);

#endif