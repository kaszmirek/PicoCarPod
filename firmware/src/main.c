// PicoCarPod firmware - Bluetooth A2DP receiver that will emulate an iPod on a
// car's 30-pin dock cable. Current stage: A2DP sink + AVRCP metadata/control
// with auto-reconnect, console over USB serial.
#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "btstack.h"

#include "bt_audio.h"
#include "bt_avrcp.h"
#include "bt_browse.h"
#include "bt_conn.h"
#include "console.h"
#include "diag.h"

// implemented in btstack_audio_pico.c (I2S via pico_audio_i2s)
const btstack_audio_sink_t * btstack_audio_pico_sink_get_instance(void);

int main(void){
    stdio_init_all();
    printf("\n\nPicoCarPod starting\n");
    diag_init();

    if (cyw43_arch_init() != PICO_OK){
        panic("cyw43_arch_init failed");
    }

    btstack_audio_sink_set_instance(btstack_audio_pico_sink_get_instance());

    l2cap_init();
    sdp_init();

    bt_audio_init();
    bt_avrcp_init();
    bt_browse_init();
    bt_conn_init();
    console_init();
    diag_start_watchdog_feeder();

    printf("I2S  : DIN=GP%d BCK=GP%d LRCK=GP%d\n", PICO_AUDIO_I2S_DATA_PIN, PICO_AUDIO_I2S_CLOCK_PIN_BASE, PICO_AUDIO_I2S_CLOCK_PIN_BASE + 1);
    printf("Press 'h' for console help\n");

    hci_power_control(HCI_POWER_ON);
    btstack_run_loop_execute();
    return 0;
}
