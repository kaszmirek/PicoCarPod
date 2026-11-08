// Shared "now playing" model: filled by the AVRCP controller, consumed by the
// console today and by the iPod (iAP) emulator later.
#ifndef NOW_PLAYING_H
#define NOW_PLAYING_H

#include <stdbool.h>
#include <stdint.h>

#define NP_STR_LEN 64

typedef enum {
    NP_STATUS_STOPPED = 0,
    NP_STATUS_PLAYING,
    NP_STATUS_PAUSED,
    NP_STATUS_UNKNOWN,
} np_status_t;

typedef struct {
    char title[NP_STR_LEN];
    char artist[NP_STR_LEN];
    char album[NP_STR_LEN];
    uint32_t track_number;   // 0 = unknown
    uint32_t total_tracks;   // 0 = unknown
    uint32_t length_ms;      // 0 = unknown
    uint32_t position_ms;    // last reported position
    uint32_t position_ts_ms; // run-loop time when position_ms was reported
    np_status_t status;
    uint32_t generation;     // incremented on every track/metadata change
} now_playing_t;

typedef enum {
    NP_CHANGE_METADATA,   // title/artist/album/track numbers updated
    NP_CHANGE_STATUS,     // play/pause/stop changed
    NP_CHANGE_POSITION,   // position/length updated
} np_change_t;

typedef void (*np_listener_t)(np_change_t what, const now_playing_t *np);

const now_playing_t *now_playing_get(void);
void now_playing_set_listener(np_listener_t listener);
// Current position extrapolated from the last report while playing.
uint32_t now_playing_position_now_ms(void);

#endif
