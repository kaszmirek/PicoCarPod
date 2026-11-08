cmake_minimum_required(VERSION 3.13)

# --- Locate SDK / extras -------------------------------------------------------
# Default to the VS Code Pico extension install if nothing else is configured.
if (NOT PICO_SDK_PATH AND NOT DEFINED ENV{PICO_SDK_PATH})
    if (EXISTS "$ENV{HOME}/.pico-sdk/sdk/2.2.0")
        set(PICO_SDK_PATH "$ENV{HOME}/.pico-sdk/sdk/2.2.0")
    endif()
endif()
# pico-extras provides pico_audio_i2s. Cloned into lib/pico-extras (see README).
if (NOT PICO_EXTRAS_PATH AND NOT DEFINED ENV{PICO_EXTRAS_PATH})
    if (EXISTS "${CMAKE_CURRENT_LIST_DIR}/lib/pico-extras")
        set(PICO_EXTRAS_PATH "${CMAKE_CURRENT_LIST_DIR}/lib/pico-extras")
    endif()
endif()

set(PICO_BOARD pico_w CACHE STRING "Board type")

include(pico_sdk_import.cmake)
include(pico_extras_import.cmake)

project(picocarpod C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# --- Board / pin configuration -------------------------------------------------
# Waveshare Pico-Audio (PCM5101A revision): DIN=GP26, BCK=GP27, LRCK=GP28.
# pico_audio_i2s drives BCK on CLOCK_PIN_BASE and LRCK on CLOCK_PIN_BASE+1.
# Rev2.1 (CS4344) has a different pinout and needs MCLK - see README.
set(PICOCARPOD_I2S_DATA_PIN 26 CACHE STRING "I2S data (DIN) GPIO")
set(PICOCARPOD_I2S_CLOCK_PIN_BASE 27 CACHE STRING "I2S BCK GPIO (LRCK = BCK+1)")

add_executable(picocarpod
    src/main.c
    src/bt_audio.c
    src/bt_avrcp.c
    src/bt_browse.c
    src/bt_conn.c
    src/console.c
    src/diag.c
    src/btstack_audio_pico.c
    lib/btstack_patches/a2dp.c   # patched copy, replaces the SDK file (see below)
)

# BTstack 1.6.x a2dp.c overflows its remote-SEP array when the source has >10 SEPs
# (Linux/PipeWire does). Build our patched copy instead of the SDK one.
set_source_files_properties(${PICO_BTSTACK_PATH}/src/classic/a2dp.c PROPERTIES HEADER_FILE_ONLY TRUE)

target_include_directories(picocarpod PRIVATE src ${PICO_BTSTACK_PATH}/src/classic)

target_link_libraries(picocarpod PRIVATE
    pico_stdlib
    pico_cyw43_arch_none        # no lwip / WiFi
    pico_btstack_cyw43          # BT HCI transport over the CYW43
    pico_btstack_classic        # A2DP / AVRCP / SDP
    pico_btstack_sbc_decoder
    pico_audio_i2s
    hardware_watchdog
)

target_compile_definitions(picocarpod PRIVATE
    PICO_AUDIO_I2S_DATA_PIN=${PICOCARPOD_I2S_DATA_PIN}
    PICO_AUDIO_I2S_CLOCK_PIN_BASE=${PICOCARPOD_I2S_CLOCK_PIN_BASE}
    CYW43_LWIP=0
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=0
    PICO_PANIC_FUNCTION=picocarpod_panic
)

option(PICOCARPOD_BT_LOG_INFO "Print BTstack log_info messages on the console" OFF)
option(PICOCARPOD_AVRCP_DEBUG "Print every AVRCP controller subevent" OFF)
if (PICOCARPOD_AVRCP_DEBUG)
    target_compile_definitions(picocarpod PRIVATE BT_AVRCP_DEBUG=1)
endif()
if (PICOCARPOD_BT_LOG_INFO)
    target_compile_definitions(picocarpod PRIVATE PICOCARPOD_BT_LOG_INFO=1)
endif()

pico_enable_stdio_usb(picocarpod 1)
pico_enable_stdio_uart(picocarpod 0)

pico_add_extra_outputs(picocarpod)
