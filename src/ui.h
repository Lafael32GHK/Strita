/* ui.h — drawing helpers, font, image cache, on-screen keypad */
#ifndef VST_UI_H
#define VST_UI_H

#include <vita2d.h>
#include <stddef.h>

#define VITA_WIDTH    960
#define SCREEN_HEIGHT 544

/* palette */
#define COL_BG      RGBA8(18, 18, 24, 255)
#define COL_CARD    RGBA8(30, 30, 40, 255)
#define COL_CARD_D  RGBA8(22, 22, 30, 255)
#define COL_ACCENT  RGBA8(125, 95, 255, 255)
#define COL_TEXT    RGBA8(235, 235, 242, 255)
#define COL_TEXTD   RGBA8(150, 150, 160, 255)
#define COL_ERR     RGBA8(255, 90, 90, 255)
#define COL_OK      RGBA8(120, 220, 140, 255)

#define FS_H   22   /* header */
#define FS_D   16   /* row text */
#define FS_S   12   /* footer small */
#define FS_BIG 26   /* titles */

int ui_init(void);          /* loads fonts, returns <0 if no TTF available */
void ui_term(void);
vita2d_font *ui_font(void);

/* primitives (vita2d has no fill_rectangle; draw_rectangle is filled) */
void ui_panel(float x, float y, float w, float h, unsigned int color);
void ui_panel_dim(float x, float y, float w, float h, unsigned int color);

/* draw text with simple hardware-measured truncation ("...") if wider than w */
void ui_text_clip(vita2d_font *f, float x, float y, unsigned int color,
                  int size, const char *text, float maxw);

/* word-wrap: draws up to max_lines lines with 1.35 spacing between baseline y
 * and next; returns number of lines drawn */
void ui_text_wrap(vita2d_font *f, float x, float y, unsigned int color,
                  int size, const char *text, float maxw, int max_lines);

/* ---- poster/banner image cache (one network fetch per service call) ---- */
vita2d_texture *imgcache_get(const char *url);
void imgcache_service(void); /* call once per frame */
void imgcache_term(void);

/* ---- full-screen on-screen keypad ----
 * Buffer is edited in place. Returns 1 = accepted, 0 = cancelled. */
int keypad_run(const char *title, char *buf, size_t bufsz);

#endif
