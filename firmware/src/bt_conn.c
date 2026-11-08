// Connection manager: GAP configuration, remembered phone (TLV in flash),
// pairing mode, auto-reconnect loop and auto-play after (re)connection.
#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "btstack_tlv.h"
#include "pico/cyw43_arch.h"

#include "bt_conn.h"
#include "bt_audio.h"
#include "bt_avrcp.h"
#include "now_playing.h"

#define DEVICE_NAME                 "PicoCarPod"
// Service class Audio, major Audio/Video, minor Car Audio
#define CLASS_OF_DEVICE             0x200420

#define TLV_TAG_LAST_DEVICE         0x50435031u   // 'PCP1'

#define RECONNECT_FIRST_MS          1000   // after boot
#define RECONNECT_RETRY_MS          4000   // after a failed attempt
#define RECONNECT_AFTER_DROP_MS     3000   // after the phone disconnected
#define RECONNECT_GUARD_MS         25000   // no event after establish_stream() -> retry
#define AVRCP_CHECK_MS              3000   // open AVRCP ourselves if the phone did not
#define AUTOPLAY_DELAY_MS           2500   // after AVRCP connected
#define PAIRING_TIMEOUT_MS        180000   // pairing mode length when a bond exists
#define LED_TICK_MS                  100

static btstack_packet_callback_registration_t hci_event_callback_registration;

static bt_conn_state_t state = BT_CONN_STATE_OFF;
static bool hci_ready;
static bool has_bond;
static bd_addr_t bond_addr;
static bool pairing_mode;
static bool manual_disconnect;   // suppress auto-reconnect after console 'd'
static bool autoplay = true;
static uint8_t retry_count;

static btstack_timer_source_t reconnect_timer;
static btstack_timer_source_t avrcp_timer;
static btstack_timer_source_t autoplay_timer;
static btstack_timer_source_t pairing_timer;
static btstack_timer_source_t led_timer;

static const btstack_tlv_t *tlv_impl;
static void *tlv_ctx;

static void schedule_reconnect(uint32_t delay_ms);

// ---------------------------------------------------------------------------
// bond storage

static void bond_load(void){
    btstack_tlv_get_instance(&tlv_impl, &tlv_ctx);
    has_bond = false;
    if (!tlv_impl) {
        printf("CONN : no TLV storage available\n");
        return;
    }
    int len = tlv_impl->get_tag(tlv_ctx, TLV_TAG_LAST_DEVICE, bond_addr, sizeof(bd_addr_t));
    if (len == (int) sizeof(bd_addr_t)){
        has_bond = true;
        printf("CONN : remembered phone %s\n", bd_addr_to_str(bond_addr));
    } else {
        printf("CONN : no remembered phone\n");
    }
}

static void bond_store(bd_addr_t addr){
    if (has_bond && memcmp(addr, bond_addr, 6) == 0) return;
    memcpy(bond_addr, addr, 6);
    has_bond = true;
    if (tlv_impl){
        tlv_impl->store_tag(tlv_ctx, TLV_TAG_LAST_DEVICE, bond_addr, sizeof(bd_addr_t));
    }
    printf("CONN : remembered phone -> %s\n", bd_addr_to_str(addr));
}

static void bond_clear(void){
    has_bond = false;
    memset(bond_addr, 0, 6);
    if (tlv_impl){
        tlv_impl->delete_tag(tlv_ctx, TLV_TAG_LAST_DEVICE);
    }
}

// ---------------------------------------------------------------------------
// state / LED

static bt_conn_state_listener_t state_listener;

void bt_conn_set_state_listener(bt_conn_state_listener_t listener){ state_listener = listener; }

static void set_state(bt_conn_state_t s){
    if (state == s) return;
    state = s;
    printf("CONN : state -> %s\n", bt_conn_state_str(s));
    if (state_listener) state_listener(s);
}

static void led_timer_handler(btstack_timer_source_t *ts){
    static uint32_t tick;
    tick++;
    bool on;
    switch (state){
        case BT_CONN_STATE_CONNECTED:    on = true; break;                 // solid
        case BT_CONN_STATE_PAIRING:      on = (tick & 1) != 0; break;      // fast blink
        case BT_CONN_STATE_RECONNECTING: on = (tick % 10) == 0; break;     // short blip
        default:                         on = false; break;
    }
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
    btstack_run_loop_set_timer(ts, LED_TICK_MS);
    btstack_run_loop_add_timer(ts);
}

// ---------------------------------------------------------------------------
// pairing mode

static void pairing_timer_handler(btstack_timer_source_t *ts){
    UNUSED(ts);
    if (pairing_mode && has_bond){
        printf("CONN : pairing mode timed out\n");
        bt_conn_set_pairing_mode(false);
    }
}

void bt_conn_set_pairing_mode(bool on){
    pairing_mode = on;
    if (!hci_ready) return;
    gap_discoverable_control(on ? 1 : 0);
    btstack_run_loop_remove_timer(&pairing_timer);
    if (on){
        printf("CONN : pairing mode ON - device visible as \"%s\"\n", DEVICE_NAME);
        if (state != BT_CONN_STATE_CONNECTED) set_state(BT_CONN_STATE_PAIRING);
        if (has_bond){
            btstack_run_loop_set_timer_handler(&pairing_timer, &pairing_timer_handler);
            btstack_run_loop_set_timer(&pairing_timer, PAIRING_TIMEOUT_MS);
            btstack_run_loop_add_timer(&pairing_timer);
        }
    } else {
        printf("CONN : pairing mode OFF\n");
        if (state == BT_CONN_STATE_PAIRING){
            if (has_bond && !manual_disconnect){
                set_state(BT_CONN_STATE_RECONNECTING);
                schedule_reconnect(RECONNECT_FIRST_MS);
            } else {
                set_state(BT_CONN_STATE_OFF);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// reconnect loop

static void reconnect_attempt(btstack_timer_source_t *ts){
    UNUSED(ts);
    if (state != BT_CONN_STATE_RECONNECTING || !has_bond || manual_disconnect) return;
    if (bt_audio_a2dp_cid() != 0) return;   // already connected

    retry_count++;
    uint16_t cid;
    uint8_t status = a2dp_sink_establish_stream(bond_addr, &cid);
    if (status != ERROR_CODE_SUCCESS){
        printf("CONN : reconnect #%u to %s could not start (status 0x%02x)\n", retry_count, bd_addr_to_str(bond_addr), status);
        schedule_reconnect(RECONNECT_RETRY_MS);
        return;
    }
    printf("CONN : reconnect #%u -> %s ...\n", retry_count, bd_addr_to_str(bond_addr));
    // guard: if nothing comes back, try again
    schedule_reconnect(RECONNECT_GUARD_MS);
}

static void schedule_reconnect(uint32_t delay_ms){
    btstack_run_loop_remove_timer(&reconnect_timer);
    btstack_run_loop_set_timer_handler(&reconnect_timer, &reconnect_attempt);
    btstack_run_loop_set_timer(&reconnect_timer, delay_ms);
    btstack_run_loop_add_timer(&reconnect_timer);
}

static void start_after_hci_ready(void){
    bond_load();
    if (pairing_mode || !has_bond){
        bt_conn_set_pairing_mode(true);
    } else {
        set_state(BT_CONN_STATE_RECONNECTING);
        schedule_reconnect(RECONNECT_FIRST_MS);
    }
}

// ---------------------------------------------------------------------------
// AVRCP follow-up + autoplay

static void avrcp_timer_handler(btstack_timer_source_t *ts){
    UNUSED(ts);
    if (state != BT_CONN_STATE_CONNECTED) return;
    if (bt_avrcp_connected()) return;
    printf("CONN : phone did not open AVRCP, connecting ourselves\n");
    bt_avrcp_connect(bond_addr);
}

static void autoplay_timer_handler(btstack_timer_source_t *ts){
    UNUSED(ts);
    if (!autoplay || !bt_avrcp_connected()) return;
    const now_playing_t *np = now_playing_get();
    if (np->status == NP_STATUS_PLAYING){
        printf("CONN : autoplay - phone already playing\n");
        return;
    }
    printf("CONN : autoplay - sending PLAY\n");
    bt_avrcp_play();
}

// ---------------------------------------------------------------------------
// HCI events

static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;
    bd_addr_t addr;

    switch (hci_event_packet_get_type(packet)){
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) break;
            gap_local_bd_addr(addr);
            printf("CONN : Bluetooth up, local address %s\n", bd_addr_to_str(addr));
            hci_ready = true;
            start_after_hci_ready();
            break;

        case HCI_EVENT_PIN_CODE_REQUEST:
            // legacy pairing fallback
            hci_event_pin_code_request_get_bd_addr(packet, addr);
            printf("CONN : PIN code request from %s - using 0000\n", bd_addr_to_str(addr));
            gap_pin_code_response(addr, "0000");
            break;

        case HCI_EVENT_USER_CONFIRMATION_REQUEST:
            // SSP "just works": auto-accepted by the stack (gap_ssp_set_auto_accept)
            printf("CONN : SSP confirmation request (auto-accepted)\n");
            break;

        case HCI_EVENT_SIMPLE_PAIRING_COMPLETE:
            hci_event_simple_pairing_complete_get_bd_addr(packet, addr);
            printf("CONN : pairing with %s %s\n", bd_addr_to_str(addr),
                   hci_event_simple_pairing_complete_get_status(packet) == 0 ? "complete" : "FAILED");
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// events from bt_audio / bt_avrcp

void bt_conn_on_a2dp_signaling_established(bd_addr_t addr, uint8_t status){
    btstack_run_loop_remove_timer(&reconnect_timer);
    if (status != ERROR_CODE_SUCCESS){
        if (state == BT_CONN_STATE_RECONNECTING){
            schedule_reconnect(RECONNECT_RETRY_MS);
        }
        return;
    }
    retry_count = 0;
    manual_disconnect = false;
    bond_store(addr);
    if (pairing_mode) bt_conn_set_pairing_mode(false);
    set_state(BT_CONN_STATE_CONNECTED);

    // phones normally open AVRCP right after A2DP; if not, we do it
    btstack_run_loop_remove_timer(&avrcp_timer);
    btstack_run_loop_set_timer_handler(&avrcp_timer, &avrcp_timer_handler);
    btstack_run_loop_set_timer(&avrcp_timer, AVRCP_CHECK_MS);
    btstack_run_loop_add_timer(&avrcp_timer);
}

void bt_conn_on_a2dp_signaling_released(void){
    btstack_run_loop_remove_timer(&avrcp_timer);
    btstack_run_loop_remove_timer(&autoplay_timer);
    if (pairing_mode){
        set_state(BT_CONN_STATE_PAIRING);
        return;
    }
    if (has_bond && !manual_disconnect){
        set_state(BT_CONN_STATE_RECONNECTING);
        schedule_reconnect(RECONNECT_AFTER_DROP_MS);
    } else {
        set_state(BT_CONN_STATE_OFF);
    }
}

void bt_conn_on_avrcp_connected(bd_addr_t addr){
    UNUSED(addr);
    btstack_run_loop_remove_timer(&avrcp_timer);
    btstack_run_loop_remove_timer(&autoplay_timer);
    btstack_run_loop_set_timer_handler(&autoplay_timer, &autoplay_timer_handler);
    btstack_run_loop_set_timer(&autoplay_timer, AUTOPLAY_DELAY_MS);
    btstack_run_loop_add_timer(&autoplay_timer);
}

void bt_conn_on_avrcp_released(void){
    btstack_run_loop_remove_timer(&autoplay_timer);
}

// ---------------------------------------------------------------------------
// Public API

void bt_conn_init(void){
    gap_set_local_name(DEVICE_NAME);
    gap_set_class_of_device(CLASS_OF_DEVICE);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_auto_accept(1);
    gap_set_bondable_mode(1);
    gap_connectable_control(1);
    gap_discoverable_control(0);
    // allow role switch + sniff; lets the phone become master when we reconnect
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH | LM_LINK_POLICY_ENABLE_SNIFF_MODE);
    gap_set_allow_role_switch(true);

    hci_event_callback_registration.callback = &hci_packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    btstack_run_loop_set_timer_handler(&led_timer, &led_timer_handler);
    btstack_run_loop_set_timer(&led_timer, LED_TICK_MS);
    btstack_run_loop_add_timer(&led_timer);
}

bt_conn_state_t bt_conn_get_state(void){ return state; }

const char *bt_conn_state_str(bt_conn_state_t s){
    switch (s){
        case BT_CONN_STATE_OFF:          return "idle";
        case BT_CONN_STATE_PAIRING:      return "pairing";
        case BT_CONN_STATE_RECONNECTING: return "reconnecting";
        case BT_CONN_STATE_CONNECTED:    return "connected";
        default:                         return "?";
    }
}

bool bt_conn_has_bond(void){ return has_bond; }

void bt_conn_get_bonded_addr(bd_addr_t out){ memcpy(out, bond_addr, 6); }

void bt_conn_forget_all(void){
    printf("CONN : forgetting all paired devices\n");
    bt_conn_disconnect();
    gap_delete_all_link_keys();
    bond_clear();
    manual_disconnect = false;
    bt_conn_set_pairing_mode(true);
}

void bt_conn_connect_now(void){
    manual_disconnect = false;
    if (!has_bond){
        printf("CONN : no remembered phone - enter pairing mode first\n");
        return;
    }
    if (bt_audio_a2dp_cid() != 0){
        printf("CONN : already connected\n");
        return;
    }
    set_state(BT_CONN_STATE_RECONNECTING);
    schedule_reconnect(1);
}

void bt_conn_disconnect(void){
    manual_disconnect = true;
    btstack_run_loop_remove_timer(&reconnect_timer);
    btstack_run_loop_remove_timer(&avrcp_timer);
    btstack_run_loop_remove_timer(&autoplay_timer);
    bt_avrcp_disconnect();
    uint16_t cid = bt_audio_a2dp_cid();
    if (cid){
        printf("CONN : disconnecting (auto-reconnect paused until 'c')\n");
        a2dp_sink_disconnect(cid);
    } else if (state == BT_CONN_STATE_RECONNECTING){
        printf("CONN : auto-reconnect paused until 'c'\n");
        set_state(BT_CONN_STATE_OFF);
    }
}

void bt_conn_set_autoplay(bool on){ autoplay = on; }
bool bt_conn_get_autoplay(void){ return autoplay; }
