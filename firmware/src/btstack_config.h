// BTstack configuration for PicoCarPod (Bluetooth Classic only: A2DP sink + AVRCP)
// Based on pico-examples/bluetooth/config/btstack_config_common.h
#ifndef PICOCARPOD_BTSTACK_CONFIG_H
#define PICOCARPOD_BTSTACK_CONFIG_H

// ENABLE_CLASSIC is defined by the pico_btstack_classic CMake target

// AVRCP Browsing channel runs over L2CAP ERTM
#define ENABLE_L2CAP_ENHANCED_RETRANSMISSION_MODE

// Logging: errors only. Define ENABLE_LOG_INFO for verbose stack logs.
#define ENABLE_LOG_ERROR
#ifdef PICOCARPOD_BT_LOG_INFO
#define ENABLE_LOG_INFO
#endif
#define ENABLE_PRINTF_HEXDUMP

// BTstack configuration: buffers, sizes, ...
#define HCI_OUTGOING_PRE_BUFFER_SIZE 4
#define HCI_ACL_PAYLOAD_SIZE (1691 + 4)
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT 4
#define MAX_NR_AVDTP_CONNECTIONS 1
#define MAX_NR_AVDTP_STREAM_ENDPOINTS 1
#define MAX_NR_AVRCP_CONNECTIONS 2
#define MAX_NR_AVRCP_BROWSING_CONNECTIONS 2
#define MAX_NR_BTSTACK_LINK_KEY_DB_MEMORY_ENTRIES 2
#define MAX_NR_HCI_CONNECTIONS 2
#define MAX_NR_L2CAP_CHANNELS 8
#define MAX_NR_L2CAP_SERVICES 3
#define MAX_NR_SERVICE_RECORD_ITEMS 4

// Limit number of ACL/SCO Buffer to use by stack to avoid cyw43 shared bus overrun
#define MAX_NR_CONTROLLER_ACL_BUFFERS 3
#define MAX_NR_CONTROLLER_SCO_PACKETS 3

// Enable and configure HCI Controller to Host Flow Control to avoid cyw43 shared bus overrun
#define ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL
#define HCI_HOST_ACL_PACKET_LEN 1024
#define HCI_HOST_ACL_PACKET_NUM 3
#define HCI_HOST_SCO_PACKET_LEN 120
#define HCI_HOST_SCO_PACKET_NUM 3

// Link Key DB using TLV on top of Flash Sector interface (set up by pico_btstack_cyw43)
#define NVM_NUM_DEVICE_DB_ENTRIES 16
#define NVM_NUM_LINK_KEYS 16

#define MAX_ATT_DB_SIZE 512

// BTstack HAL configuration
#define HAVE_EMBEDDED_TIME_MS
#define HAVE_ASSERT
#define HCI_RESET_RESEND_TIMEOUT_MS 1000
#define ENABLE_SOFTWARE_AES128

// Console over stdio (USB CDC)
#define HAVE_BTSTACK_STDIN

#endif
