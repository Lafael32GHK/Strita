/* net.c — HTTP(S) fetch helpers over SceHttp (Stremio Vita)
 *
 * Initializes SceNet + SceHttp + SceSsl, creates one shared template with
 * auto-redirect enabled, and exposes small helpers used by the addon engine
 * and the media player (Range GETs for progressive MP4 playback).
 */
#include "net.h"

#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/net/http.h>
#include <psp2/libssl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define NET_POOL_SIZE   (1024 * 1024)
#define HTTP_POOL_SIZE  (1024 * 1024)
#define SSL_POOL_SIZE   (2048 * 1024)
#define DEFAULT_CAP     (16u * 1024 * 1024)
#define READ_CHUNK      (16 * 1024)

static int g_tpl = 0;
static void *g_net_pool, *g_http_pool, *g_ssl_pool;
static char g_last_error[192];
static int g_inited;

static void seterr(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(g_last_error, sizeof(g_last_error), fmt, ap);
	va_end(ap);
}

const char *net_last_error(void)
{
	return g_last_error;
}

int net_init(void)
{
	if (g_inited)
		return 0;

	g_last_error[0] = '\0';

	if (sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0) {
		seterr("failed to load NET module");
		return -1;
	}
	if (sceSysmoduleLoadModule(SCE_SYSMODULE_HTTP) < 0) {
		seterr("failed to load HTTP module");
		return -1;
	}
	if (sceSysmoduleLoadModule(SCE_SYSMODULE_SSL) < 0) {
		seterr("failed to load SSL module");
		return -1;
	}

	g_net_pool = malloc(NET_POOL_SIZE);
	g_http_pool = malloc(HTTP_POOL_SIZE);
	g_ssl_pool = malloc(SSL_POOL_SIZE);
	if (!g_net_pool || !g_http_pool || !g_ssl_pool) {
		seterr("out of memory (net pools)");
		return -1;
	}

	SceNetInitParam np = {0};
	np.memory = g_net_pool;
	np.size = NET_POOL_SIZE;
	np.flags = 0;
	if (sceNetInit(&np) < 0) {
		seterr("sceNetInit failed");
		return -1;
	}
	if (sceNetCtlInit() < 0) {
		seterr("no network connection (sceNetCtlInit)");
		return -1;
	}
	if (sceHttpInit(HTTP_POOL_SIZE) < 0) {
		seterr("sceHttpInit failed");
		return -1;
	}
	/* optional: not fatal if SSL init fails (plain http still works) */
	sceSslInit(SSL_POOL_SIZE);

	g_tpl = sceHttpCreateTemplate("StremioVita/1.0", SCE_HTTP_VERSION_1_1, SCE_TRUE);
	if (g_tpl < 0) {
		g_tpl = 0;
		seterr("sceHttpCreateTemplate failed");
		return -1;
	}
	sceHttpSetAutoRedirect(g_tpl, SCE_HTTP_ENABLE);
	sceHttpSetResolveTimeOut(g_tpl, 8 * 1000 * 1000);
	sceHttpSetConnectTimeOut(g_tpl, 12 * 1000 * 1000);
	sceHttpSetSendTimeOut(g_tpl, 30 * 1000 * 1000);
	sceHttpSetRecvTimeOut(g_tpl, 45 * 1000 * 1000);

	g_inited = 1;
	return 0;
}

void net_term(void)
{
	if (!g_inited)
		return;
	if (g_tpl) {
		sceHttpDeleteTemplate(g_tpl);
		g_tpl = 0;
	}
	sceSslTerm();
	sceHttpTerm();
	sceNetCtlTerm();
	sceNetTerm();
	free(g_net_pool);
	free(g_http_pool);
	free(g_ssl_pool);
	g_net_pool = g_http_pool = g_ssl_pool = NULL;
	g_inited = 0;
}

/* ---- shared request machinery ---------------------------------------- */

typedef struct {
	int conn, req, status;
} http_req_t;

static void req_close(http_req_t *r)
{
	if (r->req) { sceHttpDeleteRequest(r->req); r->req = 0; }
	if (r->conn) { sceHttpDeleteConnection(r->conn); r->conn = 0; }
}

static int req_open(http_req_t *r, const char *url, int use_head)
{
	memset(r, 0, sizeof(*r));
	r->conn = sceHttpCreateConnectionWithURL(g_tpl, url, SCE_HTTP_ENABLE);
	if (r->conn < 0) {
		seterr("connect failed (0x%08X)", r->conn);
		r->conn = 0;
		return -1;
	}
	r->req = sceHttpCreateRequestWithURL(r->conn,
		use_head ? SCE_HTTP_METHOD_HEAD : SCE_HTTP_METHOD_GET, url, 0);
	if (r->req < 0) {
		seterr("request failed (0x%08X)", r->req);
		r->req = 0;
		req_close(r);
		return -1;
	}
	return 0;
}

/* Send + basic validation. Returns 0 if the status is usable. */
static int req_send(http_req_t *r)
{
	int ok = sceHttpSendRequest(r->req, NULL, 0);
	if (ok < 0) {
		if (ok == SCE_HTTP_ERROR_BEFORE_SEND || ok == SCE_HTTP_ERROR_TIMEOUT)
			seterr("request timed out");
		else
			seterr("send failed (0x%08X)", ok);
		return -1;
	}
	int code = 0;
	if (sceHttpGetStatusCode(r->req, &code) >= 0)
		r->status = code;

	switch (r->status) {
	case SCE_HTTP_STATUS_CODE_OK:
	case SCE_HTTP_STATUS_CODE_PARTIAL_CONTENT:
		return 0;
	case SCE_HTTP_STATUS_CODE_NOT_FOUND:
		seterr("server says 404 Not Found");
		return -1;
	case SCE_HTTP_STATUS_CODE_FORBIDDDEN:
		seterr("server says 403 Forbidden");
		return -1;
	case SCE_HTTP_STATUS_CODE_REQUEST_RANGE_NOT_SATISFIBLE:
		seterr("range not satisfiable (416)");
		return -1;
	default:
		if (r->status >= 300 && r->status < 400) {
			seterr("redirects disabled / too many redirects (%d)", r->status);
			return -1;
		}
		if (r->status >= 400)
			seterr("server error %d", r->status);
		else
			seterr("unexpected response %d", r->status);
		return r->status >= 400 ? -1 : 0;
	}
}

/* Pull a single header field of interest. Returns malloc'd string or NULL. */
static char *response_header(http_req_t *r, const char *field)
{
	char *raw = NULL;
	unsigned int rawlen = 0;
	if (sceHttpGetAllResponseHeaders(r->req, &raw, &rawlen) < 0 || !raw)
		return NULL;
	const char *val = NULL;
	unsigned int vlen = 0;
	if (sceHttpParseResponseHeader(raw, rawlen, field, &val, &vlen) < 0 || !val || !vlen)
		return NULL;
	char *out = (char *)malloc(vlen + 1);
	if (!out) return NULL;
	memcpy(out, val, vlen);
	out[vlen] = '\0';
	return out;
}

/* Parse "bytes 1024-9999/12345" -> 12345. */
static int parse_content_range_total(const char *v, uint64_t *out)
{
	const char *slash = strrchr(v, '/');
	if (!slash) return -1;
	unsigned long long t = 0;
	char *end = NULL;
	t = strtoull(slash + 1, &end, 10);
	if (end == slash + 1) return -1;
	*out = t;
	return 0;
}

/* ---- public API -------------------------------------------------------- */

int http_get_buf(const char *url, char **out, size_t *out_len, size_t max_bytes)
{
	char *buf = NULL;
	http_req_t r;
	if (!g_inited) { seterr("net not initialized"); return -1; }
	if (max_bytes == 0) max_bytes = DEFAULT_CAP;

	if (req_open(&r, url, 0) < 0)
		return -1;
	if (req_send(&r) < 0) {
		req_close(&r);
		return -1;
	}

	size_t cap = READ_CHUNK, len = 0;
	buf = malloc(cap);
	if (!buf) { seterr("out of memory"); req_close(&r); return -1; }

	int rc = -1;
	for (;;) {
		if (len + READ_CHUNK > cap) {
			if (cap >= max_bytes) { seterr("response too large (> %u KB)", (unsigned)(max_bytes / 1024)); goto done; }
			cap *= 2;
			if (cap > max_bytes) cap = max_bytes;
			char *nb = (char *)realloc(buf, cap);
			if (!nb) { seterr("out of memory"); goto done; }
			buf = nb;
		}
		int got = sceHttpReadData(r.req, buf + len, READ_CHUNK);
		if (got < 0) { seterr("read failed (0x%08X)", got); goto done; }
		if (got == 0) { rc = 0; goto done; }
		len += (size_t)got;
		if (len >= max_bytes) { seterr("response too large"); goto done; }
	}
done:
	req_close(&r);
	if (rc < 0) {
		free(buf);
		return -1;
	}
	buf[len] = '\0';
	*out = buf;
	*out_len = len;
	return 0;
}

int http_probe_size(const char *url, uint64_t *out_total)
{
	http_req_t r;
	char hdr[64];
	if (!g_inited) { seterr("net not initialized"); return -1; }

	if (req_open(&r, url, 0) < 0)
		return -1;
	snprintf(hdr, sizeof(hdr), "bytes=0-1");
	sceHttpAddRequestHeader(r.req, "Range", hdr, SCE_HTTP_HEADER_OVERWRITE);
	if (req_send(&r) < 0) {
		req_close(&r);
		return -1;
	}

	uint64_t total = 0;
	char *cr = response_header(&r, "Content-Range");
	if (cr) {
		parse_content_range_total(cr, &total);
		free(cr);
	}
	if (!total) {
		unsigned long long clen = 0;
		if (sceHttpGetResponseContentLength(r.req, &clen) >= 0)
			total = clen == 1 ? 0 : clen; /* single-byte probe trick artifacts */
	}
	req_close(&r);
	if (!total) {
		seterr("could not determine stream size");
		return -1;
	}
	*out_total = total;
	return 0;
}

int http_read_range(const char *url, uint64_t pos, void *dst, unsigned int want,
                    unsigned int *got, uint64_t *total)
{
	http_req_t r;
	char hdr[64];
	if (!g_inited) { seterr("net not initialized"); return -1; }

	if (req_open(&r, url, 0) < 0)
		return -1;
	snprintf(hdr, sizeof(hdr), "bytes=%llu-", (unsigned long long)pos);
	sceHttpAddRequestHeader(r.req, "Range", hdr, SCE_HTTP_HEADER_OVERWRITE);
	if (req_send(&r) < 0) {
		req_close(&r);
		return -1;
	}

	if (total) {
		char *cr = response_header(&r, "Content-Range");
		if (cr) {
			parse_content_range_total(cr, total);
			free(cr);
		} else if (r.status == SCE_HTTP_STATUS_CODE_OK) {
			unsigned long long clen = 0;
			if (sceHttpGetResponseContentLength(r.req, &clen) >= 0 && pos == 0)
				*total = clen;
		}
	}

	unsigned int acc = 0;
	while (acc < want) {
		int gotn = sceHttpReadData(r.req, (uint8_t *)dst + acc, want - acc);
		if (gotn < 0) { seterr("stream read failed (0x%08X)", gotn); break; }
		if (gotn == 0) break;
		acc += (unsigned int)gotn;
	}
	req_close(&r);
	if (!acc) {
		seterr("stream read returned 0 bytes");
		return -1;
	}
	*got = acc;
	return 0;
}

int http_get_image(const char *url, void **out, size_t *out_len)
{
	char *buf = NULL;
	size_t len = 0;
	if (http_get_buf(url, &buf, &len, 12u * 1024 * 1024) < 0)
		return -1;

	/* format sniff: PNG, JPEG, GIF, BMP only are renderable */
	int ok = 0;
	if (len > 8) {
		unsigned char *b = (unsigned char *)buf;
		if (b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') ok = 1;
		else if (b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) ok = 1;
		else if (b[0] == 'G' && b[1] == 'I' && b[2] == 'F') ok = -1; /* gif: known unsupported */
	}
	if (!ok) {
		free(buf);
		seterr("unsupported image format");
		return -1;
	}
	*out = buf;
	*out_len = len;
	return 0;
}
