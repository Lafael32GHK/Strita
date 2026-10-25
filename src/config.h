/* config.h — ux0:/data/StremioVita/config.txt read/write */
#ifndef VST_CONFIG_H
#define VST_CONFIG_H

#define VITA_DATA_DIR  "ux0:/data/StremioVita"
#define VITA_CACHE_DIR VITA_DATA_DIR "/cache"
#define CONFIG_PATH    VITA_DATA_DIR "/config.txt"

/* App settings (URL entry, addons, etc.). addon list is plain text. */
typedef struct {
	char **addon_urls;      /* heap strings */
	int addon_count;
	int load_images;        /* fetch posters/banner art: 1/0 */
	char overlay_uv_order;  /* 'v' = NV21 order (AvPlayer frames); 'u' to swap if colors are wrong */
} config_t;

extern config_t g_cfg;

int  config_load_or_create(void /* seeds default addons on first run */);
int  config_save(void);
void config_free(void);

/* manages the addon list in memory */
int  config_addon_add(const char *url);
int  config_addon_remove(int index);

#endif
