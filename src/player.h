/* player.h — SceAvPlayer playback of HTTP MP4 streams */
#ifndef VST_PLAYER_H
#define VST_PLAYER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef struct {
	float progress;    /* 0..1 */
	int   connecting;  /* 1 while the source is being readied */
	int   buffering;
} st_player_status;

/* Blocks until playback ends / user stops. Returns 0 on normal end.
 * status_out may be NULL; it is filled during the call. */
int  player_play(const char *url, const char *title, st_player_status *status_out);

#ifdef __cplusplus
}
#endif
#endif
