/*
 * layoutdump - run the page layout engine on the host and print the display list
 *
 * The engine lives in userland/omilayout.c and only needs a measure function, so
 * a bug in wrapping can be seen in milliseconds instead of a four-minute ISO
 * boot. Nothing here is part of the OS image.
 *
 *   gcc -O1 -o /tmp/layoutdump tools/layoutdump.c userland/omilayout.c \
 *       kernel/lib/gumbo/src/*.c -Ikernel/lib/gumbo/src -Iuserland
 *   /tmp/layoutdump page.html 438            # dump ops
 *   /tmp/layoutdump page.html 438 --check    # only report the problems
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "omilayout.h"

/* A stand-in for the compositor's glyph table: 8px per character at scale 1,
 * 16px at scale 2, which is what the 8x16 bitmap face really does. */
static int measure(int font, const char *s) {
  return (int)strlen(s) * 8 * web_font_scale(font);
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s <html> [width] [--check]\n", argv[0]); return 2; }
  int width = argc > 2 ? atoi(argv[2]) : 438;
  int check_only = 0;
  for (int i = 3; i < argc; i++) if (!strcmp(argv[i], "--check")) check_only = 1;

  FILE *f = fopen(argv[1], "rb");
  if (!f) { perror(argv[1]); return 2; }
  static char buf[1 << 20];
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  buf[n] = 0;
  fclose(f);

  if (web_build(buf, (int)n, width, measure) != 0) {
    fprintf(stderr, "layout failed\n");
    return 1;
  }

  int count = web_count();
  const web_op_t *ops = web_ops();
  int h = web_height();
  int over = 0, neg = 0, empty = 0, tall = 0;

  if (!check_only) printf("page %dx%d, %d ops\n", width, h, count);

  int prev_bottom = 0;
  for (int i = 0; i < count; i++) {
    const web_op_t *o = &ops[i];
    if (o->x + o->w > width + 1) over++;
    if (o->x < 0 || o->y < 0) neg++;
    if (o->kind == 1 && (!o->text || !o->text[0])) empty++;
    if (o->w > width * 2) tall++;
    if (!check_only) {
      printf("%3d kind=%d %4d,%4d %4dx%-4d font=%d%s%s ", i, o->kind, o->x, o->y, o->w, o->h,
             o->font, o->bold ? " bold" : "", o->underline ? " ul" : "");
      if (o->text) {
        printf("\"");
        for (const char *p = o->text; *p; p++) putchar(*p == '\n' ? ' ' : *p);
        printf("\"");
      }
      if (o->href) printf(" -> %s", o->href);
      putchar('\n');
    }
    if (o->y + o->h > prev_bottom) prev_bottom = o->y + o->h;
  }

  printf("[dump] width=%d height=%d ops=%d last_bottom=%d overflow_x=%d\n",
         width, h, count, prev_bottom, over);
  if (web_ops_full()) printf("[dump] FAIL display list full - page truncated\n");
  if (neg) printf("[dump] FAIL %d ops at negative coordinates\n", neg);
  if (empty) printf("[dump] FAIL %d empty text runs\n", empty);
  if (over) printf("[dump] FAIL %d ops run past the right edge\n", over);
  if (tall) printf("[dump] FAIL %d ops wider than twice the page\n", tall);
  if (h > prev_bottom + 40) printf("[dump] note page height %d but content ends at %d\n", h, prev_bottom);
  return 0;
}