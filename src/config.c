/* config.c — ux0:/data/StremioVita/config.txt read/write
 *
 * Plain "key=value" lines; only `addon` entries exist in v1 plus a couple of
 * booleans. File format is intentionally simple so users can also edit it
 * from a PC over USB/MTP.
 */
#include "config.h"

#include <psp2/io/fcntl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_MAX_LINE 1024
#define CONFIG_MAX_ADDONS 16

config_t g_cfg;

static void strtrim(char *s)
{
	size_t n = strlen(s);
	while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' ||
	             s[n - 1] == ' ' || s[n - 1] == '\t'))
		s[--n] = '\0';
	size_t start = 0;
	while (s[start] == ' ' || s[start] == '\t') start++;
	if (start) memmove(s, s + start, strlen(s + start) + 1);
}

int config_load_or_create(void)
{
	memset(&g_cfg, 0, sizeof(g_cfg));
	g_cfg.load_images = 1;
	g_cfg.overlay_uv_order = 'v'; /* AvPlayer frames are NV21-ish (Y + VU) */

	sceIoMkdir(VITA_DATA_DIR, 0777);
	sceIoMkdir(VITA_CACHE_DIR, 0777);

	SceUID fd = sceIoOpen(CONFIG_PATH, SCE_O_RDONLY, 0);
	if (fd >= 0) {
		char line[CONFIG_MAX_LINE];
		char ch;
		size_t n = 0;
		while (sceIoRead(fd, &ch, 1) == 1) {
			if (ch == '\n') {
				line[n] = '\0';
				strtrim(line);
				if (!strncmp(line, "addon=", 6) && line[6] && g_cfg.addon_count < CONFIG_MAX_ADDONS) {
					config_addon_add(line + 6);
				} else if (!strncmp(line, "load_images=", 12)) {
					g_cfg.load_images = line[12] == '0' ? 0 : 1;
				} else if (!strncmp(line, "uv_order=", 9)) {
					g_cfg.overlay_uv_order = line[9];
				}
				n = 0;
			} else if (n + 1 < sizeof(line)) {
				line[n++] = ch;
			}
		}
		sceIoClose(fd);
	} else {
		/* first launch: seed the official metadata addon */
		config_addon_add("https://v3-cinemeta.strem.io/manifest.json");
		config_save();
	}
	return g_cfg.addon_count > 0 ? 1 : 0;
}

int config_save(void)
{
	const char *head =
		"# VitaStremio configuration\n"
		"# addon URLs, one per line. You can also edit this on a PC.\n";
	char img_line[64], uv_line[64];
	snprintf(img_line, sizeof(img_line), "load_images=%c\n", g_cfg.load_images ? '1' : '0');
	snprintf(uv_line, sizeof(uv_line), "uv_order=%c\n", g_cfg.overlay_uv_order);

	SceUID fd = sceIoOpen(CONFIG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
	if (fd < 0)
		return -1;
	sceIoWrite(fd, head, strlen(head));
	sceIoWrite(fd, img_line, strlen(img_line));
	sceIoWrite(fd, uv_line, strlen(uv_line));
	for (int i = 0; i < g_cfg.addon_count; i++) {
		char lineout[1200];
		snprintf(lineout, sizeof(lineout), "addon=%s\n", g_cfg.addon_urls[i]);
		sceIoWrite(fd, lineout, strlen(lineout));
	}
	sceIoClose(fd);
	return 0;
}

int config_addon_add(const char *url)
{
	if (g_cfg.addon_count >= CONFIG_MAX_ADDONS)
		return -1;
	for (int i = 0; i < g_cfg.addon_count; i++)
		if (!strcmp(g_cfg.addon_urls[i], url))
			return 0;
	char *copy = strdup(url);
	if (!copy) return -1;
	int n = g_cfg.addon_count;
	char **list = (char **)realloc(g_cfg.addon_urls, sizeof(char *) * (size_t)(n + 1));
	if (!list) { free(copy); return -1; }
	g_cfg.addon_urls = list;
	list[n] = copy;
	g_cfg.addon_count = n + 1;
	return 0;
}

int config_addon_remove(int index)
{
	if (index < 0 || index >= g_cfg.addon_count)
		return -1;
	free(g_cfg.addon_urls[index]);
	memmove(&g_cfg.addon_urls[index], &g_cfg.addon_urls[index + 1],
		sizeof(char *) * (size_t)(g_cfg.addon_count - index - 1));
	g_cfg.addon_count--;
	return 0;
}

void config_free(void)
{
	for (int i = 0; i < g_cfg.addon_count; i++)
		free(g_cfg.addon_urls[i]);
	free(g_cfg.addon_urls);
	g_cfg.addon_urls = NULL;
	g_cfg.addon_count = 0;
}
