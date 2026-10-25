/* ui.c — drawing helpers, font, image cache, on-screen keypad */
#include "ui.h"
#include "config.h"
#include "net.h"
#include "addons.h"

#include <psp2/ctrl.h>
#include <psp2/kernel/threadmgr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FONT_COUNT 3
static const char *k_font_paths[FONT_COUNT] = {
	"ux0:/data/StremioVita/font.ttf",          /* user override */
	"app0:/font/OpenSans-Bold.ttf",            /* bundled */
	"app0:/font.ttf",
};

static vita2d_font *g_font;
static int g_font_failed;

vita2d_font *ui_font(void)
{
	return g_font;
}

int ui_init(void)
{
	g_font_failed = 0;
	for (int i = 0; i < FONT_COUNT; i++) {
		g_font = vita2d_load_font_file(k_font_paths[i]);
		if (g_font)
			break;
	}
	if (!g_font) {
		g_font_failed = 1;
		return -1;
	}
	return 0;
}

void ui_term(void)
{
	if (g_font) {
		vita2d_free_font(g_font);
		g_font = NULL;
	}
	imgcache_term();
}

/* ---------------------------------------------------------- primitives */

void ui_panel(float x, float y, float w, float h, unsigned int color)
{
	vita2d_draw_rectangle(x, y, w, h, color);
}

void ui_panel_dim(float x, float y, float w, float h, unsigned int color)
{
	unsigned int alpha = (color & 0xFF000000u) >> 5; /* ~1/8 of the alpha */
	vita2d_draw_rectangle(x, y, w, h, RGBA8((color & 0xFF), (color >> 8) & 0xFF,
		(color >> 16) & 0xFF, alpha > 0xFF ? 0xFF : alpha));
}

void ui_text_clip(vita2d_font *f, float x, float y, unsigned int color,
                  int size, const char *text, float maxw)
{
	if (!f) return;
	if (vita2d_font_text_width(f, (unsigned int)size, text) <= maxw ||
		maxw <= 10.0f) {
		vita2d_font_draw_text(f, (int)x, (int)y, color, (unsigned int)size, "%s", text);
		return;
	}
	/* linear back-off until it fits, then append '..' */
	char buf[192];
	size_t n = strlen(text);
	if (n > sizeof(buf) - 4) n = sizeof(buf) - 4;
	memcpy(buf, text, n);
	buf[n] = '\0';
	while (strlen(buf) > 3) {
		buf[strlen(buf) - 1] = '\0';
		int w = vita2d_font_text_width(f, (unsigned int)size, buf);
		if (w <= maxw - 12) {
			strcat(buf, "..");
			vita2d_font_draw_text(f, (int)x, (int)y, color, (unsigned int)size, "%s", buf);
			return;
		}
	}
}

void ui_text_wrap(vita2d_font *f, float x, float y, unsigned int color,
                  int size, const char *text, float maxw, int max_lines)
{
	if (!f || !text || !*text) return;
	char line[256];
	int lh = (int)(size * 1.4f);
	int lines = 0;
	const char *p = text;

	while (*p && lines < max_lines) {
		/* fill as many words as fit */
		line[0] = '\0';
		while (*p) {
			size_t wstart = 0;
			while (p[wstart] == ' ') wstart++;
			size_t wlen = strcspn(p + wstart, " ");
			if (*p != ' ' && wlen == 0)
				wlen = strcspn(p, " ");
			size_t take = (wstart == 0 && *p != ' ') ? 0 : wstart;
			(void)take;
			char wbuf[96];
			size_t wl = sizeof(wbuf) - 1 < wlen ? sizeof(wbuf) - 1 : wlen;
			memcpy(wbuf, p + wstart, wl);
			wbuf[wl] = '\0';

			size_t curlen = strlen(line);
			size_t add = wl + (line[0] ? 1 : 0);
			if (curlen + add >= sizeof(line)) break;
			char trial[256];
			snprintf(trial, sizeof(trial), "%s%s%s", line[0] ? line : "", line[0] ? " " : "", wbuf);
			int tw = vita2d_font_text_width(f, (unsigned int)size, trial);
			if (tw > maxw && line[0]) break;
			snprintf(line, sizeof(line), "%s", trial);

			p += wstart + wl;
			while (*p == ' ') p++;
		}
		if (!line[0]) {
			/* single word longer than maxw: hard chop */
			size_t avail = strcspn(p, " ");
			size_t n = avail < sizeof(line) - 1 ? avail : sizeof(line) - 1;
			memcpy(line, p, n);
			line[n] = '\0';
			p += n;
		}
		if (!line[0]) break;
		lines++;
		vita2d_font_draw_text(f, (int)x, (int)y + lines * lh, color, (unsigned int)size, "%s", line);
		if (*p == '\0' || lines >= max_lines) break;
	}
}

/* ---------------------------------------------------------- img cache */

#define IMG_SLOTS 64
typedef struct {
	char *url;              /* NULL = free slot */
	vita2d_texture *tex;
	int state;              /* 0 = pending, 1 = loaded, -1 = failed */
} imgslot;

static imgslot g_cache[IMG_SLOTS];

static imgslot *slot_find(const char *url)
{
	for (int i = 0; i < IMG_SLOTS; i++)
		if (g_cache[i].url && !strcmp(g_cache[i].url, url))
			return &g_cache[i];
	return NULL;
}

static imgslot *slot_alloc(const char *url)
{
	/* reuse failed slot with same url first */
	for (int i = 0; i < IMG_SLOTS; i++)
		if (!g_cache[i].url)
			return &g_cache[i];
	/* evict a loaded-but-oldest slot (report LRU-ish: lowest index loaded) */
	for (int i = 0; i < IMG_SLOTS; i++)
		if (g_cache[i].tex) {
			vita2d_wait_rendering_done();
			vita2d_free_texture(g_cache[i].tex);
			g_cache[i].tex = NULL;
			free(g_cache[i].url);
			g_cache[i].url = NULL;
			g_cache[i].state = 0;
			return &g_cache[i];
		}
	return NULL;
}

vita2d_texture *imgcache_get(const char *url)
{
	if (!url || !*url)
		return NULL;
	imgslot *s = slot_find(url);
	if (s && s->state == 1)
		return s->tex;
	if (!s) {
		s = slot_alloc(url);
		if (!s) return NULL;
		s->url = strdup(url);
		s->tex = NULL;
		s->state = 0;
	}
	return NULL;
}

void imgcache_service(void)
{
	if (!g_cfg.load_images)
		return;
	for (int i = 0; i < IMG_SLOTS; i++) {
		if (g_cache[i].url && g_cache[i].state == 0) {
			void *data = NULL;
			size_t len = 0;
			if (http_get_image(g_cache[i].url, &data, &len) < 0) {
				g_cache[i].state = -1;
				continue;
			}
			unsigned char *b = (unsigned char *)data;
			if (b[0] == 0x89 && b[1] == 'P') {
				g_cache[i].tex = vita2d_load_PNG_buffer(data);
			} else {
				g_cache[i].tex = vita2d_load_JPEG_buffer(data, len);
			}
			free(data);
			g_cache[i].state = g_cache[i].tex ? 1 : -1;
			return; /* at most one download per frame */
		}
	}
}

void imgcache_term(void)
{
	for (int i = 0; i < IMG_SLOTS; i++) {
		if (g_cache[i].tex) {
			vita2d_wait_rendering_done();
			vita2d_free_texture(g_cache[i].tex);
		}
		free(g_cache[i].url);
		g_cache[i].url = NULL;
		g_cache[i].tex = NULL;
		g_cache[i].state = 0;
	}
}

/* ---------------------------------------------------------- keypad */

#define KB_COLS 11
#define KB_ROWS 6
static const char *kb_rows[KB_ROWS] = {
	"1234567890",
	"qwertyuiop",
	"asdfghjkl",
	"zxcvbnm",
	":/.-_",
	" ",
};
struct kb_state {
	int row, col;
};

static void kb_draw_row(vita2d_font *f, const char *row, int y, int sel)
{
	int x = 40;
	int cw = 78;
	for (int i = 0; (unsigned)i < strlen(row); i++) {
		int is_sel = (i == sel);
		char one[2] = { row[i], 0 };
		if (is_sel)
			ui_panel_dim(x - 22, y - 24, cw - 26, 30, COL_ACCENT);
		vita2d_font_draw_text(f, x, y, is_sel ? COL_BG : COL_TEXT,
			FS_D + 4, "%s", one == NULL ? "" : one);
		x += cw;
	}
}

int keypad_run(const char *title, char *buf, size_t bufsz)
{
	uint32_t prev = 0;
	int repeat = 0;
	int sr = 0, sc = 0;
	int done = 0;

	while (!done) {
		sceKernelDelayThread(16 * 1000);
		SceCtrlData pad;
		sceCtrlReadBufferPositive(&pad, 1);
		uint32_t pressed = pad.buttons & ~prev;
		uint32_t held = pad.buttons & prev;
		prev = pad.buttons;

		/* movement with selection state */
		if (pressed & SCE_CTRL_UP) {
			sr = sr > 0 ? sr - 1 : KB_ROWS - 1;
			repeat = 0;
		} else if (pressed & SCE_CTRL_DOWN) {
			sr = (sr + 1) % KB_ROWS;
			repeat = 0;
		} else if (pressed & SCE_CTRL_LEFT) {
			sc--;
			repeat = 0;
		} else if (pressed & SCE_CTRL_RIGHT) {
			sc++;
			repeat = 0;
		}

		int rowlen = (int)strlen(kb_rows[sr]);
		if (sc < 0) sc = rowlen - 1;
		if (sc >= rowlen) sc = 0;

		if (held & SCE_CTRL_CROSS) {
			repeat++;
			if (repeat > 25) {
				repeat = 0;
				size_t cl = strlen(buf);
				if (cl + 1 < bufsz) {
					buf[cl] = kb_rows[sr][sc];
					buf[cl + 1] = '\0';
				}
			}
		} else {
			repeat = 0;
		}
		if (pressed & SCE_CTRL_CROSS) {
			size_t cl = strlen(buf);
			if (cl + 1 < bufsz) {
				buf[cl] = kb_rows[sr][sc];
				buf[cl + 1] = '\0';
			}
		}
		if (pressed & SCE_CTRL_SELECT) {
			size_t cl = strlen(buf);
			if (cl) buf[cl - 1] = '\0';
		}
		if (pressed & SCE_CTRL_START)
			done = 1;
		if (pressed & SCE_CTRL_CIRCLE)
			done = -1;

		/* ---------- draw ---------- */
		vita2d_start_drawing();
		vita2d_clear_screen();

		if (g_font) {
			vita2d_font_draw_text(g_font, 20, 34, COL_TEXT, FS_H, "%s", title);
			ui_panel(20, 48, VITA_WIDTH - 40, 64, COL_CARD_D);
			vita2d_font_draw_text(g_font, 32, 90, COL_TEXT, FS_D + 4, "%s", *buf ? buf : "(empty)");

			int y = 150;
			for (int r = 0; r < KB_ROWS; r++) {
				const char *rw = kb_rows[r];
				if (!strcmp(rw, " ")) {
					/* space bar row */
					if (r == sr)
						ui_panel_dim(60, y - 24, VITA_WIDTH - 120, 30, COL_ACCENT);
					vita2d_font_draw_text(g_font, VITA_WIDTH / 2, y, r == sr ? COL_BG : COL_TEXT, FS_S, "%s", "space");
					y += 44;
				} else {
					int x = 40;
					int cw = 78;
					for (int i = 0; (unsigned)i < strlen(rw); i++) {
						int is_sel = (i == sc && r == sr);
						char one[2] = { rw[i], 0 };
						if (is_sel)
							ui_panel_dim(x - 22, y - 26, cw - 26, 34, COL_ACCENT);
						vita2d_font_draw_text(g_font, x, y - 1,
							is_sel ? COL_BG : COL_TEXT, FS_D + 4, "%s", one);
						x += cw;
					}
					y += 44;
				}
			}

			vita2d_font_draw_text(g_font, 20, SCREEN_HEIGHT - 10, COL_TEXTD, FS_S,
				"%s", "(X) type   (SELECT) backspace   (START) done   (O) cancel");
		}

		vita2d_end_drawing();
		vita2d_swap_buffers();
	}

	return done == 1 ? 1 : 0;
}
