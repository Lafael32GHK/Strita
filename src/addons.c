/* addons.c — Stremio addon manifest/catalog/meta/stream engine
 *
 * Speaks the basic subset of the Stremio Addon protocol (v3 HTTP addons):
 *   {base}/manifest.json            -> resources + catalog list
 *   {base}/catalog/{type}/{id}.json ( + optional /skip=N, /genre={g}) -> metas
 *   {base}/meta/{type}/{id}.json    -> metadata
 *   {base}/stream/{type}/{id}.json  -> playable sources
 */
#include "addons.h"
#include "config.h"
#include "json.h"
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static st_addon *g_addons;
static int g_addon_count;

/* ---------------------------------------------------------------- helpers */

static char *dupstr(const char *s)
{
	if (!s) return NULL;
	size_t n = strlen(s);
	char *out = (char *)malloc(n + 1);
	if (out) memcpy(out, s, n + 1);
	return out;
}

static void normalize_config_url(const char *in, char *out, size_t outsz)
{
	snprintf(out, outsz, "%s", in);
	while (strlen(out) > 1 && out[strlen(out) - 1] == '/')
		out[strlen(out) - 1] = '\0';
	if (!strstr(out, "manifest.json")) {
		size_t cur = strlen(out);
		snprintf(out + cur, outsz - cur, "/manifest.json");
	}
}

static void build_url(char *dst, size_t dstsz, st_addon *a, const char *kind,
                      const char *type, const char *cid, int skip,
                      const char *genre, const char *search)
{
	char extras[256] = "";
	int have = 0;
	if (skip >= 0) {
		size_t l = strlen(extras);
		snprintf(extras + l, sizeof(extras) - l, "%sskip=%d", have ? "&" : "", skip);
		have = 1;
	}
	if (genre && *genre) {
		size_t l = strlen(extras);
		snprintf(extras + l, sizeof(extras) - l, "%sgenre=%s", have ? "&" : "", genre);
		have = 1;
	}
	if (search && *search) {
		size_t l = strlen(extras);
		snprintf(extras + l, sizeof(extras) - l, "%ssearch=%s", have ? "&" : "", search);
		have = 1;
	}
	if (kind && cid) {
		if (have)
			snprintf(dst, dstsz, "%s/%s/%s/%s/%s.json", a->url_base, kind, type, cid, extras);
		else
			snprintf(dst, dstsz, "%s/%s/%s/%s.json", a->url_base, kind, type, cid);
	} else if (type) {
		snprintf(dst, dstsz, "%s/%s/%s.json", a->url_base, kind, type);
	} else {
		snprintf(dst, dstsz, "%s/manifest.json", a->url_base);
	}
}

void st_meta_item_free(st_meta_item *item)
{
	free(item->id); free(item->name); free(item->type);
	free(item->year); free(item->rating);
	free(item->description); free(item->poster); free(item->banner);
	memset(item, 0, sizeof(*item));
}

void st_catalog_page_free(st_catalog_page *page)
{
	if (!page) return;
	for (int i = 0; i < page->count; i++)
		st_meta_item_free(&page->items[i]);
	free(page->items);
	page->items = NULL;
	page->count = 0;
}

void st_streams_free(st_stream *streams, int count)
{
	if (!streams) return;
	for (int i = 0; i < count; i++) {
		free(streams[i].title);
		free(streams[i].description);
		free(streams[i].url);
		free(streams[i].source_addon);
	}
	free(streams);
}

/* ---------------------------------------------------------------- manifest */

static void addon_free(st_addon *a)
{
	if (!a) return;
	for (int i = 0; i < a->catalog_count; i++) {
		free(a->catalogs[i].type);
		free(a->catalogs[i].cid);
	}
	free(a->url_base);
	free(a->name);
	free(a->id);
	memset(a, 0, sizeof(*a));
}

static void parse_catalog_entry(st_addon *a, const json_value *entry, const char *fallback_type)
{
	if (a->catalog_count >= (int)(sizeof(a->catalogs) / sizeof(a->catalogs[0])))
		return;

	const char *type = fallback_type;
	const char *cid = "top";
	const json_value *extra = NULL;

	if (entry && entry->type == JSON_OBJ) {
		type = json_str(entry, "type", fallback_type);
		cid  = json_str(entry, "id", "top");
		extra = json_get(entry, "extra");
	}
	if (!type || !*type) return;
	if (!cid || !*cid) cid = "top";

	int idx = a->catalog_count;
	a->catalogs[idx].type = dupstr(type);
	a->catalogs[idx].cid = dupstr(cid);

	if (extra && extra->type == JSON_OBJ) {
		const json_value *supported = json_get(extra, "supported");
		int n = json_arr_count(supported);
		for (int i = 0; i < n; i++) {
			const json_value *s = json_arr_get(supported, i);
			if (!s || s->type != JSON_STR) continue;
			if (!strcmp(s->str, "search")) a->catalogs[idx].supports_search = 1;
			if (!strcmp(s->str, "skip"))   a->catalogs[idx].supports_skip = 1;
			if (!strcmp(s->str, "genre"))  a->catalogs[idx].supports_genre = 1;
		}
	}
	a->catalog_count++;
}

static int addon_load_manifest(st_addon *a, const char *manifest_url)
{
	char *body = NULL;
	size_t blen = 0;

	if (http_get_buf(manifest_url, &body, &blen, 2u * 1024 * 1024) < 0) {
		snprintf(a->error, sizeof(a->error), "%.150s", net_last_error());
		return -1;
	}
	json_value *root = json_parse(body, blen);
	free(body);
	if (!root || root->type != JSON_OBJ) {
		json_free(root);
		snprintf(a->error, sizeof(a->error), "manifest is not valid JSON");
		return -1;
	}

	a->id = dupstr(json_str(root, "id", "unknown"));
	a->name = dupstr(json_str(root, "name", "Addon"));
	a->okay = 1;

	const json_value *res = json_get(root, "resources");
	int nres = json_arr_count(res);
	for (int i = 0; i < nres; i++) {
		const json_value *r = json_arr_get(res, i);
		const char *rn = NULL;
		if (r->type == JSON_STR) {
			rn = r->str;
		} else if (r->type == JSON_OBJ) {
			rn = json_str(r, "name", NULL);
		}
		if (!rn) continue;
		if (!strcmp(rn, "catalog")) a->res_catalog = 1;
		if (!strcmp(rn, "meta"))    a->res_meta = 1;
		if (!strcmp(rn, "stream"))  a->res_stream = 1;
	}

	const json_value *cats = json_get(root, "catalogs");
	int ncats = json_arr_count(cats);
	for (int i = 0; i < ncats; i++) {
		const json_value *c = json_arr_get(cats, i);
		if (c->type == JSON_STR)
			parse_catalog_entry(a, NULL, c->str);
		else
			parse_catalog_entry(a, c, NULL);
	}

	json_free(root);
	return a->okay ? 0 : -1;
}

/* ------------------------------------------------------------- lifecycle */

static void addon_push(const char *cfg_url)
{
	char url[640];
	normalize_config_url(cfg_url, url, sizeof(url));

	char base[640];
	snprintf(base, sizeof(base), "%s", url);
	size_t bn = strlen(base);
	if (bn > 13 && !strcmp(base + bn - 13, "/manifest.json"))
		base[bn - 13] = '\0';

	st_addon *tmp = (st_addon *)calloc(1, sizeof(st_addon));
	if (!tmp) return;
	tmp->url_base = dupstr(base);

	st_addon *list = (st_addon *)realloc(g_addons, sizeof(st_addon) * (size_t)(g_addon_count + 1));
	if (!list) {
		addon_free(tmp);
		free(tmp);
		return;
	}
	g_addons = list;
	st_addon *dst = &list[g_addon_count];
	memcpy(dst, tmp, sizeof(st_addon));
	g_addon_count++;
	addon_load_manifest(dst, url);
	free(tmp);
}

int st_addons_init(void)
{
	if (g_addons) st_addons_term();
	g_addons = NULL;
	g_addon_count = 0;

	for (int i = 0; i < g_cfg.addon_count; i++)
		addon_push(g_cfg.addon_urls[i]);
	return 0;
}

void st_addons_reload(void)
{
	st_addons_init();
}

void st_addons_term(void)
{
	for (int i = 0; i < g_addon_count; i++)
		addon_free(&g_addons[i]);
	free(g_addons);
	g_addons = NULL;
	g_addon_count = 0;
}

int st_addons_count(void)
{
	return g_addon_count;
}

st_addon *st_addons_get(int index)
{
	if (index < 0 || index >= g_addon_count) return NULL;
	return &g_addons[index];
}

/* ------------------------------------------------------------- catalog */

int st_load_catalog(st_addon *a, const char *type, const char *cid,
                    int skip, const char *genre,
                    st_catalog_page *out, char *err, int errsize)
{
	memset(out, 0, sizeof(*out));
	if (!a || !a->okay || !a->res_catalog) {
		snprintf(err, errsize, "addon has no catalog resource");
		return -1;
	}
	char url[1024];
	int use_skip = (skip > 0);
	for (int i = 0; i < a->catalog_count; i++) {
		if (!strcmp(a->catalogs[i].type, type) && !strcmp(a->catalogs[i].cid, cid)) {
			use_skip = use_skip && a->catalogs[i].supports_skip;
			if (!a->catalogs[i].supports_genre) genre = NULL;
		}
	}
	build_url(url, sizeof(url), a, "catalog", type, cid, use_skip ? skip : -1, genre, NULL);

	char *body = NULL;
	size_t blen = 0;
	if (http_get_buf(url, &body, &blen, 6u * 1024 * 1024) < 0) {
		snprintf(err, errsize, "%.140s", net_last_error());
		return -1;
	}
	json_value *root = json_parse(body, blen);
	free(body);
	if (!root) {
		snprintf(err, errsize, "bad JSON from %.60s", a->name);
		return -1;
	}

	const json_value *metas = json_get(root, "metas");
	int n = json_arr_count(metas);
	if (n <= 0) {
		json_free(root);
		snprintf(err, errsize, "no items");
		return -1;
	}

	st_meta_item *items = (st_meta_item *)calloc((size_t)n, sizeof(st_meta_item));
	if (!items) {
		json_free(root);
		snprintf(err, errsize, "out of memory");
		return -1;
	}
	int kept = 0;
	for (int i = 0; i < n; i++) {
		const json_value *j = json_arr_get(metas, i);
		if (!j || j->type != JSON_OBJ) continue;
		st_meta_item *d = &items[kept];
		memset(d, 0, sizeof(*d));
		d->id = dupstr(json_str(j, "id", ""));
		d->name = dupstr(json_str(j, "name", ""));
		d->poster = dupstr(json_str(j, "poster", NULL));
		d->description = dupstr(json_str(j, "description", NULL));
		const char *rel = json_str(j, "releaseInfo", NULL);
		if (!rel) rel = json_str(j, "year", NULL);
		d->year = rel ? dupstr(rel) : NULL;
		double rating = json_num(j, "imdbRating", -1);
		if (rating > 0) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%.1f", rating);
			d->rating = dupstr(buf);
		}
		if (!*d->id || !*d->name) {
			st_meta_item_free(d);
			continue;
		}
		kept++;
	}
	json_free(root);
	if (!kept) {
		free(items);
		snprintf(err, errsize, "no valid items");
		return -1;
	}
	out->items = items;
	out->count = kept;
	return 0;
}

/* ----------------------------------------------------------------- meta */

int st_load_meta(st_addon *a, const char *type, const char *id,
                 st_meta_item *item, char *err, int errsize)
{
	memset(item, 0, sizeof(*item));
	if (!a || !a->okay || !a->res_meta) {
		snprintf(err, errsize, "addon does not provide meta");
		return -1;
	}
	char url[1024];
	build_url(url, sizeof(url), a, "meta", type, id, -1, NULL, NULL);

	char *body = NULL;
	size_t blen = 0;
	if (http_get_buf(url, &body, &blen, 4u * 1024 * 1024) < 0) {
		snprintf(err, errsize, "%.140s", net_last_error());
		return -1;
	}
	json_value *root = json_parse(body, blen);
	free(body);
	if (!root) {
		snprintf(err, errsize, "bad JSON");
		return -1;
	}

	const json_value *meta = json_get(root, "meta");
	if (!meta) meta = root;

	item->id = dupstr(json_str(meta, "id", id));
	item->name = dupstr(json_str(meta, "name", ""));
	item->poster = dupstr(json_str(meta, "poster", NULL));
	item->banner = dupstr(json_str(meta, "background", NULL));
	item->description = dupstr(json_str(meta, "description", NULL));
	const char *rel = json_str(meta, "releaseInfo", NULL);
	if (!rel) rel = json_str(meta, "year", NULL);
	item->year = rel ? dupstr(rel) : NULL;
	double rating = json_num(meta, "imdbRating", -1);
	if (rating > 0) {
		char buf[16];
		snprintf(buf, sizeof(buf), "%.1f", rating);
		item->rating = dupstr(buf);
	}
	json_free(root);

	if (!item->name || !*item->name) {
		st_meta_item_free(item);
		snprintf(err, errsize, "no meta content");
		return -1;
	}
	return 0;
}

/* --------------------------------------------------------------- streams */

static void mark_stream_flags(st_stream *s)
{
	if (s->url && strstr(s->url, ".m3u8")) {
		s->hls = 1;
		s->parked = 0;
	} else if (s->url) {
		s->parked = 1;
	} else {
		s->parked = 0;
	}
}

int st_load_streams(st_addon *a, const char *type, const char *id,
                    st_stream **out, int *out_count, char *err, int errsize)
{
	*out = NULL;
	*out_count = 0;
	if (!a || !a->okay || !a->res_stream) {
		snprintf(err, errsize, "%s does not provide streams",
			a && a->name ? a->name : "addon");
		return -1;
	}
	char url[1024];
	build_url(url, sizeof(url), a, "stream", type, id, -1, NULL, NULL);

	char *body = NULL;
	size_t blen = 0;
	if (http_get_buf(url, &body, &blen, 2u * 1024 * 1024) < 0) {
		snprintf(err, errsize, "%.140s", net_last_error());
		return -1;
	}
	json_value *root = json_parse(body, blen);
	free(body);
	if (!root) {
		snprintf(err, errsize, "bad stream JSON");
		return -1;
	}

	const json_value *arr = json_get(root, "streams");
	int n = json_arr_count(arr);
	if (n <= 0) {
		json_free(root);
		snprintf(err, errsize, "no streams offered");
		return -1;
	}

	st_stream *list = (st_stream *)calloc((size_t)n, sizeof(st_stream));
	if (!list) {
		json_free(root);
		snprintf(err, errsize, "out of memory");
		return -1;
	}
	int kept = 0;
	for (int i = 0; i < n; i++) {
		const json_value *j = json_arr_get(arr, i);
		if (!j || j->type != JSON_OBJ) continue;
		st_stream *s = &list[kept];
		const char *h = json_str(j, "infoHash", NULL);
		const char *u = json_str(j, "url", NULL);
		s->title = dupstr(json_str(j, "title", "?"));
		s->description = dupstr(json_str(j, "description", NULL));
		s->source_addon = dupstr(a->name);
		if (h) {
			/* torrent swarm: requires peer discovery -> unsupported in v1 */
			s->torrent = 1;
		} else if (u && *u) {
			s->url = dupstr(u);
		}
		mark_stream_flags(s);
		kept++;
	}
	json_free(root);
	if (!kept) {
		free(list);
		snprintf(err, errsize, "no streams offered");
		return -1;
	}
	*out = list;
	*out_count = kept;
	return 0;
}
