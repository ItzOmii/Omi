/* The compositor as a server: how a ring-3 process asks for a window.
 *
 * There is no shared memory and no pointer to pixels. A user process sends a
 * flat list of draw commands; the kernel copies each one (and its text) out of
 * user space, validates it, and replays it into the window's client area. The
 * process never sees the framebuffer, and a bug in the process cannot paint
 * outside its own window.
 *
 * Commands are one at a time through SYS_WIN_CMD so nothing has to be trusted
 * across a syscall boundary: by the time the kernel draws, the data is its own
 * copy in kernel memory. */

#ifndef GUIIPC_H
#define GUIIPC_H

#include <stdint.h>

#define WINIPC_MAX       6     /* concurrent foreign windows */
#define WINIPC_MAX_CMDS  48    /* commands kept per window */
#define WINIPC_TEXT_MAX  128   /* bytes of text copied per command */

#define WCMD_RECT        1     /* filled rectangle, colour in `color`   */
#define WCMD_TEXT        2     /* small text at x,y                      */
#define WCMD_FRAME       3     /* 1px rectangle outline                  */
#define WCMD_TEXT_LARGE  4     /* bigger text at x,y                     */

/* The user-side command: 32 bytes, layout shared with the user programs. */
typedef struct {
  int32_t  kind;
  int32_t  x, y, w, h;
  uint32_t color;          /* 0x00RRGGBB */
  uint64_t text;           /* user pointer for the text kinds, else 0 */
} win_cmd_u;

/* Events delivered to the owning process, 16 bytes, polled with SYS_WIN_POLL. */
#define WIN_EV_MOVE   1
#define WIN_EV_DOWN   2
#define WIN_EV_UP     3
#define WIN_EV_KEY    4
#define WIN_EV_CLOSE  5

typedef struct {
  int32_t kind;
  int32_t x, y;      /* client coordinates for the mouse events */
  int32_t code;      /* button for up/down, key code for KEY          */
} win_event_t;

int  winipc_open(const char *title, int32_t w, int32_t h);
void winipc_close(int id);
void winipc_close_owned(int owner);   /* a dead task's windows */
int  winipc_clear(int id, uint32_t color);
/* `text` is a kernel copy made by the syscall layer (already validated). */
int  winipc_add(int id, const win_cmd_u *cmd, const char *text);
void winipc_present(int id);
void winipc_draw_all(void);

/* ==================================================================== */
/* Surfaces: a window a process paints into directly.                    */
/*                                                                      */
/* The command list above is a retained scene the kernel replays - fine  */
/* for a form, hopeless for anything that redraws at a frame rate, since */
/* every pixel would cross a syscall. A surface is the opposite split:    */
/* the kernel owns the frames and the placement, the process owns the     */
/* pixels. It maps them, writes them, and says which rectangle changed.   */
/*                                                                      */
/* The pixels are plain 32-bit 0x00RRGGBB, the same word the framebuffer  */
/* uses, so compositing is a row copy - no scaling, no resampling.       */
/* ==================================================================== */

#define SURF_MAX_W      620
#define SURF_MAX_H      420

/* Bounds are clamped to the window limits. Returns the address the pixels
 * appeared at, or -errno (-ENOSPC when every slot is taken). */
uint64_t surf_create(int win_id, int w, int h);
/* The rectangle the process changed, in client coordinates. */
int      surf_present(int win_id, int x, int y, int w, int h);
/* Kernel-side view of the pixels, for the compositor: 1 when a surface is
 * bound to that window, plus its size. `surf_row` is the address of row `y` -
 * the frames behind a surface are a page list, not one buffer, so a reader
 * takes it a row at a time. */
int       surf_size(int win_id, int *w, int *h);
uint32_t *surf_row(int win_id, int y);
/* The rectangle presented since the last frame (client coordinates), cleared
 * by the read. 0 when nothing new was presented. */
int       surf_take_damage(int win_id, int *x, int *y, int *w, int *h);
void     surf_release_window(int win_id);

/* input: called from the compositor's poll loop. Returns 1 when a foreign
 * window owns the pointer, so the built-in apps see nothing. */
int  winipc_input(int mx, int my, int buttons, int pressed, int released);
int  winipc_key(int key);           /* 1 = consumed by the focused window */
int  winipc_poll(int id, win_event_t *out);   /* 1 = event copied, 0 = empty */
int  winipc_focused(void);          /* window id with keyboard focus, or 0 */

/* The process asks for its own geometry, or changes it. Bounds are clamped by
 * the kernel: a process may not place a window off-screen or resize it past the
 * limits. Returns 0 on success. */
int  winipc_set_rect(int id, int32_t x, int32_t y, int32_t w, int32_t h);
int  winipc_get_rect(int id, int32_t *x, int32_t *y, int32_t *w, int32_t *h);
int  winipc_owner(int id);          /* task id that opened the window */

#endif