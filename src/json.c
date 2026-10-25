/* json.c — minimal JSON DOM parser (Stremio Vita)
 *
 * Recursive-descent parser producing a small DOM tree. Strings are decoded
 * (including \uXXXX -> UTF-8). Not designed for giant documents; document
 * fragments parsed here are at most a few MB (catalog pages).
 */
#include "json.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
	const char *p, *end;
	int depth;
} jctx;

static json_value *parse_value(jctx *c);

static int head_is(const char *p, const char *end, const char *word)
{
	size_t n = strlen(word);
	if ((size_t)(end - p) < n) return 0;
	return !strncmp(p, word, n);
}

static json_value *node_new(void)
{
	json_value *v = (json_value *)calloc(1, sizeof(json_value));
	return v;
}

void json_free(json_value *v)
{
	while (v) {
		json_value *next = v->next;
		if (v->child)
			json_free(v->child);
		free(v->payload);
		free(v);
		v = next;
	}
}

static void skip_ws(jctx *c)
{
	while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r'))
		c->p++;
}

static void utf8_emit(char *out, size_t cap, size_t *len, unsigned int cp)
{
	char tmp[4];
	int n = 0;
	if (cp < 0x80) { tmp[n++] = (char)cp; }
	else if (cp < 0x800) {
		tmp[n++] = (char)(0xC0 | (cp >> 6));
		tmp[n++] = (char)(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		tmp[n++] = (char)(0xE0 | (cp >> 12));
		tmp[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
		tmp[n++] = (char)(0x80 | (cp & 0x3F));
	} else {
		tmp[n++] = (char)(0xF0 | (cp >> 18));
		tmp[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
		tmp[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
		tmp[n++] = (char)(0x80 | (cp & 0x3F));
	}
	for (int i = 0; i < n; i++) {
		if (*len + 1 >= cap) break;
		out[(*len)++] = tmp[i];
	}
}

static int hex4(const char *s)
{
	int v = 0;
	for (int i = 0; i < 4; i++) {
		char ch = s[i];
		int d;
		if (ch >= '0' && ch <= '9') d = ch - '0';
		else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
		else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
		else return -1;
		v = v * 16 + d;
	}
	return v;
}

/* parse a string starting at c->p ('"'), returns decoded malloc str */
static char *parse_string_raw(jctx *c)
{
	if (c->p >= c->end || *c->p != '"') return NULL;
	c->p++;
	size_t cap = 32, len = 0;
	char *out = (char *)malloc(cap);
	if (!out) return NULL;
#define PUT(ch) do { if (len + 1 >= cap) { size_t nc = cap * 2; \
	char *nn = (char *)realloc(out, nc); if (!nn) { free(out); return NULL; } \
	out = nn; cap = nc; } out[len++] = (ch); } while (0)

	while (c->p < c->end) {
		unsigned char ch = (unsigned char)*c->p;
		if (ch == '"') { c->p++; break; }
		if (ch == '\\') {
			c->p++;
			if (c->p >= c->end) break;
			char esc = *c->p++;
			switch (esc) {
			case '"': PUT('"'); break;
			case '\\': PUT('\\'); break;
			case '/': PUT('/'); break;
			case 'b': PUT('\b'); break;
			case 'f': PUT('\f'); break;
			case 'n': PUT('\n'); break;
			case 'r': PUT('\r'); break;
			case 't': PUT('\t'); break;
			case 'u': {
				if (c->p + 4 > c->end) { free(out); return NULL; }
				int cp = hex4(c->p);
				if (cp < 0) { free(out); return NULL; }
				c->p += 4;
				/* surrogate pair */
				if (cp >= 0xD800 && cp <= 0xDBFF && c->p + 6 <= c->end &&
					c->p[0] == '\\' && c->p[1] == 'u') {
					int lo = hex4(c->p + 2);
					if (lo >= 0xDC00 && lo <= 0xDFFF) {
						c->p += 6;
						cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
					}
				}
				utf8_emit(out, cap, &len, (unsigned int)cp);
				break;
			}
			default: free(out); return NULL;
			}
		} else {
			PUT(ch);
			c->p++;
		}
	}
	out[len] = '\0';
	return out;
#undef PUT
}

static json_value *parse_string(jctx *c)
{
	char *s = parse_string_raw(c);
	if (!s) return NULL;
	json_value *v = node_new();
	if (!v) { free(s); return NULL; }
	v->type = JSON_STR;
	v->str = s;
	v->payload = s;
	return v;
}

static json_value *parse_number(jctx *c)
{
	const char *start = c->p;
	while (c->p < c->end && ((*c->p >= '0' && *c->p <= '9') || *c->p == '-' ||
		*c->p == '+' || *c->p == '.' || *c->p == 'e' || *c->p == 'E'))
		c->p++;
	json_value *v = node_new();
	if (!v) return NULL;
	v->type = JSON_NUM;
	v->num = strtod(start, NULL);
	return v;
}

static json_value *parse_object(jctx *c)
{
	json_value *v = node_new();
	if (!v) return NULL;
	v->type = JSON_OBJ;
	c->p++; /* '{' */
	json_value **tail = &v->child;

	skip_ws(c);
	if (c->p < c->end && *c->p == '}') { c->p++; return v; }

	for (;;) {
		skip_ws(c);
		const char *kstart = c->p;
		char *key = parse_string_raw(c);
		skip_ws(c);
		if (!key) goto fail;
		skip_ws(c);
		if (c->p >= c->end || *c->p != ':') { free(key); goto fail; }
		c->p++;
		skip_ws(c);
		json_value *item = parse_value(c);
		if (!item) { free(key); goto fail; }
		item->key = key;
		/* attach key storage: reuse payload field for key ownership */
		item->payload = key;
		*tail = item;
		tail = &item->next;

		skip_ws(c);
		if (c->p >= c->end)
			goto fail;
		if (*c->p == ',') { c->p++; continue; }
		if (*c->p == '}') {
			c->p++;
			return v;
		}
		goto fail;
	}
}

static json_value *parse_array(jctx *c)
{
	json_value *v = node_new();
	if (!v) return NULL;
	v->type = JSON_ARR;
	c->p++; /* '[' */
	json_value **tail = &v->child;

	skip_ws(c);
	if (c->p < c->end && *c->p == ']') { c->p++; return v; }

	for (;;) {
		json_value *item = parse_value(c);
		if (!item || c->p >= c->end) { if (item) json_free(item); goto fail; }
		*tail = item;
		tail = &item->next;

		skip_ws(c);
		if (c->p >= c->end || (*c->p != ',' && *c->p != ']')) goto fail;
		if (*c->p == ',') {
			c->p++;
			continue;
		}
		break;
	}
	skip_ws(c);
	if (c->p < c->end && *c->p == ']') { c->p++; return v; }
fail:
	json_free(v);
	return NULL;
}

static json_value *parse_value(jctx *c)
{
	if (++c->depth > 64) { c->depth--; return NULL; }
	skip_ws(c);
	json_value *v = NULL;
	if (c->p >= c->end) { c->depth--; return NULL; }

	if (*c->p == '{') v = parse_object(c);
	else if (*c->p == '[') v = parse_array(c);
	else if (*c->p == '"') v = parse_string(c);
	else if (head_is(c->p, c->end, "true")) {
		v = node_new(); if (v) { v->type = JSON_BOOL; v->boolean = 1; }
		if (v) c->p += 4;
	}
	else if (head_is(c->p, c->end, "false")) {
		v = node_new(); if (v) { v->type = JSON_BOOL; v->boolean = 0; }
		if (v) c->p += 5;
	}
	else if (head_is(c->p, c->end, "null")) {
		v = node_new();
		if (v) c->p += 4;
	}
	else if (*c->p == '-' || (*c->p >= '0' && *c->p <= '9')) v = parse_number(c);
	else v = NULL;

	if (!v) return NULL;
	c->depth--;
	return v;
}

json_value *json_parse(const char *s, size_t len)
{
	if (!s) return NULL;
	/* skip UTF-8 BOM */
	size_t off = 0;
	if (len >= 3 && (unsigned char)s[0] == 0xEF &&
		(unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
		off = 3;
	jctx ctx = { s + off, s + len, 0 };
	json_value *v = parse_value(&ctx);
	return v;
}

const json_value *json_get(const json_value *obj, const char *key)
{
	if (!obj || obj->type != JSON_OBJ) return NULL;
	for (const json_value *c = obj->child; c; c = c->next)
		if (c->key && !strcmp(c->key, key)) return c;
	return NULL;
}

const char *json_str(const json_value *obj, const char *key, const char *fallback)
{
	if (!obj) return fallback;
	const json_value *v = (obj->type == JSON_OBJ && key) ? json_get(obj, key) : obj;
	if (v && v->type == JSON_STR) return v->str;
	return fallback;
}

double json_num(const json_value *obj, const char *key, double fallback)
{
	if (!obj) return fallback;
	const json_value *v = (obj->type == JSON_OBJ && key) ? json_get(obj, key) : obj;
	if (v && v->type == JSON_NUM) return v->num;
	return fallback;
}

int json_arr_count(const json_value *arr)
{
	if (!arr || arr->type != JSON_ARR) return 0;
	int n = 0;
	for (const json_value *c = arr->child; c; c = c->next) n++;
	return n;
}

const json_value *json_arr_get(const json_value *arr, int index)
{
	if (!arr || arr->type != JSON_ARR) return NULL;
	int i = 0;
	for (const json_value *c = arr->child; c; c = c->next, i++)
		if (i == index) return c;
	return NULL;
}
