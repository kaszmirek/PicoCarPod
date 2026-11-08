// AVRCP Browsing (AVRCP 1.4+ browsing channel over L2CAP ERTM).
// Flow after the control channel is up: connect browsing -> GetFolderItems(MediaPlayerList)
// -> SetBrowsedPlayer(active player) -> ready. Then Now Playing / virtual file system queries
// on request. Selected tracks are started with PlayItem on the control channel.
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "btstack.h"

#include "bt_browse.h"
#include "bt_avrcp.h"

#define BROWSE_CONNECT_DELAY_MS 1500
#define LIST_PAGE               (BR_MAX_ITEMS - 1)

typedef enum {
    BR_IDLE, BR_W4_PLAYERS, BR_W4_SET_PLAYER, BR_READY, BR_W4_LIST, BR_W4_PATH
} br_state_t;

typedef struct { uint16_t id; uint8_t play_status; char name[BR_NAME_LEN]; } player_t;

static uint8_t sdp_dummy; // (SDP record is created in bt_avrcp.c with the browsing feature bit)

static uint8_t ertm_buffer[6000];
static l2cap_ertm_config_t ertm_config = {
    1,      // ertm mandatory
    2,      // max transmit
    2000,   // retransmission timeout ms
    12000,  // monitor timeout ms
    512,    // local mtu
    4,      // num tx buffers
    4,      // num rx buffers
    1,      // 16-bit FCS
};

static bd_addr_t remote_addr;
static uint16_t browsing_cid;
static bool connected;
static bool busy;
static br_state_t state = BR_IDLE;
static btstack_timer_source_t connect_timer;

static player_t players[6];
static uint8_t  player_count;
static uint16_t browsed_player = 0xFFFF;

static br_item_t items[BR_MAX_ITEMS];
static uint16_t item_count;        // stored
static uint32_t item_total;        // received (may exceed stored)
static uint16_t uid_counter;
static avrcp_browsing_scope_t list_scope = AVRCP_BROWSING_MEDIA_PLAYER_VIRTUAL_FILESYSTEM;
static avrcp_browsing_scope_t pending_scope;
static bool list_after_path;       // after a ChangePath completes, list the folder

static const char *browsing_status_str(uint8_t st){
    switch (st){
        case 0x00: return "invalid command"; case 0x01: return "invalid parameter"; case 0x02: return "parameter content error";
        case 0x03: return "internal error"; case 0x05: return "UID changed"; case 0x07: return "invalid direction";
        case 0x08: return "not a directory"; case 0x09: return "does not exist"; case 0x0A: return "invalid scope";
        case 0x0B: return "range out of bounds"; case 0x0C: return "folder not playable"; case 0x0D: return "media in use";
        case 0x0E: return "now playing list full"; case 0x0F: return "search not supported"; case 0x10: return "search in progress";
        case 0x11: return "invalid player id"; case 0x12: return "player not browsable"; case 0x13: return "player not addressed";
        case 0x14: return "no valid search results"; case 0x15: return "no available players"; case 0x16: return "addressed player changed";
        default: return "?";
    }
}

static void print_items(void){
    printf("BROWSE: %s - %u item(s)%s\n",
           list_scope == AVRCP_BROWSING_NOW_PLAYING ? "Now Playing" : "folder",
           (unsigned) item_total, item_total > item_count ? " (first page shown)" : "");
    for (uint16_t i = 0; i < item_count; i++){
        const br_item_t *it = &items[i];
        if (it->type == BR_ITEM_FOLDER) printf("  [%2u] <%s>%s\n", i, it->name, it->playable ? " (playable)" : "");
        else printf("  [%2u] %s%s%s\n", i, it->name, it->artist[0] ? " - " : "", it->artist);
    }
}

static void start_query(avrcp_browsing_scope_t scope){
    if (!connected || busy) { printf("BROWSE: not ready (connected %d, busy %d)\n", connected, busy); return; }
    item_count = 0; item_total = 0; pending_scope = scope;
    uint8_t status;
    if (scope == AVRCP_BROWSING_NOW_PLAYING){
        status = avrcp_browsing_controller_browse_now_playing_list(browsing_cid, 0, LIST_PAGE, AVRCP_MEDIA_ATTR_ALL);
    } else {
        status = avrcp_browsing_controller_browse_file_system(browsing_cid, 0, LIST_PAGE, AVRCP_MEDIA_ATTR_ALL);
    }
    if (status != ERROR_CODE_SUCCESS){ printf("BROWSE: query failed to start, status 0x%02x\n", status); return; }
    busy = true; state = BR_W4_LIST;
}

static void select_player(void){
    if (player_count == 0){ printf("BROWSE: phone lists no media players\n"); state = BR_IDLE; return; }
    uint8_t pick = 0;
    for (uint8_t i = 0; i < player_count; i++){ if (players[i].play_status == 0x01){ pick = i; break; } }  // playing
    browsed_player = players[pick].id;
    printf("BROWSE: browsed player -> 0x%04x \"%s\"\n", browsed_player, players[pick].name);
    uint8_t status = avrcp_browsing_controller_set_browsed_player(browsing_cid, browsed_player);
    if (status != ERROR_CODE_SUCCESS){ printf("BROWSE: set browsed player failed 0x%02x\n", status); state = BR_IDLE; return; }
    busy = true; state = BR_W4_SET_PLAYER;
}

static void get_players(void){
    item_count = 0; player_count = 0;
    uint8_t status = avrcp_browsing_controller_get_media_players(browsing_cid, 0, 5, AVRCP_MEDIA_ATTR_ALL);
    if (status != ERROR_CODE_SUCCESS){ printf("BROWSE: get media players failed 0x%02x\n", status); return; }
    busy = true; state = BR_W4_PLAYERS;
}

static void copy_str(char *dst, const uint8_t *src, uint16_t len){
    if (len >= BR_NAME_LEN) len = BR_NAME_LEN - 1;
    memcpy(dst, src, len); dst[len] = 0;
}

static void handle_data(const uint8_t *packet, uint16_t size){
    uint16_t pos = 0;
    avrcp_browsing_item_type_t type = (avrcp_browsing_item_type_t) packet[pos++];
    switch (type){
        case AVRCP_BROWSING_MEDIA_ROOT_FOLDER: {
            char name[BR_NAME_LEN]; copy_str(name, packet + pos, size - pos);
            printf("BROWSE: root folder \"%s\"\n", name);
            break;
        }
        case AVRCP_BROWSING_MEDIA_PLAYER_ITEM: {
            if (player_count >= (uint8_t)(sizeof(players)/sizeof(players[0]))) break;
            player_t *p = &players[player_count++];
            p->id = big_endian_read_16(packet, pos); pos += 2;
            pos += 1;           // major type
            pos += 4;           // subtype
            p->play_status = packet[pos++];
            pos += 16;          // feature bitmask
            pos += 2;           // charset
            uint16_t name_len = big_endian_read_16(packet, pos); pos += 2;
            copy_str(p->name, packet + pos, btstack_min(name_len, size - pos));
            printf("BROWSE: player 0x%04x status 0x%02x \"%s\"\n", p->id, p->play_status, p->name);
            break;
        }
        case AVRCP_BROWSING_FOLDER_ITEM: {
            item_total++;
            if (item_count >= BR_MAX_ITEMS) break;
            br_item_t *it = &items[item_count++]; memset(it, 0, sizeof(*it));
            it->type = BR_ITEM_FOLDER;
            memcpy(it->uid, packet + pos, 8); pos += 8;
            pos += 1;                              // folder type
            it->playable = packet[pos++];
            pos += 2;                              // charset
            uint16_t name_len = big_endian_read_16(packet, pos); pos += 2;
            copy_str(it->name, packet + pos, btstack_min(name_len, size - pos));
            break;
        }
        case AVRCP_BROWSING_MEDIA_ELEMENT_ITEM: {
            item_total++;
            if (item_count >= BR_MAX_ITEMS) break;
            br_item_t *it = &items[item_count++]; memset(it, 0, sizeof(*it));
            it->type = BR_ITEM_MEDIA; it->playable = true;
            memcpy(it->uid, packet + pos, 8); pos += 8;
            pos += 1;                              // media type
            pos += 2;                              // charset
            uint16_t name_len = big_endian_read_16(packet, pos); pos += 2;
            copy_str(it->name, packet + pos, btstack_min(name_len, size - pos));
            pos += name_len;
            if (pos >= size) break;
            pos += 1;                              // num attributes
            avrcp_media_item_context_t ctx;
            for (avrcp_media_item_iterator_init(&ctx, size - pos, packet + pos); avrcp_media_item_iterator_has_more(&ctx); avrcp_media_item_iterator_next(&ctx)){
                uint32_t attr = avrcp_media_item_iterator_get_attr_id(&ctx);
                if (attr == AVRCP_MEDIA_ATTR_ARTIST){
                    copy_str(it->artist, avrcp_media_item_iterator_get_attr_value(&ctx), avrcp_media_item_iterator_get_attr_value_len(&ctx));
                }
            }
            break;
        }
        default:
            break;
    }
}

static void browsing_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    bd_addr_t addr; uint8_t status;

    if (packet_type == AVRCP_BROWSING_DATA_PACKET){ handle_data(packet, size); return; }
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;

    switch (packet[2]){
        case AVRCP_SUBEVENT_INCOMING_BROWSING_CONNECTION:
            browsing_cid = avrcp_subevent_incoming_browsing_connection_get_browsing_cid(packet);
            printf("BROWSE: phone opens browsing channel (cid 0x%02x)\n", browsing_cid);
            avrcp_browsing_configure_incoming_connection(browsing_cid, ertm_buffer, sizeof(ertm_buffer), &ertm_config);
            break;

        case AVRCP_SUBEVENT_BROWSING_CONNECTION_ESTABLISHED:
            status = avrcp_subevent_browsing_connection_established_get_status(packet);
            avrcp_subevent_browsing_connection_established_get_bd_addr(packet, addr);
            if (status != ERROR_CODE_SUCCESS){
                printf("BROWSE: connection to %s failed, status 0x%02x\n", bd_addr_to_str(addr), status);
                browsing_cid = 0; connected = false;
                break;
            }
            browsing_cid = avrcp_subevent_browsing_connection_established_get_browsing_cid(packet);
            connected = true; busy = false;
            printf("BROWSE: browsing channel connected (cid 0x%02x)\n", browsing_cid);
            btstack_run_loop_remove_timer(&connect_timer);
            get_players();
            break;

        case AVRCP_SUBEVENT_BROWSING_CONNECTION_RELEASED:
            printf("BROWSE: browsing channel released\n");
            browsing_cid = 0; connected = false; busy = false; state = BR_IDLE;
            break;

        case AVRCP_SUBEVENT_BROWSING_DONE: {
            busy = false;
            uint8_t bstatus = avrcp_subevent_browsing_done_get_browsing_status(packet);
            if (bstatus != AVRCP_BROWSING_ERROR_CODE_SUCCESS){
                printf("BROWSE: query failed, browsing status 0x%02x (%s), bt status 0x%02x\n", bstatus, browsing_status_str(bstatus), avrcp_subevent_browsing_done_get_bluetooth_status(packet));
                br_state_t failed = state;
                state = connected ? BR_READY : BR_IDLE;
                if (failed == BR_W4_SET_PLAYER){
                    // no virtual file system on this phone; the Now Playing queue does not need a browsed player
                    browsed_player = 0xFFFF;
                    printf("BROWSE: player has no browsable library, trying the Now Playing queue\n");
                    start_query(AVRCP_BROWSING_NOW_PLAYING);
                }
                break;
            }
            uid_counter = avrcp_subevent_browsing_done_get_uid_counter(packet);
            switch (state){
                case BR_W4_PLAYERS:
                    select_player();
                    break;
                case BR_W4_SET_PLAYER:
                    state = BR_READY;
                    printf("BROWSE: ready. Console: l = now playing, r = root folder, u = up, 0-9 = open/play item, < > = page\n");
                    start_query(AVRCP_BROWSING_NOW_PLAYING);
                    break;
                case BR_W4_LIST:
                    list_scope = pending_scope; state = BR_READY;
                    print_items();
                    break;
                case BR_W4_PATH:
                    state = BR_READY;
                    if (list_after_path){ list_after_path = false; start_query(AVRCP_BROWSING_MEDIA_PLAYER_VIRTUAL_FILESYSTEM); }
                    break;
                default:
                    state = BR_READY;
                    break;
            }
            break;
        }
        default:
            break;
    }
}

static void connect_timer_handler(btstack_timer_source_t *ts){
    UNUSED(ts);
    if (connected || browsing_cid) return;
    uint8_t status = avrcp_browsing_connect(remote_addr, ertm_buffer, sizeof(ertm_buffer), &ertm_config, &browsing_cid);
    if (status != ERROR_CODE_SUCCESS){
        printf("BROWSE: connect failed to start, status 0x%02x\n", status);
        browsing_cid = 0;
    } else {
        printf("BROWSE: opening browsing channel to %s ...\n", bd_addr_to_str(remote_addr));
    }
}

// ---------------------------------------------------------------------------
// Public API

void bt_browse_init(void){
    (void) sdp_dummy;
    avrcp_browsing_init();
    avrcp_browsing_controller_init();
    avrcp_browsing_register_packet_handler(&browsing_packet_handler);
    avrcp_browsing_controller_register_packet_handler(&browsing_packet_handler);
}

bool bt_browse_connected(void){ return connected; }
bool bt_browse_busy(void){ return busy; }

void bt_browse_now_playing(void){ start_query(AVRCP_BROWSING_NOW_PLAYING); }

void bt_browse_players(void){
    if (!connected || busy){ printf("BROWSE: not ready\n"); return; }
    browsed_player = 0xFFFF;
    get_players();
}
void bt_browse_folder(void){ start_query(AVRCP_BROWSING_MEDIA_PLAYER_VIRTUAL_FILESYSTEM); }

void bt_browse_root(void){
    if (!connected || busy){ printf("BROWSE: not ready\n"); return; }
    if (browsed_player == 0xFFFF){ printf("BROWSE: no browsable player on this phone (only the Now Playing queue, key l)\n"); return; }
    // SetBrowsedPlayer resets the current path to the root folder
    uint8_t status = avrcp_browsing_controller_set_browsed_player(browsing_cid, browsed_player);
    if (status != ERROR_CODE_SUCCESS){ printf("BROWSE: set browsed player failed 0x%02x\n", status); return; }
    busy = true; state = BR_W4_PATH; list_after_path = true;
}

void bt_browse_up(void){
    if (!connected || busy){ printf("BROWSE: not ready\n"); return; }
    uint8_t status = avrcp_browsing_controller_go_up_one_level(browsing_cid);
    if (status != ERROR_CODE_SUCCESS){ printf("BROWSE: go up failed 0x%02x\n", status); return; }
    busy = true; state = BR_W4_PATH; list_after_path = true;
}

void bt_browse_select(uint16_t index){
    if (!connected || busy){ printf("BROWSE: not ready\n"); return; }
    if (index >= item_count){ printf("BROWSE: no item %u\n", index); return; }
    br_item_t *it = &items[index];
    uint8_t status;
    if (it->type == BR_ITEM_FOLDER){
        printf("BROWSE: open folder \"%s\"\n", it->name);
        status = avrcp_browsing_controller_go_down_one_level(browsing_cid, it->uid);
        if (status != ERROR_CODE_SUCCESS){ printf("BROWSE: go down failed 0x%02x\n", status); return; }
        busy = true; state = BR_W4_PATH; list_after_path = true;
    } else {
        printf("BROWSE: PlayItem \"%s\" (scope %s, uid counter %u)\n", it->name,
               list_scope == AVRCP_BROWSING_NOW_PLAYING ? "now playing" : "file system", uid_counter);
        status = avrcp_controller_play_item_for_scope(bt_avrcp_cid(), it->uid, uid_counter, list_scope);
        if (status != ERROR_CODE_SUCCESS) printf("BROWSE: PlayItem failed 0x%02x\n", status);
    }
}

const br_item_t *bt_browse_items(uint16_t *count, uint32_t *total){
    if (count) *count = item_count;
    if (total) *total = item_total;
    return items;
}

void bt_browse_on_avrcp_connected(bd_addr_t addr){
    memcpy(remote_addr, addr, 6);
    if (connected || browsing_cid) return;
    btstack_run_loop_remove_timer(&connect_timer);
    btstack_run_loop_set_timer_handler(&connect_timer, &connect_timer_handler);
    btstack_run_loop_set_timer(&connect_timer, BROWSE_CONNECT_DELAY_MS);
    btstack_run_loop_add_timer(&connect_timer);
}

void bt_browse_on_avrcp_released(void){
    btstack_run_loop_remove_timer(&connect_timer);
    if (browsing_cid && connected) avrcp_browsing_disconnect(browsing_cid);
    browsing_cid = 0; connected = false; busy = false; state = BR_IDLE; browsed_player = 0xFFFF;
}
