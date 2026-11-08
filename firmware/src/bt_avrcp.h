// AVRCP controller (metadata, transport control) and target (absolute volume).
#ifndef BT_AVRCP_H
#define BT_AVRCP_H

#include <stdbool.h>
#include <stdint.h>
#include "btstack.h"

void bt_avrcp_init(void);

bool bt_avrcp_connected(void);
uint16_t bt_avrcp_cid(void);
uint8_t bt_avrcp_connect(bd_addr_t addr);
void bt_avrcp_disconnect(void);

// Transport control towards the phone. Safe to call when not connected (no-op).
void bt_avrcp_play(void);
void bt_avrcp_pause(void);
void bt_avrcp_toggle_play_pause(void);
void bt_avrcp_next(void);
void bt_avrcp_prev(void);

// Re-query metadata / play status from the phone.
void bt_avrcp_refresh_now_playing(void);
void bt_avrcp_refresh_play_status(void);

#endif
