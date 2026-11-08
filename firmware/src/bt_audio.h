// A2DP sink: stream endpoint, SBC decoding and playback via btstack_audio sink.
#ifndef BT_AUDIO_H
#define BT_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

void bt_audio_init(void);
bool bt_audio_stream_playing(void);
uint16_t bt_audio_a2dp_cid(void);   // 0 when no signaling connection

#endif
