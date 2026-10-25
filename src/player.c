/* player.c — SceAvPlayer playback of HTTP MP4 streams (Stremio Vita)
 *
 * SceAvPlayer gives us the Vita's hardware H.264/AAC decoder. To feed it a
 * network stream we implement its FileReplacement I/O with HTTP Range
 * requests (open/readOffset/size over our SceHttp layer).
 *
 * Video frames come back as Y + interleaved VU ("YVU420P2" / NV21 ordering).
 * We hand the raw frame data straight to the GPU as a GXM texture in that
 * format — sampling uses the hardware CSC1 color-space conversion, so no
 * CPU conversion is needed (same approach Vita-Media-Player uses).
 * If colors look wrong on your unit, set uv_order=u in config.txt to use
 * the CPU fallback ordering.
 */
#include "player.h"
#include "net.h"
#include "config.h"

#include <psp2/sysmodule.h>
#include <psp2/avplayer.h>
#include <psp2/audioout.h>
#include <psp2/display.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/ctrl.h>
#include <psp2/types.h>
#include <vita2d.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

/* Event ids (SceAvPlayer state events, identical family to the PS4 SDK):
 * 1 = state stop, 2 = state ready, 3 = state play, 4 = pause,
 * 5 = buffering, 8 = timed text delivery. */
#define EVT_STATE_STOP     1
#define EVT_STATE_READY    2
#define EVT_STATE_PLAY     3
#define EVT_STATE_BUFFERING 5

#define HUD_OFF        0
#define HUD_SECONDS    30

typedef struct {
	const char *url;
	uint64_t size;
} net_file_t;

static net_file_t g_file;
static volatile int g_eof;
static volatile int g_buffering;
static volatile SceBool g_started;
static volatile int g_enabled;
static char g_error[192];
static SceAvPlayerHandle g_player;

/* ---- allocator replacement ---------------------------------------------- */

static void *mem_alloc(void *arg, uint32_t alignment, uint32_t size)
{
	(void)arg;
	return memalign(alignment ? alignment : 16, size);
}

static void mem_free(void *arg, void *p)
{
	(void)arg;
	free(p);
}

/* ---- file replacement over HTTP ------------------------------------------- */

static int fr_open(void *p, const char *filename)
{
	net_file_t *f = (net_file_t *)p;
	if (!f->size) {
		if (http_probe_size(filename, &f->size) < 0) {
			snprintf(g_error, sizeof(g_error), "size probe failed: %.120s", net_last_error());
			return -1;
		}
	}
	if (!f->size) {
		snprintf(g_error, sizeof(g_error), "stream has unknown length");
		return -1;
	}
	return 0;
}

static int fr_close(void *p)
{
	(void)p;
	return 0;
}

static int fr_read_offset(void *p, uint8_t *buffer, uint64_t position, uint32_t length)
{
	net_file_t *f = (net_file_t *)p;
	if (position >= f->size)
		return -1;
	uint32_t want = length;
	if ((uint64_t)want > f->size - position)
		want = (uint32_t)(f->size - position);
	unsigned int got = 0;
	uint64_t total = 0;
	int rc = http_read_range(f->url, position, buffer, want, &got, &total);
	if (rc < 0 && total) {
		/* size arrived late (server ignored the initial probe) */
		if (total != f->size) {
			f->size = total;
			if ((uint64_t)want > f->size - position)
				want = (uint32_t)(f->size - position);
			g_buffering = 1;
			return fr_read_offset(p, buffer, position, want);
		}
		g_buffering = 1;
		return -1;
	}
	if (rc < 0) {
		g_buffering = 1;
		return -1;
	}
	g_buffering = 0;
	return (int)got;
}

static uint64_t fr_size(void *p)
{
	net_file_t *f = (net_file_t *)p;
	return f->size;
}

/* ---- event callback ------------------------------------------------------- */

static uint64_t g_duration_ms;

static void ev_callback(void *p, int32_t eventId, int32_t sourceId, void *eventData)
{
	(void)p; (void)sourceId; (void)eventData;
	switch (eventId) {
	case EVT_STATE_READY:
		g_started = SCE_TRUE;
		break;
	case EVT_STATE_PLAY:
		g_started = SCE_TRUE;
		g_buffering = 0;
		break;
	case EVT_STATE_STOP:
		g_eof = 1;
		break;
	case EVT_STATE_BUFFERING:
		g_buffering = 1;
		break;
	default:
		break;
	}
}

/* ---- audio output thread -------------------------------------------------- */

static volatile int g_run;
static SceUID g_audio_thread;
static int g_port = -1;
static int g_volume = 100;   /* 0..100, changed from the UI thread */

static SceInt32 audio_thread(SceSize args, void *argp)
{
	(void)args; (void)argp;
	static int16_t silence[2 * 1024];
	memset(silence, 0, sizeof(silence));

	SceAvPlayerFrameInfo af;
	memset(&af, 0, sizeof(af));
	int cur_freq = 48000, cur_ch = 2;

	while (g_run) {
		int did = 0;
		if (sceAvPlayerIsActive(g_player) && sceAvPlayerGetAudioData(g_player, &af)) {
			int freq = (int)af.details.audio.sampleRate;
			int ch = af.details.audio.channelCount ? (int)af.details.audio.channelCount : 2;
			int bytes = (int)af.details.audio.size;
			if (freq <= 0) freq = 48000;
			if (bytes > 0) {
				if (freq != cur_freq || ch != cur_ch) {
					if (sceAudioOutSetConfig(g_port, -1, freq,
						ch == 1 ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO) >= 0) {
						cur_freq = freq;
						cur_ch = ch;
					}
				}
				int vol = g_volume * SCE_AUDIO_OUT_MAX_VOL / 100;
				int vols[2] = { vol, vol };
				sceAudioOutSetVolume(g_port,
					SCE_AUDIO_OUT_CHANNEL_FLAG_LEFT | SCE_AUDIO_OUT_CHANNEL_FLAG_RIGHT, vols);
				sceAudioOutOutput(g_port, af.pData);
				did = 1;
			}
		}
		if (!did) {
			int vol = g_volume * SCE_AUDIO_OUT_MAX_VOL / 100;
			int vols[2] = { vol, vol };
			sceAudioOutSetVolume(g_port,
				SCE_AUDIO_OUT_CHANNEL_FLAG_LEFT | SCE_AUDIO_OUT_CHANNEL_FLAG_RIGHT, vols);
			sceAudioOutOutput(g_port, silence);
			sceKernelDelayThread(20 * 1000);
		}
	}
	return sceKernelExitThread(0);
}

/* ---- helpers --------------------------------------------------------------- */

static void fmt_time(char *out, size_t cap, uint64_t ms)
{
	uint64_t total = ms / 1000;
	snprintf(out, cap, "%02u:%02u:%02u",
		(unsigned)(total / 3600), (unsigned)((total / 60) % 60), (unsigned)(total % 60));
}

static void draw_hud(vita2d_font *font, const char *title, int paused,
                     int hud_visible, uint64_t cur_ms, uint64_t file_size,
                     uint32_t vw, uint32_t vh)
{
	if (!font)
		return;
	if (hud_visible) {
		char tbuf[16];
		fmt_time(tbuf, sizeof(tbuf), cur_ms);
		char sbuf[96];
		snprintf(sbuf, sizeof(sbuf), "%u x %u  |  %.1f MB  |  %s%s",
			vw, vh, (double)file_size / 1048576.0, tbuf, paused ? "  [paused]" : "");
		vita2d_draw_rectangle(0, 0, VITA_WIDTH, 26, RGBA8(0, 0, 0, 180));
		vita2d_draw_rectangle(0, SCREEN_HEIGHT - 26, VITA_WIDTH, 26, RGBA8(0, 0, 0, 180));
		ui_text_clip(font, 8, 19, RGBA8(255, 255, 255, 255), 17, title, VITA_WIDTH - 200);
		vita2d_font_draw_text(font, VITA_WIDTH - 8 - (int)vita2d_font_text_width(font, 15, sbuf),
			SCREEN_HEIGHT - 9, RGBA8(255, 255, 255, 255), 15, "%s", sbuf);
		if (g_buffering) {
			const char *btxt = "buffering...";
			int bw = (int)vita2d_font_text_width(font, 18, btxt);
			vita2d_font_draw_text(font, (VITA_WIDTH - bw) / 2, 120, RGBA8(255, 255, 90, 255), 18, "%s", btxt);
		}
	} else {
		if (g_buffering || paused) {
			const char *msg = g_buffering ? "buffering..." : "[ paused - press (X) ]";
			int bw = (int)vita2d_font_text_width(font, 18, msg);
			vita2d_draw_rectangle(0, 0, VITA_WIDTH, 30, RGBA8(0, 0, 0, 150));
			vita2d_font_draw_text(font, (VITA_WIDTH - bw) / 2, 22, RGBA8(255, 255, 255, 255), 18, "%s", msg);
		}
	}
}

int player_play(const char *url, const char *title, st_player_status *status_out)
{
	memset(g_error, 0, sizeof(g_error));
	g_eof = g_buffering = 0;
	g_started = SCE_FALSE;
	g_duration_ms = 0;
	memset(&g_file, 0, sizeof(g_file));
	g_file.url = url;
	g_volume = 100;

	if (sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER) < 0) {
		snprintf(g_error, sizeof(g_error), "failed to load AVPLAYER module");
		return -1;
	}

	/* input polling context */
	static SceCtrlData pad;
	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
	memset(&pad, 0, sizeof(pad));

	SceAvPlayerInitData init;
	memset(&init, 0, sizeof(init));
	init.memoryReplacement.objectPointer = NULL;
	init.memoryReplacement.allocate = mem_alloc;
	init.memoryReplacement.deallocate = mem_free;
	init.memoryReplacement.allocateTexture = mem_alloc;
	init.memoryReplacement.deallocateTexture = mem_free;
	init.fileReplacement.objectPointer = &g_file;
	init.fileReplacement.open = fr_open;
	init.fileReplacement.close = fr_close;
	init.fileReplacement.readOffset = fr_read_offset;
	init.fileReplacement.size = fr_size;
	init.eventReplacement.objectPointer = NULL;
	init.eventReplacement.eventCallback = ev_callback;
	init.basePriority = 0;            /* use sensible default thread priority */
	init.autoStart = SCE_TRUE;        /* start feeding as soon as the source is ready */
	init.numOutputVideoFrameBuffers = 3;
	init.debugLevel = 0;

	SceAvPlayerHandle player = sceAvPlayerInit(&init);
	if (player < 0) {
		snprintf(g_error, sizeof(g_error), "sceAvPlayerInit failed (0x%08X)", player);
		sceSysmoduleUnloadModule(SCE_SYSMODULE_AVPLAYER);
		return -1;
	}
	g_player = player;

	/* open the audio port while the source is still being probed */
	g_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, 1024, 48000, SCE_AUDIO_OUT_MODE_STEREO);
	if (g_port < 0)
		g_port = -1;

	g_run = 1;
	SceUID audio_thread_id = sceKernelCreateThread("st_audio", audio_thread, 0x10000100, 16 * 1024, 0, 0, NULL);
	if (audio_thread_id >= 0)
		sceKernelStartThread(audio_thread_id, 0, NULL);

	int rc = sceAvPlayerAddSource(player, url);
	if (rc < 0) {
		snprintf(g_error, sizeof(g_error), "cannot open stream (0x%08X) — only progressive MP4 works", rc);
		sceAvPlayerClose(player);
		if (g_port >= 0) { sceAudioOutReleasePort(g_port); g_port = -1; }
		g_run = 0;
		if (audio_thread_id >= 0) sceKernelWaitThreadEnd(audio_thread_id, NULL, NULL);
		sceSysmoduleUnloadModule(SCE_SYSMODULE_AVPLAYER);
		return -1;
	}

	/* wait for the file replacement probe (called from the player's thread) */
	int wait = 0;
	while (!g_started && !g_eof && g_error[0] == '\0' && wait < 400) {
		if (!g_enabled && sceAvPlayerStreamCount(player) > 0) {
			/* belt and braces: explicitly enable video + audio streams in case
			 * autoStart does not imply stream enabling on this firmware */
			int sc = sceAvPlayerStreamCount(player);
			for (int i = 0; i < sc; i++) {
				SceAvPlayerStreamInfo si;
				memset(&si, 0, sizeof(si));
				if (sceAvPlayerGetStreamInfo(player, (uint32_t)i, &si) >= 0 &&
					(si.type == SCE_AVPLAYER_VIDEO || si.type == SCE_AVPLAYER_AUDIO)) {
					sceAvPlayerEnableStream(player, (uint32_t)i);
				}
			}
			g_enabled = 1;
		}
		sceDisplayWaitVblankStart();
		wait++;
	}

	vita2d_texture *vidtex = NULL;
	vita2d_texture *rgb_tex = NULL;   /* used by the CPU-convert fallback */

	int paused = 0;
	int hud = HUD_OFF;

	uint32_t vw = 0, vh = 0;
	int have_frame = 0;
	double tot = (double)g_file.size;

	for (;;) {
		sceDisplayWaitVblankStart();

		int terminate = 0;
		SceCtrlData pad_new;
		sceCtrlReadBufferPositive(&pad_new, 1);
		static uint32_t prev = 0;
		uint32_t pressed = pad_new.buttons & ~prev;
		prev = pad_new.buttons;

		if (pressed & SCE_CTRL_CIRCLE)
			terminate = 1;
		if (pressed & SCE_CTRL_CROSS) {
			if (sceAvPlayerPause(player) < 0)
				sceAvPlayerResume(player);
			else
				paused = 1;
		}
		if (pressed & SCE_CTRL_TRIANGLE) {
			sceAvPlayerResume(player);
			paused = 0;
		}
		if (pressed & SCE_CTRL_START) {
			hud = (hud + 1) % 3;
		}
		if (pressed & SCE_CTRL_LEFT)
			sceAvPlayerJumpToTime(player, sceAvPlayerCurrentTime(player) - HUD_SECONDS * 1000ULL);
		if (pressed & SCE_CTRL_RIGHT)
			sceAvPlayerJumpToTime(player, sceAvPlayerCurrentTime(player) + HUD_SECONDS * 1000ULL);
		if (pressed & SCE_CTRL_UP)
			g_volume = g_volume < 90 ? g_volume + 10 : 100;
		if (pressed & SCE_CTRL_DOWN)
			g_volume = g_volume > 10 ? g_volume - 10 : 0;

		SceAvPlayerFrameInfo frame;
		memset(&frame, 0, sizeof(frame));
		if (sceAvPlayerGetVideoData(player, &frame) && frame.pData) {
			uint32_t w = frame.details.video.width;
			uint32_t h = frame.details.video.height;
			if (w && h) {
				if (g_cfg.overlay_uv_order == 'v') {
					/* GPU path: raw Y + interleaved VU planes, hardware CSC */
					if (!vidtex || (uint32_t)vita2d_texture_get_width(vidtex) != w ||
						(uint32_t)vita2d_texture_get_height(vidtex) != h) {
						if (vidtex) { vita2d_wait_rendering_done(); vita2d_free_texture(vidtex); vidtex = NULL; }
						vidtex = vita2d_create_empty_texture_format(w, h, SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1);
						if (rgb_tex) { vita2d_wait_rendering_done(); vita2d_free_texture(rgb_tex); rgb_tex = NULL; }
						vw = w; vh = h;
					}
					if (vidtex &&
						sceGxmTextureInitLinear(&vidtex->gxm_tex, frame.pData,
							SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1, (int)w, (int)h, 0) >= 0) {
						vita2d_texture_set_filters(vidtex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
						have_frame = 1;
					}
				} else {
					/* CPU fallback: NV12/NV21 -> RGBA */
					if (!rgb_tex || (uint32_t)vita2d_texture_get_width(rgb_tex) != w ||
						(uint32_t)vita2d_texture_get_height(rgb_tex) != h) {
						if (rgb_tex) { vita2d_wait_rendering_done(); vita2d_free_texture(rgb_tex); rgb_tex = NULL; }
						rgb_tex = vita2d_create_empty_texture_format(w, h, SCE_GXM_TEXTURE_FORMAT_U8U8U8X8_RGBA);
						if (vidtex) { vita2d_wait_rendering_done(); vita2d_free_texture(vidtex); vidtex = NULL; }
						vw = w; vh = h;
					}
					if (rgb_tex) {
						int uv_first_is_u = (g_cfg.overlay_uv_order == 'u');
						uint8_t *dst = (uint8_t *)vita2d_texture_get_datap(rgb_tex);
						unsigned int stride = vita2d_texture_get_stride(rgb_tex);
						const uint8_t *Y = frame.pData;
						const uint8_t *UV = Y + (size_t)w * h;
						for (uint32_t y = 0; y < h; y++) {
							const uint8_t *yline = Y + (size_t)y * w;
							const uint8_t *cline = UV + (size_t)(y >> 1) * w;
							uint8_t *oline = dst + (size_t)y * stride;
							for (uint32_t x = 0; x < w; x += 2) {
								int cadr = (int)x; /* chroma plane stride == width */
								int yy0 = yline[x] - 16;
								int yy1 = (x + 1 < w) ? yline[x + 1] - 16 : yy0;
								if (yy0 < 0) yy0 = 0;
								if (yy1 < 0) yy1 = 0;
								int cb = cline[cadr + (uv_first_is_u ? 0 : 1)] - 128;
								int cr = cline[cadr + (uv_first_is_u ? 1 : 0)] - 128;
								for (int k = 0; k < 2 && (x + (uint32_t)k) < w; k++) {
									int yy = k ? yy1 : yy0;
									int pr = (298 * yy + 409 * cr + 128) >> 8;
									int pg = (298 * yy - 100 * cb - 208 * cr + 128) >> 8;
									int pb = (298 * yy + 516 * cb + 128) >> 8;
									uint8_t *o = oline + (x + (uint32_t)k) * 4;
									o[0] = (uint8_t)(pr < 0 ? 0 : pr > 255 ? 255 : pr);
									o[1] = (uint8_t)(pg < 0 ? 0 : pg > 255 ? 255 : pg);
									o[2] = (uint8_t)(pb < 0 ? 0 : pb > 255 ? 255 : pb);
									o[3] = 255;
								}
							}
						}
						have_frame = 1;
					}
				}
			}
		}

		/* ---- render ---- */
		vita2d_start_drawing();
		vita2d_clear_screen();
		if (!have_frame) {
			if (g_eof) {
				vita2d_font_draw_textf(ui_font(), VITA_WIDTH / 2, 100, RGBA8(255, 255, 255, 255), 18, "done");
			} else {
				vita2d_font_draw_textf(ui_font(), VITA_WIDTH / 2, 100, RGBA8(255, 255, 255, 255), 18, "loading stream...");
				vita2d_font_draw_textf(ui_font(), VITA_WIDTH / 2, 130, RGBA8(180, 180, 180, 255), 14, "%s", title ? title : url);
			}
			if (g_error[0])
				vita2d_font_draw_textf(ui_font(), VITA_WIDTH / 2, 160, RGBA8(255, 80, 80, 255), 15, "%.90s", g_error);
		} else if (vidtex && g_cfg.overlay_uv_order == 'v' && vw && vh) {
			double scale = (double)VITA_WIDTH / (double)vw;
			if (scale * vh > SCREEN_HEIGHT)
				scale = (double)SCREEN_HEIGHT / (double)vh;
			float x = (float)((VITA_WIDTH - vw * scale) / 2);
			float y = (float)((SCREEN_HEIGHT - vh * scale) / 2);
			vita2d_draw_texture_scale(vidtex, x, y, (float)scale, (float)scale);
		} else if (rgb_tex && g_cfg.overlay_uv_order != 'v' && vw && vh) {
			double scale = (double)VITA_WIDTH / (double)vw;
			if (scale * vh > SCREEN_HEIGHT)
				scale = (double)SCREEN_HEIGHT / (double)vh;
			float x = (float)((VITA_WIDTH - vw * scale) / 2);
			float y = (float)((SCREEN_HEIGHT - vh * scale) / 2);
			vita2d_draw_texture_scale(rgb_tex, x, y, (float)scale, (float)scale);
		}
		{
			vita2d_font *f = ui_font();
			uint64_t cur = sceAvPlayerCurrentTime(player);
			draw_hud(f, title ? title : "stream", paused, hud, cur, g_file.size, vw, vh);
		}
		vita2d_end_drawing();
		vita2d_swap_buffers();

		if (terminate || g_eof || (g_error[0] && !have_frame))
			break;
		if (!sceAvPlayerIsActive(player) && !have_frame)
			break;
	}

	/* teardown */
	sceAvPlayerClose(player);
	if (g_port >= 0) { sceAudioOutReleasePort(g_port); g_port = -1; }
	g_run = 0;
	if (audio_thread_id >= 0)
		sceKernelWaitThreadEnd(audio_thread_id, NULL, NULL);
	if (vidtex) { vita2d_wait_rendering_done(); vita2d_free_texture(vidtex); vidtex = NULL; }
	if (rgb_tex) { vita2d_wait_rendering_done(); vita2d_free_texture(rgb_tex); rgb_tex = NULL; }
	sceSysmoduleUnloadModule(SCE_SYSMODULE_AVPLAYER);

	if (status_out) {
		status_out->progress = 0;
		status_out->connecting = 0;
		status_out->buffering = 0;
	}
	if (g_error[0])
		return -1;
	return 0;
}
