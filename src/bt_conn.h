// Connection manager: GAP setup, bonding storage, pairing mode, auto-reconnect,
// auto-play. Receives connection-level events forwarded by bt_audio / bt_avrcp.
#ifndef BT_CONN_H
#define BT_CONN_H

#include <stdbool.h>
#include <stdint.h>
#include "btstack.h"

typedef enum {
    BT_CONN_STATE_OFF = 0,
    BT_CONN_STATE_PAIRING,       // discoverable, waiting for a new phone
    BT_CONN_STATE_RECONNECTING,  // bonded phone known, trying to connect
    BT_CONN_STATE_CONNECTED,     // A2DP signaling connection up
} bt_conn_state_t;

void bt_conn_init(void);

bt_conn_state_t bt_conn_get_state(void);
typedef void (*bt_conn_state_listener_t)(bt_conn_state_t state);
void bt_conn_set_state_listener(bt_conn_state_listener_t listener);
const char *bt_conn_state_str(bt_conn_state_t s);
bool bt_conn_has_bond(void);
void bt_conn_get_bonded_addr(bd_addr_t out);

void bt_conn_set_pairing_mode(bool on);
void bt_conn_forget_all(void);     // delete link keys + remembered phone
void bt_conn_connect_now(void);    // force an immediate reconnect attempt
void bt_conn_disconnect(void);

void bt_conn_set_autoplay(bool on);
bool bt_conn_get_autoplay(void);

// Called by bt_audio / bt_avrcp
void bt_conn_on_a2dp_signaling_established(bd_addr_t addr, uint8_t status);
void bt_conn_on_a2dp_signaling_released(void);
void bt_conn_on_avrcp_connected(bd_addr_t addr);
void bt_conn_on_avrcp_released(void);

#endif
