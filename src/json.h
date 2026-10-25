/* json.h — minimal JSON DOM parser (Stremio Vita) */
#ifndef VST_JSON_H
#define VST_JSON_H

#include <stddef.h>

typedef enum {
	JSON_NULL,
	JSON_BOOL,
	JSON_NUM,
	JSON_STR,
	JSON_ARR,
	JSON_OBJ,
} json_type;

typedef struct json_value {
	json_type type;
	/* JSON_OBJ / JSON_ARR: child chain (objects carry .key) */
	struct json_value *child;
	struct json_value *next;
	const char *key;           /* points into input, must not be freed */
	const char *str;           /* JSON_STR: NUL-terminated, points into decoded buf */
	double num;                /* JSON_NUM */
	int boolean;               /* JSON_BOOL */
	char *payload;             /* malloc'd string storage of this node */
} json_value;

json_value *json_parse(const char *s, size_t len);
void json_free(json_value *v);

const json_value *json_get(const json_value *obj, const char *key);
const char       *json_str(const json_value *obj, const char *key, const char *fallback);
double            json_num(const json_value *obj, const char *key, double fallback);
int               json_arr_count(const json_value *arr);
const json_value *json_arr_get(const json_value *arr, int index);

#endif
