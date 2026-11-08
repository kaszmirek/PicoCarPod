// Diagnostics: hard-fault / panic capture into watchdog scratch registers (they
// survive a watchdog reboot), watchdog feeding from the BTstack run loop, and a
// minimal hci_dump that forwards BTstack log messages to stdout without packet dumps.
#include <stdarg.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "hardware/sync.h"
#include "hardware/structs/watchdog.h"
#include "btstack.h"
#include "hci_dump.h"

#include "diag.h"

#define DIAG_MAGIC_HARDFAULT 0xDEAD0001u
#define DIAG_MAGIC_PANIC     0xDEAD0002u
#define WATCHDOG_TIMEOUT_MS  8000
#define WATCHDOG_FEED_MS     1000

static btstack_timer_source_t feed_timer;

// ---------------------------------------------------------------------------
// hard fault -> store PC/LR in scratch[0..3], reboot

void __attribute__((used)) diag_hardfault_c(uint32_t *sp){
    watchdog_hw->scratch[0] = DIAG_MAGIC_HARDFAULT;
    watchdog_hw->scratch[1] = sp[6];   // stacked PC
    watchdog_hw->scratch[2] = sp[5];   // stacked LR
    watchdog_hw->scratch[3] = (uint32_t) sp;
    watchdog_reboot(0, 0, 10);
    while (1) { __wfi(); }
}

// Overrides the weak crt0 handler. Selects MSP/PSP and hands the frame to C.
void __attribute__((naked)) isr_hardfault(void){
    __asm volatile(
        "movs r0, #4        \n"
        "mov  r1, lr        \n"
        "tst  r0, r1        \n"
        "beq  1f            \n"
        "mrs  r0, psp       \n"
        "b    2f            \n"
        "1: mrs r0, msp     \n"
        "2: ldr r1, =diag_hardfault_c \n"
        "bx   r1            \n"
        ".ltorg             \n"
    );
}

// ---------------------------------------------------------------------------
// panic -> print, store marker, reboot

void __attribute__((noreturn)) picocarpod_panic(const char *fmt, ...){
    printf("\n*** PANIC ***\n");
    if (fmt){
        va_list args;
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);
        printf("\n");
    }
    watchdog_hw->scratch[0] = DIAG_MAGIC_PANIC;
    watchdog_hw->scratch[1] = (uint32_t) __builtin_return_address(0);
    watchdog_hw->scratch[2] = 0;
    sleep_ms(500);   // let USB CDC flush
    watchdog_reboot(0, 0, 10);
    while (1) { __wfi(); }
}

// ---------------------------------------------------------------------------
// BTstack log -> stdout (messages only)

static void dump_reset(void){}
static void dump_log_packet(uint8_t packet_type, uint8_t in, uint8_t *packet, uint16_t len){
    (void) packet_type; (void) in; (void) packet; (void) len;
}
static void dump_log_message(int log_level, const char * format, va_list argptr){
    printf("BTSTK%s: ", log_level == HCI_DUMP_LOG_LEVEL_ERROR ? "!" : "");
    vprintf(format, argptr);
    printf("\n");
}
static const hci_dump_t diag_hci_dump = {
    .reset = &dump_reset,
    .log_packet = &dump_log_packet,
    .log_message = &dump_log_message,
};

// ---------------------------------------------------------------------------

static void feed_timer_handler(btstack_timer_source_t *ts){
    watchdog_update();
    btstack_run_loop_set_timer(ts, WATCHDOG_FEED_MS);
    btstack_run_loop_add_timer(ts);
}

void diag_start_watchdog_feeder(void){
    btstack_run_loop_set_timer_handler(&feed_timer, &feed_timer_handler);
    btstack_run_loop_set_timer(&feed_timer, WATCHDOG_FEED_MS);
    btstack_run_loop_add_timer(&feed_timer);
}

void diag_init(void){
    uint32_t magic = watchdog_hw->scratch[0];
    if (magic == DIAG_MAGIC_HARDFAULT){
        printf("DIAG : previous run HARD FAULT at PC=0x%08lx LR=0x%08lx SP=0x%08lx\n",
               watchdog_hw->scratch[1], watchdog_hw->scratch[2], watchdog_hw->scratch[3]);
        printf("DIAG : decode with: arm-none-eabi-addr2line -f -e build/picocarpod.elf 0x%08lx\n", watchdog_hw->scratch[1]);
    } else if (magic == DIAG_MAGIC_PANIC){
        printf("DIAG : previous run PANIC (caller 0x%08lx)\n", watchdog_hw->scratch[1]);
    } else if (watchdog_caused_reboot()){
        printf("DIAG : previous run was rebooted by the WATCHDOG (firmware hang)\n");
    }
    watchdog_hw->scratch[0] = 0;

    hci_dump_init(&diag_hci_dump);
    watchdog_enable(WATCHDOG_TIMEOUT_MS, true);
}
