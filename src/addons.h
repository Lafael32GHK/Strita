/* addons.h — Stremio addon manifest/catalog/meta/stream engine */
#ifndef VST_ADDONS_H
#define VST_ADDONS_H

#ifdef __cplusplus
extern "C" {
#endif

#define ST_ERRSZ 160

typedef struct {
	int  flags;
	char *id, *name, *type;
	char *year;        /* releaseInfo */
	char *rating;      /* imdbRating as string, may be NULL */
	char *description;
	char *poster;      /* url - maybe jpg/png */
	char *banner;      /* background url */
} st_meta_item;

typedef struct {
	st_meta_item *items;
	int count;
} st_catalog_page;

typedef struct {
	int parked;          /* 1 = playable via our player, 0 = not-playable */
	int torrent;         /* magnet/infoHash based */
	int hls;             /* .m3u8 */
	char *title;         /* human label, e.g. "1080p - CDN" */
	char *description;   /* extra info from addon */
	char *url;           /* direct http(s) stream source */
	char *source_addon;  /* which addon produced it */
} st_stream;

typedef struct {
	char *url_base;         /* manifest-less base of the addon */
	char *name;             /* human name from manifest */
	char *id;               /* addon id */
	int   okay;             /* 1 once the manifest parsed cleanly */
	int   res_catalog, res_meta, res_stream;
	char  error[ST_ERRSZ];  /* why it failed to load (if not okay) */

	struct {
		char *type, *cid;
		int supports_search, supports_skip, supports_genre;
	} catalogs[16];
	int catalog_count;
} st_addon;

int st_addons_count(void);
st_addon *st_addons_get(int index);
int st_addons_init(void);
void st_addons_reload(void);
void st_addons_term(void);

/* fetch one catalog page (skip/genre only honored when the addon supports it) */
int st_load_catalog(st_addon *a, const char *type, const char *cid,
                    int skip, const char *genre,
                    st_catalog_page *out, char *err, int errsize);

/* fetch a single meta item (fills an item; caller must st_meta_item_free() it) */
int st_load_meta(st_addon *a, const char *type, const char *id,
                 st_meta_item *item, char *err, int errsize);

/* fetch streams for a media item; caller frees via st_streams_free() */
int st_load_streams(st_addon *a, const char *type, const char *id,
                    st_stream **out, int *out_count, char *err, int errsize);

void st_meta_item_free(st_meta_item *item);
void st_catalog_page_free(st_catalog_page *page);
void st_streams_free(st_stream *streams, int count);

#ifdef __cplusplus
}
#endif

#endif
