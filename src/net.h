/* net.h — HTTP(S) fetch helpers over SceHttp (Stremio Vita) */
#ifndef VST_NET_H
#define VST_NET_H

#include <stddef.h>
#include <stdint.h>

/* 0 on success, negative on failure. net_last_error() gives a short reason. */
int net_init(void);
void net_term(void);
const char *net_last_error(void);

/* Download a full resource into a NUL-terminated heap buffer.
 * *out must be free()'d by the caller. max_bytes caps the response
 * (safety against huge downloads); pass 0 for the default cap. */
int http_get_buf(const char *url, char **out, size_t *out_len, size_t max_bytes);

/* Best-effort total-size probe via a 1-byte range request. */
int http_probe_size(const char *url, uint64_t *out_total);

/* Progressive read for the media player's FileReplacement (Range GET).
 * Reads up to `want` bytes starting at `pos`; *got receives bytes read.
 * *total (if non-NULL) receives Content-Range/Content-Length derived size. */
int http_read_range(const char *url, uint64_t pos, void *dst, unsigned int want,
                    unsigned int *got, uint64_t *total);

/* Load poster/banner images into memory (for vita2d_load_*_buffer).
 * *out must be free()'d. Returns <0 on error/unsupported format. */
int http_get_image(const char *url, void **out, size_t *out_len);

#endif
