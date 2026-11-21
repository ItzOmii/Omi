/* omiskia.h - the C face of the Skia text backend (see omiskia.cpp).
 *
 * Same contract as omittf.h's omi_face_* functions, so a caller that includes
 * omittf.h with OMI_SKIA_TEXT defined does not know which one it is talking to.
 * y is the baseline; colours are 0x00RRGGBB; the clip rectangle is the caller's.
 */
#ifndef OMISKIA_H
#define OMISKIA_H

#define OMI_SYS_FONT 59

#ifdef __cplusplus
extern "C" {
#endif

int  omiskia_init(void);
int  omiskia_face_loaded(int which);
int  omiskia_line_h(int font);
int  omiskia_ascent(int font);
int  omiskia_measure(int font, const char *s);
void omiskia_draw(unsigned *pix, int cw, int ch, int font, int x, int y,
                  const char *s, unsigned c, int clip_on, int cx0, int cy0,
                  int cx1, int cy1);

#ifdef __cplusplus
}
#endif

#endif /* OMISKIA_H */
