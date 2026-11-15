/*
 * MeepOS x86_64 - Complete GUI Compositor
 * Desktop, Dock, Context Menu, Terminal, File Manager, Windows
 * With HD Wallpaper support and real dock icons
 */

#include "../include/e1000.h"
#include "../include/font.h"
#include "../include/gui.h"
#include "../include/kmalloc.h"
#include "../include/media.h"
#include "../include/pci.h"
#include "../include/ps2.h"
#include "../include/usb.h"
#include "../include/string.h"
#include "../include/vfs.h"
#include "dock_icons.h"
#include "meep_font.h"
#include "icons_ri.h"
#include "omi_net.h"
#include "../include/task.h"
#include "limine_modules.h"
#include "mp3.h"
#include "../include/hda.h"
#include "omi_http.h"
#include "omiweb.h"
#include "ttf.h"
#include "webimg.h"

static const web_image_t *on_page_image(const char *src, int len);
#include "omi_tls.h"

/* External font data */
extern const uint8_t font_data[256][16];

/* External HD wallpaper JPEG data */
extern unsigned char hd_wallpaper_landscape_jpg[];
extern unsigned int hd_wallpaper_landscape_jpg_len;
extern unsigned char hd_wallpaper_nature_jpg[];
extern unsigned int hd_wallpaper_nature_jpg_len;
extern unsigned char hd_wallpaper_city_jpg[];
extern unsigned int hd_wallpaper_city_jpg_len;

/* ===================================================================== */
/* Theme Colors - Catppuccin Mocha inspired                              */
/* ===================================================================== */

#define THEME_BASE 0x1E1E2E
#define THEME_SURFACE 0x313244
#define THEME_OVERLAY 0x45475A
#define THEME_TEXT 0xCDD6F4
#define THEME_SUBTEXT 0xA6ADC8
#define THEME_LAVENDER 0xB4BEFE
#define THEME_BLUE 0x89B4FA
#define THEME_SAPPHIRE 0x74C7EC
#define THEME_SKY 0x89DCEB
#define THEME_TEAL 0x94E2D5
#define THEME_GREEN 0xA6E3A1
#define THEME_YELLOW 0xF9E2AF
#define THEME_PEACH 0xFAB387
#define THEME_MAROON 0xEBA0AC
#define THEME_RED 0xF38BA8
#define THEME_MAUVE 0xCBA6F7
#define THEME_PINK 0xF5C2E7
#define THEME_FLAMINGO 0xF2CDCD
#define THEME_ROSEWATER 0xF5E0DC

/* UI Element colors */
#define COLOR_WINDOW_BG THEME_BASE
#define COLOR_TITLE_BAR 0x181825
#define COLOR_MENU_BAR_BG 0x11111B
#define COLOR_DOCK_BG 0x1E1E28
#define COLOR_DOCK_BORDER 0x45475A
#define COLOR_CTX_MENU_BG 0x313244
#define COLOR_CTX_MENU_HOVER 0x45475A
#define COLOR_TERM_BG 0x1E1E2E
#define COLOR_TERM_FG 0xCDD6F4

/* ===================================================================== */
/* MeepOS palette - values lifted from sites/meepos/index.html :root     */
/* ===================================================================== */
/* The palette is runtime state, not compile-time constants: the menubar's
   appearance toggle swaps every value below between the web shell's "paper"
   and "midnight" :root blocks, and a full redraw repaints everything with it.
   Keep the names - every draw site already reads them. */
static uint32_t MEEP_PAPER      = MAKE_COLOR(0xED, 0xEB, 0xDD); /* --paper     */
static uint32_t MEEP_PAPER2     = MAKE_COLOR(0xF7, 0xF5, 0xEA); /* --paper-2   */
static uint32_t MEEP_PAPER3     = MAKE_COLOR(0xE2, 0xE0, 0xCE); /* --paper-3   */
static uint32_t MEEP_INK        = MAKE_COLOR(0x16, 0x16, 0x0F); /* --ink       */
static uint32_t MEEP_INK_SOFT   = MAKE_COLOR(0x63, 0x63, 0x5A); /* --ink-soft  */
static uint32_t MEEP_LINE       = MAKE_COLOR(0xC6, 0xC5, 0xB3); /* --line      */
static uint32_t MEEP_ACCENT     = MAKE_COLOR(0xFF, 0xC2, 0x3D); /* --accent    */
static uint32_t MEEP_ACCENT_INK = MAKE_COLOR(0x23, 0x1A, 0x00); /* --accent-ink*/
static uint32_t MEEP_OK         = MAKE_COLOR(0x4A, 0x9D, 0x58); /* --ok        */
static uint32_t MEEP_LINE_SOFT  = MAKE_COLOR(0xD8, 0xD7, 0xC6); /* --line-soft */
static uint32_t MEEP_PRESS      = MAKE_COLOR(0xD2, 0xD0, 0xBE); /* pressed tile*/

/* 0 = paper, 1 = midnight. Mirrors index.html's :root[data-theme] blocks. */
static int appearance_dark = 1;   /* boot appearance: midnight */

static int appearance_wall_dirty = 0;

static void meep_apply_appearance(int dark) {
  appearance_dark = dark ? 1 : 0;
  appearance_wall_dirty = 1;
  if (!appearance_dark) {
    MEEP_PAPER      = MAKE_COLOR(0xED, 0xEB, 0xDD);
    MEEP_PAPER2     = MAKE_COLOR(0xF7, 0xF5, 0xEA);
    MEEP_PAPER3     = MAKE_COLOR(0xE2, 0xE0, 0xCE);
    MEEP_INK        = MAKE_COLOR(0x16, 0x16, 0x0F);
    MEEP_INK_SOFT   = MAKE_COLOR(0x63, 0x63, 0x5A);
    MEEP_LINE       = MAKE_COLOR(0xC6, 0xC5, 0xB3);
    MEEP_LINE_SOFT  = MAKE_COLOR(0xD8, 0xD7, 0xC6);
    MEEP_PRESS      = MAKE_COLOR(0xD2, 0xD0, 0xBE);
    MEEP_ACCENT     = MAKE_COLOR(0xFF, 0xC2, 0x3D);
    MEEP_ACCENT_INK = MAKE_COLOR(0x23, 0x1A, 0x00);
    MEEP_OK         = MAKE_COLOR(0x4A, 0x9D, 0x58);
  } else {
    MEEP_PAPER      = MAKE_COLOR(0x17, 0x18, 0x1B);
    MEEP_PAPER2     = MAKE_COLOR(0x1E, 0x20, 0x24);
    MEEP_PAPER3     = MAKE_COLOR(0x26, 0x28, 0x2D);
    MEEP_INK        = MAKE_COLOR(0xEF, 0xED, 0xE3);
    MEEP_INK_SOFT   = MAKE_COLOR(0x8E, 0x8F, 0x86);
    MEEP_LINE       = MAKE_COLOR(0x32, 0x34, 0x3A);
    MEEP_LINE_SOFT  = MAKE_COLOR(0x2A, 0x2C, 0x31);
    MEEP_PRESS      = MAKE_COLOR(0x2E, 0x30, 0x36);
    MEEP_ACCENT     = MAKE_COLOR(0xFF, 0xC2, 0x3D);
    MEEP_ACCENT_INK = MAKE_COLOR(0x1A, 0x14, 0x00);
    MEEP_OK         = MAKE_COLOR(0x5F, 0xBF, 0x72);
  }
}

/* UI Scale - dynamically set based on resolution */
int ui_scale = 1;
#define UI_SCALE_VAL(x) ((x) * ui_scale)

/* Redraw flags - declared early for keyboard callback */
static int needs_redraw = 1;
static int full_redraw = 1;

/* Dock Configuration - scaled at runtime */
static int dock_height = 70;
static int dock_icon_size = 48;
static int dock_padding = 10;
#define NUM_DOCK_ICONS 10

/* Menu Bar - scaled at runtime */
static int menu_bar_height = 28;

/* For compatibility with existing code using constants */
#define DOCK_HEIGHT dock_height
#define DOCK_ICON_SIZE dock_icon_size
#define DOCK_PADDING dock_padding
#define MENU_BAR_HEIGHT menu_bar_height

/* ===================================================================== */
/* Global State                                                          */
/* ===================================================================== */

/* Mouse state */
static int mouse_x = 400;
static int mouse_y = 300;
static int mouse_buttons = 0;
/* shell interaction state (declared up here: draw_dock() runs before the menus) */
static int open_menu = -1, hover_menu = -1, hover_item = -1;
static int hover_dock = -1, press_dock = -1;
static int dock_hover_index = -1;
static int dock_smooth_sizes[NUM_DOCK_ICONS] = {0};

/* Window dragging state */
#define DRAG_NONE 0
#define DRAG_TERMINAL 1
#define DRAG_FILE_MANAGER 2
#define DRAG_CALCULATOR 3
#define DRAG_SNAKE 4
#define DRAG_NOTEPAD 5
#define DRAG_CLOCK 6
#define DRAG_HELP 7
#define DRAG_IMAGE_VIEWER 8
static int dragging_window = DRAG_NONE;
static int drag_offset_x = 0;
static int drag_offset_y = 0;

/* Window z-order tracking - higher index = on top */
#define WIN_FILE_MANAGER 0
#define WIN_TERMINAL 1
#define WIN_CALCULATOR 2
#define WIN_SNAKE 3
#define WIN_NOTEPAD 4
#define WIN_CLOCK 5
#define WIN_HELP 6
#define WIN_IMAGE_VIEWER 7
#define NUM_MANAGED_WINDOWS 8
static int window_z_order[NUM_MANAGED_WINDOWS] = {0, 1, 2, 3, 4, 5, 6, 7};
static int focused_window_id = WIN_TERMINAL; /* Terminal focused by default */

/* Bring window to front (highest z-order) */
static void bring_window_to_front(int win_id) {
  if (win_id < 0 || win_id >= NUM_MANAGED_WINDOWS) return;
  focused_window_id = win_id;
  
  /* Find current z of this window */
  int current_z = -1;
  for (int i = 0; i < NUM_MANAGED_WINDOWS; i++) {
    if (window_z_order[i] == win_id) {
      current_z = i;
      break;
    }
  }
  if (current_z < 0) return;
  
  /* Shift all windows above it down, put this one on top */
  for (int i = current_z; i < NUM_MANAGED_WINDOWS - 1; i++) {
    window_z_order[i] = window_z_order[i + 1];
  }
  window_z_order[NUM_MANAGED_WINDOWS - 1] = win_id;
}

/* Context menu state */
typedef struct {
  int visible;
  int x, y;
  int width, height;
  int hover_index;
  int item_count;
  struct {
    char label[32];
    int enabled;
    int separator;
  } items[16];
} context_menu_t;

static context_menu_t ctx_menu = {0};

/* Terminal state */
#define TERM_COLS 80
#define TERM_ROWS 24
#define TERM_MAX_INPUT 256

typedef struct {
  char chars[TERM_ROWS][TERM_COLS];
  uint8_t colors[TERM_ROWS][TERM_COLS];
  int cursor_x, cursor_y;
  int content_x, content_y; /* Screen position */
  int width, height;
  char input[TERM_MAX_INPUT];
  int input_len;
  char cwd[128];
  int visible;
} terminal_t;

static terminal_t terminal = {0};

/* File Manager state */
typedef struct {
  char current_path[256];
  struct {
    char name[64];
    int is_dir;
  } entries[32];
  int entry_count;
  int selected;
  int visible;
  int x, y, width, height;
} file_manager_t;

static file_manager_t file_manager = {0};

/* Simulated filesystem - matches reference OS */
static const struct {
  const char *path;
  const char *name;
  int is_dir;
} sim_files[] = {
  /* Root directories */
  {"/", "Documents", 1},
  {"/", "Downloads", 1},
  {"/", "Pictures", 1},
  {"/", "Desktop", 1},
  /* Desktop contents */
  {"/Desktop", "notes.txt", 0},
  {"/Desktop", "readme.txt", 0},
  {"/Desktop", "Projects", 1},
  /* Documents contents */
  {"/Documents", "report.txt", 0},
  /* Pictures contents */
  {"/Pictures", "landscape.jpg", 0},
  {"/Pictures", "nature.jpg", 0},
  {"/Pictures", "city.jpg", 0},
  {"/Pictures", "wallpaper.jpg", 0},
  /* Projects folder */
  {"/Desktop/Projects", "project1.txt", 0},
  {NULL, NULL, 0}
};

/* Window state - MAX_WINDOWS defined in gui.h */

typedef struct {
  int id;
  char title[64];
  int x, y, width, height;
  int visible;
  int focused;
  int type; /* 0=normal, 1=terminal, 2=filemanager, 3=settings */
} win_t;

static win_t windows[MAX_WINDOWS] = {0};
static int window_count = 0;
static int next_window_id = 1;

/* ===================================================================== */
/* Wallpaper Manager                                                     */
/* ===================================================================== */

#define NUM_WALLPAPERS 8
static int current_wallpaper = 0;

/* Wallpaper types: 0 = gradient, 1 = JPEG image */
static struct {
  int type;           /* 0 = gradient, 1 = JPEG image */
  uint8_t tr, tg, tb; /* Gradient: Top color */
  uint8_t br, bg, bb; /* Gradient: Bottom color */
  const char *name;   /* Display name */
} wallpapers[NUM_WALLPAPERS] = {
    {1, 0, 0, 0, 0, 0, 0, "Landscape"},
    {1, 0, 0, 0, 0, 0, 0, "Nature"},
    {1, 0, 0, 0, 0, 0, 0, "City"},
    {0, 30, 27, 75, 15, 27, 62, "Indigo Night"},
    {0, 20, 60, 100, 10, 30, 60, "Ocean Blue"},
    {0, 60, 20, 60, 30, 15, 45, "Purple Haze"},
    {0, 20, 20, 20, 5, 5, 10, "Midnight"},
    {0, 80, 60, 30, 40, 30, 20, "Golden Hour"},
};

/* Cached wallpaper image for desktop background */
static media_image_t wallpaper_image = {0, 0, (void *)0};
static int wallpaper_loaded = -1;

/* Static buffer for wallpaper to avoid heap fragmentation (4MB for 1024x1024) */
static uint32_t wallpaper_buffer[1024 * 1024];

/* Scaled wallpaper cache (screen-sized) */
static uint32_t *wallpaper_cache = (uint32_t *)0;
static size_t wallpaper_cache_size = 0;
static int wallpaper_cache_w = 0;
static int wallpaper_cache_h = 0;
static int wallpaper_cache_dirty = 1;

/* Load wallpaper image if needed */
static void wallpaper_ensure_loaded(void) {
  if (wallpapers[current_wallpaper].type != 1)
    return; /* Gradient, no load */
  if (wallpaper_loaded == current_wallpaper && wallpaper_image.pixels != (void *)0)
    return; /* Already loaded */

  /* Reset previous image state */
  wallpaper_image.width = 0;
  wallpaper_image.height = 0;
  wallpaper_loaded = -1;

  /* Select the right embedded JPEG data based on wallpaper index */
  const uint8_t *data = (void *)0;
  size_t size = 0;

  switch (current_wallpaper) {
  case 0:
    data = hd_wallpaper_landscape_jpg;
    size = hd_wallpaper_landscape_jpg_len;
    break;
  case 1:
    data = hd_wallpaper_nature_jpg;
    size = hd_wallpaper_nature_jpg_len;
    break;
  case 2:
    data = hd_wallpaper_city_jpg;
    size = hd_wallpaper_city_jpg_len;
    break;
  default:
    return; /* Not an image wallpaper */
  }

  if (data && size > 0) {
    if (media_decode_jpeg_buffer(data, size, &wallpaper_image, wallpaper_buffer,
                                 sizeof(wallpaper_buffer)) == 0) {
      wallpaper_loaded = current_wallpaper;
      wallpaper_cache_dirty = 1;
    } else {
      /* Fallback to gradient if decode fails */
      wallpapers[current_wallpaper].type = 0;
      wallpaper_loaded = -1;
      wallpaper_cache_dirty = 1;
    }
  }
}

/* Get wallpaper pixel color at position */
static uint32_t wallpaper_get_pixel(int x, int y, int width, int height) {
  int idx = current_wallpaper;

  /* Image wallpaper */
  if (wallpapers[idx].type == 1 && wallpaper_image.pixels) {
    /* Scale image to fit screen */
    int img_x = (x * (int)wallpaper_image.width) / width;
    int img_y = (y * (int)wallpaper_image.height) / height;
    if (img_x >= 0 && img_x < (int)wallpaper_image.width && img_y >= 0 &&
        img_y < (int)wallpaper_image.height) {
      uint32_t px = wallpaper_image.pixels[img_y * wallpaper_image.width + img_x];
      if (appearance_dark) {
        /* midnight desktop: same paper and grain, flipped to a dark ground so
           the dots read as speckle instead of dirt */
        int r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF, b = px & 0xFF;
        int lum = (r * 77 + g * 150 + b * 29) >> 8;
        if (lum < 226) return MAKE_COLOR(0x2C, 0x2E, 0x34);
        return MAKE_COLOR(0x17, 0x18, 0x1B);
      }
      return px;
    }
  }

  /* Gradient fallback */
  int progress = (y * 256) / height;
  if (progress < 0)
    progress = 0;
  if (progress > 255)
    progress = 255;

  uint8_t r = wallpapers[idx].tr +
              ((wallpapers[idx].br - wallpapers[idx].tr) * progress) / 256;
  uint8_t g = wallpapers[idx].tg +
              ((wallpapers[idx].bg - wallpapers[idx].tg) * progress) / 256;
  uint8_t b = wallpapers[idx].tb +
              ((wallpapers[idx].bb - wallpapers[idx].tb) * progress) / 256;

  return (r << 16) | (g << 8) | b;
}

/* Cycle to next wallpaper */
static void wallpaper_next(void) {
  current_wallpaper = (current_wallpaper + 1) % NUM_WALLPAPERS;
  if (wallpapers[current_wallpaper].type == 1) {
    wallpaper_ensure_loaded();
  }
  wallpaper_cache_dirty = 1;
}

/* Set specific wallpaper */
static void wallpaper_set(int index) {
  if (index >= 0 && index < NUM_WALLPAPERS) {
    current_wallpaper = index;
    if (wallpapers[current_wallpaper].type == 1) {
      wallpaper_ensure_loaded();
    }
    wallpaper_cache_dirty = 1;
  }
}

static void wallpaper_build_cache(void) {
  size_t needed = (size_t)screen_width * screen_height * sizeof(uint32_t);
  if (needed == 0)
    return;

  if (wallpaper_cache == (void *)0 || needed > wallpaper_cache_size ||
      wallpaper_cache_w != (int)screen_width ||
      wallpaper_cache_h != (int)screen_height) {
    if (wallpaper_cache) {
      kfree(wallpaper_cache);
    }
    wallpaper_cache = (uint32_t *)kmalloc(needed);
    wallpaper_cache_size = needed;
    wallpaper_cache_w = (int)screen_width;
    wallpaper_cache_h = (int)screen_height;
  }

  if (!wallpaper_cache)
    return;

  for (uint32_t y = 0; y < screen_height; y++) {
    for (uint32_t x = 0; x < screen_width; x++) {
      wallpaper_cache[y * screen_width + x] =
          wallpaper_get_pixel(x, y, screen_width, screen_height);
    }
  }

  wallpaper_cache_dirty = 0;
}

/* Dock icons info - matching test folder */
static const char *dock_labels[] = {"Terminal", "Files", "Calculator", "Notes", "Settings",
                                    "Clock", "Snake", "Help", "Browser", "DOOM"};

/* Icon background colors for macOS Big Sur style dock */
static const uint32_t dock_icon_colors[] = {
    0x1E1E1E, /* 0 - Terminal - dark gray/black */
    0x3B82F6, /* 1 - Files - blue */
    0xFF9500, /* 2 - Calculator - orange */
    0xFFCC00, /* 3 - Notes - yellow */
    0x8E8E93, /* 4 - Settings - gray */
    0x000000, /* 5 - Clock - black */
    0x34D399, /* 6 - Snake - teal green */
    0x3B82F6, /* 7 - Help - blue */
    0x0EA5E9, /* 8 - Browser - sky blue */
    0xCC0000, /* 9 - DOOM - red */
};

/* ===================================================================== */
/* Drawing Helpers                                                       */
/* ===================================================================== */

/* Draw character with foreground color */
void gui_draw_char(int x, int y, char c, uint32_t fg) {
  if (c < 0)
    c = 0;
  const uint8_t *glyph = font_data[(uint8_t)c];
  int scale = ui_scale > 0 ? ui_scale : 1;
  for (int row = 0; row < 16; row++) {
    uint8_t bits = glyph[row];
    for (int col = 0; col < 8; col++) {
      if (bits & (0x80 >> col)) {
        int px = x + col * scale;
        int py = y + row * scale;
        if (scale == 1) {
          fb_put_pixel(px, py, fg);
        } else {
          fb_fill_rect(px, py, scale, scale, fg);
        }
      }
    }
  }
}

/* Draw string */
void gui_draw_string(int x, int y, const char *str, uint32_t color) {
  int scale = ui_scale > 0 ? ui_scale : 1;
  while (*str) {
    if (*str == '\n') {
      y += 16 * scale;
    } else {
      gui_draw_char(x, y, *str, color);
      x += 8 * scale;
    }
    str++;
  }
}

/* Draw string with background */
void gui_draw_string_bg(int x, int y, const char *str, uint32_t fg,
                        uint32_t bg) {
  int scale = ui_scale > 0 ? ui_scale : 1;
  while (*str) {
    fb_fill_rect(x, y, 8 * scale, 16 * scale, bg);
    gui_draw_char(x, y, *str, fg);
    x += 8 * scale;
    str++;
  }
}

/* Draw line (Bresenham) */
void gui_draw_line(int x0, int y0, int x1, int y1, uint32_t color) {
  int dx = x1 - x0;
  int dy = y1 - y0;
  int sx = dx > 0 ? 1 : -1;
  int sy = dy > 0 ? 1 : -1;
  dx = dx < 0 ? -dx : dx;
  dy = dy < 0 ? -dy : dy;

  int err = (dx > dy ? dx : -dy) / 2;

  while (1) {
    fb_put_pixel(x0, y0, color);
    if (x0 == x1 && y0 == y1)
      break;
    int e2 = err;
    if (e2 > -dx) {
      err -= dy;
      x0 += sx;
    }
    if (e2 < dy) {
      err += dx;
      y0 += sy;
    }
  }
}

/* Draw filled circle */
void gui_draw_circle(int cx, int cy, int r, uint32_t color) {
  for (int y = -r; y <= r; y++) {
    for (int x = -r; x <= r; x++) {
      if (x * x + y * y <= r * r) {
        fb_put_pixel(cx + x, cy + y, color);
      }
    }
  }
}

/* Draw traffic light buttons with icons (macOS style) */
static void draw_traffic_lights(int x, int y, int title_h) {
  int btn_cx = x + UI_SCALE_VAL(20);
  int btn_cy = y + UI_SCALE_VAL(16);
  int btn_r = UI_SCALE_VAL(6);
  int icon_size = UI_SCALE_VAL(2);  /* Icon line thickness */
  
  /* Close button - Red with X icon */
  gui_draw_circle(btn_cx, btn_cy, btn_r, THEME_RED);
  for (int i = -icon_size; i <= icon_size; i++) {
    fb_put_pixel(btn_cx + i, btn_cy + i, 0x7F1D1D);  /* Dark red X */
    fb_put_pixel(btn_cx + i, btn_cy - i, 0x7F1D1D);
  }
  
  /* Minimize button - Yellow with - icon */
  btn_cx += UI_SCALE_VAL(20);
  gui_draw_circle(btn_cx, btn_cy, btn_r, THEME_YELLOW);
  for (int i = -icon_size; i <= icon_size; i++) {
    fb_put_pixel(btn_cx + i, btn_cy, 0x78350F);  /* Dark amber - */
  }
  
  /* Maximize/Zoom button - Green with + icon */
  btn_cx += UI_SCALE_VAL(20);
  gui_draw_circle(btn_cx, btn_cy, btn_r, THEME_GREEN);
  for (int i = -icon_size; i <= icon_size; i++) {
    fb_put_pixel(btn_cx + i, btn_cy, 0x14532D);  /* Dark green + */
    fb_put_pixel(btn_cx, btn_cy + i, 0x14532D);
  }
}

/* Draw rounded rectangle */
void gui_draw_rounded_rect(int x, int y, int w, int h, int r, uint32_t color) {
  /* Main body */
  fb_fill_rect(x + r, y, w - 2 * r, h, color);
  fb_fill_rect(x, y + r, w, h - 2 * r, color);

  /* Corners */
  for (int py = 0; py < r; py++) {
    for (int px = 0; px < r; px++) {
      if ((r - px - 1) * (r - px - 1) + (r - py - 1) * (r - py - 1) <= r * r) {
        fb_put_pixel(x + px, y + py, color);
        fb_put_pixel(x + w - 1 - px, y + py, color);
        fb_put_pixel(x + px, y + h - 1 - py, color);
        fb_put_pixel(x + w - 1 - px, y + h - 1 - py, color);
      }
    }
  }
}

/* ===================================================================== */
/* Dock with Magnification                                               */
/* ===================================================================== */

/* Get dock icon data pointer by index - matching dock_labels order */
static const uint32_t *get_dock_icon_data(int icon_idx) {
  switch (icon_idx) {
  case 0:
    return dock_icon_terminal;    /* Terminal */
  case 1:
    return dock_icon_folder;      /* Files */
  case 2:
    return dock_icon_calculator;  /* Calculator */
  case 3:
    return dock_icon_notes;       /* Notes */
  case 4:
    return dock_icon_settings;    /* Settings */
  case 5:
    return dock_icon_clock;       /* Clock */
  case 6:
    return dock_icon_snake;       /* Snake */
  case 7:
    return dock_icon_help;        /* Help */
  case 8:
    return dock_icon_web;         /* Browser */
  case 9:
    return dock_icon_doom;        /* DOOM */
  default:
    return dock_icon_terminal;
  }
}

static void draw_dock_icon(int x, int y, int size, int icon_idx,
                           uint32_t color) {
  (void)color; /* Not used when drawing real icons */

  const uint32_t *icon_data = get_dock_icon_data(icon_idx);

  /* Scale the 48x48 icon to the requested size */
  for (int py = 0; py < size; py++) {
    int src_y = (py * DOCK_ICON_BITMAP_SIZE) / size;
    for (int px = 0; px < size; px++) {
      int src_x = (px * DOCK_ICON_BITMAP_SIZE) / size;
      uint32_t pixel = icon_data[src_y * DOCK_ICON_BITMAP_SIZE + src_x];

      /* Check alpha (0xFFFFFFFF = white, 0x00000000 = transparent) */
      if (pixel == 0xFFFFFFFF) {
        fb_put_pixel(x + px, y + py, 0xFFFFFFFF);
      } else if (pixel != 0x00000000) {
        /* Semi-transparent or colored pixel */
        fb_put_pixel(x + px, y + py, pixel);
      }
      /* Skip fully transparent pixels */
    }
  }
}


/* ===================================================================== */
/* MeepOS shell helpers                                                  */
/* ===================================================================== */

/* a 1px rounded outline: fill the rounded shape, then its interior again */
static void meep_box(int x, int y, int w, int h, int r, uint32_t stroke,
                     uint32_t fill) {
  int t = ui_scale > 0 ? ui_scale : 1;      /* 1 CSS px, whatever we render at */
  gui_draw_rounded_rect(x, y, w, h, r, stroke);
  if (w > 2 * t && h > 2 * t)
    gui_draw_rounded_rect(x + t, y + t, w - 2 * t, h - 2 * t, r > t ? r - t : 0, fill);
}

/* ---- real time, straight off the CMOS, so the menubar clock is live ---- */
static uint8_t meep_inb(uint16_t port) {
  uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v;
}
static void meep_outb(uint16_t port, uint8_t v) {
  __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}
static uint8_t meep_cmos(uint8_t reg) { meep_outb(0x70, reg); return meep_inb(0x71); }
static int meep_bcd(uint8_t v) { return (v & 0x0F) + ((v >> 4) * 10); }

static int meep_isqrt(int v) {
  if (v <= 0) return 0;
  int x = v, y = (x + 1) / 2;
  while (y < x) { x = y; y = (x + v / x) / 2; }
  return x;
}

/* "Sat, Nov 14  2:32 PM" - the same string the web shell puts in the menubar */
static void meep_clock_text(char *out) {
  static const char *DAYS[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char *MONS[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  for (int i = 0; i < 100000; i++)
    if (!(meep_cmos(0x0A) & 0x80)) break;      /* wait out an update in progress */

  uint8_t min = meep_cmos(0x02), hr = meep_cmos(0x04);
  uint8_t dow = meep_cmos(0x06), day = meep_cmos(0x07), mon = meep_cmos(0x08);
  uint8_t regB = meep_cmos(0x0B);

  int pm = (hr & 0x80) != 0;
  int h = hr & 0x7F, d = day, m = mon;
  if (!(regB & 0x04)) { h = meep_bcd((uint8_t)h); d = meep_bcd(day); m = meep_bcd(mon);
                        min = (uint8_t)meep_bcd(min); }
  if (regB & 0x02) pm = (h >= 12);
  int h12 = h % 12; if (h12 == 0) h12 = 12;
  if (m < 1 || m > 12) m = 1;
  if (d < 1 || d > 31) d = 1;
  if (dow < 1 || dow > 7) dow = 1;

  char *o = out;
  const char *s = DAYS[dow - 1];             while (*s) *o++ = *s++;
  *o++ = ','; *o++ = ' ';
  s = MONS[m - 1];                           while (*s) *o++ = *s++;
  *o++ = ' ';
  if (d >= 10) *o++ = (char)('0' + d / 10);
  *o++ = (char)('0' + d % 10);
  *o++ = ' '; *o++ = ' ';
  if (h12 >= 10) *o++ = (char)('0' + h12 / 10);
  *o++ = (char)('0' + h12 % 10);
  *o++ = ':';
  *o++ = (char)('0' + (min / 10) % 10);
  *o++ = (char)('0' + min % 10);
  *o++ = ' ';
  *o++ = pm ? 'P' : 'A';
  *o++ = 'M';
  *o = 0;
}

/* thin horizontal rule, used for the sliders / wifi glyphs */
static void meep_hline(int x, int y, int w, int t, uint32_t c) {
  fb_fill_rect(x, y, w, t, c);
}

static void meep_vline(int x, int y, int h, int t, uint32_t c) {
  fb_fill_rect(x, y, t, h, c);
}

/* upper part of a circle outline, used for the wifi fan and the globe meridians.
   thresh = how far below the centre the arc is allowed to reach. */
static void meep_arc_up(int cx, int cy, int r, int thresh, int t, uint32_t c) {
  int x = r, y = 0, err = 1 - r;
  while (x >= y) {
    int pts[8][2] = {{x, y}, {y, x}, {-y, x}, {-x, y},
                     {-x, -y}, {-y, -x}, {y, -x}, {x, -y}};
    for (int i = 0; i < 8; i++) {
      if (pts[i][1] <= 0 && -pts[i][1] <= thresh)   /* only the top half */
        fb_fill_rect(cx + pts[i][0], cy + pts[i][1], t, t, c);
    }
    y++;
    if (err < 0) err += 2 * y + 1;
    else { x--; err += 2 * (y - x) + 1; }
  }
}

/* the dock glyphs of MeepOS, redrawn as flat line art (icons only, nothing runs) */
static void meep_dock_glyph(int idx, int cx, int cy, int s, uint32_t ink) {
  int t = UI_SCALE_VAL(2);            /* stroke width, like SVG stroke-width 1.45 */
  int r1 = s / 2;
  switch (idx) {
  case 0: {                            /* chat bubble with a tail on the left */
    meep_box(cx - r1, cy - r1, s, s - UI_SCALE_VAL(5), UI_SCALE_VAL(5), ink,
             MEEP_PAPER2);
    gui_draw_line(cx - UI_SCALE_VAL(5), cy + r1 - UI_SCALE_VAL(5),
                  cx - UI_SCALE_VAL(5), cy + r1, ink);
    gui_draw_line(cx - UI_SCALE_VAL(5), cy + r1,
                  cx - UI_SCALE_VAL(1), cy + r1, ink);
    break;
  }
  case 1: {                            /* two stacked windows */
    meep_box(cx - r1, cy - r1, s - UI_SCALE_VAL(5), s - UI_SCALE_VAL(5),
             UI_SCALE_VAL(3), ink, MEEP_PAPER2);
    meep_box(cx - r1 + UI_SCALE_VAL(5), cy - r1 + UI_SCALE_VAL(5),
             s - UI_SCALE_VAL(5), s - UI_SCALE_VAL(5), UI_SCALE_VAL(3), ink,
             MEEP_PAPER2);
    break;
  }
  case 2:
  case 3: {                            /* globe: ring, equator, curved meridian */
    int rm = r1 - t;
    gui_draw_circle(cx, cy, r1, ink);
    gui_draw_circle(cx, cy, rm, MEEP_PAPER2);
    meep_hline(cx - rm, cy - t / 2, rm * 2, t, ink);          /* equator */
    for (int dy = -rm; dy <= rm; dy++) {                      /* meridian ellipse */
      int w2 = rm * rm - dy * dy;
      if (w2 < 0) continue;
      int xi = meep_isqrt(w2) * 45 / 100;
      fb_fill_rect(cx - xi - t / 2, cy + dy, t, 1, ink);
      fb_fill_rect(cx + xi - t / 2, cy + dy, t, 1, ink);
    }
    break;
  }
  case 4: {                            /* folder: a raised tab over a rounded body */
    int tx0 = cx - r1, ty0 = cy - r1 + UI_SCALE_VAL(1);
    int tw = UI_SCALE_VAL(9), th = UI_SCALE_VAL(6);
    fb_fill_rect(tx0, ty0, tw, t, ink);                    /* tab top */
    fb_fill_rect(tx0, ty0, t, th, ink);                    /* tab left */
    fb_fill_rect(tx0 + tw - t, ty0, t, th - t, ink);       /* tab right */
    meep_box(cx - r1, ty0 + th - t, s, s - UI_SCALE_VAL(6), UI_SCALE_VAL(3),
             ink, MEEP_PAPER2);
    break;
  }
  case 5: {                            /* terminal: frame + ">" prompt */
    meep_box(cx - r1, cy - r1, s, s, UI_SCALE_VAL(4), ink, MEEP_PAPER2);
    gui_draw_line(cx - UI_SCALE_VAL(5), cy - UI_SCALE_VAL(4), cx,
                  cy, ink);
    gui_draw_line(cx, cy, cx - UI_SCALE_VAL(5), cy + UI_SCALE_VAL(4), ink);
    gui_draw_line(cx + UI_SCALE_VAL(1), cy + UI_SCALE_VAL(4),
                  cx + UI_SCALE_VAL(5), cy + UI_SCALE_VAL(4), ink);
    break;
  }
  case 6: {                            /* gear: ring plus eight short teeth */
    int ro = r1 - UI_SCALE_VAL(1);
    int ri = UI_SCALE_VAL(4);
    gui_draw_circle(cx, cy, ro - UI_SCALE_VAL(2), ink);
    gui_draw_circle(cx, cy, ro - UI_SCALE_VAL(2) - t, MEEP_PAPER2);
    gui_draw_circle(cx, cy, ri, ink);
    gui_draw_circle(cx, cy, ri - t, MEEP_PAPER2);
    static const int dx[8] = {1000, 707, 0, -707, -1000, -707, 0, 707};
    static const int dy[8] = {0, 707, 1000, 707, 0, -707, -1000, -707};
    for (int i = 0; i < 8; i++)
      fb_fill_rect(cx + dx[i] * ro / 1000 - t / 2,
                   cy + dy[i] * ro / 1000 - t / 2, t, t, ink);
    break;
  }
  case 7: {                            /* sliders */
    int off[3] = {-UI_SCALE_VAL(5), 0, UI_SCALE_VAL(5)};
    int kx[3] = {-UI_SCALE_VAL(4), UI_SCALE_VAL(3), -UI_SCALE_VAL(1)};
    for (int i = 0; i < 3; i++) {
      meep_hline(cx - r1, cy + off[i], s, UI_SCALE_VAL(1), ink);
      fb_fill_rect(cx + kx[i] - UI_SCALE_VAL(1), cy + off[i] - UI_SCALE_VAL(2),
                   UI_SCALE_VAL(4), UI_SCALE_VAL(5), MEEP_PAPER2);
      gui_draw_circle(cx + kx[i], cy + off[i], UI_SCALE_VAL(2), ink);
    }
    break;
  }
  case 8: {                            /* wifi: two arcs and a dot */
    int by = cy + UI_SCALE_VAL(7);
    meep_arc_up(cx, by, UI_SCALE_VAL(12), UI_SCALE_VAL(9), t, ink);
    meep_arc_up(cx, by, UI_SCALE_VAL(7), UI_SCALE_VAL(5), t, ink);
    fb_fill_rect(cx - t / 2, by - UI_SCALE_VAL(3), t, t, ink);
    break;
  }
  default: {                           /* toggle: pill with the knob to the right */
    meep_box(cx - r1, cy - UI_SCALE_VAL(6), s, UI_SCALE_VAL(12),
             UI_SCALE_VAL(6), ink, MEEP_PAPER2);
    gui_draw_circle(cx + UI_SCALE_VAL(4), cy, UI_SCALE_VAL(3), ink);
    break;
  }
  }
}

/* ---- dock geometry, one source of truth (also used by the supersampler) ---- */
static void meep_dock_rect(int s, int *rx, int *ry, int *rw, int *rh) {
  int tile = 44 * s, pad = 7 * s, gap = 6 * s, brd = s, n = NUM_DOCK_ICONS;
  int sep_w = 7 * s;
  int dh = brd * 2 + pad * 2 + tile;
  int dw = brd * 2 + pad * 2 + n * tile + n * gap + sep_w;
  int x = ((int)screen_width - dw) / 2;
  x -= x % s;                       /* snap: keeps 1px strokes on whole pixels */
  *rx = x;
  *ry = (int)screen_height - 14 * s - dh;
  *rw = dw; *rh = dh;
}

static void draw_dock(void) {
  /* MeepOS dock, measured off the running web shell at 1280x800:
     outer 523x60 (1px border + 7px padding + 10 * 44px tiles + 6px gaps + a 1px
     separator with 3px side margins), 14px radius, centred, 14px off the bottom. */
  int tile = UI_SCALE_VAL(44);
  int pad  = UI_SCALE_VAL(7);
  int gap  = UI_SCALE_VAL(6);
  int brd  = UI_SCALE_VAL(1);
  int n    = NUM_DOCK_ICONS;                 /* 10 tiles, split 4 | 6 by the separator */
  int sep_margin = UI_SCALE_VAL(3);
  int sep_w = UI_SCALE_VAL(7);

  int x, y, dw, dh;
  meep_dock_rect(ui_scale, &x, &y, &dw, &dh);

  /* paper-2 at 88% over paper, flattened (the framebuffer has no alpha) */
  meep_box(x, y, dw, dh, UI_SCALE_VAL(14), MEEP_LINE, MEEP_PAPER2);

  int cx = x + brd + pad;
  int cy = y + brd + pad;
  for (int i = 0; i < n; i++) {
    if (i == 4) {                            /* separator between the two groups */
      fb_fill_rect(cx + sep_margin, cy + UI_SCALE_VAL(2), UI_SCALE_VAL(1),
                   tile - UI_SCALE_VAL(4), MEEP_LINE);
      cx += sep_w + gap;
    }
    if (i == press_dock)
      meep_box(cx, cy, tile, tile, UI_SCALE_VAL(11), MEEP_LINE, MEEP_PRESS);
    else if (i == hover_dock)
      meep_box(cx, cy, tile, tile, UI_SCALE_VAL(11), MEEP_LINE, MEEP_PAPER3);
    else
      meep_box(cx, cy, tile, tile, UI_SCALE_VAL(11), MEEP_LINE, MEEP_PAPER2);
    meep_dock_glyph(i, cx + tile / 2, cy + tile / 2, UI_SCALE_VAL(22), MEEP_INK);
    cx += tile + gap;
  }
}

/* ===================================================================== */
/* Context Menu                                                          */
/* ===================================================================== */

static void ctx_menu_clear(void) {
  ctx_menu.item_count = 0;
  ctx_menu.hover_index = -1;
}

static void ctx_menu_add(const char *label, int enabled) {
  if (ctx_menu.item_count >= 16)
    return;
  strncpy(ctx_menu.items[ctx_menu.item_count].label, label, 31);
  ctx_menu.items[ctx_menu.item_count].label[31] = '\0';
  ctx_menu.items[ctx_menu.item_count].enabled = enabled;
  ctx_menu.items[ctx_menu.item_count].separator = 0;
  ctx_menu.item_count++;
}

static void ctx_menu_add_separator(void) {
  if (ctx_menu.item_count >= 16)
    return;
  ctx_menu.items[ctx_menu.item_count].label[0] = '\0';
  ctx_menu.items[ctx_menu.item_count].separator = 1;
  ctx_menu.item_count++;
}

static void ctx_menu_show(int x, int y) {
  ctx_menu_clear();
  ctx_menu_add("Open", 1);
  ctx_menu_add("Open With...", 1);
  ctx_menu_add_separator();
  ctx_menu_add("New Folder", 1);
  ctx_menu_add("New File", 1);
  ctx_menu_add_separator();
  ctx_menu_add("Cut", 1);
  ctx_menu_add("Copy", 1);
  ctx_menu_add("Paste", 0);
  ctx_menu_add_separator();
  ctx_menu_add("Rename", 1);
  ctx_menu_add("Delete", 1);
  ctx_menu_add_separator();
  ctx_menu_add("Properties", 1);

  ctx_menu.x = x;
  ctx_menu.y = y;
  ctx_menu.width = UI_SCALE_VAL(160);
  ctx_menu.height = ctx_menu.item_count * UI_SCALE_VAL(24) + UI_SCALE_VAL(8);
  ctx_menu.visible = 1;

  /* Adjust if off screen */
  if (ctx_menu.x + ctx_menu.width > (int)screen_width) {
    ctx_menu.x = screen_width - ctx_menu.width - UI_SCALE_VAL(4);
  }
  if (ctx_menu.y + ctx_menu.height > (int)screen_height) {
    ctx_menu.y = screen_height - ctx_menu.height - UI_SCALE_VAL(4);
  }
}

static void draw_context_menu(void) {
  if (!ctx_menu.visible)
    return;

  int x = ctx_menu.x;
  int y = ctx_menu.y;
  int w = ctx_menu.width;
  int h = ctx_menu.height;

  /* Shadow */
  fb_fill_rect(x + UI_SCALE_VAL(4), y + UI_SCALE_VAL(4), w, h, 0x11111B);

  /* Background */
  gui_draw_rounded_rect(x, y, w, h, UI_SCALE_VAL(8), COLOR_CTX_MENU_BG);

  /* Items */
  int item_h = UI_SCALE_VAL(24);
  int item_y = y + UI_SCALE_VAL(4);
  for (int i = 0; i < ctx_menu.item_count; i++) {
    if (ctx_menu.items[i].separator) {
      fb_fill_rect(x + UI_SCALE_VAL(8), item_y + UI_SCALE_VAL(10), w - UI_SCALE_VAL(16), 1, THEME_OVERLAY);
      item_y += item_h;
      continue;
    }

    /* Hover highlight */
    if (i == ctx_menu.hover_index) {
      gui_draw_rounded_rect(x + UI_SCALE_VAL(4), item_y + UI_SCALE_VAL(2), w - UI_SCALE_VAL(8), UI_SCALE_VAL(20), UI_SCALE_VAL(4),
                            COLOR_CTX_MENU_HOVER);
    }

    uint32_t color = ctx_menu.items[i].enabled ? THEME_TEXT : THEME_SUBTEXT;
    gui_draw_string(x + UI_SCALE_VAL(12), item_y + UI_SCALE_VAL(4), ctx_menu.items[i].label, color);

    item_y += item_h;
  }
}

/* ===================================================================== */
/* Terminal                                                              */
/* ===================================================================== */

static void term_init(void) {
  int font_w = FONT_WIDTH * ui_scale;
  int font_h = FONT_HEIGHT * ui_scale;
  terminal.width = TERM_COLS * font_w + UI_SCALE_VAL(20);
  terminal.height = TERM_ROWS * font_h + UI_SCALE_VAL(40);
  terminal.content_x = (screen_width - terminal.width) / 2;
  terminal.content_y = (screen_height - terminal.height) / 2;
  terminal.cursor_x = 0;
  terminal.cursor_y = 0;
  terminal.input_len = 0;
  strcpy(terminal.cwd, "/");
  terminal.visible = 0;

  /* Clear screen */
  for (int r = 0; r < TERM_ROWS; r++) {
    for (int c = 0; c < TERM_COLS; c++) {
      terminal.chars[r][c] = ' ';
      terminal.colors[r][c] = 7;
    }
  }
}

static void term_scroll(void) {
  for (int r = 0; r < TERM_ROWS - 1; r++) {
    memcpy(terminal.chars[r], terminal.chars[r + 1], TERM_COLS);
    memcpy(terminal.colors[r], terminal.colors[r + 1], TERM_COLS);
  }
  for (int c = 0; c < TERM_COLS; c++) {
    terminal.chars[TERM_ROWS - 1][c] = ' ';
    terminal.colors[TERM_ROWS - 1][c] = 7;
  }
}

static void term_putc(char c, uint8_t color) {
  if (c == '\n') {
    terminal.cursor_x = 0;
    terminal.cursor_y++;
    if (terminal.cursor_y >= TERM_ROWS) {
      term_scroll();
      terminal.cursor_y = TERM_ROWS - 1;
    }
  } else if (c == '\r') {
    terminal.cursor_x = 0;
  } else {
    terminal.chars[terminal.cursor_y][terminal.cursor_x] = c;
    terminal.colors[terminal.cursor_y][terminal.cursor_x] = color;
    terminal.cursor_x++;
    if (terminal.cursor_x >= TERM_COLS) {
      terminal.cursor_x = 0;
      terminal.cursor_y++;
      if (terminal.cursor_y >= TERM_ROWS) {
        term_scroll();
        terminal.cursor_y = TERM_ROWS - 1;
      }
    }
  }
}

static void term_puts(const char *str, uint8_t color) {
  while (*str) {
    term_putc(*str++, color);
  }
}

static void term_prompt(void) {
  term_puts("meepos", 10); /* Green */
  term_puts(":", 7);       /* White */
  term_puts("~", 12);      /* Blue */
  term_puts("$ ", 7);      /* White */
}

static void term_execute(const char *cmd) {
  term_putc('\n', 7);

  if (strcmp(cmd, "ls") == 0) {
    /* List files in current directory */
    for (int i = 0; sim_files[i].path != NULL; i++) {
      if (strcmp(sim_files[i].path, terminal.cwd) == 0) {
        if (sim_files[i].is_dir) {
          term_puts(sim_files[i].name, 12); /* Blue */
        } else {
          term_puts(sim_files[i].name, 7);
        }
        term_puts("  ", 7);
      }
    }
    term_putc('\n', 7);
  } else if (strcmp(cmd, "pwd") == 0) {
    term_puts(terminal.cwd, 7);
    term_putc('\n', 7);
  } else if (strcmp(cmd, "clear") == 0) {
    for (int r = 0; r < TERM_ROWS; r++) {
      for (int c = 0; c < TERM_COLS; c++) {
        terminal.chars[r][c] = ' ';
      }
    }
    terminal.cursor_x = 0;
    terminal.cursor_y = 0;
  } else if (strcmp(cmd, "help") == 0) {
    term_puts("Omi Terminal v2.0\n", 14);
    term_puts("File Commands:\n", 11);
    term_puts("  ls        - List directory contents\n", 7);
    term_puts("  cd <dir>  - Change directory\n", 7);
    term_puts("  pwd       - Print working directory\n", 7);
    term_puts("  cat <f>   - Display file contents\n", 7);
    term_puts("System:\n", 11);
    term_puts("  neofetch  - System info\n", 7);
    term_puts("  uname     - Show OS info\n", 7);
    term_puts("  id        - Show user/group info\n", 7);
    term_puts("  hostname  - Show hostname\n", 7);
    term_puts("  free      - Memory usage\n", 7);
    term_puts("  ps        - Process list\n", 7);
    term_puts("  clear     - Clear screen\n", 7);
    term_puts("  help      - This help message\n", 7);
  } else if (strcmp(cmd, "uname") == 0 || strcmp(cmd, "uname -a") == 0) {
    term_puts("Omi 1.0.0 x86_64\n", 11);
  } else if (strcmp(cmd, "neofetch") == 0) {
    term_puts("       _  _         ___  ____  \n", 14);
    term_puts(" __   _(_)| |__     / _ \\/ ___| \n", 14);
    term_puts(" \\ \\ / / || '_ \\   | | | \\___ \\ \n", 14);
    term_puts("  \\ V /| || |_) |  | |_| |___) |\n", 14);
    term_puts("   \\_/ |_||_.__/    \\___/|____/ \n", 14);
    term_puts("\n", 7);
    term_puts("OS:      ", 11);
    term_puts("Omi 1.0.0\n", 7);
    term_puts("Host:    ", 11);
    term_puts("QEMU x86_64 Virtual Machine\n", 7);
    term_puts("Kernel:  ", 11);
    term_puts("1.0.0-x86_64\n", 7);
    term_puts("Uptime:  ", 11);
    term_puts("0 mins\n", 7);
    term_puts("Shell:   ", 11);
    term_puts("vsh 1.0\n", 7);
    term_puts("Memory:  ", 11);
    term_puts("12 MB / 512 MB\n", 7);
    term_puts("CPU:     ", 11);
    term_puts("QEMU Virtual CPU\n", 7);
  } else if (strcmp(cmd, "id") == 0) {
    term_puts("uid=0(root) gid=0(root) groups=0(root)\n", 7);
  } else if (strcmp(cmd, "hostname") == 0) {
    term_puts("meepos\n", 7);
  } else if (strcmp(cmd, "free") == 0) {
    term_puts("              total        used        free\n", 7);
    term_puts("Mem:         512 MB       12 MB      500 MB\n", 7);
    term_puts("Swap:          0 MB        0 MB        0 MB\n", 7);
  } else if (strcmp(cmd, "ps") == 0) {
    term_puts("  PID TTY          TIME CMD\n", 7);
    term_puts("    1 ?        00:00:00 init\n", 7);
    term_puts("    2 ?        00:00:00 kthread\n", 7);
    term_puts("   10 tty1     00:00:00 shell\n", 7);
  } else if (strcmp(cmd, "whoami") == 0) {
    term_puts("root\n", 7);
  } else if (strncmp(cmd, "cd ", 3) == 0) {
    const char *dir = cmd + 3;
    while (*dir == ' ')
      dir++;
    if (strcmp(dir, "..") == 0) {
      /* Go up */
      char *last = terminal.cwd + strlen(terminal.cwd) - 1;
      while (last > terminal.cwd && *last != '/')
        last--;
      if (last > terminal.cwd)
        *last = '\0';
      else
        strcpy(terminal.cwd, "/");
    } else if (strcmp(dir, "/") == 0) {
      strcpy(terminal.cwd, "/");
    } else {
      /* Check if dir exists */
      int found = 0;
      for (int i = 0; sim_files[i].path != NULL; i++) {
        if (strcmp(sim_files[i].path, terminal.cwd) == 0 &&
            strcmp(sim_files[i].name, dir) == 0 && sim_files[i].is_dir) {
          found = 1;
          if (strcmp(terminal.cwd, "/") == 0) {
            strcpy(terminal.cwd + 1, dir);
          } else {
            strcat(terminal.cwd, "/");
            strcat(terminal.cwd, dir);
          }
          break;
        }
      }
      if (!found) {
        term_puts("cd: no such directory: ", 9);
        term_puts(dir, 9);
        term_putc('\n', 7);
      }
    }
  } else if (strlen(cmd) > 0) {
    term_puts("command not found: ", 9);
    term_puts(cmd, 9);
    term_putc('\n', 7);
  }

  term_prompt();
}

static void draw_terminal(void) {
  if (!terminal.visible)
    return;

  int x = terminal.content_x;
  int y = terminal.content_y;
  int w = terminal.width;
  int h = terminal.height;
  int title_h = UI_SCALE_VAL(32);
  int font_w = FONT_WIDTH * ui_scale;
  int font_h = FONT_HEIGHT * ui_scale;

  /* Window shadow */
  fb_fill_rect(x + UI_SCALE_VAL(6), y + UI_SCALE_VAL(6), w, h, 0x11111B);

  /* Window background */
  fb_fill_rect(x, y, w, h, COLOR_TERM_BG);

  /* Title bar */
  fb_fill_rect(x, y, w, title_h, COLOR_TITLE_BAR);

  /* Traffic lights with icons */
  draw_traffic_lights(x, y, title_h);

  /* Title */
  gui_draw_string(x + w / 2 - (font_string_width("Terminal") / 2),
                  y + (title_h - font_h) / 2, "Terminal", THEME_TEXT);

  /* Terminal content */
  int tx = x + UI_SCALE_VAL(10);
  int ty = y + title_h + UI_SCALE_VAL(6);

  for (int r = 0; r < TERM_ROWS; r++) {
    for (int c = 0; c < TERM_COLS; c++) {
      char ch = terminal.chars[r][c];
      if (ch != ' ') {
        uint32_t color = THEME_TEXT;
        switch (terminal.colors[r][c]) {
        case 9:
          color = THEME_RED;
          break;
        case 10:
          color = THEME_GREEN;
          break;
        case 11:
          color = THEME_YELLOW;
          break;
        case 12:
          color = THEME_BLUE;
          break;
        case 13:
          color = THEME_MAUVE;
          break;
        case 14:
          color = THEME_SAPPHIRE;
          break;
        }
        gui_draw_char(tx + c * font_w, ty + r * font_h, ch, color);
      }
    }
  }

  /* Input buffer display - draw starting at cursor position */
  for (int i = 0; i < terminal.input_len; i++) {
    int cx = terminal.cursor_x + i;
    if (cx >= 0 && cx < TERM_COLS) {
      gui_draw_char(tx + cx * font_w, ty + terminal.cursor_y * font_h,
                    terminal.input[i],
                    THEME_TEXT);
    }
  }

  /* Cursor - blinks after the input text */
  static int cursor_blink = 0;
  cursor_blink++;
  int cursor_draw_x = terminal.cursor_x + terminal.input_len;
  if (cursor_draw_x < TERM_COLS && (cursor_blink / 15) % 2 == 0) {
    fb_fill_rect(tx + cursor_draw_x * font_w,
                 ty + terminal.cursor_y * font_h + (font_h - UI_SCALE_VAL(2)),
                 font_w, UI_SCALE_VAL(2), THEME_TEXT);
  }
}

/* ===================================================================== */
/* File Manager                                                          */
/* ===================================================================== */

static void fm_init(void) {
  file_manager.width = 450;
  file_manager.height = 350;
  file_manager.x = 80;
  file_manager.y = 50;
  strcpy(file_manager.current_path, "/");
  file_manager.selected = -1;
  file_manager.visible = 0;
}

static void fm_refresh(void) {
  file_manager.entry_count = 0;

  /* Add parent directory */
  if (strcmp(file_manager.current_path, "/") != 0) {
    strcpy(file_manager.entries[0].name, "..");
    file_manager.entries[0].is_dir = 1;
    file_manager.entry_count = 1;
  }

  /* Add entries for current path */
  for (int i = 0; sim_files[i].path != NULL && file_manager.entry_count < 32;
       i++) {
    if (strcmp(sim_files[i].path, file_manager.current_path) == 0) {
      strcpy(file_manager.entries[file_manager.entry_count].name,
             sim_files[i].name);
      file_manager.entries[file_manager.entry_count].is_dir =
          sim_files[i].is_dir;
      file_manager.entry_count++;
    }
  }
}

static void draw_file_manager(void) {
  if (!file_manager.visible)
    return;

  int x = file_manager.x;
  int y = file_manager.y;
  int w = file_manager.width;
  int h = file_manager.height;
  int title_h = UI_SCALE_VAL(32);
  int toolbar_h = UI_SCALE_VAL(40);

  /* Window shadow */
  fb_fill_rect(x + UI_SCALE_VAL(6), y + UI_SCALE_VAL(6), w, h, 0x11111B);

  /* Window background */
  fb_fill_rect(x, y, w, h, THEME_BASE);

  /* Title bar */
  fb_fill_rect(x, y, w, title_h, COLOR_TITLE_BAR);

  /* Traffic lights with icons */
  draw_traffic_lights(x, y, title_h);

  /* Title */
  gui_draw_string(x + UI_SCALE_VAL(80),
                  y + (title_h - FONT_HEIGHT * ui_scale) / 2,
                  "File Manager", THEME_TEXT);

  /* Toolbar */
  fb_fill_rect(x, y + title_h, w, toolbar_h, 0x2A2A35);
  fb_fill_rect(x, y + title_h + toolbar_h - 1, w, UI_SCALE_VAL(1), 0x404050);
  
  /* Back button */
  fb_fill_rect(x + UI_SCALE_VAL(10), y + title_h + UI_SCALE_VAL(8),
               UI_SCALE_VAL(60), UI_SCALE_VAL(24), 0x404050);
  gui_draw_string(x + UI_SCALE_VAL(22), y + title_h + UI_SCALE_VAL(12),
                  "Back", THEME_TEXT);
  
  /* New Folder button */
  fb_fill_rect(x + UI_SCALE_VAL(80), y + title_h + UI_SCALE_VAL(8),
               UI_SCALE_VAL(90), UI_SCALE_VAL(24), 0x404050);
  gui_draw_string(x + UI_SCALE_VAL(88), y + title_h + UI_SCALE_VAL(12),
                  "New Folder", THEME_TEXT);
  
  /* New File button */
  fb_fill_rect(x + UI_SCALE_VAL(180), y + title_h + UI_SCALE_VAL(8),
               UI_SCALE_VAL(80), UI_SCALE_VAL(24), 0x404050);
  gui_draw_string(x + UI_SCALE_VAL(190), y + title_h + UI_SCALE_VAL(12),
                  "New File", THEME_TEXT);

  /* Location bar */
  int loc_y = y + title_h + toolbar_h + UI_SCALE_VAL(2);
  gui_draw_string(x + UI_SCALE_VAL(10), loc_y + UI_SCALE_VAL(4),
                  "Location:", THEME_SUBTEXT);
  gui_draw_string(x + UI_SCALE_VAL(90), loc_y + UI_SCALE_VAL(4),
                  file_manager.current_path, THEME_TEXT);

  /* File list - grid style */
  int list_y = loc_y + UI_SCALE_VAL(24);
  int icon_size = UI_SCALE_VAL(48);
  int item_w = UI_SCALE_VAL(90);  /* Wider items for text */
  int item_h = UI_SCALE_VAL(80);  /* Taller items for spacing */
  int padding = UI_SCALE_VAL(20);
  int cols = (w - padding) / item_w;
  if (cols < 1) cols = 1;
  if (cols > 6) cols = 6;  /* Limit columns */
  
  for (int i = 0; i < file_manager.entry_count; i++) {
    int col = i % cols;
    int row = i / cols;
    int ix = x + UI_SCALE_VAL(10) + col * item_w;
    int iy = list_y + row * item_h;
    
    if (iy > y + h - 30)
      break;

    /* Selection highlight */
    if (i == file_manager.selected) {
      gui_draw_rounded_rect(ix, iy, item_w - UI_SCALE_VAL(8),
                            item_h - UI_SCALE_VAL(8), UI_SCALE_VAL(6),
                            THEME_OVERLAY);
    }

    /* Icon - centered */
    int icon_x = ix + (item_w - UI_SCALE_VAL(8) - UI_SCALE_VAL(32)) / 2;
    int icon_y = iy + UI_SCALE_VAL(4);
    
    if (file_manager.entries[i].is_dir) {
      /* Folder icon - larger */
      fb_fill_rect(icon_x, icon_y + UI_SCALE_VAL(8),
                   UI_SCALE_VAL(32), UI_SCALE_VAL(24), THEME_YELLOW);
      fb_fill_rect(icon_x, icon_y + UI_SCALE_VAL(4),
                   UI_SCALE_VAL(14), UI_SCALE_VAL(6), THEME_PEACH);
    } else {
      /* File icon */
      fb_fill_rect(icon_x + UI_SCALE_VAL(4), icon_y + UI_SCALE_VAL(2),
                   UI_SCALE_VAL(24), UI_SCALE_VAL(30), THEME_ROSEWATER);
      /* Lines on file */
      fb_fill_rect(icon_x + UI_SCALE_VAL(8), icon_y + UI_SCALE_VAL(10),
                   UI_SCALE_VAL(16), UI_SCALE_VAL(2), THEME_BASE);
      fb_fill_rect(icon_x + UI_SCALE_VAL(8), icon_y + UI_SCALE_VAL(16),
                   UI_SCALE_VAL(16), UI_SCALE_VAL(2), THEME_BASE);
      fb_fill_rect(icon_x + UI_SCALE_VAL(8), icon_y + UI_SCALE_VAL(22),
                   UI_SCALE_VAL(12), UI_SCALE_VAL(2), THEME_BASE);
    }

    /* Name - centered below icon, truncated to fit */
    char name_buf[12];
    int name_len = strlen(file_manager.entries[i].name);
    int max_chars = (item_w - UI_SCALE_VAL(10)) / (FONT_WIDTH * ui_scale);
    if (max_chars > 10) max_chars = 10;
    if (max_chars < 3) max_chars = 3;
    
    if (name_len > max_chars) {
      memcpy(name_buf, file_manager.entries[i].name, max_chars - 2);
      name_buf[max_chars - 2] = '.';
      name_buf[max_chars - 1] = '.';
      name_buf[max_chars] = '\0';
    } else {
      strcpy(name_buf, file_manager.entries[i].name);
    }
    int text_w = font_string_width(name_buf);
    int text_x = ix + (item_w - UI_SCALE_VAL(10) - text_w) / 2;
    if (text_x < ix) text_x = ix;
    uint32_t color = file_manager.entries[i].is_dir ? THEME_BLUE : THEME_TEXT;
    gui_draw_string(text_x, iy + UI_SCALE_VAL(50), name_buf, color);
  }
}

/* ===================================================================== */
/* Image Viewer                                                          */
/* ===================================================================== */

#include "toolbar_icons.h"

typedef struct {
  int visible;
  int x, y, width, height;
  int img_width, img_height;
  int rotation;      /* 0, 90, 180, 270 */
  int zoom;          /* Percentage 10-400, 0=fit */
  int offset_x, offset_y;
  int current_image; /* Index in image list */
  int fullscreen;
  char current_path[256];
  int hover_button;  /* For toolbar hover effect */
} image_viewer_t;

static image_viewer_t img_viewer = {0, 200, 100, 600, 450, 0, 0, 0, 0, 0, 0, 0, 0, "", -1};

/* Sample images (for demo) */
static const char *sample_images[] = {
  "landscape.jpg", "nature.jpg", "city.jpg", "wallpaper.jpg", NULL
};

static void draw_toolbar_icon(int x, int y, int icon_idx, int hover) {
  /* Button background */
  int btn_size = UI_SCALE_VAL(36);
  uint32_t btn_color = hover ? 0x4B5563 : 0x374151;
  gui_draw_rounded_rect(x, y, btn_size, btn_size, UI_SCALE_VAL(4), btn_color);
  
  /* Draw icon (24x24 scaled and centered in button) */
  if (icon_idx >= 0 && icon_idx < 8) {
    const uint32_t *icon = toolbar_icons[icon_idx];
    int icon_draw_size = UI_SCALE_VAL(TOOLBAR_ICON_SIZE);
    int ix = x + (btn_size - icon_draw_size) / 2;
    int iy = y + (btn_size - icon_draw_size) / 2;
    
    /* Scale the icon */
    for (int py = 0; py < icon_draw_size; py++) {
      int src_y = py * TOOLBAR_ICON_SIZE / icon_draw_size;
      for (int px = 0; px < icon_draw_size; px++) {
        int src_x = px * TOOLBAR_ICON_SIZE / icon_draw_size;
        uint32_t pixel = icon[src_y * TOOLBAR_ICON_SIZE + src_x];
        if (pixel != 0) {
          fb_put_pixel(ix + px, iy + py, 0xE5E7EB);
        }
      }
    }
  }
}

static void draw_image_viewer(void) {
  if (!img_viewer.visible) return;
  
  int x = img_viewer.x;
  int y = img_viewer.y;
  int w = img_viewer.width;
  int h = img_viewer.height;
  int title_h = UI_SCALE_VAL(32);
  int font_h = FONT_HEIGHT * ui_scale;
  
  /* Window shadow */
  fb_fill_rect(x + UI_SCALE_VAL(6), y + UI_SCALE_VAL(6), w, h, 0x11111B);
  
  /* Window background - dark for image viewing */
  fb_fill_rect(x, y, w, h, 0x0D0D0D);
  
  /* Title bar */
  fb_fill_rect(x, y, w, title_h, COLOR_TITLE_BAR);
  
  /* Traffic lights with icons */
  draw_traffic_lights(x, y, title_h);
  
  /* Title */
  char title[64] = "Image Viewer";
  if (sample_images[img_viewer.current_image]) {
    snprintf(title, 64, "Image Viewer - %s", sample_images[img_viewer.current_image]);
  }
  gui_draw_string(x + UI_SCALE_VAL(80), y + (title_h - font_h) / 2, title, THEME_TEXT);
  
  /* Image content area */
  int toolbar_h = UI_SCALE_VAL(48);
  int content_y = y + title_h;
  int content_h = h - title_h - toolbar_h - UI_SCALE_VAL(16);
  
  /* Draw placeholder image representation */
  int img_area_x = x + UI_SCALE_VAL(20);
  int img_area_y = content_y + UI_SCALE_VAL(20);
  int img_area_w = w - UI_SCALE_VAL(40);
  int img_area_h = content_h - UI_SCALE_VAL(40);
  
  /* Image placeholder - draw a nice gradient/pattern */
  for (int py = 0; py < img_area_h; py++) {
    for (int px = 0; px < img_area_w; px++) {
      int real_x = img_area_x + px;
      int real_y = img_area_y + py;
      
      /* Create a nice gradient pattern to represent image */
      int r = 30 + (py * 50 / img_area_h);
      int g = 40 + (px * 30 / img_area_w);
      int b = 60 + ((px + py) * 20 / (img_area_w + img_area_h));
      
      fb_put_pixel(real_x, real_y, (r << 16) | (g << 8) | b);
    }
  }
  
  /* Draw image info */
  char info[64];
  snprintf(info, 64, "%d x %d  |  %d%%  |  %d deg", 
           800, 600, img_viewer.zoom ? img_viewer.zoom : 100, img_viewer.rotation);
  int info_w = font_string_width(info);
  int info_h = UI_SCALE_VAL(20);
  fb_fill_rect(x + (w - info_w - UI_SCALE_VAL(16)) / 2, content_y + content_h - UI_SCALE_VAL(30), info_w + UI_SCALE_VAL(16), info_h, 0x1F2937);
  gui_draw_string(x + (w - info_w) / 2, content_y + content_h - UI_SCALE_VAL(30) + (info_h - font_h) / 2, info, 0x9CA3AF);
  
  /* Floating toolbar at bottom */
  int toolbar_w = UI_SCALE_VAL(520);
  int toolbar_x = x + (w - toolbar_w) / 2;
  int toolbar_y = y + h - toolbar_h - UI_SCALE_VAL(8);
  
  /* Toolbar background with glassmorphism effect */
  fb_fill_rect(toolbar_x, toolbar_y, toolbar_w, toolbar_h, 0x1F1F28);
  fb_fill_rect(toolbar_x, toolbar_y, toolbar_w, 1, 0x3F3F48);
  gui_draw_rounded_rect(toolbar_x, toolbar_y, toolbar_w, toolbar_h, UI_SCALE_VAL(8), 0x252530);
  
  /* Toolbar buttons */
  int btn_size = UI_SCALE_VAL(36);
  int btn_spacing = UI_SCALE_VAL(8);
  int btn_x = toolbar_x + UI_SCALE_VAL(12);
  int btn_y = toolbar_y + UI_SCALE_VAL(6);
  
  /* Previous */
  draw_toolbar_icon(btn_x, btn_y, TOOLBAR_ICON_PREV, img_viewer.hover_button == 0);
  btn_x += btn_size + btn_spacing;
  
  /* Next */
  draw_toolbar_icon(btn_x, btn_y, TOOLBAR_ICON_NEXT, img_viewer.hover_button == 1);
  btn_x += btn_size + btn_spacing + UI_SCALE_VAL(16); /* Extra gap */
  
  /* Rotate CW */
  draw_toolbar_icon(btn_x, btn_y, TOOLBAR_ICON_ROTATE_CW, img_viewer.hover_button == 2);
  btn_x += btn_size + btn_spacing;
  
  /* Rotate CCW */
  draw_toolbar_icon(btn_x, btn_y, TOOLBAR_ICON_ROTATE_CCW, img_viewer.hover_button == 3);
  btn_x += btn_size + btn_spacing + UI_SCALE_VAL(16);
  
  /* Zoom In */
  draw_toolbar_icon(btn_x, btn_y, TOOLBAR_ICON_ZOOM_IN, img_viewer.hover_button == 4);
  btn_x += btn_size + btn_spacing;
  
  /* Zoom Out */
  draw_toolbar_icon(btn_x, btn_y, TOOLBAR_ICON_ZOOM_OUT, img_viewer.hover_button == 5);
  btn_x += btn_size + btn_spacing + UI_SCALE_VAL(16);
  
  /* Fit */
  draw_toolbar_icon(btn_x, btn_y, TOOLBAR_ICON_FIT, img_viewer.hover_button == 6);
  btn_x += btn_size + btn_spacing;
  
  /* Fullscreen */
  draw_toolbar_icon(btn_x, btn_y, TOOLBAR_ICON_FULLSCREEN, img_viewer.hover_button == 7);
}

static void image_viewer_next(void) {
  img_viewer.current_image++;
  if (sample_images[img_viewer.current_image] == NULL) {
    img_viewer.current_image = 0;
  }
}

static void image_viewer_prev(void) {
  if (img_viewer.current_image > 0) {
    img_viewer.current_image--;
  } else {
    /* Find last image */
    int i = 0;
    while (sample_images[i] != NULL) i++;
    img_viewer.current_image = i > 0 ? i - 1 : 0;
  }
}

static void image_viewer_rotate_cw(void) {
  img_viewer.rotation = (img_viewer.rotation + 90) % 360;
}

static void image_viewer_rotate_ccw(void) {
  img_viewer.rotation = (img_viewer.rotation + 270) % 360;
}

static void image_viewer_zoom_in(void) {
  if (img_viewer.zoom == 0) img_viewer.zoom = 100;
  img_viewer.zoom += 25;
  if (img_viewer.zoom > 400) img_viewer.zoom = 400;
}

static void image_viewer_zoom_out(void) {
  if (img_viewer.zoom == 0) img_viewer.zoom = 100;
  img_viewer.zoom -= 25;
  if (img_viewer.zoom < 10) img_viewer.zoom = 10;
}

static void image_viewer_fit(void) {
  img_viewer.zoom = 0;
  img_viewer.offset_x = 0;
  img_viewer.offset_y = 0;
}

/* ===================================================================== */
/* Calculator                                                            */
/* ===================================================================== */

typedef struct {
  int visible;
  int x, y, width, height;
  long display;
  long pending;
  char op;
  int clear_next;
} calculator_t;

static calculator_t calc = {0, 650, 100, 200, 300, 0, 0, 0, 0};

static void calc_button_click(char key) {
  if (key >= '0' && key <= '9') {
    int digit = key - '0';
    if (calc.clear_next) {
      calc.display = digit;
      calc.clear_next = 0;
    } else {
      calc.display = calc.display * 10 + digit;
    }
  } else if (key == 'C') {
    calc.display = 0;
    calc.pending = 0;
    calc.op = 0;
    calc.clear_next = 0;
  } else if (key == '=') {
    if (calc.op == '+')
      calc.display = calc.pending + calc.display;
    else if (calc.op == '-')
      calc.display = calc.pending - calc.display;
    else if (calc.op == '*')
      calc.display = calc.pending * calc.display;
    else if (calc.op == '/' && calc.display != 0)
      calc.display = calc.pending / calc.display;
    calc.op = 0;
    calc.clear_next = 1;
  } else if (key == '+' || key == '-' || key == '*' || key == '/') {
    calc.pending = calc.display;
    calc.op = key;
    calc.clear_next = 1;
  }
}

static void draw_calculator(void) {
  if (!calc.visible)
    return;

  int x = calc.x;
  int y = calc.y;
  int w = calc.width;
  int h = calc.height;
  int title_h = UI_SCALE_VAL(32);

  /* Window shadow */
  fb_fill_rect(x + UI_SCALE_VAL(6), y + UI_SCALE_VAL(6), w, h, 0x11111B);

  /* Window background */
  fb_fill_rect(x, y, w, h, THEME_BASE);

  /* Title bar */
  fb_fill_rect(x, y, w, title_h, COLOR_TITLE_BAR);
  draw_traffic_lights(x, y, title_h);
  gui_draw_string(x + w / 2 - font_string_width("Calculator") / 2, y + (title_h - FONT_HEIGHT * ui_scale) / 2, "Calculator", THEME_TEXT);

  /* Display */
  int disp_h = UI_SCALE_VAL(40);
  fb_fill_rect(x + UI_SCALE_VAL(10), y + title_h + UI_SCALE_VAL(10), w - UI_SCALE_VAL(20), disp_h, 0x2A2A30);
  char display_buf[20];
  itoa(calc.display, display_buf, 10);
  int disp_text_w = font_string_width(display_buf);
  gui_draw_string(x + w - UI_SCALE_VAL(20) - disp_text_w, y + title_h + UI_SCALE_VAL(10) + (disp_h - FONT_HEIGHT * ui_scale) / 2, display_buf, THEME_TEXT);

  /* Buttons (4x5 grid) */
  static const char buttons[] = "C+-/789*456-123+0.=";
  int btn_w = UI_SCALE_VAL(40), btn_h = UI_SCALE_VAL(35);
  int btn_pad = UI_SCALE_VAL(8);
  int start_x = x + UI_SCALE_VAL(15);
  int start_y = y + title_h + disp_h + UI_SCALE_VAL(25);

  for (int row = 0; row < 5; row++) {
    for (int col = 0; col < 4; col++) {
      int idx = row * 4 + col;
      if (idx >= 19)
        break;

      int bx = start_x + col * (btn_w + btn_pad);
      int by = start_y + row * (btn_h + btn_pad);

      /* Button background */
      uint32_t btn_color = THEME_SURFACE;
      char c = buttons[idx];
      if (c >= '0' && c <= '9')
        btn_color = THEME_OVERLAY;
      else if (c == '=')
        btn_color = THEME_BLUE;
      else if (c == 'C')
        btn_color = THEME_RED;

      gui_draw_rounded_rect(bx, by, btn_w, btn_h, UI_SCALE_VAL(4), btn_color);

      /* Button label */
      gui_draw_char(bx + (btn_w - FONT_WIDTH * ui_scale) / 2, by + (btn_h - FONT_HEIGHT * ui_scale) / 2, c, THEME_TEXT);
    }
  }
}

/* ===================================================================== */
/* Snake Game                                                            */
/* ===================================================================== */

#define SNAKE_MAX_LEN 100
#define SNAKE_GRID_W 20
#define SNAKE_GRID_H 15

typedef struct {
  int visible;
  int x, y, width, height;
  int snake_x[SNAKE_MAX_LEN];
  int snake_y[SNAKE_MAX_LEN];
  int len;
  int dir;
  int food_x, food_y;
  int score;
  int game_over;
  int tick;
} snake_game_t;

static snake_game_t snake = {0};

static void snake_init(void) {
  /* Only set position/size if not already set by gui_init */
  if (snake.width == 0) {
    snake.x = UI_SCALE_VAL(100);
    snake.y = UI_SCALE_VAL(100);
    snake.width = UI_SCALE_VAL(360);
    snake.height = UI_SCALE_VAL(320);
  }
  /* Reset game state */
  snake.len = 4;
  snake.dir = 1;
  snake.score = 0;
  snake.game_over = 0;
  snake.tick = 0;

  for (int i = 0; i < snake.len; i++) {
    snake.snake_x[i] = 10 - i;
    snake.snake_y[i] = 7;
  }
  snake.food_x = 15;
  snake.food_y = 7;
}

static void snake_move(void) {
  if (snake.game_over || !snake.visible)
    return;

  int new_x = snake.snake_x[0];
  int new_y = snake.snake_y[0];

  switch (snake.dir) {
  case 0:
    new_y--;
    break;
  case 1:
    new_x++;
    break;
  case 2:
    new_y++;
    break;
  case 3:
    new_x--;
    break;
  }

  /* Wrap around */
  if (new_x < 0)
    new_x = SNAKE_GRID_W - 1;
  if (new_x >= SNAKE_GRID_W)
    new_x = 0;
  if (new_y < 0)
    new_y = SNAKE_GRID_H - 1;
  if (new_y >= SNAKE_GRID_H)
    new_y = 0;

  /* Check self-collision */
  for (int i = 0; i < snake.len; i++) {
    if (snake.snake_x[i] == new_x && snake.snake_y[i] == new_y) {
      snake.game_over = 1;
      return;
    }
  }

  /* Check food */
  if (new_x == snake.food_x && new_y == snake.food_y) {
    snake.score += 10;
    if (snake.len < SNAKE_MAX_LEN - 1)
      snake.len++;
    snake.food_x = (snake.food_x * 7 + 3) % SNAKE_GRID_W;
    snake.food_y = (snake.food_y * 5 + 7) % SNAKE_GRID_H;
  }

  /* Move body */
  for (int i = snake.len - 1; i > 0; i--) {
    snake.snake_x[i] = snake.snake_x[i - 1];
    snake.snake_y[i] = snake.snake_y[i - 1];
  }
  snake.snake_x[0] = new_x;
  snake.snake_y[0] = new_y;
}

static void draw_snake_game(void) {
  if (!snake.visible)
    return;

  int x = snake.x;
  int y = snake.y;
  int w = snake.width;
  int h = snake.height;
  int title_h = UI_SCALE_VAL(32);

  /* Update game every few frames */
  snake.tick++;
  if (snake.tick >= 8) {
    snake.tick = 0;
    snake_move();
  }

  /* Window shadow */
  fb_fill_rect(x + UI_SCALE_VAL(6), y + UI_SCALE_VAL(6), w, h, 0x11111B);

  /* Window background */
  fb_fill_rect(x, y, w, h, THEME_BASE);

  /* Title bar */
  fb_fill_rect(x, y, w, title_h, COLOR_TITLE_BAR);
  draw_traffic_lights(x, y, title_h);
  gui_draw_string(x + w / 2 - font_string_width("Snake") / 2, y + (title_h - FONT_HEIGHT * ui_scale) / 2, "Snake", THEME_TEXT);

  /* Score */
  char score_buf[20] = "Score: ";
  itoa(snake.score, score_buf + 7, 10);
  gui_draw_string(x + UI_SCALE_VAL(10), y + title_h + UI_SCALE_VAL(8), score_buf, THEME_YELLOW);

  /* Game area */
  int game_x = x + UI_SCALE_VAL(10);
  int game_y = y + title_h + UI_SCALE_VAL(28);
  int game_area_w = w - UI_SCALE_VAL(20);
  int game_area_h = h - title_h - UI_SCALE_VAL(48);
  int cell_w = game_area_w / SNAKE_GRID_W;
  int cell_h = game_area_h / SNAKE_GRID_H;

  fb_fill_rect(game_x, game_y, SNAKE_GRID_W * cell_w, SNAKE_GRID_H * cell_h,
               0x181820);

  /* Draw food */
  fb_fill_rect(game_x + snake.food_x * cell_w + 2,
               game_y + snake.food_y * cell_h + 2, cell_w - 4, cell_h - 4,
               THEME_RED);

  /* Draw snake */
  for (int i = 0; i < snake.len; i++) {
    uint32_t color = (i == 0) ? THEME_TEAL : THEME_GREEN;
    fb_fill_rect(game_x + snake.snake_x[i] * cell_w + 1,
                 game_y + snake.snake_y[i] * cell_h + 1, cell_w - 2, cell_h - 2,
                 color);
  }

  /* Game over */
  if (snake.game_over) {
    gui_draw_string(x + w / 2 - font_string_width("GAME OVER!") / 2, y + h / 2, "GAME OVER!", THEME_RED);
    gui_draw_string(x + w / 2 - font_string_width("Press R to restart") / 2, y + h / 2 + UI_SCALE_VAL(20), "Press R to restart",
                    THEME_SUBTEXT);
  }
}

/* ===================================================================== */
/* Notepad                                                               */
/* ===================================================================== */

#define NOTEPAD_MAX_TEXT 2048

typedef struct {
  int visible;
  int x, y, width, height;
  char text[NOTEPAD_MAX_TEXT];
  int cursor;
} notepad_t;

static notepad_t notepad = {0, 100, 80, 400, 350, "", 0};

static void notepad_key(int key) {
  if (!notepad.visible)
    return;

  if (key == '\b' || key == 127) {
    /* Backspace */
    if (notepad.cursor > 0) {
      notepad.cursor--;
      notepad.text[notepad.cursor] = '\0';
    }
  } else if (key == '\n' || key == '\r') {
    /* Newline */
    if (notepad.cursor < NOTEPAD_MAX_TEXT - 1) {
      notepad.text[notepad.cursor++] = '\n';
      notepad.text[notepad.cursor] = '\0';
    }
  } else if (key >= 32 && key < 127 && notepad.cursor < NOTEPAD_MAX_TEXT - 1) {
    /* Printable character */
    notepad.text[notepad.cursor++] = (char)key;
    notepad.text[notepad.cursor] = '\0';
  }
}

static void draw_notepad(void) {
  if (!notepad.visible)
    return;

  int x = notepad.x;
  int y = notepad.y;
  int w = notepad.width;
  int h = notepad.height;
  int title_h = UI_SCALE_VAL(32);
  int font_w = FONT_WIDTH * ui_scale;
  int font_h = FONT_HEIGHT * ui_scale;

  /* Window shadow */
  fb_fill_rect(x + UI_SCALE_VAL(6), y + UI_SCALE_VAL(6), w, h, 0x11111B);

  /* Window background */
  fb_fill_rect(x, y, w, h, 0xFFFBE6); /* Notepad yellow background */

  /* Title bar */
  fb_fill_rect(x, y, w, title_h, COLOR_TITLE_BAR);
  draw_traffic_lights(x, y, title_h);
  gui_draw_string(x + w / 2 - font_string_width("Notepad") / 2, y + (title_h - font_h) / 2, "Notepad", THEME_TEXT);

  /* Text area */
  int text_x = x + UI_SCALE_VAL(10);
  int text_y = y + title_h + UI_SCALE_VAL(10);
  int max_cols = (w - UI_SCALE_VAL(20)) / font_w;
  int line = 0;
  int col = 0;
  
  for (int i = 0; i < notepad.cursor && notepad.text[i]; i++) {
    if (notepad.text[i] == '\n') {
      line++;
      col = 0;
    } else {
      gui_draw_char(text_x + col * font_w, text_y + line * font_h, notepad.text[i], 0x333333);
      col++;
      if (col >= max_cols) {
        col = 0;
        line++;
      }
    }
    if (text_y + line * font_h > y + h - UI_SCALE_VAL(20))
      break;
  }

  /* Cursor */
  static int blink = 0;
  blink++;
  if ((blink / 15) % 2 == 0) {
    fb_fill_rect(text_x + col * font_w, text_y + line * font_h, UI_SCALE_VAL(2), font_h, 0x333333);
  }
}

/* ===================================================================== */
/* Analog Clock                                                          */
/* ===================================================================== */

typedef struct {
  int visible;
  int x, y, size;
  int hours, minutes, seconds;
} clock_widget_t;

static clock_widget_t analog_clock = {0, 700, 100, 180, 10, 10, 30};

/* Sine/cosine lookup for clock hands (fixed point, scale 256) */
static const int clock_sin[60] = {
    0,    26,   53,   79,   104,  128,  150,  171,  189,  205,  219,  231,
    240,  248,  253,  256,  253,  248,  240,  231,  219,  205,  189,  171,
    150,  128,  104,  79,   53,   26,   0,    -26,  -53,  -79,  -104, -128,
    -150, -171, -189, -205, -219, -231, -240, -248, -253, -256, -253, -248,
    -240, -231, -219, -205, -189, -171, -150, -128, -104, -79,  -53,  -26};
static const int clock_cos[60] = {
    -256, -253, -248, -240, -231, -219, -205, -189, -171, -150, -128, -104,
    -79,  -53,  -26,  0,    26,   53,   79,   104,  128,  150,  171,  189,
    205,  219,  231,  240,  248,  253,  256,  253,  248,  240,  231,  219,
    205,  189,  171,  150,  128,  104,  79,   53,   26,   0,    -26,  -53,
    -79,  -104, -128, -150, -171, -189, -205, -219, -231, -240, -248, -253};

static void draw_clock_widget(void) {
  if (!analog_clock.visible)
    return;

  int title_h = UI_SCALE_VAL(32);
  int cx = analog_clock.x + analog_clock.size / 2;
  int cy = analog_clock.y + analog_clock.size / 2 + title_h / 2;
  int r = analog_clock.size / 2 - UI_SCALE_VAL(10);

  /* Window shadow */
  fb_fill_rect(analog_clock.x + UI_SCALE_VAL(6), analog_clock.y + UI_SCALE_VAL(6), analog_clock.size, analog_clock.size + title_h, 0x11111B);

  /* Window background */
  fb_fill_rect(analog_clock.x, analog_clock.y, analog_clock.size, analog_clock.size + title_h, THEME_BASE);

  /* Title bar */
  fb_fill_rect(analog_clock.x, analog_clock.y, analog_clock.size, title_h, COLOR_TITLE_BAR);
  draw_traffic_lights(analog_clock.x, analog_clock.y, title_h);
  gui_draw_string(analog_clock.x + analog_clock.size / 2 - font_string_width("Clock") / 2, analog_clock.y + (title_h - FONT_HEIGHT * ui_scale) / 2, "Clock", THEME_TEXT);

  /* Clock face - white circle */
  gui_draw_circle(cx, cy, r, 0xFFFFFF);
  gui_draw_circle(cx, cy, r - 2, 0xF0F0F0);

  /* Hour markers */
  for (int i = 0; i < 12; i++) {
    int idx = i * 5;
    int marker_inner = r - UI_SCALE_VAL(8);
    int marker_outer = r - UI_SCALE_VAL(2);
    int x1 = cx + marker_inner * clock_sin[idx] / 256;
    int y1 = cy + marker_inner * clock_cos[idx] / 256;
    int x2 = cx + marker_outer * clock_sin[idx] / 256;
    int y2 = cy + marker_outer * clock_cos[idx] / 256;
    gui_draw_line(x1, y1, x2, y2, 0x333333);
  }

  /* Update time (simple incrementing for demo) */
  static int tick = 0;
  tick++;
  if (tick >= 30) {
    tick = 0;
    analog_clock.seconds++;
    if (analog_clock.seconds >= 60) {
      analog_clock.seconds = 0;
      analog_clock.minutes++;
      if (analog_clock.minutes >= 60) {
        analog_clock.minutes = 0;
        analog_clock.hours++;
        if (analog_clock.hours >= 12)
          analog_clock.hours = 0;
      }
    }
  }

  int h = analog_clock.hours;
  int m = analog_clock.minutes;
  int s = analog_clock.seconds;

  /* Hour hand */
  int h_idx = (h * 5 + m / 12) % 60;
  int hx = cx + (r * 50 / 100) * clock_sin[h_idx] / 256;
  int hy = cy + (r * 50 / 100) * clock_cos[h_idx] / 256;
  for (int t = -2; t <= 2; t++) {
    gui_draw_line(cx + t, cy, hx + t, hy, 0x333333);
  }

  /* Minute hand */
  int mx = cx + (r * 70 / 100) * clock_sin[m] / 256;
  int my = cy + (r * 70 / 100) * clock_cos[m] / 256;
  for (int t = -1; t <= 1; t++) {
    gui_draw_line(cx + t, cy, mx + t, my, 0x555555);
  }

  /* Second hand */
  int sx = cx + (r * 80 / 100) * clock_sin[s] / 256;
  int sy = cy + (r * 80 / 100) * clock_cos[s] / 256;
  gui_draw_line(cx, cy, sx, sy, 0xFF0000);

  /* Center dot */
  gui_draw_circle(cx, cy, UI_SCALE_VAL(4), 0xFF0000);
}

/* ===================================================================== */
/* Help Window                                                           */
/* ===================================================================== */

typedef struct {
  int visible;
  int x, y, width, height;
} help_window_t;

static help_window_t help_win = {0, 200, 120, 450, 380};

static void draw_help_window(void) {
  if (!help_win.visible)
    return;

  int x = help_win.x;
  int y = help_win.y;
  int w = help_win.width;
  int h = help_win.height;
  int title_h = UI_SCALE_VAL(32);
  int font_h = FONT_HEIGHT * ui_scale;
  int line_h = UI_SCALE_VAL(18);
  int section_gap = UI_SCALE_VAL(32);
  int line_gap = UI_SCALE_VAL(24);

  /* Window shadow */
  fb_fill_rect(x + UI_SCALE_VAL(6), y + UI_SCALE_VAL(6), w, h, 0x11111B);

  /* Window background */
  fb_fill_rect(x, y, w, h, THEME_BASE);

  /* Title bar */
  fb_fill_rect(x, y, w, title_h, COLOR_TITLE_BAR);
  draw_traffic_lights(x, y, title_h);
  gui_draw_string(x + w / 2 - font_string_width("Help & About") / 2, y + (title_h - font_h) / 2, "Help & About", THEME_TEXT);

  /* Content */
  int ty = y + title_h + UI_SCALE_VAL(18);
  gui_draw_string(x + UI_SCALE_VAL(20), ty, "Omi Desktop", THEME_BLUE);
  ty += line_gap;
  gui_draw_string(x + UI_SCALE_VAL(20), ty, "Version 1.0", THEME_SUBTEXT);
  ty += section_gap;
  
  gui_draw_string(x + UI_SCALE_VAL(20), ty, "Keyboard Shortcuts:", THEME_TEXT);
  ty += line_gap;
  gui_draw_string(x + UI_SCALE_VAL(30), ty, "F1-F8    Change wallpaper", THEME_SUBTEXT);
  ty += line_h;
  gui_draw_string(x + UI_SCALE_VAL(30), ty, "F9       Calculator", THEME_SUBTEXT);
  ty += line_h;
  gui_draw_string(x + UI_SCALE_VAL(30), ty, "F10      Snake game", THEME_SUBTEXT);
  ty += line_h;
  gui_draw_string(x + UI_SCALE_VAL(30), ty, "F11      Notepad", THEME_SUBTEXT);
  ty += line_h;
  gui_draw_string(x + UI_SCALE_VAL(30), ty, "F12      Clock", THEME_SUBTEXT);
  ty += line_h;
  gui_draw_string(x + UI_SCALE_VAL(30), ty, "ESC      Close windows", THEME_SUBTEXT);
  ty += section_gap;
  
  gui_draw_string(x + UI_SCALE_VAL(20), ty, "Snake Controls:", THEME_TEXT);
  ty += line_gap;
  gui_draw_string(x + UI_SCALE_VAL(30), ty, "WASD / Arrow keys to move", THEME_SUBTEXT);
  ty += line_h;
  gui_draw_string(x + UI_SCALE_VAL(30), ty, "R to restart after game over", THEME_SUBTEXT);
  ty += section_gap;
  
  gui_draw_string(x + UI_SCALE_VAL(20), ty, "Click dock icons to open apps", THEME_TEXT);
}

/* ===================================================================== */
/* Desktop Background                                                    */
/* ===================================================================== */


static void draw_wallpaper(void) {
  /* MeepOS paper desktop: #EDEBDD, a 22px dot grid at 16,16, and
     radial-gradient(120% 80% at 50% 0%, rgba(255,255,255,.34), transparent 62%).
     Built once into a cache because the compositor redraws on every event. */
  static uint32_t *cache = (uint32_t *)0;
  static int cw = 0, ch = 0, cdark = -1;
  int w = (int)screen_width, h = (int)screen_height;

  if (!cache || cw != w || ch != h || cdark != appearance_dark) {
    if (cache) kfree(cache);
    cache = (uint32_t *)kmalloc((size_t)w * h * sizeof(uint32_t));
    cw = w; ch = h; cdark = appearance_dark;
    if (!cache) return;

    /* the palette is runtime state, so the desktop follows the toggle too */
    const int pr = (MEEP_PAPER >> 16) & 0xFF, pg = (MEEP_PAPER >> 8) & 0xFF,
              pb = MEEP_PAPER & 0xFF;             /* --paper */
    const int ir = (MEEP_INK >> 16) & 0xFF, ig = (MEEP_INK >> 8) & 0xFF,
              ib = MEEP_INK & 0xFF;               /* --ink   */
    /* the web shell's white radial bloom is a light-theme device; in midnight
       the same gradient lifts toward paper-2 instead, so it never fogs out */
    const int hr = appearance_dark ? ((MEEP_PAPER2 >> 16) & 0xFF) : 255;
    const int hg = appearance_dark ? ((MEEP_PAPER2 >> 8) & 0xFF) : 255;
    const int hb = appearance_dark ? (MEEP_PAPER2 & 0xFF) : 255;
    int cx = w / 2, cy = 0;
    int rx = w * 12 / 10, ry = h * 8 / 10;
    if (rx < 1) rx = 1;
    if (ry < 1) ry = 1;

    for (int y = 0; y < h; y++) {
      int dy = y - cy;
      int dy2 = (int)(((int64_t)dy * dy * 1000000) / ((int64_t)ry * ry));
      int dy22 = y % 22;
      int dot_y = (dy22 == 18 || dy22 == 19);      /* dots are 2x2, phase 18 mod 22 */
      for (int x = 0; x < w; x++) {
        int dx = x - cx;
        int s = dy2 + (int)(((int64_t)dx * dx * 1000000) / ((int64_t)rx * rx));
        int t = meep_isqrt(s);                    /* 0..1000, gradient position */
        int r = pr, g = pg, b = pb;
        if (t < 620) {
          /* #desktop::after paints this at opacity .5, so .34 becomes .17 */
          int a = 43 - (43 * t) / 620;
          r += (hr - r) * a / 255;
          g += (hg - g) * a / 255;
          b += (hb - b) * a / 255;
        }
        if (dot_y) {                              /* dot = rgba(ink, .10) */
          int dx22 = x % 22;
          if (dx22 == 4 || dx22 == 5) {
            r = (r * 90 + ir * 10 + 50) / 100;
            g = (g * 90 + ig * 10 + 50) / 100;
            b = (b * 90 + ib * 10 + 50) / 100;
          }
        }
        cache[y * w + x] = MAKE_COLOR(r, g, b);
      }
    }
  }

  for (int y = 0; y < h; y++)
    memcpy(backbuffer + (size_t)y * w, cache + (size_t)y * w, (size_t)w * sizeof(uint32_t));
}

/* no desktop icons in the MeepOS environment: the shell is the layout, nothing else */

/* ---- real type: Bricolage Grotesque + JetBrains Mono, alpha-blended at 1x ---- */
static int meep_text_w(const mface *f, const char *s) {
  int w = 0;
  for (; *s; s++) { int c = (unsigned char)*s; if (c < 32 || c > 126) c = '?'; w += f->g[c - 32].adv; }
  return w;
}

static void meep_blend_px(int x, int y, uint32_t color, int a) {
  if (x < 0 || y < 0 || x >= (int)screen_width || y >= (int)screen_height) return;
  uint32_t *p = backbuffer + (size_t)y * screen_width + x;
  uint32_t d = *p;
  int ia = 255 - a;
  int r = (((color >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * ia) / 255;
  int g = (((color >>  8) & 0xFF) * a + ((d >>  8) & 0xFF) * ia) / 255;
  int b = (( color        & 0xFF) * a + ( d        & 0xFF) * ia) / 255;
  *p = MAKE_COLOR(r, g, b);
}

/* A Remix Icon glyph. The baked mask is ICON_SRC square and box-filtered down
   to the target box, so the same mask is crisp at 1x and at any ui_scale. */
static void meep_icon(int id, int x, int y, int size, uint32_t color) {
  if (id < 0 || id >= ICON_COUNT || size <= 0) return;
  int d = UI_SCALE_VAL(size);
  const unsigned char *m = ri_icon_mask[id];
  for (int ty = 0; ty < d; ty++) {
    int sy0 = ty * ICON_SRC / d, sy1 = (ty + 1) * ICON_SRC / d;
    if (sy1 <= sy0) sy1 = sy0 + 1;
    for (int tx = 0; tx < d; tx++) {
      int sx0 = tx * ICON_SRC / d, sx1 = (tx + 1) * ICON_SRC / d;
      if (sx1 <= sx0) sx1 = sx0 + 1;
      int acc = 0, n = 0;
      for (int sy = sy0; sy < sy1; sy++)
        for (int sx = sx0; sx < sx1; sx++) {
          acc += m[sy * ICON_SRC + sx];
          n++;
        }
      int a = acc / n;
      if (a) meep_blend_px(x + tx, y + ty, color, a);
    }
  }
}

/* draw at 1x with the atlas' own coverage - ytop is the top of the line box */
static void meep_text(const mface *f, int x, int ytop, const char *s, uint32_t color) {
  int pen = x;
  for (; *s; s++) {
    int c = (unsigned char)*s; if (c < 32 || c > 126) c = '?';
    const mglyph *g = &f->g[c - 32];
    for (int gy = 0; gy < g->h; gy++) {
      const uint8_t *row = f->bm + g->off + gy * g->w;
      for (int gx = 0; gx < g->w; gx++)
        if (row[gx]) meep_blend_px(pen + g->ox + gx, ytop + g->oy + gy, color, row[gx]);
    }
    pen += g->adv;
  }
}

static int meep_line_h(const mface *f) { return f->asc + f->desc; }

#define BRAND_WORD "Omi"

/* ---- menus, transcribed from the web shell's index.html ---- */
typedef struct { const char *label; const char *kbd; } mitem;
/* ===================== MeepOS apps and widgets ======================= */
/* =========================================================================
 * MeepOS apps: real windows, a calculator, a notepad, and the desktop
 * widgets (sticky note / weather / big clock).  All integer maths - the
 * kernel is built with -mno-80387, so there is no FPU to lean on.
 * ========================================================================= */

/* MeepOS is its own OS, not a front-end for the house dashboard: only the apps
   that were actually asked for live here (plus the widgets, below). */
enum { APP_NONE = 0, APP_CALC, APP_NOTES, APP_MEDIA, APP_BROWSER, APP_TASKS, APP_COUNT };

typedef struct { int open, x, y, w, h; int min; } meep_app_t;

static meep_app_t mapp[APP_COUNT];
static int mapp_focus = APP_NONE;
static int drag_app = APP_NONE, drag_dx = 0, drag_dy = 0;
static int resize_app = APP_NONE, resize_dx = 0, resize_dy = 0;

/* ---- widgets ----------------------------------------------------------- */
enum { WG_STICKY = 0, WG_WEATHER, WG_CLOCK, WG_COUNT };

typedef struct { int on, x, y, w, h; } meep_wg_t;

/* default layout copied off the web shell: a 206px column down the right side */
static meep_wg_t mwg[WG_COUNT] = {
  { 0, 1058, 372, 206, 168 },     /* sticky note */
  { 1, 1058, 174, 206, 152 },     /* weather     */
  { 1, 1058,  56, 206, 110 }      /* big clock   */
};
static int wg_focus = -1;
static int wg_drag = -1, wg_dx = 0, wg_dy = 0;

static char sticky_text[600] = "";
static int sticky_len = 0, sticky_cur = 0;

/* weather is fed by the MeepNet uplink once it lands; until then it says so
   rather than inventing a forecast. */
static int wx_ok = 0, wx_now = 0, wx_hi = 0, wx_lo = 0, wx_hi_ok = 0;
static char wx_desc[32] = "no uplink";
static char wx_city[28] = "this network";

/* ---- tiny string helpers (no libc snprintf in here) -------------------- */
static int s_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void s_cpy(char *d, const char *s, int cap) {
  int i = 0;
  while (s[i] && i < cap - 1) { d[i] = s[i]; i++; }
  d[i] = 0;
}

static void s_num(long long v, char *out) {
  char tmp[24];
  int n = 0, i = 0;
  int neg = v < 0;
  unsigned long long u = neg ? (unsigned long long)(-v) : (unsigned long long)v;
  if (!u) tmp[n++] = '0';
  while (u) { tmp[n++] = (char)('0' + (int)(u % 10ULL)); u /= 10ULL; }
  if (neg) out[i++] = '-';
  while (n) out[i++] = tmp[--n];
  out[i] = 0;
}

/* cents -> "12.5" / "12" / "-3.25" */
static void s_money(long long v100, char *out) {
  long long v = v100;
  int i = 0;
  if (v < 0) { out[i++] = '-'; v = -v; }
  char ip[24];
  s_num(v / 100, ip);
  for (int k = 0; ip[k]; k++) out[i++] = ip[k];
  int c = (int)(v % 100);
  if (c) {
    out[i++] = '.';
    out[i++] = (char)('0' + c / 10);
    if (c % 10) out[i++] = (char)('0' + c % 10);
  }
  out[i] = 0;
}

/* ---- seven-segment numerals: the shell's own big type ------------------ */
static const unsigned char seg7[10] = { 63, 6, 91, 79, 102, 109, 125, 7, 127, 111 };

static void seg_bar(int x, int y, int w, int h, uint32_t ink) {
  if (w > 0 && h > 0) fb_fill_rect(x, y, w, h, ink);
}

static void big_char(int x, int ytop, int w, int h, int t, char c, uint32_t ink) {
  int mid = ytop + h / 2;
  if (c >= '0' && c <= '9') {
    unsigned char m = seg7[c - '0'];
    if (m & 1)  seg_bar(x + t, ytop, w - 2 * t, t, ink);                  /* A */
    if (m & 2)  seg_bar(x + w - t, ytop + t, t, h / 2 - t, ink);          /* B */
    if (m & 4)  seg_bar(x + w - t, mid + 1, t, h / 2 - t, ink);           /* C */
    if (m & 8)  seg_bar(x + t, ytop + h - t, w - 2 * t, t, ink);          /* D */
    if (m & 16) seg_bar(x, mid + 1, t, h / 2 - t, ink);                   /* E */
    if (m & 32) seg_bar(x, ytop + t, t, h / 2 - t, ink);                  /* F */
    if (m & 64) seg_bar(x + t, mid - t / 2, w - 2 * t, t, ink);           /* G */
  } else if (c == ':') {
    seg_bar(x + w / 2 - t / 2, ytop + h / 4, t, t, ink);
    seg_bar(x + w / 2 - t / 2, ytop + (3 * h) / 4 - t, t, t, ink);
  } else if (c == '.') {
    seg_bar(x + w / 2 - t / 2, ytop + h - t, t, t, ink);
  } else if (c == '-') {
    seg_bar(x + t, mid - t / 2, w - 2 * t, t, ink);
  }
}

static int big_text(int x, int ytop, int h, const char *s, uint32_t ink) {
  int t = h / 7;   if (t < 2) t = 2;
  int w = h / 2;   if (w < 6) w = 6;
  int gap = h / 8; if (gap < 2) gap = 2;
  int cx = x;
  for (int i = 0; s[i]; i++) {
    int cw = (s[i] == ':' || s[i] == '.') ? t * 2 : w;
    big_char(cx, ytop, cw, h, t, s[i], ink);
    cx += cw + gap;
  }
  return cx - x - gap;
}

static int big_w(int h, const char *s) {
  int t = h / 7;   if (t < 2) t = 2;
  int w = h / 2;   if (w < 6) w = 6;
  int gap = h / 8; if (gap < 2) gap = 2;
  int total = 0, n = 0;
  for (int i = 0; s[i]; i++) {
    total += (s[i] == ':' || s[i] == '.') ? t * 2 : w;
    n++;
  }
  return n ? total + gap * (n - 1) : 0;
}

/* ---- window chrome ----------------------------------------------------- */
/* Mirrors the web shell's CSS: .titlebar is 31px, paper-2 with a 1px --line
   hairline, title 12.5px/600, and a .wctl group of 30x26 buttons on the right
   (minimise, maximise, close - close turns #c0392b on hover). The framebuffer
   has no alpha, so the CSS drop shadow is flattened into two offset rings. */
#define APP_TITLE_H 31

/* Rounded top corners, flat bottom. A plain fb_fill_rect inside a rounded
   window squares off the corner and leaves the border arc floating, which is
   exactly what made the corners read as "not connected". */
static void fill_cap(int x, int y, int w, int h, int r, uint32_t color) {
  gui_draw_rounded_rect(x, y, w, 2 * r, r, color);
  if (h > r) fb_fill_rect(x, y + r, w, h - r, color);
}

/* .wg-title: mono, 9px, uppercase, letter-spacing .16em */
static int meep_tracked_w(const char *s) {
  return meep_text_w(&meep_m100, s) + s_len(s);
}

static void meep_tracked(int x, int y, const char *s, uint32_t color) {
  char one[2] = { 0, 0 };
  for (int i = 0; s[i]; i++) {
    one[0] = s[i];
    meep_text(&meep_m100, x, y, one, color);
    x += meep_text_w(&meep_m100, one) + 1;
  }
}

/* .wctl button glyphs: 0 = minimise, 1 = maximise, 2 = close (Remix icons) */
static void wctl_glyph(int cx, int cy, int kind, uint32_t color) {
  static const int id[3] = { ICON_MIN, ICON_MAX, ICON_CLOSE };
  const int sz = 12;
  meep_icon(id[kind % 3], cx - UI_SCALE_VAL(sz) / 2, cy - UI_SCALE_VAL(sz) / 2,
            sz, color);
}

static void app_win(int x, int y, int w, int h, const char *title,
                    const char *meta, int focused) {
  meep_box(x, y, w, h, 10, focused ? MEEP_LINE : MEEP_LINE_SOFT, MEEP_PAPER2);
  /* titlebar keeps the body tone when focused; the blurred window greys it */
  fill_cap(x + 1, y + 1, w - 2, APP_TITLE_H - 1, 9,
           focused ? MEEP_PAPER2 : MEEP_PAPER);
  meep_hline(x + 1, y + APP_TITLE_H, w - 2, 1, focused ? MEEP_LINE : MEEP_LINE_SOFT);

  int ty = y + (APP_TITLE_H - meep_line_h(&meep_s130)) / 2;
  meep_text(&meep_s130, x + 11, ty, title, focused ? MEEP_INK : MEEP_INK_SOFT);

  /* the .wctl group: three 30x26 buttons, 2px apart, 6px off the right edge */
  int bw = 30, bh = 26, gap = 2;
  int bx = x + w - 6 - (bw * 3 + gap * 2);
  int by = y + (APP_TITLE_H - bh) / 2;
  for (int i = 0; i < 3; i++) {
    wctl_glyph(bx + i * (bw + gap) + bw / 2, by + bh / 2, i, focused ? MEEP_INK : MEEP_INK_SOFT);
  }
  /* corner grip: three hairlines, the resize affordance */
  for (int i = 0; i < 3; i++) {
    int gx = x + w - 5 - i * 4, gy = y + h - 5;
    gui_draw_line(gx - 6, gy, gx, gy - 6, focused ? MEEP_INK_SOFT : MEEP_LINE);
  }
  if (meta && meta[0]) {
    int mw = meep_text_w(&meep_m100, meta);
    meep_text(&meep_m100, bx - 8 - mw, y + (APP_TITLE_H - meep_line_h(&meep_m100)) / 2,
              meta, MEEP_INK_SOFT);
  }
}

static int app_close_hit(int x, int y, int wx, int wy, int ww) {
  return (y >= wy && y < wy + APP_TITLE_H && x >= wx + ww - 38 && x < wx + ww - 4);
}

/* 0 none, 1 minimise, 2 maximise, 3 close - the .wctl group on the right */
static int app_wctl_hit(int x, int y, int wx, int wy, int ww) {
  if (y < wy + (APP_TITLE_H - 26) / 2 || y >= wy + (APP_TITLE_H - 26) / 2 + 26) return 0;
  int bx = wx + ww - 6 - (30 * 3 + 2 * 2);
  for (int i = 0; i < 3; i++) {
    int x0 = bx + i * 32;
    if (x >= x0 && x < x0 + 30) return i + 1;
  }
  return 0;
}

/* maximise keeps the old rect so it can be restored */
static int app_max[APP_COUNT];
static int app_saved[APP_COUNT][4];

/* ---- calculator -------------------------------------------------------- */
typedef struct {
  long long cur;      /* scaled by 100: 12.34 -> 1234 */
  long long acc;
  char op;
  int fresh, dec, decn, err;
} calc_state_t;

static calc_state_t cs = { 0, 0, 0, 1, 0, 0, 0 };
static int calc_hover = -1;

static void calc_digit(int d) {
  if (cs.err) { cs.err = 0; cs.cur = 0; }
  if (cs.fresh) { cs.cur = 0; cs.fresh = 0; cs.dec = 0; cs.decn = 0; }
  if (cs.dec) {
    if (cs.decn >= 2) return;
    cs.cur += (long long)d * (cs.decn == 0 ? 10 : 1);
    cs.decn++;
  } else {
    cs.cur = cs.cur * 10 + (long long)d * 100;
  }
}

static void calc_apply(char op) {
  if (op == '+') cs.cur = cs.acc + cs.cur;
  else if (op == '-') cs.cur = cs.acc - cs.cur;
  else if (op == '*') cs.cur = (cs.acc * cs.cur) / 100;
  else if (op == '/') {
    if (cs.cur) cs.cur = (cs.acc * 100) / cs.cur;
    else { cs.err = 1; cs.cur = 0; }
  }
}

static void calc_key(char k) {
  if (k >= '0' && k <= '9') { calc_digit(k - '0'); return; }
  if (k == '.') { if (!cs.dec) { cs.dec = 1; cs.decn = 0; cs.fresh = 0; } return; }
  if (k == 'C') { cs.cur = cs.acc = 0; cs.op = 0; cs.fresh = 1; cs.dec = cs.decn = 0; cs.err = 0; return; }
  if (k == 'n') { cs.cur = -cs.cur; return; }
  if (k == '%') { cs.cur = cs.cur / 100; return; }
  if (k == '=') {
    if (cs.op) calc_apply(cs.op);
    cs.op = 0; cs.fresh = 1; cs.dec = cs.decn = 0;
    return;
  }
  if (k == '+' || k == '-' || k == '*' || k == '/') {
    if (cs.op) calc_apply(cs.op);
    cs.acc = cs.cur; cs.op = k; cs.fresh = 1; cs.dec = cs.decn = 0;
  }
}

static const char calc_lbl[19] = { 'C', 'n', '%', '/', '7', '8', '9', '*',
                                   '4', '5', '6', '-', '1', '2', '3', '+',
                                   '0', '.', '=' };

static void calc_key_rect(int i, int bx, int by, int *kx, int *ky, int *kw, int *kh) {
  const int kww = 52, khh = 40, gap = 6;
  int col, row;
  if (i < 18) { col = i % 4; row = i / 4; }
  else { col = 2; row = 4; }
  *kx = bx + col * (kww + gap);
  *ky = by + row * (khh + gap);
  *kw = (i == 18) ? kww * 2 + gap : kww;
  *kh = khh;
}

static void calc_label(int i, char *out) {
  switch (calc_lbl[i]) {
    case 'n': out[0] = '+'; out[1] = '/'; out[2] = '-'; out[3] = 0; return;
    case '/': out[0] = '/'; out[1] = 0; return;
    case '*': out[0] = 'x'; out[1] = 0; return;
    case '-': out[0] = '-'; out[1] = 0; return;
    case '%': out[0] = '%'; out[1] = 0; return;
    default: out[0] = calc_lbl[i]; out[1] = 0; return;
  }
}


/* Task manager: the scheduler's own view of itself. */
static void u64s(uint64_t v, char *out) {
  char tmp[24]; int n = 0;
  if (!v) { out[0] = '0'; out[1] = 0; return; }
  while (v && n < 23) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
  int i = 0;
  while (n > 0) out[i++] = tmp[--n];
  out[i] = 0;
}

static void draw_tasks(void) {
  meep_app_t *a = &mapp[APP_TASKS];
  if (!a->open) return;
  int nt = task_count();
  char sub[24];
  u64s((uint64_t)nt, sub);
  int sl = 0; while (sub[sl]) sl++;
  const char *tail = " tasks";
  for (int i = 0; tail[i] && sl < 22; i++) sub[sl++] = tail[i];
  sub[sl] = 0;
  app_win(a->x, a->y, a->w, a->h, "task manager", sub, mapp_focus == APP_TASKS);

  int x0 = a->x + UI_SCALE_VAL(14), y0 = a->y + APP_TITLE_H + UI_SCALE_VAL(14);
  int lh = meep_line_h(&meep_m100);
  int cx_name = x0, cx_state = x0 + UI_SCALE_VAL(108);
  int cx_ticks = x0 + UI_SCALE_VAL(196), cx_sw = x0 + UI_SCALE_VAL(268);

  meep_text(&meep_m100, cx_name,  y0, "task",     MEEP_INK_SOFT);
  meep_text(&meep_m100, cx_state, y0, "state",    MEEP_INK_SOFT);
  meep_text(&meep_m100, cx_ticks, y0, "ticks",    MEEP_INK_SOFT);
  meep_text(&meep_m100, cx_sw,    y0, "switches", MEEP_INK_SOFT);
  y0 += lh + UI_SCALE_VAL(6);
  fb_fill_rect(x0, y0, a->w - 2 * UI_SCALE_VAL(14), 1, MEEP_LINE_SOFT);
  y0 += UI_SCALE_VAL(6);

  uint64_t total = 0;
  for (int i = 0; i < nt; i++) {
    task_t *t = task_at(i);
    if (t) total += t->ticks;
  }
  if (!total) total = 1;

  for (int i = 0; i < nt && y0 + lh < a->y + a->h - UI_SCALE_VAL(10); i++) {
    task_t *t = task_at(i);
    if (!t) break;
    uint32_t ink = (t->state == TASK_RUN) ? MEEP_INK : MEEP_INK_SOFT;
    if (t->state == TASK_RUN)
      fb_fill_rect(x0 - UI_SCALE_VAL(6), y0 + lh / 2 - 2, 4, 4, MEEP_ACCENT_INK);
    meep_text(&meep_m100, cx_name,  y0, t->name, ink);
    meep_text(&meep_m100, cx_state, y0, task_state_name(t->state), ink);
    char buf[24];
    u64s(t->ticks, buf);
    meep_text(&meep_m100, cx_ticks, y0, buf, ink);
    u64s(t->switches, buf);
    meep_text(&meep_m100, cx_sw, y0, buf, ink);

    /* share of cpu, as a bar under the row */
    int bw = a->w - 2 * UI_SCALE_VAL(14);
    int filled = (int)((t->ticks * (uint64_t)bw) / total);
    if (filled > bw) filled = bw;
    int by = y0 + lh + 1;
    fb_fill_rect(x0, by, bw, 2, MEEP_LINE_SOFT);
    if (filled > 0) fb_fill_rect(x0, by, filled, 2, MEEP_ACCENT);
    y0 += lh + UI_SCALE_VAL(8);
  }

  meep_text(&meep_m100, x0, a->y + a->h - UI_SCALE_VAL(18),
            "scheduler: round robin, 100 hz tick", MEEP_INK_SOFT);
}

static void draw_calc(void) {
  meep_app_t *a = &mapp[APP_CALC];
  if (!a->open) return;
  app_win(a->x, a->y, a->w, a->h, "Calculator", cs.err ? "error" : "", mapp_focus == APP_CALC);

  int bx = a->x + 12, by = a->y + APP_TITLE_H + 12;
  int dw = a->w - 24, dh = 64;
  meep_box(bx, by, dw, dh, 8, MEEP_LINE_SOFT, MEEP_PAPER3);

  char txt[32];
  if (cs.err) s_cpy(txt, "cannot divide by 0", sizeof txt);
  else s_money(cs.cur, txt);
  int th = 34;
  int tw = big_w(th, txt);
  int tx = bx + dw - 12 - tw;
  if (tx < bx + 10) {
    th = 22;
    tw = big_w(th, txt);
    tx = bx + dw - 12 - tw;
  }
  big_text(tx, by + (dh - th) / 2, th, txt, MEEP_INK);

  int kbx = bx + 1, kby = by + dh + 10;
  for (int i = 0; i < 19; i++) {
    int kx, ky, kw, kh;
    calc_key_rect(i, kbx, kby, &kx, &ky, &kw, &kh);
    int hot = (calc_hover == i);
    int accent = (calc_lbl[i] == '=');
    meep_box(kx, ky, kw, kh, 6, MEEP_LINE,
             accent ? (hot ? MAKE_COLOR(0xF2, 0xB4, 0x30) : MEEP_ACCENT)
                    : (hot ? MEEP_PAPER3 : MEEP_PAPER2));
    char lbl[8];
    calc_label(i, lbl);
    int lw = s_len(lbl) == 1 ? meep_text_w(&meep_s130, lbl) : meep_text_w(&meep_m100, lbl);
    int lx = kx + (kw - lw) / 2;
    int ly = ky + (kh - meep_line_h(&meep_s130)) / 2;
    if (s_len(lbl) == 1)
      meep_text(&meep_s130, lx, ly, lbl, accent ? MEEP_ACCENT_INK : MEEP_INK);
    else
      meep_text(&meep_m100, lx, ky + (kh - meep_line_h(&meep_m100)) / 2, lbl,
                accent ? MEEP_ACCENT_INK : MEEP_INK);
  }
}

static int calc_body_click(int x, int y) {
  meep_app_t *a = &mapp[APP_CALC];
  int bx = a->x + 12, by = a->y + APP_TITLE_H + 12 + 64 + 10;
  for (int i = 0; i < 19; i++) {
    int kx, ky, kw, kh;
    calc_key_rect(i, bx + 1, by, &kx, &ky, &kw, &kh);
    if (x >= kx && x < kx + kw && y >= ky && y < ky + kh) {
      calc_key(calc_lbl[i]);
      return 1;
    }
  }
  return 0;
}

/* ---- notepad ----------------------------------------------------------- */
#define NP_MAX 4000
static char np_text[NP_MAX];
static int np_len = 0, np_cur = 0;
static int np_blink = 0;

static void np_key(int key) {
  if (key == '\b' || key == 127) {
    if (np_cur > 0) {
      for (int i = np_cur - 1; i < np_len; i++) np_text[i] = np_text[i + 1];
      np_cur--; np_len--;
    }
    return;
  }
  if (key == '\n' || key == '\r') {
    if (np_len < NP_MAX - 1) {
      for (int i = np_len; i >= np_cur; i--) np_text[i + 1] = np_text[i];
      np_text[np_cur] = '\n';
      np_cur++; np_len++;
    }
    return;
  }
  if (key == KEY_LEFT)  { if (np_cur > 0) np_cur--; return; }
  if (key == KEY_RIGHT) { if (np_cur < np_len) np_cur++; return; }
  if (key >= 32 && key < 127 && np_len < NP_MAX - 1) {
    for (int i = np_len; i >= np_cur; i--) np_text[i + 1] = np_text[i];
    np_text[np_cur] = (char)key;
    np_cur++; np_len++;
  }
}

static void draw_notes(void) {
  meep_app_t *a = &mapp[APP_NOTES];
  if (!a->open) return;

  char meta[24];
  char num[16];
  s_num(np_len, num);
  s_cpy(meta, num, sizeof meta);
  int ml = s_len(meta);
  meta[ml] = ' '; meta[ml + 1] = 'c'; meta[ml + 2] = 'h'; meta[ml + 3] = 0;

  app_win(a->x, a->y, a->w, a->h, "Notepad", meta, mapp_focus == APP_NOTES);

  int bx = a->x + 1, by = a->y + APP_TITLE_H + 1;
  int bw = a->w - 2, bh = a->h - APP_TITLE_H - 2;
  fb_fill_rect(bx, by, bw, bh, MEEP_PAPER3);

  const mface *f = &meep_m105;
  int lh = meep_line_h(f) + 3;
  int cw = meep_text_w(f, "M");
  int pad = 12;
  int maxx = bx + bw - pad;
  int line_x = bx + pad, line_y = by + pad;
  int caret_x = line_x, caret_y = line_y;
  int caret_set = 0;

  for (int i = 0; i <= np_len; i++) {
    if (i == np_cur) { caret_x = line_x; caret_y = line_y; caret_set = 1; }
    if (i == np_len) break;
    char ch = np_text[i];
    if (ch == '\n') { line_x = bx + pad; line_y += lh; continue; }
    if (line_x + cw > maxx) { line_x = bx + pad; line_y += lh; }
    if (line_y < by + bh - 20) {
      char s[2]; s[0] = ch; s[1] = 0;
      meep_text(f, line_x, line_y, s, MEEP_INK);
    }
    line_x += cw;
  }
  if (!caret_set) { caret_x = line_x; caret_y = line_y; }

  if (mapp_focus == APP_NOTES && ((np_blink / 15) & 1) == 0) {
    fb_fill_rect(caret_x, caret_y, 1, meep_line_h(f), MEEP_ACCENT);
  }
  if (np_len == 0 && mapp_focus != APP_NOTES) {
    meep_text(&meep_s130, bx + pad, by + pad, "click, then type", MEEP_INK_SOFT);
  }

  /* footer hairline + hint */
  meep_hline(bx, by + bh - 22, bw, 1, MEEP_LINE);
  meep_text(&meep_m100, bx + pad, by + bh - 17, "RAM only until the disk driver lands",
            MEEP_INK_SOFT);
}

/* ---- widgets: shared chrome ------------------------------------------- */
/* .wg head strip: paper-3, 1px --line-soft hairline, mono 9px uppercase title
   with .16em tracking, and an 18px .wg-x box on the right. */
#define WG_HEAD_H 23

static int wg_chrome(meep_wg_t *w, int icon, const char *title) {
  meep_box(w->x, w->y, w->w, w->h, 12, MEEP_LINE, MEEP_PAPER2);
  fill_cap(w->x + 1, w->y + 1, w->w - 2, WG_HEAD_H - 1, 11, MEEP_PAPER3);
  meep_hline(w->x + 1, w->y + WG_HEAD_H, w->w - 2, 1, MEEP_LINE_SOFT);
  int isz = 12;
  meep_icon(icon, w->x + 9, w->y + (WG_HEAD_H - UI_SCALE_VAL(isz)) / 2, isz,
            MEEP_INK_SOFT);
  meep_tracked(w->x + 9 + UI_SCALE_VAL(isz) + UI_SCALE_VAL(5),
               w->y + (WG_HEAD_H - meep_line_h(&meep_m100)) / 2, title,
               MEEP_INK_SOFT);
  int cx = w->x + w->w - 14, cy = w->y + WG_HEAD_H / 2;
  meep_icon(ICON_CLOSE, cx - UI_SCALE_VAL(6), cy - UI_SCALE_VAL(6), 12,
            MEEP_INK_SOFT);
  return w->y + WG_HEAD_H;
}

/* ---- widgets: sticky note ---------------------------------------------- */
static void draw_sticky(void) {
  meep_wg_t *w = &mwg[WG_STICKY];
  if (!w->on) return;
  int by = wg_chrome(w, ICON_STICKY, "sticky");

  const mface *f = &meep_s130;
  int lh = meep_line_h(f) + 4;
  int cwmax = w->w - 24;
  int x = w->x + 12, y = by + 10;
  int cx = x, cy = y;

  if (!sticky_len && wg_focus != WG_STICKY) {
    meep_text(f, x, y, "click and type", MEEP_INK_SOFT);
    return;
  }
  for (int i = 0; i <= sticky_len; i++) {
    if (i == sticky_cur) { cx = x; cy = y; }
    if (i == sticky_len) break;
    char ch = sticky_text[i];
    if (ch == '\n') { x = w->x + 10; y += lh; continue; }
    char s[2]; s[0] = ch; s[1] = 0;
    if (x + meep_text_w(f, s) > w->x + cwmax) { x = w->x + 12; y += lh; }
    if (y < w->y + w->h - 12) meep_text(f, x, y, s, MEEP_INK);
    x += meep_text_w(f, s);
  }
  if (wg_focus == WG_STICKY && ((np_blink / 15) & 1) == 0)
    fb_fill_rect(cx, cy, 1, meep_line_h(f), MEEP_ACCENT);
}

/* ---- widgets: big clock ------------------------------------------------ */
static void wd_text(char *out) {
  static const char *wday[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
  static const char *mon[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
  int dom = meep_bcd(meep_cmos(0x07));
  int m   = meep_bcd(meep_cmos(0x08));
  int wd  = meep_bcd(meep_cmos(0x06));
  int i = 0;
  wd = (wd + 6) % 7;           /* cmos 1..7 = Sun..Sat */
  if (m < 1 || m > 12) m = 1;
  const char *a = wday[wd];
  while (*a) out[i++] = *a++;
  out[i++] = ' ';
  char num[8];
  s_num(dom, num);
  for (int k = 0; num[k]; k++) out[i++] = num[k];
  out[i++] = ' ';
  const char *b = mon[m - 1];
  while (*b) out[i++] = *b++;
  out[i] = 0;
}

static void clock_hm(char *out, char *ampm) {
  int h = meep_bcd(meep_cmos(0x04));
  int m = meep_bcd(meep_cmos(0x02));
  if (h > 23) h = 0;
  if (m > 59) m = 0;
  ampm[0] = h < 12 ? 'A' : 'P';
  ampm[1] = 'M';
  ampm[2] = 0;
  int h12 = h % 12;
  if (h12 == 0) h12 = 12;
  char a[8], b[8];
  s_num(h12, a);
  int i = 0;
  for (int k = 0; a[k]; k++) out[i++] = a[k];
  out[i++] = ':';
  if (m < 10) out[i++] = '0';
  s_num(m, b);
  for (int k = 0; b[k]; k++) out[i++] = b[k];
  out[i] = 0;
}

static void draw_bigclock(void) {
  meep_wg_t *w = &mwg[WG_CLOCK];
  if (!w->on) return;
  int by = wg_chrome(w, ICON_CLOCK, "clock");

  char hm[16], ampm[4];
  clock_hm(hm, ampm);

  int apw = meep_text_w(&meep_m115, ampm);
  int avail = w->w - 24 - apw - 6;
  int h = 34;
  int ttf = ttf_ready();
  int tw = ttf ? ttf_width(TTF_DISPLAY, h, hm) : big_w(h, hm);
  while (tw > avail && h > 18) {
    h -= 2;
    tw = ttf ? ttf_width(TTF_DISPLAY, h, hm) : big_w(h, hm);
  }
  int x = w->x + 12;
  int ty = by + 9;
  if (ttf) ttf_draw(TTF_DISPLAY, h, x, ty + ttf_ascent(TTF_DISPLAY, h), hm, MEEP_INK);
  else big_text(x, ty, h, hm, MEEP_INK);
  meep_text(&meep_m115, x + tw + 9, ty + h - meep_line_h(&meep_m115) - 3, ampm,
            MEEP_INK_SOFT);

  char dt[24];
  wd_text(dt);
  for (int i = 0; dt[i]; i++) if (dt[i] >= 'a' && dt[i] <= 'z') dt[i] -= 32;
  meep_tracked(w->x + 12, ty + h + 8, dt, MEEP_INK_SOFT);
}

/* ---- widgets: weather -------------------------------------------------- */
/* One fetch, at boot, over TLS. No location is passed: wttr.in geolocates the
 * client by IP, so the widget names whatever city this network is in instead of
 * a city compiled into the kernel. Format: "<place>|<temp>|<condition>". */
static void wx_fetch(void) {
  static char body[512];
  static http_result_t wr;
  const net_cfg_t *cfg = net_get();
  if (!cfg->valid) return;

  if (!http_get(cfg, "https://wttr.in/?format=%l|%t|%C", body, sizeof body, &wr))
    return;
  if (wr.status != 200 || !wr.secure) return;

  int i = 0;

  /* place, up to the first comma: "<City>, <Region>, <Country>" -> "<City>" */
  int o = 0, seen = 0;
  while (body[i] && body[i] != '|' && body[i] != ',') {
    char ch = body[i++];
    if (ch < 32 || ch >= 127) continue;
    if (ch == ' ' && !seen) continue;             /* leading spaces */
    if (o < (int)sizeof wx_city - 1) { wx_city[o++] = ch; seen = 1; }
  }
  while (o > 0 && wx_city[o - 1] == ' ') o--;
  wx_city[o] = 0;
  if (o == 0) {
    const char *fb = "this network";
    for (o = 0; fb[o] && o < (int)sizeof wx_city - 1; o++) wx_city[o] = fb[o];
    wx_city[o] = 0;
  }

  /* skip to the temperature field */
  while (body[i] && body[i] != '|') i++;
  if (body[i] == '|') i++;

  int sign = 1, val = 0, digits = 0;
  if (body[i] == '+' || body[i] == '-') { if (body[i] == '-') sign = -1; i++; }
  while (body[i] >= '0' && body[i] <= '9') { val = val * 10 + (body[i] - '0'); i++; digits++; }
  if (!digits || val > 150) return;               /* no temp, no forecast */

  while (body[i] && body[i] != '|') i++;
  if (body[i] == '|') i++;

  o = 0;
  while (body[i] && body[i] != '\n' && body[i] != '\r' && body[i] != '|'
         && o < (int)sizeof wx_desc - 1) {
    char ch = body[i++];
    if (ch >= 32 && ch < 127) wx_desc[o++] = ch;
  }
  while (o > 0 && wx_desc[o - 1] == ' ') o--;     /* wttr.in pads the field */
  wx_desc[o] = 0;
  if (o == 0) {
    const char *fb = "clear";
    for (o = 0; fb[o]; o++) wx_desc[o] = fb[o];
    wx_desc[o] = 0;
  }

  wx_now = sign * val;
  wx_ok = 1;
  wx_hi_ok = 0;          /* this source gives the current temp, not the day's range */
}

static void draw_weather(void) {
  meep_wg_t *w = &mwg[WG_WEATHER];
  if (!w->on) return;
  int by = wg_chrome(w, ICON_WEATHER, "weather");
  int x = w->x + 12;

  if (!wx_ok) {
    /* no forecast source yet: report the link truthfully, invent nothing */
    int up = e1000_link();
    meep_text(&meep_m115, x, by + 11, up ? "link up" : "no uplink", MEEP_INK);
    meep_tracked(x, by + 33, up ? "OMI NET ONLINE" : "OMI NET OFFLINE",
                 MEEP_INK_SOFT);
    meep_hline(x, by + 51, w->w - 24, 1, MEEP_LINE_SOFT);
    if (up) {
      meep_text(&meep_m100, x, by + 60, "tcp, http and tls are up.",
                MEEP_INK_SOFT);
      meep_text(&meep_m100, x, by + 74, "no forecast came back yet.",
                MEEP_INK_SOFT);
    } else {
      meep_text(&meep_m100, x, by + 60, "a NIC and the tcp stack",
                MEEP_INK_SOFT);
      meep_text(&meep_m100, x, by + 74, "come before a forecast.",
                MEEP_INK_SOFT);
    }
    return;
  }

  char t[16];
  s_num(wx_now, t);
  big_text(x, by + 11, 27, t, MEEP_INK);
  meep_text(&meep_m105, x + big_w(27, t) + 4, by + 13, "F", MEEP_INK_SOFT);
  meep_text(&meep_m115, x, by + 46, wx_desc, MEEP_INK_SOFT);

  if (wx_hi_ok) {
    char a[12], b[12];
    s_num(wx_hi, a); s_num(wx_lo, b);
    int hy = by + 68;
    meep_tracked(x, hy, "HI", MEEP_INK_SOFT);
    meep_text(&meep_m105, x + 24, hy - 1, a, MEEP_INK);
    meep_tracked(x + 62, hy, "LO", MEEP_INK_SOFT);
    meep_text(&meep_m105, x + 86, hy - 1, b, MEEP_INK);
    meep_hline(x, hy + 17, w->w - 24, 1, MEEP_LINE_SOFT);
  } else {
    meep_hline(x, by + 68, w->w - 24, 1, MEEP_LINE_SOFT);
    meep_text(&meep_m100, x, by + 74, "now, over tls", MEEP_INK_SOFT);
  }

  {
    char foot[48];
    int o = 0;
    for (int k = 0; wx_city[k] && o < 30; k++) foot[o++] = wx_city[k];
    const char *tail = " - wttr.in";
    for (int k = 0; tail[k] && o < (int)sizeof foot - 1; k++) foot[o++] = tail[k];
    foot[o] = 0;
    meep_text(&meep_m100, x, w->y + w->h - 18, foot, MEEP_INK_SOFT);
  }
}


/* ---- dashboard apps: replica of the catalog behind :8081/services.html ---- */
/* Read-only copy of the house catalogue. MeepOS never writes to the dashboard;
   these tables are regenerated from /api/catalog when the ISO is built. */
/* ---- Media player / MeepNet: asked for, not built yet ------------------ */

/* ---- audio: sample.mp3 on the HDA DAC --------------------------------
   There is no mixer graph and no interrupt handler, so this is a pump: the
   main loop hands the codec ~21 ms of audio at a time and the codec blocks
   until it has been consumed, which paces playback to real time. MP3s are
   not always 48 kHz, so everything is resampled to the codec's 48 kHz with
   linear interpolation. */
static meep_mp3_t *mp3h;
static int   mp_playing;
static int   mp_rate = 48000;
static int   mp_ch = 2;
static uint32_t mp_phase;   /* 16.16 fixed point, source frames */
static int   mp_src_frames;
static int16_t mp_src[4096];
static int16_t mp_out[2048];
static char  mp_note[40] = "ready";

static void mp_note_set(const char *m) {
  int k = 0;
  while (m[k] && k < (int)sizeof(mp_note) - 1) { mp_note[k] = m[k]; k++; }
  mp_note[k] = 0;
}

static void mp_stop(void) {
  mp_playing = 0;
  hda_stream_stop();
}

static int mp_start(void) {
  if (!mp3h) {
    if (!meep_media_mp3 || !meep_media_mp3_len) { mp_note_set("no boot module"); return 0; }
    mp3h = meep_mp3_open(meep_media_mp3, meep_media_mp3_len);
    if (!mp3h) { mp_note_set("decode failed"); return 0; }
    mp_rate = (int)meep_mp3_rate(mp3h);
    mp_ch = meep_mp3_channels(mp3h);
    if (mp_ch < 1) mp_ch = 1;
    if (mp_rate <= 0) mp_rate = 48000;
  }
  if (hda_init() != 0) { mp_note_set("no hda codec"); return 0; }
  meep_mp3_seek(mp3h, 0);
  mp_phase = 0;
  mp_src_frames = 0;
  if (hda_stream_start() != 0) { mp_note_set("stream start failed"); return 0; }
  mp_note_set("playing sample.mp3");
  mp_playing = 1;
  return 1;
}

/* Hands the codec one chunk per call: decode -> resample to 48 kHz -> write
   into the running stream. Integer phase only; a float accumulator went NaN
   here once and killed playback without a word. */
static void mp_pump(void) {
  if (!mp_playing || !mp3h) return;
  int free_bytes = hda_stream_free();
  for (int guard = 0; guard < 64 && free_bytes >= 4096; guard++) {
    if (mp_src_frames < 2) {
      int got = meep_mp3_read(mp3h, mp_src, 4096);
      if (got <= 0) {
        mp_playing = 0;
        hda_stream_stop();
        mp_note_set("end of track");
        return;
      }
      mp_src_frames = got / mp_ch;
      mp_phase = 0;
      if (mp_src_frames < 2) return;
    }
    uint32_t step = ((uint32_t)mp_rate << 16) / 48000u;
    if (step == 0) step = 65536u;
    int o = 0;
    uint32_t ph = mp_phase;
    int idx = (int)(ph >> 16);
    while (o < 1024 && idx + 1 < mp_src_frames) {
      int frac = (int)((ph >> 8) & 0xFF);
      for (int c = 0; c < 2; c++) {
        int sc = (c < mp_ch) ? c : mp_ch - 1;
        int a = mp_src[idx * mp_ch + sc];
        int b = mp_src[(idx + 1) * mp_ch + sc];
        mp_out[o * 2 + c] = (int16_t)(a + (((b - a) * frac) >> 8));
      }
      o++;
      ph += step;
      idx = (int)(ph >> 16);
    }
    mp_phase = ph;   /* position within this source buffer */
    if (idx + 1 >= mp_src_frames) { mp_src_frames = 0; mp_phase = 0; }
    if (o <= 0) return;
    int w = hda_stream_write(mp_out, o * 4);
    if (w <= 0) return;
    free_bytes -= w;
  }
}

/* samples are interleaved: divide by rate * channels, or a stereo track
   reports twice its real elapsed time while the percent bar stays correct. */
static void mp_time(uint64_t frames, int rate, int ch, char *out) {
  uint32_t secs = (rate > 0 && ch > 0) ? (uint32_t)(frames / (uint64_t)(rate * ch)) : 0;
  int i = 0;
  out[i++] = (char)('0' + (secs / 60) / 10);
  out[i++] = (char)('0' + (secs / 60) % 10);
  out[i++] = ':';
  out[i++] = (char)('0' + (secs % 60) / 10);
  out[i++] = (char)('0' + (secs % 60) % 10);
  out[i] = 0;
}

static void draw_media(void) {
  meep_app_t *a = &mapp[APP_MEDIA];
  app_win(a->x, a->y, a->w, a->h, "Media player",
          mp_playing ? "playing" : "idle", mapp_focus == APP_MEDIA);
  int x = a->x + 14, y = a->y + APP_TITLE_H + 14;

  meep_tracked(x, y, "SAMPLE.MP3", MEEP_INK_SOFT);
  y += 22;
  meep_text(&meep_s130, x, y, "sample.mp3", MEEP_INK);
  y += 26;

  char pos[8], pct[8];
  uint64_t done = mp3h ? meep_mp3_pos(mp3h) : 0;
  uint64_t tf = mp3h ? meep_mp3_total(mp3h) : 0;
  int pm = tf > 0 ? (int)((done * 1000) / tf) : 0;   /* permille */
  mp_time(done, mp_rate, mp_ch, pos);
  { int pc = pm / 10;               /* permille -> percent */
    int i = 0;
    if (pc >= 100) pct[i++] = (char)('0' + (pc / 100) % 10);
    if (pc >= 10) pct[i++] = (char)('0' + (pc / 10) % 10);
    pct[i++] = (char)('0' + pc % 10);
    pct[i++] = '%';
    pct[i] = 0; }

  meep_text(&meep_m100, x, y, "intel hda - 48 khz stereo", MEEP_INK_SOFT);
  y += 20;

  /* progress bar */
  int bw = a->w - 28;
  meep_box(x, y, bw, 6, 3, MEEP_LINE, MEEP_PAPER3);
  if (mp3h) {
    int fill = (bw - 4) * pm / 1000;
    if (fill > 0) meep_box(x + 2, y + 1, fill, 4, 2, 0, MEEP_ACCENT);
  }
  y += 14;
  meep_text(&meep_m100, x, y, pos, MEEP_INK);
  meep_text(&meep_m100, x + 60, y, pct, MEEP_INK_SOFT);
  y += 22;

  /* play / stop */
  int pw = 74, ph = 24;
  meep_box(x, y, pw, ph, 7, MEEP_LINE, MEEP_PAPER3);
  meep_text(&meep_m100, x + 14, y + (ph - meep_line_h(&meep_m100)) / 2,
            mp_playing ? "stop" : "play", MEEP_INK);

  meep_text(&meep_m100, x, a->y + a->h - 20, mp_note, MEEP_INK_SOFT);
}

/* ---- Omi Net ----------------------------------------------------------
   A real browser needs three things this has: a TCP stream, an HTTP client
   (net/http.c) and something to paint. There is no TLS, so https-only hosts
   are refused up front rather than failing mysteriously; the renderer is a
   tag stripper that keeps block breaks, not a layout engine. */
#define ON_MAX 40000
static char on_url[192] = "example.com";
static char on_text[ON_MAX];
static char on_lines[400][92];
static int  on_nlines;
static int  on_state;          /* 0 idle, 1 loading, 2 ok, 3 failed */
static int  on_scroll;
static int  on_caret;
static int  on_status;
static http_result_t on_res;

static int on_lc(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int on_tag(const char *s, int len, int i, const char *name, int *closing) {
  int n = 0;
  while (name[n]) n++;
  int at = i + 1;
  *closing = 0;
  if (at < len && s[at] == '/') { *closing = 1; at++; }
  if (at + n > len) return 0;
  for (int k = 0; k < n; k++)
    if (on_lc(s[at + k]) != on_lc(name[k])) return 0;
  char after = (at + n < len) ? s[at + n] : ' ';
  return after == ' ' || after == '>' || after == '/' || after == '\r' ||
         after == '\n' || after == '\t';
}

/* ---- page rendering -------------------------------------------------- */
/* The renderer in gui/omiweb.c owns layout; it only needs to measure text,
 * which is why the font bitmaps stay in this file. */
#define ON_PAD_L 14

static const mface *web_faces(int f) {
  switch (f) {
    case 1: return &meep_s145;
    case 2: return &meep_m100;
    case 3: return &meep_m105;
    case 4: return &meep_m115;
    default: return &meep_s130;
  }
}

static int web_measure(int font, const char *s) { return meep_text_w(web_faces(font), s); }

/* re-flow the current document for a new viewport width - no refetch */
static void web_relayout(void) {
  if (on_state != 2 || !on_text[0]) return;
  int cw = mapp[APP_BROWSER].w - 2 * ON_PAD_L;
  if (cw < 120) cw = 120;
  web_image_provider(on_page_image);
  if (web_build(on_text, (int)strlen(on_text), cw, web_measure) == 0) on_nlines = web_height();
}


/* page images: bilinear, 16.16 fixed point, clipped to the window.
 * Weights are 0..256 in each axis and the four of them sum to 65536, so the
 * accumulation must be shifted by 16 - dividing by 256 lets a channel spill
 * into the next one and the result looks like noise. */
static void blit_scaled(int dx, int dy, int dw, int dh, const uint32_t *src,
                        int sw, int sh, int clip_y, int clip_b) {
  if (!backbuffer || !src || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
  uint32_t sx = (uint32_t)((sw << 16) / dw), sy = (uint32_t)((sh << 16) / dh);
  int W = (int)screen_width;
  for (int y = 0; y < dh; y++) {
    int py = dy + y;
    if (py < clip_y || py >= clip_b) continue;
    uint32_t fy = (uint32_t)y * sy;
    int y0 = (int)(fy >> 16), y1 = y0 + 1 < sh ? y0 + 1 : sh - 1;
    int wy = (int)((fy >> 8) & 0xFF);
    const uint32_t *r0 = src + (size_t)y0 * sw, *r1 = src + (size_t)y1 * sw;
    uint32_t *dst = backbuffer + (size_t)py * W;
    for (int x = 0; x < dw; x++) {
      int px = dx + x;
      if (px < 0 || px >= W) continue;
      uint32_t fx = (uint32_t)x * sx;
      int x0 = (int)(fx >> 16), x1 = x0 + 1 < sw ? x0 + 1 : sw - 1;
      int wx = (int)((fx >> 8) & 0xFF);
      int w00 = (256 - wx) * (256 - wy), w01 = wx * (256 - wy);
      int w10 = (256 - wx) * wy,        w11 = wx * wy;
      uint32_t c00 = r0[x0], c01 = r0[x1], c10 = r1[x0], c11 = r1[x1];
      uint32_t r = (uint32_t)((((c00 >> 16) & 0xFF) * w00 + ((c01 >> 16) & 0xFF) * w01 +
                               ((c10 >> 16) & 0xFF) * w10 + ((c11 >> 16) & 0xFF) * w11) >> 16);
      uint32_t g = (uint32_t)((((c00 >> 8) & 0xFF) * w00 + ((c01 >> 8) & 0xFF) * w01 +
                               ((c10 >> 8) & 0xFF) * w10 + ((c11 >> 8) & 0xFF) * w11) >> 16);
      uint32_t b = (uint32_t)(((c00 & 0xFF) * w00 + (c01 & 0xFF) * w01 +
                               (c10 & 0xFF) * w10 + (c11 & 0xFF) * w11) >> 16);
      if (r > 255) r = 255;
      if (g > 255) g = 255;
      if (b > 255) b = 255;
      dst[px] = (r << 16) | (g << 8) | b;
    }
  }
}


static const web_image_t *on_page_image(const char *src, int len) {
  static web_image_t v;
  const oweb_image_t *im = webimg_get(src, len);
  if (!im) return 0;
  v.w = im->w; v.h = im->h; v.px = im->px;
  return &v;
}

static void draw_web(int x, int y, int bottom) {
  int n = web_count();
  const web_op_t *ops = web_ops();
  for (int i = 0; i < n; i++) {
    const web_op_t *o = &ops[i];
    int oy = y + o->y - on_scroll;
    int oh = o->h > 0 ? o->h : 4;
    if (oy > bottom) continue;
    if (oy + oh < y - 2) continue;
    if (o->kind == 0) {
      if (o->h <= 0) continue;
      meep_box(x + o->x, oy, o->w, o->h, o->radius, o->color, o->color);
    } else if (o->kind == 3 && o->px) {
      blit_scaled(x + o->x, oy, o->w, o->h, o->px, o->iw, o->ih, y, bottom);
    } else if (o->kind == 2) {
      meep_hline(x + o->x, oy, o->w, 1, o->color);
    } else if (o->text) {
      const mface *f = web_faces(o->font);
      meep_text(f, x + o->x, oy, o->text, o->color);
      if (o->bold) meep_text(f, x + o->x + 1, oy, o->text, o->color);
    }
  }
}


static void on_load(void) {
  on_scroll = 0;
  on_state = 1;
  on_nlines = 0;
  on_text[0] = 0;
  const net_cfg_t *cfg = net_get();
  if (!cfg->valid || !e1000_link()) { on_state = 3; on_status = 0; return; }
  char url[256];
  int u = 0;
  if (!(on_url[0] == 'h' && on_url[1] == 't')) {
    const char *p = "http://";
    while (*p) url[u++] = *p++;
  }
  for (int i = 0; on_url[i] && u < 254; i++) url[u++] = on_url[i];
  url[u] = 0;
  /* paint "fetching..." before the blocking request, or the desktop just freezes */
  gui_compose();
  if (!http_get(cfg, url, on_text, ON_MAX, &on_res)) {
    on_state = 3;
    on_status = on_res.status;        /* -1 dns, -2 tcp, -3 send, -4 not http, -5 tls */
    return;
  }
  on_status = on_res.status;
  on_state = (on_res.status == 200) ? 2 : 3;
  int n = on_res.len;
  if (n <= 0) n = 0;
  if (n > ON_MAX - 1) n = ON_MAX - 1;
  on_text[n] = 0;
  {
    int cw = mapp[APP_BROWSER].w - 2 * ON_PAD_L;
    if (cw < 120) cw = 120;
    web_image_provider(on_page_image);
    if (web_build(on_text, n, cw, web_measure) != 0) { on_state = 3; on_status = -4; return; }

  }
  on_nlines = web_height();
}

static void draw_browser(void) {
  meep_app_t *a = &mapp[APP_BROWSER];
  const char *state = "idle";
  if (on_state == 1) state = "loading";
  else if (on_state == 2) state = "live";
  else if (on_state == 3) state = "no page";
  app_win(a->x, a->y, a->w, a->h, "Omi Net", state, mapp_focus == APP_BROWSER);

  int ty = a->y + APP_TITLE_H;

  /* address pill: typed text, with a caret when focused */
  int uy = ty + 10, uw = a->w - 20;
  meep_box(a->x + 10, uy, uw, 24, 12, MEEP_LINE, MEEP_PAPER2);
  int tw = meep_text_w(&meep_m100, on_url);
  int avail = uw - 22;
  int skip = 0;
  while (tw > avail && skip < (int)sizeof(on_url)) {
    skip++;
    tw = meep_text_w(&meep_m100, on_url + skip);
  }
  meep_text(&meep_m100, a->x + 16, uy + (24 - meep_line_h(&meep_m100)) / 2,
            on_url + skip, MEEP_INK);
  if (mapp_focus == APP_BROWSER && ((on_caret / 20) & 1) == 0)
    gui_draw_line(a->x + 17 + tw, uy + 6, a->x + 17 + tw, uy + 18, MEEP_INK);

  meep_hline(a->x + 1, uy + 30, a->w - 2, 1, MEEP_LINE_SOFT);

  int bx = a->x + 16, byy = uy + 42;
  int bottom = a->y + a->h - 26;

  if (on_state == 0) {
    meep_text(&meep_m115, bx, byy, "type a url, press enter", MEEP_INK);
    byy += 24;
    meep_text(&meep_m100, bx, byy, "http and https both work.", MEEP_INK_SOFT);
    byy += 16;
    meep_text(&meep_m100, bx, byy, "bookmarks load, or type and press enter.", MEEP_INK_SOFT);
  } else if (on_state == 1) {
    meep_text(&meep_m115, bx, byy, "fetching...", MEEP_INK);
  } else if (on_state == 3) {
    meep_text(&meep_m115, bx, byy, "could not load that page", MEEP_INK);
    byy += 22;
    const char *why = "the reply did not parse.",
                   *hint = "try another host.";
    if (on_status == -1) { why = "dns has no address for that host."; hint = "check the name, or add a dot-com."; }
    else if (on_status == -2) { why = "the host never opened a port."; hint = "site down or blocked."; }
    else if (on_status == -3) { why = "the connection died mid request."; hint = "try again."; }
    else if (on_status == -5) {
      why = on_res.err[0] ? on_res.err : "the secure connection failed.";
      hint = "no page was trusted, so nothing was shown.";
    }
    else if (on_status > 0) { why = "the server refused to serve the page."; hint = "it answered, but not with 200."; }
    meep_text(&meep_m100, bx, byy, why, MEEP_INK_SOFT);
    byy += 16;
    meep_text(&meep_m100, bx, byy, hint, MEEP_INK_SOFT);
  } else if (on_state == 2) {
    /* the page gets its own paper: html assumes a light canvas */
    meep_box(a->x + 1, byy - 2, a->w - 2, bottom - byy + 4, 0, 0xFCFAF4, 0xFCFAF4);
    draw_web(a->x + ON_PAD_L, byy, bottom);
  }

  /* status strip, like the web shell's .mn-status */
  char foot[84];
  int f = 0;
  const char *p = "http  ";
  while (*p) foot[f++] = *p++;
  if (on_status > 0) {
    int v = on_status, d[4], n = 0;
    if (!v) d[n++] = 0;
    while (v) { d[n++] = v % 10; v /= 10; }
    while (n) foot[f++] = (char)('0' + d[--n]);
  } else if (on_status < 0) {
    const char *q = "failed";
    while (*q) foot[f++] = *q++;
  } else foot[f++] = '-';
  p = "   page ";
  while (*p) foot[f++] = *p++;
  { int v = on_nlines, d[6], n = 0; if (!v) d[n++] = 0;
    while (v) { d[n++] = v % 10; v /= 10; }
    while (n) foot[f++] = (char)('0' + d[--n]); }
  p = "px   at ";
  while (*p) foot[f++] = *p++;
  { int v = on_scroll, d[6], n = 0; if (!v) d[n++] = 0;
    while (v) { d[n++] = v % 10; v /= 10; }
    while (n) foot[f++] = (char)('0' + d[--n]); }
  foot[f] = 0;
  meep_hline(a->x + 1, a->y + a->h - 24, a->w - 2, 1, MEEP_LINE_SOFT);
  meep_text(&meep_m100, a->x + 14, a->y + a->h - 18, foot, MEEP_INK_SOFT);
}

/* ---- one z-order for windows *and* widgets ---------------------------- */
/* Everything on the desktop lives in a single back-to-front list: clicking a
   window or a widget moves it to the top, exactly like a real window manager. */
#define Z_WG 100
static int zl[APP_COUNT + WG_COUNT];
static int zl_n;

static void z_init(void) {
  zl_n = 0;
  for (int i = 1; i < APP_COUNT; i++) zl[zl_n++] = i;
  for (int i = 0; i < WG_COUNT; i++) zl[zl_n++] = Z_WG + i;
}

static void z_top(int e) {
  if (zl_n == 0) z_init();
  int k = -1;
  for (int i = 0; i < zl_n; i++) if (zl[i] == e) { k = i; break; }
  if (k < 0) return;
  for (int i = k; i < zl_n - 1; i++) zl[i] = zl[i + 1];
  zl[zl_n - 1] = e;
}

/* ---- app/widget geometry + hit testing --------------------------------- */
static void app_defaults(int id, int *x, int *y, int *w, int *h) {
  switch (id) {
    case APP_CALC:  *x = 120; *y = 320; *w = 250; *h = APP_TITLE_H + 12 + 64 + 10 + 5 * 40 + 4 * 6 + 14; break;
    case APP_NOTES: *x = 400; *y = 300; *w = 430; *h = 360; break;
    case APP_MEDIA: *x = 300; *y = 190; *w = 420; *h = 260; break;
    case APP_BROWSER: *x = 230; *y = 150; *w = 470; *h = 300; break;
    case APP_TASKS:   *x = 250; *y = 170; *w = 460; *h = 300; break;
    default:        *x = 240; *y = 140; *w = 520; *h = 340; break;
  }
}

static void app_open(int id) {
  if (id <= APP_NONE || id >= APP_COUNT) return;
  meep_app_t *a = &mapp[id];
  if (!a->open) {
    int x, y, w, h;
    app_defaults(id, &x, &y, &w, &h);
    a->open = 1; a->x = x; a->y = y; a->w = w; a->h = h; a->min = 0;
    app_max[id] = 0;
  }
  a->min = 0;
  mapp_focus = id;
  wg_focus = -1;
  z_top(id);
  needs_redraw = 1;
}

static void app_toggle(int id) {
  if (id <= APP_NONE || id >= APP_COUNT) return;
  if (mapp[id].open) {
    mapp[id].open = 0;
    if (mapp_focus == id) mapp_focus = APP_NONE;
  } else {
    app_open(id);
  }
  needs_redraw = 1;
}

static void wg_toggle(int i) {
  if (i < 0 || i >= WG_COUNT) return;
  mwg[i].on = !mwg[i].on;
  if (mwg[i].on) {
    meep_wg_t *g = &mwg[i];
    /* a widget must never come back off-screen: clamp it above the taskbar */
    int bottom = (int)screen_height - dock_height - 8;
    if (g->x < 8) g->x = 8;
    if (g->x + g->w > (int)screen_width - 8) g->x = (int)screen_width - 8 - g->w;
    if (g->y < 8) g->y = 8;
    if (g->y + g->h > bottom) g->y = bottom - g->h;
    wg_focus = i;               /* shown widgets take focus, so they paint */
    z_top(Z_WG + i);
  } else if (wg_focus == i) {
    wg_focus = -1;
  }
  needs_redraw = 1;
  full_redraw = 1;              /* a newly shown widget is not in any dirty rect */
}

static int app_at(int x, int y) {
  /* focused first, then the rest */
  if (mapp_focus != APP_NONE) {
    meep_app_t *a = &mapp[mapp_focus];
    if (a->open && x >= a->x && x < a->x + a->w && y >= a->y && y < a->y + a->h)
      return mapp_focus;
  }
  for (int id = 1; id < APP_COUNT; id++) {
    meep_app_t *a = &mapp[id];
    if (!a->open || a->min) continue;
    if (x >= a->x && x < a->x + a->w && y >= a->y && y < a->y + a->h) return id;
  }
  return APP_NONE;
}

static int wg_at(int x, int y) {
  for (int i = WG_COUNT - 1; i >= 0; i--) {
    if (!mwg[i].on) continue;
    if (x >= mwg[i].x && x < mwg[i].x + mwg[i].w &&
        y >= mwg[i].y && y < mwg[i].y + mwg[i].h) return i;
  }
  return -1;
}

/* returns 1 when the click was consumed by an app or a widget */

/* ===================================================================== */
/* Taskbar: one Windows-style bar along the bottom, carrying the start    */
/* menu button (the old top-left logo lockup), a button per running app,  */
/* and the clock at the right. It replaces the top menubar and the        */
/* floating dock completely - there is no second bar anywhere.            */
/* ===================================================================== */

#define TB_BASE_H   44
#define TB_START_W 136
#define SM_W_BASE  244
#define SM_ROW_H    32
#define SM_SEP_H     9

static int start_open = 0, start_hover = -1, tb_hover = -2;

static int taskbar_h(void) { return UI_SCALE_VAL(TB_BASE_H); }
static int taskbar_y(void) { return (int)screen_height - taskbar_h(); }

static int app_icon(int id) {
  switch (id) {
    case APP_CALC:    return ICON_CALC;
    case APP_NOTES:   return ICON_NOTES;
    case APP_MEDIA:   return ICON_MEDIA;
    case APP_BROWSER: return ICON_BROWSER;
    case APP_TASKS:   return ICON_TASKS;
    default:          return ICON_APPS;
  }
}

static const char *app_title(int id) {
  switch (id) {
    case APP_CALC:    return "calculator";
    case APP_NOTES:   return "notes";
    case APP_MEDIA:   return "media player";
    case APP_BROWSER: return "omi net";
    case APP_TASKS:   return "task manager";
  }
  return "";
}

/* left-to-right walk of the running-app buttons. Used by the draw pass, the
   1x text pass and every hit test, so they can never disagree. */
static int tb_walk(int *ids, int *xs, int *ws) {
  int s = ui_scale;
  int cx = UI_SCALE_VAL(8) + UI_SCALE_VAL(TB_START_W) + UI_SCALE_VAL(8);
  int right_limit = (int)screen_width - UI_SCALE_VAL(150);   /* leave the clock clear */
  int n = 0;
  for (int id = 1; id < APP_COUNT; id++) {
    if (!mapp[id].open) continue;
    int w = UI_SCALE_VAL(28) + meep_text_w(&meep_s130, app_title(id)) * s +
            UI_SCALE_VAL(14);
    if (cx + w > right_limit) break;
    if (ids) ids[n] = id;
    if (xs) xs[n] = cx;
    if (ws) ws[n] = w;
    n++;
    cx += w + UI_SCALE_VAL(4);
  }
  return n;
}

static int start_button_hit(int x, int y) {
  if (y < taskbar_y()) return 0;
  return x >= UI_SCALE_VAL(8) && x < UI_SCALE_VAL(8) + UI_SCALE_VAL(TB_START_W);
}

/* ---- start menu ------------------------------------------------------- */
/* kind: 0 = separator, 1 = app, 2 = widget, 3 = appearance */
typedef struct { const char *label; int kind; int arg; } sm_item;

static const sm_item sm_rows[] = {
  {"calculator",     1, APP_CALC},
  {"notes",          1, APP_NOTES},
  {"media player",   1, APP_MEDIA},
  {"omi net",        1, APP_BROWSER},
  {"task manager",   1, APP_TASKS},
  {0, 0, 0},
  {"clock widget",   2, WG_CLOCK},
  {"weather widget", 2, WG_WEATHER},
  {"sticky note",    2, WG_STICKY},
  {0, 0, 0},
  {"appearance",     3, 0},
};
#define SM_N ((int)(sizeof(sm_rows) / sizeof(sm_rows[0])))

static int sm_h(void) {
  int h = UI_SCALE_VAL(12);
  for (int i = 0; i < SM_N; i++)
    h += sm_rows[i].label ? UI_SCALE_VAL(SM_ROW_H) : UI_SCALE_VAL(SM_SEP_H);
  return h;
}

static void sm_rect(int *x, int *y, int *w, int *h) {
  *x = UI_SCALE_VAL(8);
  *w = UI_SCALE_VAL(SM_W_BASE);
  *h = sm_h();
  *y = taskbar_y() - *h - UI_SCALE_VAL(6);
}

static int sm_row_y(int i) {
  int y = taskbar_y() - sm_h() - UI_SCALE_VAL(6) + UI_SCALE_VAL(6);
  for (int k = 0; k < i; k++)
    y += sm_rows[k].label ? UI_SCALE_VAL(SM_ROW_H) : UI_SCALE_VAL(SM_SEP_H);
  return y;
}

static int sm_row_at(int x, int y) {
  int px, py, pw, ph;
  sm_rect(&px, &py, &pw, &ph);
  if (x < px || x >= px + pw || y < py || y >= py + ph) return -1;
  for (int i = 0; i < SM_N; i++) {
    if (!sm_rows[i].label) continue;
    int ry = sm_row_y(i);
    if (y >= ry && y < ry + UI_SCALE_VAL(SM_ROW_H)) return i;
  }
  return -1;
}

static void sm_activate(int i) {
  if (i < 0 || i >= SM_N || !sm_rows[i].label) return;
  if (sm_rows[i].kind == 1) app_toggle(sm_rows[i].arg);
  else if (sm_rows[i].kind == 2) wg_toggle(sm_rows[i].arg);
  else if (sm_rows[i].kind == 3) meep_apply_appearance(!appearance_dark);
}

/* state string on the right of a row, so the menu says what it will do */
static const char *sm_state(int i) {
  if (sm_rows[i].kind == 1) return mapp[sm_rows[i].arg].open ? "open" : "";
  if (sm_rows[i].kind == 2) return mwg[sm_rows[i].arg].on ? "on" : "off";
  if (sm_rows[i].kind == 3) return appearance_dark ? "midnight" : "paper";
  return "";
}

/* ---- drawing (supersampled pass: shapes only) -------------------------- */
static void tb_brand_glyph(int gx, int gy, int gs) {
  gui_draw_rounded_rect(gx, gy, gs, gs, UI_SCALE_VAL(5), MEEP_ACCENT);
  int pad = UI_SCALE_VAL(4), gap = UI_SCALE_VAL(2);
  int cell = (gs - pad * 2 - gap) / 2;
  for (int i = 0; i < 4; i++) {
    int cx = gx + pad + (i % 2) * (cell + gap);
    int cy = gy + pad + (i / 2) * (cell + gap);
    fb_fill_rect(cx, cy, cell, cell, MEEP_ACCENT_INK);
  }
}

static void draw_start_menu(void) {
  if (!start_open) return;
  int px, py, pw, ph;
  sm_rect(&px, &py, &pw, &ph);
  meep_box(px, py, pw, ph, UI_SCALE_VAL(10), MEEP_LINE, MEEP_PAPER2);

  for (int i = 0; i < SM_N; i++) {
    int ry = sm_row_y(i);
    if (!sm_rows[i].label) {
      fb_fill_rect(px + UI_SCALE_VAL(8), ry + UI_SCALE_VAL(SM_SEP_H) / 2,
                   pw - UI_SCALE_VAL(16), UI_SCALE_VAL(1), MEEP_LINE_SOFT);
      continue;
    }
    if (i == start_hover)
      meep_box(px + UI_SCALE_VAL(4), ry, pw - UI_SCALE_VAL(8),
               UI_SCALE_VAL(SM_ROW_H), UI_SCALE_VAL(6), MEEP_ACCENT, MEEP_ACCENT);
    /* icon set: apps and widgets get their Remix glyph, appearance gets the
       palette; separators have none */
    int ic = -1;
    if (sm_rows[i].kind == 1) {
      static const int app_ic[] = { ICON_CALC, ICON_NOTES, ICON_MEDIA, ICON_BROWSER, ICON_TASKS };
      int a = sm_rows[i].arg - 1;   /* app ids are 1-based (APP_NONE == 0) */
      ic = (a >= 0 && a < 5) ? app_ic[a] : ICON_APPS;
    } else if (sm_rows[i].kind == 2) {
      static const int wg_ic[] = { ICON_STICKY, ICON_WEATHER, ICON_CLOCK };
      int w = sm_rows[i].arg;
      ic = (w >= 0 && w < 3) ? wg_ic[w] : ICON_APPS;
    } else if (sm_rows[i].kind == 3) {
      ic = ICON_APPEARANCE;
    }
    if (ic >= 0)
      meep_icon(ic, px + UI_SCALE_VAL(12),
                ry + (UI_SCALE_VAL(SM_ROW_H) - UI_SCALE_VAL(15)) / 2, 15,
                (i == start_hover) ? MEEP_ACCENT_INK : MEEP_INK_SOFT);
  }
}

static void draw_taskbar(void) {
  int th = taskbar_h(), ty = taskbar_y(), W = (int)screen_width;
  fb_fill_rect(0, ty, W, th, MEEP_PAPER2);
  fb_fill_rect(0, ty, W, UI_SCALE_VAL(1), MEEP_LINE);

  /* start button: the brand lockup from the old menubar, now bottom-left */
  int sx = UI_SCALE_VAL(8), sw = UI_SCALE_VAL(TB_START_W);
  int sy = ty + UI_SCALE_VAL(4), sh = th - UI_SCALE_VAL(8);
  if (start_open || tb_hover == -1) {
    uint32_t fill = (tb_hover == -1 && !start_open) ? MEEP_PAPER3 : MEEP_PRESS;
    meep_box(sx, sy, sw, sh, UI_SCALE_VAL(8), MEEP_LINE_SOFT, fill);
  }
  int gs = UI_SCALE_VAL(19);
  int gx = sx + UI_SCALE_VAL(10);
  tb_brand_glyph(gx, ty + (th - gs) / 2, gs);

  /* wordmark is a single word now: no badge box to reserve space for */

  /* running apps */
  int ids[APP_COUNT], xs[APP_COUNT], ws[APP_COUNT];
  int n = tb_walk(ids, xs, ws);
  for (int i = 0; i < n; i++) {
    int id = ids[i];
    uint32_t fill = (mapp_focus == id) ? MEEP_PAPER3 : MEEP_PAPER2;
    uint32_t bord = (mapp_focus == id) ? MEEP_LINE : MEEP_LINE_SOFT;
    if (tb_hover == id) fill = MEEP_PRESS;
    meep_box(xs[i], sy, ws[i], sh, UI_SCALE_VAL(8), bord, fill);
    meep_icon(app_icon(id), xs[i] + UI_SCALE_VAL(8),
              sy + (sh - UI_SCALE_VAL(16)) / 2, 16,
              (mapp_focus == id) ? MEEP_INK : MEEP_INK_SOFT);
    /* the accent rule under a focused app is the "this one is active" cue */
    if (mapp_focus == id)
      fb_fill_rect(xs[i] + UI_SCALE_VAL(12), ty + th - UI_SCALE_VAL(5),
                   ws[i] - UI_SCALE_VAL(24), UI_SCALE_VAL(2), MEEP_ACCENT);
  }
}

/* ---- text: stamped at 1x so the atlas' own coverage survives ----------- */
static void draw_taskbar_text(void) {
  int th = taskbar_h(), ty = taskbar_y();
  int sx = UI_SCALE_VAL(8);
  int gs = UI_SCALE_VAL(19);
  int gx = sx + UI_SCALE_VAL(10);
  int word_x = gx + gs + UI_SCALE_VAL(7);
  int word_y = ty + (th - meep_line_h(&meep_s145)) / 2;
  meep_text(&meep_s145, word_x, word_y, BRAND_WORD, MEEP_INK);

  int ids[APP_COUNT], xs[APP_COUNT], ws[APP_COUNT];
  int n = tb_walk(ids, xs, ws);
  for (int i = 0; i < n; i++) {
    int id = ids[i];
    uint32_t col = (mapp_focus == id) ? MEEP_INK : MEEP_INK_SOFT;
    meep_text(&meep_s130, xs[i] + UI_SCALE_VAL(28),
              ty + (th - meep_line_h(&meep_s130)) / 2, app_title(id), col);
  }

  /* clock, bottom-right */
  char clock[32];
  meep_clock_text(clock);
  int cw = meep_text_w(&meep_m115, clock);
  meep_text(&meep_m115, (int)screen_width - UI_SCALE_VAL(16) - cw,
            ty + (th - meep_line_h(&meep_m115)) / 2, clock, MEEP_INK);

  /* the start menu's own labels */
  if (!start_open) return;
  int px, py, pw, ph;
  sm_rect(&px, &py, &pw, &ph);
  for (int i = 0; i < SM_N; i++) {
    if (!sm_rows[i].label) continue;
    int ry = sm_row_y(i);
    int ly = ry + (UI_SCALE_VAL(SM_ROW_H) - meep_line_h(&meep_s130)) / 2;
    uint32_t col = (i == start_hover) ? MEEP_ACCENT_INK : MEEP_INK;
    meep_text(&meep_s130, px + UI_SCALE_VAL(35), ly, sm_rows[i].label, col);
    const char *st = sm_state(i);
    if (st && st[0]) {
      uint32_t scol = (i == start_hover) ? MEEP_ACCENT_INK : MEEP_INK_SOFT;
      meep_text(&meep_m105, px + pw - UI_SCALE_VAL(14) - meep_text_w(&meep_m105, st),
                ry + (UI_SCALE_VAL(SM_ROW_H) - meep_line_h(&meep_m105)) / 2, st, scol);
    }
  }
}


/* ---- taskbar context menu (right-click, like windows) ------------------ */
static int SX;          /* supersample factor, defined with the ss buffer below */
#define CTM_W_BASE 200
#define CTM_ROW_H   28

static int ctm_open = 0, ctm_x = 0, ctm_y = 0, ctm_hover = -1;

static const char *const ctm_items[] = {
  "task manager", "appearance", "close all windows"
};
#define CTM_N ((int)(sizeof(ctm_items) / sizeof(ctm_items[0])))

static int ctm_h(void) { return UI_SCALE_VAL(CTM_ROW_H) * CTM_N + UI_SCALE_VAL(10); }

/* Anchored to the click, opening upwards like a taskbar menu; clamped on screen.
 * The anchor is stored in 1x pixels, so a caller drawing into the supersampled
 * buffer passes that buffer's scale as k (the same trick draw_cursor uses). */
static void ctm_rect_k(int *x, int *y, int *w, int *h, int k) {
  int cw = UI_SCALE_VAL(CTM_W_BASE), ch = ctm_h();
  int px = ctm_x * k;
  /* sit entirely above the bar: a menu that overlaps it gets clipped by the
     chrome pass, and the bottom row would be unreadable */
  int py = taskbar_y() * k - ch - UI_SCALE_VAL(6);
  if (py < UI_SCALE_VAL(8)) py = UI_SCALE_VAL(8);
  if (px + cw > (int)screen_width - UI_SCALE_VAL(8))
    px = (int)screen_width - UI_SCALE_VAL(8) - cw;
  if (px < UI_SCALE_VAL(8)) px = UI_SCALE_VAL(8);
  *x = px; *y = py; *w = cw; *h = ch;
}
static void ctm_rect(int *x, int *y, int *w, int *h) {
  ctm_rect_k(x, y, w, h, 1);
}

static int ctm_row_at(int x, int y) {
  int px, py, pw, ph;
  ctm_rect(&px, &py, &pw, &ph);
  if (x < px || x >= px + pw || y < py || y >= py + ph) return -1;
  int r = (y - py - UI_SCALE_VAL(5)) / UI_SCALE_VAL(CTM_ROW_H);
  if (r < 0 || r >= CTM_N) return -1;
  return r;
}

static void ctm_activate(int r) {
  ctm_open = 0;
  if (r == 0) {
    app_open(APP_TASKS);
    mapp_focus = APP_TASKS;
    z_top(APP_TASKS);
  } else if (r == 1) {
    meep_apply_appearance(!appearance_dark);
  } else if (r == 2) {
    for (int i = 1; i < APP_COUNT; i++) mapp[i].open = 0;
    if (mapp_focus > 0) mapp_focus = APP_NONE;
  }
  needs_redraw = 1;
  full_redraw = 1;
}

static void draw_ctm(void) {
  if (!ctm_open) return;
  int px, py, pw, ph;
  ctm_rect_k(&px, &py, &pw, &ph, SX);
  meep_box(px, py, pw, ph, UI_SCALE_VAL(8), MEEP_LINE, MEEP_PAPER2);
  for (int i = 0; i < CTM_N; i++) {
    if (i != ctm_hover) continue;
    meep_box(px + UI_SCALE_VAL(4), py + UI_SCALE_VAL(5) + i * UI_SCALE_VAL(CTM_ROW_H),
             pw - UI_SCALE_VAL(8), UI_SCALE_VAL(CTM_ROW_H), UI_SCALE_VAL(6),
             MEEP_ACCENT, MEEP_ACCENT);
  }
}

static void draw_ctm_text(void) {
  if (!ctm_open) return;
  int px, py, pw, ph;
  ctm_rect(&px, &py, &pw, &ph);
  for (int i = 0; i < CTM_N; i++) {
    int ry = py + UI_SCALE_VAL(5) + i * UI_SCALE_VAL(CTM_ROW_H);
    int ly = ry + (UI_SCALE_VAL(CTM_ROW_H) - meep_line_h(&meep_s130)) / 2;
    uint32_t col = (i == ctm_hover) ? MEEP_ACCENT_INK : MEEP_INK;
    meep_text(&meep_s130, px + UI_SCALE_VAL(14), ly, ctm_items[i], col);
  }
}

/* Ctrl+Alt+Del behaves like windows: raise it if it is already open. */
static void open_task_manager(void) {
  if (!mapp[APP_TASKS].open) app_open(APP_TASKS);
  else mapp[APP_TASKS].min = 0;
  mapp_focus = APP_TASKS;
  z_top(APP_TASKS);
  start_open = 0;
  ctm_open = 0;
  needs_redraw = 1;
  full_redraw = 1;
}

/* Resolve an href against the page we are on: absolute, protocol-relative,
 * root-relative or sibling-relative. Anything else (mailto:, javascript:)
 * returns empty and the click is ignored. */
static void on_resolve_url(const char *href, char *out, int cap) {
  out[0] = 0;
  if (!href || !*href) return;
  int n = 0;
  if (!strncmp(href, "http://", 7) || !strncmp(href, "https://", 8)) {
    while (href[n] && n < cap - 1) { out[n] = href[n]; n++; }
  } else if (href[0] == '/' && href[1] == '/') {
    const char *pre = "https:";
    while (pre[0] && n < cap - 1) out[n++] = *pre++;
    while (*href && n < cap - 1) out[n++] = *href++;
  } else if (href[0] == '/') {
    const char *scheme = (!strncmp(on_url, "https://", 8)) ? "https://" : "http://";
    int sl = (int)strlen(scheme);
    for (int i = 0; i < sl && n < cap - 1; i++) out[n++] = scheme[i];
    const char *hs = on_url + sl;
    while (*hs && *hs != '/' && n < cap - 1) out[n++] = *hs++;
    while (*href && n < cap - 1) out[n++] = *href++;
  } else if (href[0] == '#') {
    return;                              /* in-page anchors are not handled */
  } else {
    const char *base = on_url;
    const char *last = 0;
    for (const char *q = base; *q; q++) if (*q == '/') last = q;
    if (!last) return;
    last++;                              /* keep the trailing slash */
    while (base < last && n < cap - 1) out[n++] = *base++;
    while (*href && n < cap - 1) out[n++] = *href++;
  }
  out[n] = 0;
}

static int apps_click(int x, int y, int button) {
  if (button == 2) return 0;
  if (zl_n == 0) z_init();

  for (int i = zl_n - 1; i >= 0; i--) {          /* topmost first: what you see is what you hit */
    int e = zl[i];
    if (e >= Z_WG) {
      int w = e - Z_WG;
      meep_wg_t *g = &mwg[w];
      if (!g->on) continue;
      if (x < g->x || x >= g->x + g->w || y < g->y || y >= g->y + g->h) continue;
      wg_focus = w;
      mapp_focus = APP_NONE;
      z_top(e);
      if (app_close_hit(x, y, g->x, g->y, g->w)) { wg_toggle(w); return 1; }
      if (y < g->y + 24) { wg_drag = w; wg_dx = x - g->x; wg_dy = y - g->y; }
      needs_redraw = 1;
      return 1;
    }
    meep_app_t *a = &mapp[e];
    if (!a->open || a->min) continue;
    if (x < a->x || x >= a->x + a->w || y < a->y || y >= a->y + a->h) continue;
    mapp_focus = e;
    wg_focus = -1;
    z_top(e);
    int wc = app_wctl_hit(x, y, a->x, a->y, a->w);
    if (wc == 3) { a->open = 0; a->min = 0; mapp_focus = APP_NONE; needs_redraw = 1; return 1; }
    if (wc == 1) { a->min = 1; mapp_focus = APP_NONE; needs_redraw = 1; return 1; }
    if (wc == 2) {
      if (app_max[e]) {
        a->x = app_saved[e][0]; a->y = app_saved[e][1];
        a->w = app_saved[e][2]; a->h = app_saved[e][3];
        app_max[e] = 0;
      } else {
        app_saved[e][0] = a->x; app_saved[e][1] = a->y;
        app_saved[e][2] = a->w; app_saved[e][3] = a->h;
        a->x = 8; a->y = MENU_BAR_HEIGHT + 8;
        a->w = (int)screen_width - 16;
        a->h = (int)screen_height - DOCK_HEIGHT - UI_SCALE_VAL(16);
        app_max[e] = 1;
      }
      needs_redraw = 1;
      return 1;
    }
    if (x >= a->x + a->w - 18 && y >= a->y + a->h - 18) {
      /* pulling the corner of a maximised window un-maximises it and keeps
       * sizing from where it is, the way desktop window managers behave */
      app_max[e] = 0;
      resize_app = e; resize_dx = a->x + a->w - x; resize_dy = a->y + a->h - y;
      needs_redraw = 1; return 1;
    }
    if (y < a->y + APP_TITLE_H) { drag_app = e; drag_dx = x - a->x; drag_dy = y - a->y; needs_redraw = 1; return 1; }
    if (e == APP_BROWSER && on_state == 2) {
      int ox = a->x + ON_PAD_L, oy = a->y + APP_TITLE_H + 52;
      int pbottom = a->y + a->h - 26;
      if (y >= oy && y < pbottom && x >= ox) {
        const char *href = web_hit_link(x - ox, y - oy + on_scroll);
        if (href) {
          char u[256];
          on_resolve_url(href, u, (int)sizeof u);
          if (u[0]) {
            int k = 0;
            while (u[k] && k < (int)sizeof on_url - 1) { on_url[k] = u[k]; k++; }
            on_url[k] = 0;
            on_load();
          }
          needs_redraw = 1;
          return 1;
        }
      }
    }
    if (e == APP_CALC) calc_body_click(x, y);
    if (e == APP_MEDIA) {
      int bx = a->x + 14;
      int by = a->y + APP_TITLE_H + 14 + 22 + 26 + 20 + 14 + 22;
      if (x >= bx - 4 && x < bx + 78 && y >= by - 4 && y < by + 28) {
        if (mp_playing) mp_stop();
        else if (!mp_start()) mp_playing = 0;
        needs_redraw = 1;
        return 1;
      }
    }
    needs_redraw = 1;
    return 1;
  }
  return 0;
}

/* keyboard: returns 1 when an app/widget took the key */
static int apps_key(int key) {
  if (key == KEY_ESC) {
    if (wg_focus >= 0) { wg_focus = -1; needs_redraw = 1; return 1; }
    if (mapp_focus != APP_NONE) { mapp[mapp_focus].open = 0; mapp_focus = APP_NONE; needs_redraw = 1; return 1; }
    return 0;
  }
  if (wg_focus == WG_STICKY) {
    if (key == '\b' || key == 127) {
      if (sticky_cur > 0) {
        for (int i = sticky_cur - 1; i < sticky_len; i++) sticky_text[i] = sticky_text[i + 1];
        sticky_cur--; sticky_len--;
      }
    } else if (key == '\n' || key == '\r') {
      if (sticky_len < (int)sizeof(sticky_text) - 1) {
        for (int i = sticky_len; i >= sticky_cur; i--) sticky_text[i + 1] = sticky_text[i];
        sticky_text[sticky_cur] = '\n'; sticky_cur++; sticky_len++;
      }
    } else if (key >= 32 && key < 127 && sticky_len < (int)sizeof(sticky_text) - 1) {
      for (int i = sticky_len; i >= sticky_cur; i--) sticky_text[i + 1] = sticky_text[i];
      sticky_text[sticky_cur] = (char)key; sticky_cur++; sticky_len++;
    } else {
      return 0;
    }
    needs_redraw = 1;
    return 1;
  }
  if (mapp_focus == APP_NOTES) {
    np_key(key);
    needs_redraw = 1;
    return 1;
  }
  if (mapp_focus == APP_BROWSER) {
    int n = 0;
    while (on_url[n]) n++;
    if (key == '\b' || key == 127) {
      if (n > 0) on_url[n - 1] = 0;
    } else if (key == '\n' || key == '\r') {
      on_load();
    } else if (key == KEY_UP) {
      on_scroll -= 36;
      if (on_scroll < 0) on_scroll = 0;
    } else if (key == KEY_DOWN) {
      int maxs = on_nlines - (mapp[APP_BROWSER].h - APP_TITLE_H - 100);
      if (maxs < 0) maxs = 0;
      on_scroll += 36;
      if (on_scroll > maxs) on_scroll = maxs;
    } else if (key >= 32 && key < 127 && n < (int)sizeof(on_url) - 1) {
      on_url[n] = (char)key;
      on_url[n + 1] = 0;
    } else {
      return 0;
    }
    needs_redraw = 1;
    return 1;
  }
  return 0;
}

static void apps_draw(void) {
  if (zl_n == 0) z_init();
  for (int i = 0; i < zl_n; i++) {
    int e = zl[i];
    if (e >= Z_WG) {
      int w = e - Z_WG;
      if (!mwg[w].on) continue;
      if (w == WG_STICKY) draw_sticky();
      else if (w == WG_WEATHER) draw_weather();
      else if (w == WG_CLOCK) draw_bigclock();
      continue;
    }
    if (!mapp[e].open || mapp[e].min) continue;
    if (e == APP_CALC)  draw_calc();
    if (e == APP_TASKS) draw_tasks();
    else if (e == APP_NOTES) draw_notes();
    else if (e == APP_MEDIA) draw_media();
    else if (e == APP_BROWSER) draw_browser();
  }
}
typedef struct { const char *title; const mitem *items; int n; } mmenu;

static const mitem mi_file[]   = {{"New file window","F"},{"New note","N"},{"New terminal","T"},
                                  {0,0},{"Close window","esc"}};
static const mitem mi_apps[]   = {{"Calculator","F9"},{"Notepad","F11"},{0,0},
                                  {"Media player",""},{"MeepNet",""}};
static const mitem mi_view[]   = {{"Paper","1"},{"Midnight","2"},{0,0},
                                  {"Toggle dock",0},{"Toggle grid",0},{0,0},{"Reset desktop",0}};
static const mitem mi_widgets[]= {{"Sticky note","off"},{"Weather","off"},{"Big clock","off"},
                                  {0,0},{"Show all",0},{"Hide all",0}};
static const mitem mi_help[]   = {{"About Omi",0},{"Open terminal",0}};

static const mmenu menus[5] = {
  {"File", mi_file, 5}, {"Apps", mi_apps, 5}, {"View", mi_view, 7},
  {"Widgets", mi_widgets, 6}, {"Help", mi_help, 2}
};
/* button boxes measured live off :8140 at 1280 wide (5px 9px padding, 13px/500) */
static const int menu_bx[5] = {124, 164, 214, 263, 332};
static const int menu_bw[5] = { 39,  49,  48,  68,  46};
static const int menu_by = 5, menu_bh = 26;

#define POP_W       212
#define POP_ITEM_H   28
#define POP_SEP_H    11
#define POP_PAD       5
#define POP_TEXT_PAD  8

static int menu_pop_h(int m) {
  int h = 2 + POP_PAD * 2;                  /* 1px border + 5px padding, both sides */
  for (int i = 0; i < menus[m].n; i++)
    h += menus[m].items[i].label ? POP_ITEM_H : POP_SEP_H;
  return h;
}

static void menu_pop_rect(int m, int *x, int *y, int *w, int *h) {
  *x = menu_bx[m];
  *y = menu_by + menu_bh + 6;               /* top: calc(100% + 6px) */
  *w = POP_W;
  *h = menu_pop_h(m);
}

/* row top of item i, in 1x coords */
static int menu_row_y(int m, int i) {
  int y = menu_by + menu_bh + 6 + 1 + POP_PAD;
  for (int k = 0; k < i; k++)
    y += menus[m].items[k].label ? POP_ITEM_H : POP_SEP_H;
  return y;
}

static int menu_pop_item_at(int m, int x, int y) {
  if (m < 0) return -1;
  int px, py, pw, ph;
  menu_pop_rect(m, &px, &py, &pw, &ph);
  if (x < px || x >= px + pw || y < py || y >= py + ph) return -1;
  for (int i = 0; i < menus[m].n; i++) {
    if (!menus[m].items[i].label) continue;
    int ry = menu_row_y(m, i);
    if (y >= ry && y < ry + POP_ITEM_H) return i;
  }
  return -1;
}

static int menu_title_at(int x, int y) {
  if (y < menu_by || y >= menu_by + menu_bh) return -1;
  for (int i = 0; i < 5; i++)
    if (x >= menu_bx[i] && x < menu_bx[i] + menu_bw[i]) return i;
  return -1;
}

/* dock tile rects, same walk as draw_dock() but at 1x */
static void dock_icon_rect(int i, int *rx, int *ry) {
  int tile = 44, pad = 7, gap = 6, brd = 1, sep_w = 7;
  int x, y, dw, dh;
  meep_dock_rect(1, &x, &y, &dw, &dh);
  int cx = x + brd + pad, cy = y + brd + pad;
  for (int k = 0; k < NUM_DOCK_ICONS; k++) {
    if (k == 4) cx += sep_w + gap;
    if (k == i) { *rx = cx; *ry = cy; return; }
    cx += tile + gap;
  }
  *rx = -1; *ry = -1;
}

static int dock_icon_at(int x, int y) {
  for (int i = 0; i < NUM_DOCK_ICONS; i++) {
    int rx, ry;
    dock_icon_rect(i, &rx, &ry);
    if (rx >= 0 && x >= rx && x < rx + 44 && y >= ry && y < ry + 44) return i;
  }
  return -1;
}

static void update_hover(void) {
  int nh = -2;                             /* -2 nothing, -1 start button, >=0 app */
  if (mouse_y >= taskbar_y()) {
    if (start_button_hit(mouse_x, mouse_y)) {
      nh = -1;
    } else {
      int ids[APP_COUNT], xs[APP_COUNT], ws[APP_COUNT];
      int n = tb_walk(ids, xs, ws);
      for (int i = 0; i < n; i++)
        if (mouse_x >= xs[i] && mouse_x < xs[i] + ws[i]) nh = ids[i];
    }
  }
  int sh = start_open ? sm_row_at(mouse_x, mouse_y) : -1;
  {
    int ch = ctm_open ? ctm_row_at(mouse_x, mouse_y) : -1;
    if (ch != ctm_hover) { ctm_hover = ch; needs_redraw = 1; }
  }
  if (nh != tb_hover || sh != start_hover) {
    tb_hover = nh;
    start_hover = sh;
    full_redraw = 1;
    needs_redraw = 1;
  }
}

static void draw_menu_bar(void) {
  /* MeepOS menubar: 36px paper bar, hairline under it, yellow glyph + "meep OS" badge,
     five menus, then lan / appearance / clock on the right. */
  int h = MENU_BAR_HEIGHT;
  fb_fill_rect(0, 0, screen_width, h, MEEP_PAPER2);
  fb_fill_rect(0, h - UI_SCALE_VAL(1), screen_width, UI_SCALE_VAL(1), MEEP_LINE);

  int fh = FONT_HEIGHT * ui_scale;
  int ty = (h - fh) / 2;

  /* hovered or open menu title sits on a paper-3 pill, radius 6 */
  for (int i = 0; i < 5; i++)
    if (i == open_menu || i == hover_menu)
      meep_box(UI_SCALE_VAL(menu_bx[i]), UI_SCALE_VAL(menu_by),
               UI_SCALE_VAL(menu_bw[i]), UI_SCALE_VAL(menu_bh),
               UI_SCALE_VAL(6), MEEP_PAPER3, MEEP_PAPER3);

  /* the open dropdown: paper-2 panel, 1px line, radius 8, plus the hovered row */
  if (open_menu >= 0) {
    int px, py, pw, ph;
    menu_pop_rect(open_menu, &px, &py, &pw, &ph);
    meep_box(UI_SCALE_VAL(px), UI_SCALE_VAL(py), UI_SCALE_VAL(pw), UI_SCALE_VAL(ph),
             UI_SCALE_VAL(8), MEEP_LINE, MEEP_PAPER2);
    for (int i = 0; i < menus[open_menu].n; i++) {
      int ry = menu_row_y(open_menu, i);
      if (i == hover_item)
        meep_box(UI_SCALE_VAL(px + 1 + POP_PAD), UI_SCALE_VAL(ry),
                 UI_SCALE_VAL(pw - 2 - POP_PAD * 2), UI_SCALE_VAL(POP_ITEM_H),
                 UI_SCALE_VAL(5), MEEP_ACCENT, MEEP_ACCENT);
      if (!menus[open_menu].items[i].label)      /* separator: 5px 4px margins */
        fb_fill_rect(UI_SCALE_VAL(px + 4), UI_SCALE_VAL(ry + POP_SEP_H / 2),
                     UI_SCALE_VAL(pw - 8), UI_SCALE_VAL(1), MEEP_LINE_SOFT);
    }
  }

  /* brand glyph: 19px accent square, radius 5, four ink marks in a 2x2 grid */
  int gs = UI_SCALE_VAL(19);
  int gx = UI_SCALE_VAL(16);
  int gy = (h - gs) / 2;
  gui_draw_rounded_rect(gx, gy, gs, gs, UI_SCALE_VAL(5), MEEP_ACCENT);
  {
    int pad = UI_SCALE_VAL(4), gap = UI_SCALE_VAL(2);
    int cell = (gs - pad * 2 - gap) / 2;
    for (int i = 0; i < 4; i++) {
      int cx = gx + pad + (i % 2) * (cell + gap);
      int cy = gy + pad + (i / 2) * (cell + gap);
      fb_fill_rect(cx, cy, cell, cell, MEEP_ACCENT_INK);
    }
  }


  /* the one live indicator the bar draws itself: 6px dot inside a 2px halo */
  {
    int cy = ty + fh / 2 - UI_SCALE_VAL(3);
    int cx = (int)screen_width - UI_SCALE_VAL(299);
    gui_draw_circle(cx, cy + UI_SCALE_VAL(2), UI_SCALE_VAL(5), 0x00DBE7D2);
    gui_draw_circle(cx, cy + UI_SCALE_VAL(2), UI_SCALE_VAL(3), MEEP_OK);
  }

  /* the appearance glyph: U+25D0 is outside the atlas, so draw a half-filled ring */
  {
    int cy = ty + fh / 2;
    int cx = (int)screen_width - UI_SCALE_VAL(244);
    int r = UI_SCALE_VAL(4), ri = r - UI_SCALE_VAL(1);
    for (int dy = -r; dy <= r; dy++) {
      int wo = meep_isqrt(r * r - dy * dy);
      int wi = (ri > 0 && ri * ri - dy * dy > 0) ? meep_isqrt(ri * ri - dy * dy) : 0;
      if (wo <= 0) continue;
      fb_fill_rect(cx - wo, cy + dy, wo - wi, UI_SCALE_VAL(1), MEEP_INK_SOFT);  /* left rim */
      fb_fill_rect(cx + wi, cy + dy, wo - wi, UI_SCALE_VAL(1), MEEP_INK_SOFT);  /* right rim */
      /* the filled half is the active appearance: left = paper, right = midnight */
      if (wi > 0) {
        if (!appearance_dark)
          fb_fill_rect(cx - wi, cy + dy, wi, UI_SCALE_VAL(1), MEEP_INK_SOFT);
        else
          fb_fill_rect(cx, cy + dy, wi, UI_SCALE_VAL(1), MEEP_INK_SOFT);
      }
    }
  }
}

/* Text pass: 1x, after the supersampled shell has been box-filtered down, so the
   atlas' own coverage lands on whole pixels instead of being averaged away. */
static void draw_menu_bar_text(void) {
  const int h = 36;
  int bx = 16 + 19 + 7;

  int tyv = (h - meep_line_h(&meep_s145)) / 2;
  meep_text(&meep_s145, bx, tyv, BRAND_WORD, MEEP_INK);

  {
    static const char *menus[5] = {"File", "Apps", "View", "Widgets", "Help"};
    static const int at[5] = {132, 172, 223, 271, 340};
    int tym = (h - meep_line_h(&meep_s130)) / 2;
    for (int i = 0; i < 5; i++)
      meep_text(&meep_s130, at[i], tym, menus[i], MEEP_INK);
  }

  {
    int tys = (h - meep_line_h(&meep_m105)) / 2;
    meep_text(&meep_m105, (int)screen_width - 287, tys, "lan", MEEP_INK_SOFT);
    meep_text(&meep_m105, (int)screen_width - 234, tys, "appearance", MEEP_INK_SOFT);
    char clock[32];
    meep_clock_text(clock);
    meep_text(&meep_m115, (int)screen_width - 147, (h - meep_line_h(&meep_m115)) / 2,
              clock, MEEP_INK);
  }

  /* the open dropdown's rows: 13px label, 10px mono shortcut, right aligned */
  if (open_menu >= 0) {
    int px, py, pw, ph;
    menu_pop_rect(open_menu, &px, &py, &pw, &ph);
    for (int i = 0; i < menus[open_menu].n; i++) {
      const mitem *it = &menus[open_menu].items[i];
      if (!it->label) continue;
      int ry = menu_row_y(open_menu, i);
      uint32_t col = (i == hover_item) ? MEEP_ACCENT_INK : MEEP_INK;
      meep_text(&meep_s130, px + 1 + POP_PAD + POP_TEXT_PAD,
                ry + (POP_ITEM_H - meep_line_h(&meep_s130)) / 2, it->label, col);
      const char *kbd = it->kbd;
      if (open_menu == 3 && i < 3) kbd = mwg[i].on ? "on" : "off";
      if (kbd) {
        int kw = meep_text_w(&meep_m100, kbd);
        meep_text(&meep_m100, px + pw - 1 - POP_PAD - POP_TEXT_PAD - kw,
                  ry + (POP_ITEM_H - meep_line_h(&meep_m100)) / 2, kbd,
                  (i == hover_item) ? MEEP_ACCENT_INK : MEEP_INK_SOFT);
      }
    }
  }
}

/* ===================================================================== */
/* Mouse Cursor                                                          */
/* ===================================================================== */

static const uint8_t cursor_bitmap[16][16] = {
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 2, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0},
    {1, 2, 2, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0},
    {1, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0},
    {1, 2, 2, 2, 1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 2, 2, 1, 0, 1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 2, 1, 0, 0, 1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 1, 0, 0, 0, 0, 1, 2, 1, 0, 0, 0, 0, 0, 0, 0},
    {1, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0},
};

static void draw_cursor(int x, int y) {
  int scale = ui_scale > 0 ? ui_scale : 1;
  for (int py = 0; py < 16; py++) {
    for (int px = 0; px < 16; px++) {
      uint8_t p = cursor_bitmap[py][px];
      if (p == 1 || p == 2) {
        color_t col = (p == 1) ? 0xFF000000 : 0xFFFFFFFF;
        if (scale == 1) {
          fb_put_pixel(x + px, y + py, col);
        } else {
          fb_fill_rect(x + px * scale, y + py * scale, scale, scale, col);
        }
      }
    }
  }
}

/* ===================================================================== */
/* GUI Initialization                                                    */
/* ===================================================================== */

/* Keyboard callback for handling input */
static void keyboard_callback(int key) {
  static int first_cb = 1;
  if (first_cb) {
    /* [7] PURPLE BLOCK - callback invoked */
    for (int y = 0; y < 100; y++) {
      for (int x = 0; x < 100; x++) {
        fb_fill_rect(x, 600 + y, 1, 1, 0xFFAA00FF);
      }
    }
    fb_swap_buffers();
    first_cb = 0;
  }
  
  /* Any key press triggers redraw */
  needs_redraw = 1;
  
  /* F1-F8: Change wallpaper */
  if (key >= KEY_F1 && key <= KEY_F8) {
    wallpaper_set(key - KEY_F1);
    return;
  }

  /* F9: Toggle Calculator */
  if (key == KEY_F9) {
    app_toggle(APP_CALC);
    return;
  }

  /* F10: Toggle Snake Game */
  if (key == KEY_F10) {
    if (!snake.visible) {
      snake_init();
    }
    snake.visible = !snake.visible;
    return;
  }

  /* F11: Toggle Notepad */
  if (key == KEY_F11) {
    app_toggle(APP_NOTES);
    return;
  }

  /* F12: Toggle Clock */
  if (key == KEY_F12) {
    analog_clock.visible = !analog_clock.visible;
    return;
  }

  /* an open window or a focused widget takes the keystroke */
  if (apps_key(key)) return;

  /* H: Toggle Help (when no text input active) */
  if ((key == 'h' || key == 'H') && !terminal.visible && !notepad.visible && !calc.visible) {
    help_win.visible = !help_win.visible;
    return;
  }

  /* ESC: Close windows/context menu */
  if (key == KEY_ESC) {
    ctx_menu.visible = 0;
    calc.visible = 0;
    snake.visible = 0;
    notepad.visible = 0;
    analog_clock.visible = 0;
    help_win.visible = 0;
    return;
  }

  /* Snake game controls */
  if (snake.visible) {
    if (snake.game_over && (key == 'r' || key == 'R')) {
      snake_init();
      snake.visible = 1;
      return;
    }
    if (key == KEY_UP || key == 'w' || key == 'W') {
      if (snake.dir != 2)
        snake.dir = 0;
      return;
    }
    if (key == KEY_DOWN || key == 's' || key == 'S') {
      if (snake.dir != 0)
        snake.dir = 2;
      return;
    }
    if (key == KEY_LEFT || key == 'a' || key == 'A') {
      if (snake.dir != 1)
        snake.dir = 3;
      return;
    }
    if (key == KEY_RIGHT || key == 'd' || key == 'D') {
      if (snake.dir != 3)
        snake.dir = 1;
      return;
    }
  }

  /* Calculator input */
  if (calc.visible) {
    if ((key >= '0' && key <= '9') || key == '+' || key == '-' || key == '*' ||
        key == '/' || key == '=' || key == 'c' || key == 'C' || key == '\n') {
      if (key == '\n')
        key = '=';
      if (key == 'c')
        key = 'C';
      calc_button_click((char)key);
      return;
    }
  }

  /* Notepad input */
  if (notepad.visible) {
    notepad_key(key);
    return;
  }

  /* Space: Cycle wallpaper (when nothing else is focused) */
  if (key == ' ' && !terminal.visible && !snake.visible && !calc.visible && !notepad.visible) {
    wallpaper_next();
    return;
  }

  /* Terminal input handling */
  if (terminal.visible) {
    static int first_term_key = 1;
    if (first_term_key) {
      /* [8] LIME BLOCK - terminal got key */
      for (int y = 0; y < 100; y++) {
        for (int x = 0; x < 100; x++) {
          fb_fill_rect(x, 700 + y, 1, 1, 0xFF88FF00);
        }
      }
      fb_swap_buffers();
      first_term_key = 0;
    }
    
    if (key == '\n' || key == '\r') {
      /* Execute command */
      terminal.input[terminal.input_len] = '\0';
      term_execute(terminal.input);
      terminal.input_len = 0;
    } else if (key == '\b' || key == 127) {
      /* Backspace */
      if (terminal.input_len > 0) {
        terminal.input_len--;
        terminal.input[terminal.input_len] = '\0';
      }
    } else if (key >= 32 && key < 127 && terminal.input_len < TERM_MAX_INPUT - 1) {
      static int first_char = 1;
      if (first_char) {
        /* [9] BRIGHT GREEN BLOCK - char added to buffer */
        for (int y = 0; y < 100; y++) {
          for (int x = 0; x < 100; x++) {
            fb_fill_rect(x, 800 + y, 1, 1, 0xFF00FF00);
          }
        }
        fb_swap_buffers();
        first_char = 0;
      }
      
      /* Add character to input */
      terminal.input[terminal.input_len++] = (char)key;
      terminal.input[terminal.input_len] = '\0';
    }
  }
}

void gui_init(void) {
  /* Calculate UI scale from EDID if available, otherwise fall back to resolution */
  int dpi_x = 0;
  int dpi_y = 0;
  if (screen_mm_width > 0 && screen_mm_height > 0) {
    /* DPI = pixels / inches; inches = mm / 25.4 */
    dpi_x = (int)((screen_width * 254 + (screen_mm_width * 10) / 2) /
                  (screen_mm_width * 10));
    dpi_y = (int)((screen_height * 254 + (screen_mm_height * 10) / 2) /
                  (screen_mm_height * 10));
  }

  if (dpi_x > 0 && dpi_y > 0) {
    int dpi = (dpi_x + dpi_y) / 2;
    /* 96 DPI baseline. Use integer scaling for crisp fonts. */
    ui_scale = (dpi + 48) / 96;
  } else {
    /* Fallback: resolution-based heuristic */
    if (screen_width >= 2560 || screen_height >= 1440) {
      ui_scale = 2;
    } else {
      ui_scale = 1;
    }
  }
  if (ui_scale < 1)
    ui_scale = 1;
  if (ui_scale > 3)
    ui_scale = 3;

  /* Scale UI sizes */
  int base_dock_icon = 48;
  int base_dock_height = 70;
  int base_menu_height = 28;
  int base_dock_padding = 10;

  dock_icon_size = base_dock_icon * ui_scale;
  (void)base_dock_height;
  dock_height = TB_BASE_H * ui_scale;      /* still the bottom dead-zone for windows */
  /* MeepOS menubar is exactly 36px at 1x (--bar-h), not the upstream default */
  (void)base_menu_height;
  menu_bar_height = 0;                     /* the taskbar replaces the top bar */
  dock_padding = base_dock_padding * ui_scale;
  
  /* Initialize VFS first */
  vfs_init();
  vfs_seed_content();

  font_init();
  term_init();
  fm_init();

  /* Initialize PS/2 keyboard and mouse with retry */
  int ps2_ok = ps2_init();
  if (ps2_ok != 0) {
    /* Retry once more */
    ps2_init();
  }
  ps2_set_screen_bounds(screen_width, screen_height);
  /* Faster mouse on high-DPI screens */
  ps2_set_mouse_scale(ui_scale >= 2 ? (ui_scale + 1) : 2);
  ps2_set_keyboard_callback(keyboard_callback);
  usb_set_keyboard_callback(keyboard_callback);

  /* Initialize dock smooth sizes */
  for (int i = 0; i < NUM_DOCK_ICONS; i++) {
    dock_smooth_sizes[i] = dock_icon_size;
  }

  /* Load initial wallpaper (Landscape JPEG) */
  current_wallpaper = 0;
  wallpaper_ensure_loaded();

  /* Scale and position windows based on ui_scale */
  
  /* Scale terminal */
  terminal.width = UI_SCALE_VAL(650);
  terminal.height = UI_SCALE_VAL(420);
  if (terminal.width > (int)screen_width - 100) terminal.width = screen_width - 100;
  if (terminal.height > (int)screen_height - 200) terminal.height = screen_height - 200;
  terminal.content_x = (screen_width - terminal.width) / 2;
  terminal.content_y = (screen_height - terminal.height) / 2;
  
  /* Scale file manager */
  file_manager.width = 500 * ui_scale;
  file_manager.height = 400 * ui_scale;
  if (file_manager.width > (int)screen_width - 100) file_manager.width = screen_width - 100;
  if (file_manager.height > (int)screen_height - 200) file_manager.height = screen_height - 200;
  file_manager.x = UI_SCALE_VAL(80);
  file_manager.y = UI_SCALE_VAL(60);
  
  /* Scale calculator */
  calc.width = UI_SCALE_VAL(220);
  calc.height = UI_SCALE_VAL(340);
  calc.x = screen_width - calc.width - UI_SCALE_VAL(50);
  calc.y = UI_SCALE_VAL(100);
  
  /* Scale snake game */
  snake.width = UI_SCALE_VAL(360);
  snake.height = UI_SCALE_VAL(320);
  snake.x = UI_SCALE_VAL(50);
  snake.y = UI_SCALE_VAL(50);
  
  /* Scale notepad */
  notepad.width = UI_SCALE_VAL(450);
  notepad.height = UI_SCALE_VAL(380);
  notepad.x = UI_SCALE_VAL(150);
  notepad.y = UI_SCALE_VAL(100);
  
  /* Scale clock */
  analog_clock.size = UI_SCALE_VAL(200);
  analog_clock.x = UI_SCALE_VAL(100);
  analog_clock.y = UI_SCALE_VAL(50);
  
  /* Scale help window */
  help_win.width = UI_SCALE_VAL(500);
  help_win.height = UI_SCALE_VAL(420);
  help_win.x = screen_width - help_win.width - UI_SCALE_VAL(50);
  help_win.y = UI_SCALE_VAL(80);
  
  /* Scale image viewer */
  img_viewer.width = UI_SCALE_VAL(650);
  img_viewer.height = UI_SCALE_VAL(500);
  img_viewer.x = (screen_width - img_viewer.width) / 2;
  img_viewer.y = (screen_height - img_viewer.height) / 2;

  /* Initialize USB (xHCI/EHCI) for keyboard input - with visual progress */
  /* Yellow marker = starting USB init */
  fb_fill_rect(190, 10, 50, 10, 0xFFFFFF00);
  fb_swap_buffers();

  /* Orange marker = starting xHCI init */
  fb_fill_rect(250, 10, 50, 10, 0xFFFF9900);
  fb_swap_buffers();
  usb_xhci_init();
  /* Green marker = xHCI init done */
  fb_fill_rect(310, 10, 50, 10, 0xFF00FF00);
  fb_swap_buffers();

  /* Magenta marker = starting EHCI init */
  fb_fill_rect(370, 10, 50, 10, 0xFFFF00FF);
  fb_swap_buffers();
  usb_ehci_init();
  /* Cyan marker = EHCI init done */
  fb_fill_rect(430, 10, 50, 10, 0xFF00FFFF);
  fb_swap_buffers();

  /* Show terminal and file manager as demo */
  terminal.visible = 1;
  term_puts("Omi Terminal v1.0\n", 14); /* Cyan */
  term_puts("Type 'help' for commands, 'neofetch' for system info.\n\n", 7);
  term_prompt();

  file_manager.visible = 1;
  fm_refresh();

  mouse_x = screen_width / 2;
  mouse_y = screen_height / 2;
}

/* ===================================================================== */
/* Main Composition Loop - with dirty region optimization                */
/* ===================================================================== */

static uint64_t frame_count = 0;
static int prev_buttons = 0;
static int last_mouse_x = 0, last_mouse_y = 0;

/* Dirty region tracking - only copy changed areas to framebuffer */
#define MAX_DIRTY_REGIONS 32
static struct { int x, y, w, h, valid; } dirty_regions[MAX_DIRTY_REGIONS];
static int dirty_count = 0;

static void mark_dirty(int x, int y, int w, int h) {
  if (dirty_count < MAX_DIRTY_REGIONS) {
    dirty_regions[dirty_count].x = x;
    dirty_regions[dirty_count].y = y;
    dirty_regions[dirty_count].w = w;
    dirty_regions[dirty_count].h = h;
    dirty_regions[dirty_count].valid = 1;
    dirty_count++;
  } else {
    full_redraw = 1;
  }
}

/* Copy only a specific region from backbuffer to framebuffer */
static void blit_region(int x, int y, int w, int h) {
  if (!backbuffer || !framebuffer) return;
  
  /* Clip to screen */
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > (int)screen_width) w = screen_width - x;
  if (y + h > (int)screen_height) h = screen_height - y;
  if (w <= 0 || h <= 0) return;
  
  /* Copy region line by line using 64-bit operations */
  for (int row = y; row < y + h; row++) {
    uint64_t *src = (uint64_t *)(backbuffer + row * screen_width + x);
    volatile uint64_t *dst = (volatile uint64_t *)((uint8_t*)framebuffer + row * screen_pitch + x * 4);
    int count = w / 2;
    for (int i = 0; i < count; i++) {
      dst[i] = src[i];
    }
    if (w & 1) {
      volatile uint32_t *dst32 = (volatile uint32_t *)dst;
      uint32_t *src32 = (uint32_t *)src;
      dst32[w - 1] = src32[w - 1];
    }
  }
}

/* Smart buffer swap - only copy dirty regions or full screen */
static void smart_swap_buffers(void) {
  if (!backbuffer || !framebuffer) return;
  
  if (full_redraw || dirty_count == 0) {
    /* Full screen copy - pitch-aware */
    for (uint32_t row = 0; row < screen_height; row++) {
      uint64_t *src = (uint64_t *)(backbuffer + row * screen_width);
      volatile uint64_t *dst =
          (volatile uint64_t *)((uint8_t *)framebuffer + row * screen_pitch);
      size_t count64 = screen_width / 2;
      size_t i = 0;
      size_t fast_count = count64 & ~7UL;
      for (; i < fast_count; i += 8) {
        dst[i] = src[i];
        dst[i + 1] = src[i + 1];
        dst[i + 2] = src[i + 2];
        dst[i + 3] = src[i + 3];
        dst[i + 4] = src[i + 4];
        dst[i + 5] = src[i + 5];
        dst[i + 6] = src[i + 6];
        dst[i + 7] = src[i + 7];
      }
      for (; i < count64; i++) {
        dst[i] = src[i];
      }
      if (screen_width & 1) {
        volatile uint32_t *dst32 = (volatile uint32_t *)dst;
        uint32_t *src32 = (uint32_t *)src;
        dst32[screen_width - 1] = src32[screen_width - 1];
      }
    }
    full_redraw = 0;
  } else {
    /* Partial update - only dirty regions */
    for (int d = 0; d < dirty_count; d++) {
      if (dirty_regions[d].valid) {
        blit_region(dirty_regions[d].x, dirty_regions[d].y,
                    dirty_regions[d].w, dirty_regions[d].h);
      }
    }
  }
  
  dirty_count = 0;
  __asm__ volatile("mfence" ::: "memory");
}

/* Handle mouse clicks: the menubar menus open/close, dock tiles acknowledge.
   Nothing behind them exists yet - that is the next layer. */
/* the menubar appearance pill: glyph at W-244, word from W-234 */
static int appearance_hit(int x, int y) {
  if (y < 0 || y >= 36) return 0;
  return x >= (int)screen_width - UI_SCALE_VAL(252) &&
         x <  (int)screen_width - UI_SCALE_VAL(160);
}

static void handle_mouse_click(int x, int y, int button) {
  if (ctm_open) {                       /* the menu owns the next click */
    int r = ctm_row_at(x, y);
    if (r >= 0) { ctm_activate(r); return; }
    ctm_open = 0;
    needs_redraw = 1;
    full_redraw = 1;
    /* fall through: the click still lands where it was aimed */
  }
  if (button == 2 && y >= taskbar_y()) {  /* right-click the bar itself */
    ctm_open = 1;
    ctm_x = x;
    ctm_y = y;
    ctm_hover = -1;
    start_open = 0;
    needs_redraw = 1;
    full_redraw = 1;
    return;
  }
  if (button == 2) {                       /* right click dismisses */
    if (open_menu >= 0) { open_menu = -1; hover_item = -1; }
    return;
  }

  /* the taskbar is chrome: it sits above every window */
  if (y >= taskbar_y()) {
    if (start_button_hit(x, y)) {
      start_open = !start_open;
      start_hover = -1;
    } else {
      int ids[APP_COUNT], xs[APP_COUNT], ws[APP_COUNT];
      int n = tb_walk(ids, xs, ws);
      int hit = -1;
      for (int i = 0; i < n; i++)
        if (x >= xs[i] && x < xs[i] + ws[i]) hit = ids[i];
      if (hit > 0) {
        if (mapp[hit].min) mapp[hit].min = 0;      /* restore a minimized app */
        mapp_focus = hit;
        z_top(hit);
      }
      start_open = 0;
    }
    needs_redraw = 1;
    full_redraw = 1;
    return;
  }

  if (start_open) {                        /* dismiss, and let the click land */
    int r = sm_row_at(x, y);
    start_open = 0;
    needs_redraw = 1;
    full_redraw = 1;
    if (r >= 0) { sm_activate(r); return; }
  }

  apps_click(x, y, button);                 /* windows and widgets own their pixels */

}

/* Window geometry constants (matching reference, unscaled base) */
#define WIN_BORDER_WIDTH 2
#define WIN_TITLEBAR_HEIGHT 28

/* Generic window click handler - handles traffic lights for ANY window 
   Returns: 0=not handled, 1=close clicked, 2=minimize clicked, 3=maximize clicked, 4=titlebar drag */
static int handle_window_titlebar_click(int mx, int my, int wx, int wy, int ww, int wh) {
  /* Check if click is inside window */
  if (mx < wx || mx >= wx + ww || my < wy || my >= wy + wh) {
    return 0;
  }
  
  /* Traffic light button centers - matching reference exactly */
  int border = UI_SCALE_VAL(WIN_BORDER_WIDTH);
  int title_h = UI_SCALE_VAL(WIN_TITLEBAR_HEIGHT);
  int btn_cy = wy + border + title_h / 2;
  int btn_r = UI_SCALE_VAL(8);
  
  /* Close button (red) at x + BORDER_WIDTH + 18 = x + 20 */
  int close_cx = wx + border + UI_SCALE_VAL(18);
  int dx = mx - close_cx;
  int dy = my - btn_cy;
  if (dx*dx + dy*dy <= btn_r*btn_r) {
    return 1; /* Close */
  }
  
  /* Minimize button (yellow) at close + 20 */
  int min_cx = close_cx + UI_SCALE_VAL(20);
  dx = mx - min_cx;
  if (dx*dx + dy*dy <= btn_r*btn_r) {
    return 2; /* Minimize */
  }
  
  /* Maximize button (green) at min + 20 */
  int max_cx = min_cx + UI_SCALE_VAL(20);
  dx = mx - max_cx;
  if (dx*dx + dy*dy <= btn_r*btn_r) {
    return 3; /* Maximize */
  }
  
  /* Title bar drag (after traffic lights) */
  if (my >= wy + border && my < wy + border + title_h) {
    if (mx >= wx + border + UI_SCALE_VAL(70)) {
      return 4; /* Start drag */
    }
  }
  
  return 0; /* Not a titlebar action */
}

/* Check if point is in title bar of a window */
static int check_title_bar_click(int mx, int my, int wx, int wy, int ww, int visible, int *out_drag_type, int drag_type) {
  if (!visible) return 0;
  /* Title bar is first 32 pixels of window, but skip first 70 pixels (traffic lights) */
  int title_h = UI_SCALE_VAL(32);
  if (mx >= wx + UI_SCALE_VAL(70) && mx < wx + ww && my >= wy && my < wy + title_h) {
    *out_drag_type = drag_type;
    return 1;
  }
  return 0;
}

/* Poll input and check if redraw needed */
static void gui_poll_input(void) {
  ps2_poll();
  
  int new_x = ps2_get_mouse_x();
  int new_y = ps2_get_mouse_y();
  int buttons = ps2_get_mouse_buttons();
  int left_held = buttons & 1;
  int left_release = !(buttons & 1) && (prev_buttons & 1);
  
  /* Handle window dragging */
  if (dragging_window != DRAG_NONE && left_held) {
    int new_win_x = new_x - drag_offset_x;
    int new_win_y = new_y - drag_offset_y;
    
    /* Clamp to screen bounds */
    if (new_win_y < MENU_BAR_HEIGHT) new_win_y = MENU_BAR_HEIGHT;
    if (new_win_y > (int)screen_height - DOCK_HEIGHT - 32) 
      new_win_y = screen_height - DOCK_HEIGHT - 32;
    if (new_win_x < 0) new_win_x = 0;
    if (new_win_x > (int)screen_width - 100) new_win_x = screen_width - 100;
    
    /* Update window position based on which window is being dragged */
    switch (dragging_window) {
      case DRAG_TERMINAL:
        terminal.content_x = new_win_x;
        terminal.content_y = new_win_y;
        break;
      case DRAG_FILE_MANAGER:
        file_manager.x = new_win_x;
        file_manager.y = new_win_y;
        break;
      case DRAG_CALCULATOR:
        calc.x = new_win_x;
        calc.y = new_win_y;
        break;
      case DRAG_SNAKE:
        snake.x = new_win_x;
        snake.y = new_win_y;
        break;
      case DRAG_NOTEPAD:
        notepad.x = new_win_x;
        notepad.y = new_win_y;
        break;
      case DRAG_CLOCK:
        analog_clock.x = new_win_x;
        analog_clock.y = new_win_y;
        break;
      case DRAG_HELP:
        help_win.x = new_win_x;
        help_win.y = new_win_y;
        break;
      case DRAG_IMAGE_VIEWER:
        img_viewer.x = new_win_x;
        img_viewer.y = new_win_y;
        break;
    }
    full_redraw = 1;
    needs_redraw = 1;
  }
  
  /* Release dragging on mouse up */
  if (left_release) {
    if (resize_app != APP_NONE) { web_relayout(); resize_app = APP_NONE; }
    dragging_window = DRAG_NONE;
    drag_app = APP_NONE;
    wg_drag = -1;
  }

  /* MeepOS windows and widgets drag by their title strip */
  if (drag_app != APP_NONE && left_held) {
    meep_app_t *a = &mapp[drag_app];
    int nx = new_x - drag_dx, ny = new_y - drag_dy;
    if (ny < MENU_BAR_HEIGHT) ny = MENU_BAR_HEIGHT;
    if (nx < 0) nx = 0;
    if (nx > (int)screen_width - 80) nx = screen_width - 80;
    if (ny > (int)screen_height - 40) ny = screen_height - 40;
    a->x = nx; a->y = ny;
    needs_redraw = 1;
  }
  if (resize_app != APP_NONE && left_held) {
    meep_app_t *a = &mapp[resize_app];
    int nw = new_x + resize_dx - a->x;
    int nh = new_y + resize_dy - a->y;
    if (nw < 300) nw = 300;
    if (nh < 200) nh = 200;
    if (a->x + nw > (int)screen_width - 6) nw = (int)screen_width - 6 - a->x;
    if (a->y + nh > (int)screen_height - DOCK_HEIGHT - 4) nh = (int)screen_height - DOCK_HEIGHT - 4 - a->y;
    if (nw != a->w || nh != a->h) { a->w = nw; a->h = nh; full_redraw = 1; needs_redraw = 1; }
  }
  if (wg_drag >= 0 && left_held) {
    meep_wg_t *g = &mwg[wg_drag];
    int nx = new_x - wg_dx, ny = new_y - wg_dy;
    if (ny < MENU_BAR_HEIGHT) ny = MENU_BAR_HEIGHT;
    if (nx < 0) nx = 0;
    if (nx > (int)screen_width - 60) nx = screen_width - 60;
    if (ny > (int)screen_height - 40) ny = screen_height - 40;
    g->x = nx; g->y = ny;
    needs_redraw = 1;
  }
  
  /* Check if mouse moved */
  if (new_x != last_mouse_x || new_y != last_mouse_y) {
    /* Mark old and new cursor positions as dirty */
    int cursor_size = UI_SCALE_VAL(20);
    mark_dirty(last_mouse_x, last_mouse_y, cursor_size, cursor_size);
    mark_dirty(new_x, new_y, cursor_size, cursor_size);
    
    /* Mark dock area if mouse is near - include space for magnified icons and labels */
    int dock_extend = UI_SCALE_VAL(80); /* Space for magnification + label */
    if (new_y > (int)screen_height - DOCK_HEIGHT - dock_extend || 
        last_mouse_y > (int)screen_height - DOCK_HEIGHT - dock_extend) {
      mark_dirty(0, screen_height - DOCK_HEIGHT - dock_extend, screen_width, DOCK_HEIGHT + dock_extend + 10);
    }
    
    last_mouse_x = new_x;
    last_mouse_y = new_y;
    needs_redraw = 1;
  }
  
  mouse_x = new_x;
  mouse_y = new_y;
  
  /* Detect clicks */
  int left_click = (buttons & 1) && !(prev_buttons & 1);
  int right_click = (buttons & 2) && !(prev_buttons & 2);
  
  if (left_click) {
    int clicked_window = 0;
    
    /* Window data for unified handling */
    struct { int *visible; int *x; int *y; int *w; int *h; int win_id; int drag_id; } windows[] = {
      {&terminal.visible, &terminal.content_x, &terminal.content_y, &terminal.width, &terminal.height, WIN_TERMINAL, DRAG_TERMINAL},
      {&file_manager.visible, &file_manager.x, &file_manager.y, &file_manager.width, &file_manager.height, WIN_FILE_MANAGER, DRAG_FILE_MANAGER},
      {&calc.visible, &calc.x, &calc.y, &calc.width, &calc.height, WIN_CALCULATOR, DRAG_CALCULATOR},
      {&snake.visible, &snake.x, &snake.y, &snake.width, &snake.height, WIN_SNAKE, DRAG_SNAKE},
      {&notepad.visible, &notepad.x, &notepad.y, &notepad.width, &notepad.height, WIN_NOTEPAD, DRAG_NOTEPAD},
      {&analog_clock.visible, &analog_clock.x, &analog_clock.y, &analog_clock.size, &analog_clock.size, WIN_CLOCK, DRAG_CLOCK},
      {&help_win.visible, &help_win.x, &help_win.y, &help_win.width, &help_win.height, WIN_HELP, DRAG_HELP},
      {&img_viewer.visible, &img_viewer.x, &img_viewer.y, &img_viewer.width, &img_viewer.height, WIN_IMAGE_VIEWER, DRAG_IMAGE_VIEWER},
    };
    int num_windows = sizeof(windows) / sizeof(windows[0]);
    
    /* Check windows in z-order (topmost first) */
    /* the upstream demo windows are gone: MeepOS apps own their pixels now, so this
       legacy hit-testing loop is skipped entirely (it used to eat every click) */
    for (int z = NUM_MANAGED_WINDOWS - 1; z >= 0 && !clicked_window && 0; z--) {
      int win_id = window_z_order[z];
      
      /* Find matching window data */
      for (int w = 0; w < num_windows; w++) {
        if (windows[w].win_id != win_id || !*windows[w].visible) continue;
        
        int wx = *windows[w].x;
        int wy = *windows[w].y;
        int ww = *windows[w].w;
        int wh = *windows[w].h;
        
        /* Check if click is inside this window */
        if (mouse_x < wx || mouse_x >= wx + ww || mouse_y < wy || mouse_y >= wy + wh) continue;
        
        /* Click is inside this window - bring to front */
        bring_window_to_front(win_id);
        
        /* Check traffic light buttons (matching reference positions) */
        int border = UI_SCALE_VAL(WIN_BORDER_WIDTH);
        int title_h = UI_SCALE_VAL(WIN_TITLEBAR_HEIGHT);
        int btn_cy = wy + border + title_h / 2;
        int btn_r = UI_SCALE_VAL(8);
        
        /* Close button (red) at x + 20 */
        int close_cx = wx + border + UI_SCALE_VAL(18);
        int dx = mouse_x - close_cx;
        int dy = mouse_y - btn_cy;
        if (dx*dx + dy*dy <= btn_r*btn_r) {
          *windows[w].visible = 0;
          clicked_window = 1;
          break;
        }
        
        /* Minimize button (yellow) at x + 40 */
        int min_cx = close_cx + UI_SCALE_VAL(20);
        dx = mouse_x - min_cx;
        if (dx*dx + dy*dy <= btn_r*btn_r) {
          *windows[w].visible = 0;
          clicked_window = 1;
          break;
        }
        
        /* Maximize button (green) at x + 60 */
        int max_cx = min_cx + UI_SCALE_VAL(20);
        dx = mouse_x - max_cx;
        if (dx*dx + dy*dy <= btn_r*btn_r) {
          /* Toggle maximize for this window */
          static int maximized[8] = {0};
          static int saved[8][4];
          if (maximized[w]) {
            *windows[w].x = saved[w][0];
            *windows[w].y = saved[w][1];
            *windows[w].w = saved[w][2];
            *windows[w].h = saved[w][3];
            maximized[w] = 0;
          } else {
            saved[w][0] = wx; saved[w][1] = wy;
            saved[w][2] = ww; saved[w][3] = wh;
            *windows[w].x = 0;
            *windows[w].y = MENU_BAR_HEIGHT;
            *windows[w].w = screen_width;
            *windows[w].h = screen_height - DOCK_HEIGHT - 8;
            maximized[w] = 1;
          }
          clicked_window = 1;
          break;
        }
        
        /* Check for title bar drag (after traffic lights area) */
        if (mouse_y >= wy + border && mouse_y < wy + border + title_h) {
          if (mouse_x >= wx + border + UI_SCALE_VAL(70)) {
            dragging_window = windows[w].drag_id;
            drag_offset_x = mouse_x - wx;
            drag_offset_y = mouse_y - wy;
            clicked_window = 1;
            break;
          }
        }
        
        /* Window-specific content handling */
        if (win_id == WIN_FILE_MANAGER) {
          int fm_title_h = UI_SCALE_VAL(32);
          int fm_toolbar_top = wy + fm_title_h + UI_SCALE_VAL(8);
          int fm_toolbar_bottom = fm_toolbar_top + UI_SCALE_VAL(24);
          /* Toolbar buttons */
          if (mouse_y >= fm_toolbar_top && mouse_y < fm_toolbar_bottom) {
            if (mouse_x >= wx + UI_SCALE_VAL(10) &&
                mouse_x < wx + UI_SCALE_VAL(70)) {
              /* Back button */
              if (strcmp(file_manager.current_path, "/") != 0) {
                int len = strlen(file_manager.current_path);
                while (len > 0 && file_manager.current_path[len-1] != '/') len--;
                if (len > 1) len--;
                file_manager.current_path[len] = '\0';
                if (len == 0) strcpy(file_manager.current_path, "/");
                file_manager.selected = -1;
                fm_refresh();
              }
            } else if (mouse_x >= wx + UI_SCALE_VAL(80) &&
                       mouse_x < wx + UI_SCALE_VAL(170)) {
              /* New Folder */
              static int fnum = 1;
              char path[256];
              if (file_manager.current_path[0] == '/' && file_manager.current_path[1] == '\0')
                snprintf(path, 256, "/NewFolder%d", fnum++);
              else
                snprintf(path, 256, "%s/NewFolder%d", file_manager.current_path, fnum++);
              vfs_mkdir(path);
              fm_refresh();
            } else if (mouse_x >= wx + UI_SCALE_VAL(180) &&
                       mouse_x < wx + UI_SCALE_VAL(260)) {
              /* New File */
              static int fnm = 1;
              char path[256];
              if (file_manager.current_path[0] == '/' && file_manager.current_path[1] == '\0')
                snprintf(path, 256, "/NewFile%d.txt", fnm++);
              else
                snprintf(path, 256, "%s/NewFile%d.txt", file_manager.current_path, fnm++);
              vfs_create(path);
              fm_refresh();
            }
          }
          /* File list grid (must match draw_file_manager) */
          else if (mouse_y >= wy + UI_SCALE_VAL(100)) {
            int item_w = UI_SCALE_VAL(90);
            int item_h = UI_SCALE_VAL(80);
            int cols = (ww - UI_SCALE_VAL(20)) / item_w;
            if (cols < 1) cols = 1;
            if (cols > 6) cols = 6;
            int rel_x = mouse_x - wx - UI_SCALE_VAL(10);
            int rel_y = mouse_y - wy - UI_SCALE_VAL(100);
            if (rel_x >= 0) {
              int idx = (rel_y / item_h) * cols + (rel_x / item_w);
              if (idx >= 0 && idx < file_manager.entry_count) {
                file_manager.selected = idx;
                if (file_manager.entries[idx].is_dir) {
                  if (file_manager.current_path[0] == '/' && file_manager.current_path[1] == '\0') {
                    char np[256];
                    snprintf(np, 256, "/%s", file_manager.entries[idx].name);
                    strcpy(file_manager.current_path, np);
                  } else {
                    strcat(file_manager.current_path, "/");
                    strcat(file_manager.current_path, file_manager.entries[idx].name);
                  }
                  file_manager.selected = -1;
                  fm_refresh();
                }
              }
            }
          }
        }
        
        clicked_window = 1;
        break;
      }
    }
    
    /* If no window clicked, check other UI elements */
    if (!clicked_window) {
      handle_mouse_click(mouse_x, mouse_y, 1);
    }
    needs_redraw = 1;
    full_redraw = 1;
  }
  if (right_click) {
    handle_mouse_click(mouse_x, mouse_y, 2);
    needs_redraw = 1;
    full_redraw = 1;
  }
  
  if ((prev_buttons & 1) && !(buttons & 1) && press_dock >= 0) {
    press_dock = -1;                       /* release ends the tile press */
    full_redraw = 1;
    needs_redraw = 1;
  }

  update_hover();

  prev_buttons = buttons;
  mouse_buttons = buttons;
}

/* Draw everything to backbuffer */
/* ===================================================================== */
/* Supersampled shell                                                    */
/*                                                                       */
/* The framebuffer primitives are hard-edged, so the shell is drawn at   */
/* SX x into an offscreen buffer and box-filtered back down: corners,    */
/* arcs, hairlines and glyph strokes all land on fractional coverage.    */
/* The wallpaper is left at 1x - it is already pixel-exact - and the     */
/* supersampled shell is composited over it.                             */
/* ===================================================================== */

static int SX = 1;   /* chosen at first use: 3, else 2 */

static uint32_t *ss_buf = 0;
static int ss_w = 0, ss_h = 0;
static int cur_box_x = 0, cur_box_y = 0, cur_box_w = 0, cur_box_h = 0;

/* replicate a 1x rect of the wallpaper up into the ss buffer */
static void ss_fill_region(int x, int y, int w, int h) {
  if (!ss_buf) return;
  int W = (int)screen_width, H = (int)screen_height;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > W) w = W - x;
  if (y + h > H) h = H - y;
  if (w <= 0 || h <= 0) return;
  for (int py = y; py < y + h; py++) {
    const uint32_t *src = backbuffer + (size_t)py * W;
    int sy = py * SX;
    for (int px = x; px < x + w; px++) {
      uint32_t c = src[px];
      int sx = px * SX;
      for (int dy = 0; dy < SX; dy++) {
        uint32_t *row = ss_buf + (size_t)(sy + dy) * ss_w + sx;
        for (int dx = 0; dx < SX; dx++) row[dx] = c;
      }
    }
  }
}

/* average the ss block back into the 1x backbuffer */
static void ss_blend_region(int x, int y, int w, int h) {
  if (!ss_buf) return;
  int W = (int)screen_width, H = (int)screen_height;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > W) w = W - x;
  if (y + h > H) h = H - y;
  if (w <= 0 || h <= 0) return;
  const int n = SX * SX;
  for (int py = y; py < y + h; py++) {
    uint32_t *dst = backbuffer + (size_t)py * W;
    int sy = py * SX;
    for (int px = x; px < x + w; px++) {
      int sx = px * SX;
      int r = 0, g = 0, b = 0;
      for (int dy = 0; dy < SX; dy++) {
        const uint32_t *row = ss_buf + (size_t)(sy + dy) * ss_w + sx;
        for (int dx = 0; dx < SX; dx++) {
          uint32_t c = row[dx];
          r += (c >> 16) & 0xFF;
          g += (c >> 8) & 0xFF;
          b += c & 0xFF;
        }
      }
      dst[px] = MAKE_COLOR(r / n, g / n, b / n);
    }
  }
}

static void ss_prepare(void) {
  if (ss_buf) return;
  static const int want[3] = {3, 2, 1};
  for (int k = 0; k < 3; k++) {
    int s = want[k];
    if (s < 2) break;
    int w = (int)screen_width * s, h = (int)screen_height * s;
    uint32_t *b = (uint32_t *)kmalloc((size_t)w * h * sizeof(uint32_t));
    if (!b) continue;
    ss_buf = b; ss_w = w; ss_h = h; SX = s;
    /* seed with the wallpaper so partial coverage blends into the real
       background on the very first frame */
    ss_fill_region(0, 0, (int)screen_width, (int)screen_height);
    cur_box_w = cur_box_h = 0;
    return;
  }
}

static int cur_pop_x, cur_pop_y, cur_pop_w, cur_pop_h;

static void gui_draw_all(void) {
  const int rs = ui_scale > 0 ? ui_scale : 1;
  const int W = (int)screen_width, H = (int)screen_height;

  /* 1. background, at 1x - already matches the browser render pixel for pixel */
  draw_wallpaper();

  ss_prepare();
  if (!ss_buf) {                    /* out of memory: fall back to hard edges */
    draw_taskbar();
    draw_start_menu();
    draw_cursor(mouse_x, mouse_y);
    draw_taskbar_text();
    draw_ctm_text();
    return;
  }

  /* 2. the rects the shell covers, in 1x coords */
  int mb_h = 0;                    /* there is no top bar any more */
  int dx = 0, dy = taskbar_y() - 2 * rs, dw = W, dh = taskbar_h() + 4 * rs;
  int cw = 16 * rs + 4, chh = 16 * rs + 6;
  int nx = mouse_x - 2, ny = mouse_y - 2;

  /* the dropdown lives below the bar, so it needs a region of its own */
  int px = 0, py = 0, pw = 0, ph = 0;
  if (start_open) {                          /* the start menu needs its own region */
    sm_rect(&px, &py, &pw, &ph);
    px -= 4; py -= 4; pw += 8; ph += 8;      /* slack for the radius + shadow */
  }
  if (ctm_open) {                            /* same idea for the context menu */
    ctm_rect(&px, &py, &pw, &ph);
    px -= 4; py -= 4; pw += 8; ph += 8;
  }

  /* 3. repaint last frame's shell pixels from the wallpaper (kills the old cursor) */
  ss_fill_region(0, 0, W, mb_h);
  ss_fill_region(dx, dy, dw, dh);
  if (cur_box_w) ss_fill_region(cur_box_x, cur_box_y, cur_box_w, cur_box_h);
  if (cur_pop_w) ss_fill_region(cur_pop_x, cur_pop_y, cur_pop_w, cur_pop_h);

  /* 4. draw the shell into the supersampled buffer */
  uint32_t *rb = backbuffer;
  uint32_t rw = screen_width, rh = screen_height;
  backbuffer = ss_buf;
  screen_width = rw * SX;
  screen_height = rh * SX;
  ui_scale = rs * SX;
  int mb_saved = menu_bar_height;          /* menu_bar_height is cached at 1x */
  menu_bar_height = 0;

  draw_start_menu();                       /* under the bar, above the windows */
  draw_ctm();
  draw_taskbar();
  draw_cursor(mouse_x * SX, mouse_y * SX);

  backbuffer = rb;
  screen_width = rw;
  screen_height = rh;
  ui_scale = rs;
  menu_bar_height = mb_saved;

  /* 5. box-filter back down over the wallpaper */
  ss_blend_region(0, 0, W, mb_h);
  ss_blend_region(dx, dy, dw, dh);
  ss_blend_region(nx, ny, cw, chh);
  if (pw) ss_blend_region(px, py, pw, ph);

  cur_box_x = nx; cur_box_y = ny; cur_box_w = cw; cur_box_h = chh;
  cur_pop_x = px; cur_pop_y = py; cur_pop_w = pw; cur_pop_h = ph;

  /* 6. apps and widgets sit on the wallpaper, under the shell's own chrome */
  apps_draw();

  /* 6b. the taskbar and an open start menu are chrome: never behind a window */
  ss_blend_region(dx, dy, dw, dh);
  if (pw) ss_blend_region(px, py, pw, ph);

  /* 7. type on top, at 1x, with the atlas' own anti-aliasing */
  draw_taskbar_text();
  draw_ctm_text();
  draw_cursor(mouse_x, mouse_y);        /* keep the pointer above the windows */

  full_redraw = 1;                  /* dirty rects are in ss coords; blit whole frames */
}

void gui_compose(void) {
  gui_poll_input();

  if (ps2_take_ctrl_alt_del())    /* the shell owns this chord */
    open_task_manager();

  if (appearance_wall_dirty) {      /* the toggle repaints the desktop too */
    appearance_wall_dirty = 0;
    wallpaper_cache_dirty = 1;
  }

  if (wallpaper_cache_dirty) {
    full_redraw = 1;
    needs_redraw = 1;
  }
  
  if (needs_redraw) {
    gui_draw_all();
    smart_swap_buffers();
    needs_redraw = 0;
    frame_count++;
  }
}


void gui_main_loop(void) {
  meep_apply_appearance(appearance_dark);   /* midnight is the boot look */
  /* Initial full redraw */
  full_redraw = 1;
  needs_redraw = 1;

  
  unsigned idle = 0;
  while (1) {
    {
      static int wx_tried = 0;
      if (!wx_tried && frame_count > 40 && e1000_link()) { wx_tried = 1; wx_fetch(); }
    }
    mp_pump();                       /* paces playback at real time */
    /* a focused Omi Net window needs a caret tick even when nothing happens */
    if (++idle > 3000000u) {
      idle = 0;
      on_caret++;
      if (mapp_focus == APP_BROWSER && mapp[APP_BROWSER].open) needs_redraw = 1;
    }
    /* Poll input aggressively - USB legacy emulation requires frequent polling */
    for (int poll = 0; poll < 20; poll++) {
      ps2_poll();
      usb_poll();
      for (volatile int d = 0; d < 100; d++) __asm__ volatile("pause");
    }
    gui_compose();






    task_yield();   /* let any other runnable task have the cpu */

    /* Minimal yield - just a few pause instructions */
    for (volatile int i = 0; i < 50; i++) {
      __asm__ volatile("pause");
    }
  }
}
