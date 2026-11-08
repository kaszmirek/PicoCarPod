// AVRCP controller: pulls metadata (title/artist/album, play status, position)
// from the phone and sends transport commands. AVRCP target: absolute volume
// (accepted and ignored - the car controls the volume on its side).
#include <stdio.h>
#include <string.h>

#include "btstack.h"

#include "bt_avrcp.h"
#include "bt_conn.h"
#include "bt_browse.h"
#include "now_playing.h"

#define REQUEST_POLL_MS        100   // retry interval for queued controller requests
#define POSITION_REFRESH_MS   5000   // periodic play-status refresh while playing

static uint8_t sdp_avrcp_target_service_buffer[150];
static uint8_t sdp_avrcp_controller_service_buffer[200];

static struct {
    bd_addr_t addr;
    uint16_t  avrcp_cid;
    uint16_t  notifications_supported_by_target;
    bool      notifications_enabled;
    // queued requests (AVRCP controller handles one command at a time)
    bool want_now_playing;
    bool want_play_status;
} avrcp_conn;

static now_playing_t np;              // committed state
static now_playing_t np_pending;      // being filled by a GetElementAttributes response
static np_listener_t np_listener;

static btstack_timer_source_t request_timer;
static btstack_timer_source_t position_timer;
static btstack_timer_source_t commit_timer;     // debounce for GetElementAttributes replies
#define METADATA_COMMIT_MS 150
static uint8_t volume_abs = 127;

// ---------------------------------------------------------------------------
// now_playing model

const now_playing_t *now_playing_get(void){
    return &np;
}

void now_playing_set_listener(np_listener_t listener){
    np_listener = listener;
}

uint32_t now_playing_position_now_ms(void){
    if (np.status != NP_STATUS_PLAYING) return np.position_ms;
    uint32_t elapsed = btstack_run_loop_get_time_ms() - np.position_ts_ms;
    uint32_t pos = np.position_ms + elapsed;
    if (np.length_ms && pos > np.length_ms) pos = np.length_ms;
    return pos;
}

static void notify(np_change_t what){
    if (np_listener) np_listener(what, &np);
}

static np_status_t map_status(uint8_t avrcp_status){
    switch (avrcp_status){
        case AVRCP_PLAYBACK_STATUS_STOPPED:  return NP_STATUS_STOPPED;
        case AVRCP_PLAYBACK_STATUS_PLAYING:
        case AVRCP_PLAYBACK_STATUS_FWD_SEEK:
        case AVRCP_PLAYBACK_STATUS_REV_SEEK: return NP_STATUS_PLAYING;
        case AVRCP_PLAYBACK_STATUS_PAUSED:   return NP_STATUS_PAUSED;
        default:                             return NP_STATUS_UNKNOWN;
    }
}

static void set_status(np_status_t s){
    if (np.status == s) return;
    // freeze/resume position extrapolation
    np.position_ms = now_playing_position_now_ms();
    np.position_ts_ms = btstack_run_loop_get_time_ms();
    np.status = s;
    notify(NP_CHANGE_STATUS);
}

static void copy_attr(char *dst, const uint8_t *src, uint16_t len){
    if (len >= NP_STR_LEN) len = NP_STR_LEN - 1;
    memcpy(dst, src, len);
    dst[len] = 0;
}

// ---------------------------------------------------------------------------
// request queue

static void request_timer_handler(btstack_timer_source_t *ts){
    if (avrcp_conn.avrcp_cid == 0) return;
    uint8_t status;
    if (avrcp_conn.want_now_playing){
        status = avrcp_controller_get_now_playing_info(avrcp_conn.avrcp_cid);
        if (status == ERROR_CODE_SUCCESS) avrcp_conn.want_now_playing = false;
    } else if (avrcp_conn.want_play_status){
        status = avrcp_controller_get_play_status(avrcp_conn.avrcp_cid);
        if (status == ERROR_CODE_SUCCESS) avrcp_conn.want_play_status = false;
    }
    if (avrcp_conn.want_now_playing || avrcp_conn.want_play_status){
        btstack_run_loop_set_timer(ts, REQUEST_POLL_MS);
        btstack_run_loop_add_timer(ts);
    }
}

static void kick_requests(void){
    btstack_run_loop_remove_timer(&request_timer);
    btstack_run_loop_set_timer_handler(&request_timer, &request_timer_handler);
    btstack_run_loop_set_timer(&request_timer, 1);
    btstack_run_loop_add_timer(&request_timer);
}

void bt_avrcp_refresh_now_playing(void){
    if (avrcp_conn.avrcp_cid == 0) return;
    avrcp_conn.want_now_playing = true;
    kick_requests();
}

void bt_avrcp_refresh_play_status(void){
    if (avrcp_conn.avrcp_cid == 0) return;
    avrcp_conn.want_play_status = true;
    kick_requests();
}

static void position_timer_handler(btstack_timer_source_t *ts){
    if (avrcp_conn.avrcp_cid == 0) return;
    if (np.status == NP_STATUS_PLAYING){
        bt_avrcp_refresh_play_status();
    }
    btstack_run_loop_set_timer(ts, POSITION_REFRESH_MS);
    btstack_run_loop_add_timer(ts);
}

// ---------------------------------------------------------------------------
// metadata commit
//
// BTstack 1.6 (pico-sdk 2.2.0) never emits AVRCP_SUBEVENT_NOW_PLAYING_INFO_DONE for a
// single-packet GetElementAttributes reply (parser does not count the num_attributes
// byte), and the string INFO events are emitted with a size that excludes the text
// (the text is still in the buffer behind the length byte). So: collect attributes
// as they arrive and commit after a short quiet period, or on INFO_DONE if it comes.

static void commit_pending_metadata(void){
    bool changed = strcmp(np.title, np_pending.title) != 0
                || strcmp(np.artist, np_pending.artist) != 0
                || strcmp(np.album, np_pending.album) != 0
                || np.track_number != np_pending.track_number
                || np.total_tracks != np_pending.total_tracks;
    if (changed){
        memcpy(np.title, np_pending.title, NP_STR_LEN);
        memcpy(np.artist, np_pending.artist, NP_STR_LEN);
        memcpy(np.album, np_pending.album, NP_STR_LEN);
        np.track_number = np_pending.track_number;
        np.total_tracks = np_pending.total_tracks;
        if (np_pending.length_ms) np.length_ms = np_pending.length_ms;
        np.generation++;
        notify(NP_CHANGE_METADATA);
    }
    memset(&np_pending, 0, sizeof(np_pending));
}

static void commit_timer_handler(btstack_timer_source_t *ts){
    UNUSED(ts);
    commit_pending_metadata();
}

static void schedule_commit(void){
    btstack_run_loop_remove_timer(&commit_timer);
    btstack_run_loop_set_timer_handler(&commit_timer, &commit_timer_handler);
    btstack_run_loop_set_timer(&commit_timer, METADATA_COMMIT_MS);
    btstack_run_loop_add_timer(&commit_timer);
}

// ---------------------------------------------------------------------------
// AVRCP connection events

static void avrcp_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);
    uint16_t local_cid;
    uint8_t  status;
    bd_addr_t address;

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;

    switch (packet[2]){
        case AVRCP_SUBEVENT_CONNECTION_ESTABLISHED:
            local_cid = avrcp_subevent_connection_established_get_avrcp_cid(packet);
            status = avrcp_subevent_connection_established_get_status(packet);
            avrcp_subevent_connection_established_get_bd_addr(packet, address);
            if (status != ERROR_CODE_SUCCESS){
                printf("AVRCP: connection to %s failed, status 0x%02x\n", bd_addr_to_str(address), status);
                avrcp_conn.avrcp_cid = 0;
                return;
            }
            avrcp_conn.avrcp_cid = local_cid;
            memcpy(avrcp_conn.addr, address, 6);
            avrcp_conn.notifications_supported_by_target = 0;
            avrcp_conn.notifications_enabled = false;
            printf("AVRCP: connected to %s, cid 0x%02x\n", bd_addr_to_str(address), local_cid);

            // target side: we accept absolute volume, report "full"
            avrcp_target_support_event(local_cid, AVRCP_NOTIFICATION_EVENT_VOLUME_CHANGED);
            avrcp_target_volume_changed(local_cid, volume_abs);

            // controller side: discover what the phone can notify about
            avrcp_controller_get_supported_events(local_cid);

            btstack_run_loop_set_timer_handler(&position_timer, &position_timer_handler);
            btstack_run_loop_set_timer(&position_timer, POSITION_REFRESH_MS);
            btstack_run_loop_add_timer(&position_timer);

            bt_conn_on_avrcp_connected(address);
            bt_browse_on_avrcp_connected(address);
            return;

        case AVRCP_SUBEVENT_CONNECTION_RELEASED:
            printf("AVRCP: connection released (cid 0x%02x)\n", avrcp_subevent_connection_released_get_avrcp_cid(packet));
            avrcp_conn.avrcp_cid = 0;
            avrcp_conn.notifications_supported_by_target = 0;
            avrcp_conn.notifications_enabled = false;
            avrcp_conn.want_now_playing = false;
            avrcp_conn.want_play_status = false;
            btstack_run_loop_remove_timer(&request_timer);
            btstack_run_loop_remove_timer(&position_timer);
            btstack_run_loop_remove_timer(&commit_timer);
            set_status(NP_STATUS_UNKNOWN);
            // forget the last track: a reconnecting phone will report fresh data
            memset(np.title, 0, NP_STR_LEN); memset(np.artist, 0, NP_STR_LEN); memset(np.album, 0, NP_STR_LEN);
            np.track_number = 0; np.total_tracks = 0; np.length_ms = 0; np.position_ms = 0;
            np.generation++;
            memset(&np_pending, 0, sizeof(np_pending));
            bt_conn_on_avrcp_released();
            bt_browse_on_avrcp_released();
            return;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// AVRCP controller events (responses + notifications from the phone)

static void avrcp_controller_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);
    uint8_t event_id;

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;
    if (avrcp_conn.avrcp_cid == 0) return;

#ifdef BT_AVRCP_DEBUG
    printf("AVRCP: controller subevent 0x%02x (%u bytes)\n", packet[2], size);
#endif
    switch (packet[2]){
        case AVRCP_SUBEVENT_GET_CAPABILITY_EVENT_ID:
            avrcp_conn.notifications_supported_by_target |= (1 << avrcp_subevent_get_capability_event_id_get_event_id(packet));
            break;

        case AVRCP_SUBEVENT_GET_CAPABILITY_EVENT_ID_DONE:
            printf("AVRCP: phone supports notifications:");
            for (event_id = (uint8_t) AVRCP_NOTIFICATION_EVENT_FIRST_INDEX; event_id < (uint8_t) AVRCP_NOTIFICATION_EVENT_LAST_INDEX; event_id++){
                if (avrcp_conn.notifications_supported_by_target & (1 << event_id)){
                    printf(" %s", avrcp_notification2str((avrcp_notification_event_id_t)event_id));
                }
            }
            printf("\n");
            avrcp_controller_enable_notification(avrcp_conn.avrcp_cid, AVRCP_NOTIFICATION_EVENT_PLAYBACK_STATUS_CHANGED);
            avrcp_controller_enable_notification(avrcp_conn.avrcp_cid, AVRCP_NOTIFICATION_EVENT_TRACK_CHANGED);
            avrcp_controller_enable_notification(avrcp_conn.avrcp_cid, AVRCP_NOTIFICATION_EVENT_NOW_PLAYING_CONTENT_CHANGED);
            if (avrcp_conn.notifications_supported_by_target & (1 << AVRCP_NOTIFICATION_EVENT_PLAYBACK_POS_CHANGED)){
                avrcp_controller_enable_notification(avrcp_conn.avrcp_cid, AVRCP_NOTIFICATION_EVENT_PLAYBACK_POS_CHANGED);
            }
            avrcp_conn.notifications_enabled = true;
            // initial state
            bt_avrcp_refresh_now_playing();
            bt_avrcp_refresh_play_status();
            break;

        case AVRCP_SUBEVENT_NOTIFICATION_STATE:
            event_id = avrcp_subevent_notification_state_get_event_id(packet);
            printf("AVRCP: notification %s registered\n", avrcp_notification2str((avrcp_notification_event_id_t)event_id));
            break;

        case AVRCP_SUBEVENT_NOTIFICATION_PLAYBACK_STATUS_CHANGED:
            set_status(map_status(avrcp_subevent_notification_playback_status_changed_get_play_status(packet)));
            bt_avrcp_refresh_play_status();   // pick up position/length
            break;

        case AVRCP_SUBEVENT_NOTIFICATION_TRACK_CHANGED:
            bt_avrcp_refresh_now_playing();
            bt_avrcp_refresh_play_status();
            break;

        case AVRCP_SUBEVENT_NOTIFICATION_NOW_PLAYING_CONTENT_CHANGED:
            bt_avrcp_refresh_now_playing();
            break;

        case AVRCP_SUBEVENT_NOTIFICATION_PLAYBACK_POS_CHANGED:
            np.position_ms = avrcp_subevent_notification_playback_pos_changed_get_playback_position_ms(packet);
            np.position_ts_ms = btstack_run_loop_get_time_ms();
            notify(NP_CHANGE_POSITION);
            break;

        // --- GetElementAttributes (now playing info) response, attribute by attribute
        case AVRCP_SUBEVENT_NOW_PLAYING_TRACK_INFO:
            np_pending.track_number = avrcp_subevent_now_playing_track_info_get_track(packet);
            schedule_commit();
            break;
        case AVRCP_SUBEVENT_NOW_PLAYING_TOTAL_TRACKS_INFO:
            np_pending.total_tracks = avrcp_subevent_now_playing_total_tracks_info_get_total_tracks(packet);
            schedule_commit();
            break;
        case AVRCP_SUBEVENT_NOW_PLAYING_SONG_LENGTH_MS_INFO:
            np_pending.length_ms = avrcp_subevent_now_playing_song_length_ms_info_get_song_length(packet);
            schedule_commit();
            break;
        case AVRCP_SUBEVENT_NOW_PLAYING_TITLE_INFO:
            copy_attr(np_pending.title, avrcp_subevent_now_playing_title_info_get_value(packet),
                      avrcp_subevent_now_playing_title_info_get_value_len(packet));
            schedule_commit();
            break;
        case AVRCP_SUBEVENT_NOW_PLAYING_ARTIST_INFO:
            copy_attr(np_pending.artist, avrcp_subevent_now_playing_artist_info_get_value(packet),
                      avrcp_subevent_now_playing_artist_info_get_value_len(packet));
            schedule_commit();
            break;
        case AVRCP_SUBEVENT_NOW_PLAYING_ALBUM_INFO:
            copy_attr(np_pending.album, avrcp_subevent_now_playing_album_info_get_value(packet),
                      avrcp_subevent_now_playing_album_info_get_value_len(packet));
            schedule_commit();
            break;
        case AVRCP_SUBEVENT_NOW_PLAYING_INFO_DONE:
            btstack_run_loop_remove_timer(&commit_timer);
            commit_pending_metadata();
            break;

        case AVRCP_SUBEVENT_PLAY_STATUS: {
            uint32_t len = avrcp_subevent_play_status_get_song_length(packet);
            uint32_t pos = avrcp_subevent_play_status_get_song_position(packet);
            if (len != 0xFFFFFFFF) np.length_ms = len;
            if (pos != 0xFFFFFFFF) np.position_ms = pos;
            np.position_ts_ms = btstack_run_loop_get_time_ms();
            set_status(map_status(avrcp_subevent_play_status_get_play_status(packet)));
            notify(NP_CHANGE_POSITION);
            break;
        }

        case AVRCP_SUBEVENT_OPERATION_COMPLETE:
        case AVRCP_SUBEVENT_OPERATION_START:
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// AVRCP target events (commands from the phone)

static void avrcp_target_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;

    switch (packet[2]){
        case AVRCP_SUBEVENT_NOTIFICATION_VOLUME_CHANGED:
            volume_abs = avrcp_subevent_notification_volume_changed_get_absolute_volume(packet);
            printf("AVRCP: phone set absolute volume %u%% (ignored, car controls volume)\n", volume_abs * 100 / 127);
            break;
        case AVRCP_SUBEVENT_OPERATION:
            // VOLUME_UP/DOWN pass-through from the phone - ignored
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Public API

void bt_avrcp_init(void){
    avrcp_init();
    avrcp_controller_init();
    avrcp_target_init();

    avrcp_register_packet_handler(&avrcp_packet_handler);
    avrcp_controller_register_packet_handler(&avrcp_controller_packet_handler);
    avrcp_target_register_packet_handler(&avrcp_target_packet_handler);

    // Controller record: we send Category 1 commands (play/pause/next...) to the media player
    memset(sdp_avrcp_controller_service_buffer, 0, sizeof(sdp_avrcp_controller_service_buffer));
    uint16_t controller_supported_features = (1 << AVRCP_CONTROLLER_SUPPORTED_FEATURE_CATEGORY_PLAYER_OR_RECORDER)
                                           | (1 << AVRCP_CONTROLLER_SUPPORTED_FEATURE_BROWSING);
    avrcp_controller_create_sdp_record(sdp_avrcp_controller_service_buffer, sdp_create_service_record_handle(),
                                       controller_supported_features, NULL, NULL);
    btstack_assert(de_get_len(sdp_avrcp_controller_service_buffer) <= sizeof(sdp_avrcp_controller_service_buffer));
    sdp_register_service(sdp_avrcp_controller_service_buffer);

    // Target record: we receive Category 2 commands (volume) from the media player
    memset(sdp_avrcp_target_service_buffer, 0, sizeof(sdp_avrcp_target_service_buffer));
    uint16_t target_supported_features = 1 << AVRCP_TARGET_SUPPORTED_FEATURE_CATEGORY_MONITOR_OR_AMPLIFIER;
    avrcp_target_create_sdp_record(sdp_avrcp_target_service_buffer, sdp_create_service_record_handle(),
                                   target_supported_features, NULL, NULL);
    btstack_assert(de_get_len(sdp_avrcp_target_service_buffer) <= sizeof(sdp_avrcp_target_service_buffer));
    sdp_register_service(sdp_avrcp_target_service_buffer);

    np.status = NP_STATUS_UNKNOWN;
}

bool bt_avrcp_connected(void){
    return avrcp_conn.avrcp_cid != 0;
}

uint16_t bt_avrcp_cid(void){
    return avrcp_conn.avrcp_cid;
}

uint8_t bt_avrcp_connect(bd_addr_t addr){
    if (avrcp_conn.avrcp_cid != 0) return ERROR_CODE_SUCCESS;
    uint16_t cid;
    uint8_t status = avrcp_connect(addr, &cid);
    if (status != ERROR_CODE_SUCCESS){
        printf("AVRCP: connect to %s failed to start, status 0x%02x\n", bd_addr_to_str(addr), status);
    } else {
        printf("AVRCP: connecting to %s ...\n", bd_addr_to_str(addr));
    }
    return status;
}

void bt_avrcp_disconnect(void){
    if (avrcp_conn.avrcp_cid == 0) return;
    avrcp_disconnect(avrcp_conn.avrcp_cid);
}

static void report(const char *what, uint8_t status){
    if (status != ERROR_CODE_SUCCESS){
        printf("AVRCP: %s failed, status 0x%02x\n", what, status);
    } else {
        printf("AVRCP: -> %s\n", what);
    }
}

void bt_avrcp_play(void){
    if (avrcp_conn.avrcp_cid == 0) return;
    report("PLAY", avrcp_controller_play(avrcp_conn.avrcp_cid));
}

void bt_avrcp_pause(void){
    if (avrcp_conn.avrcp_cid == 0) return;
    report("PAUSE", avrcp_controller_pause(avrcp_conn.avrcp_cid));
}

void bt_avrcp_toggle_play_pause(void){
    if (np.status == NP_STATUS_PLAYING) bt_avrcp_pause();
    else bt_avrcp_play();
}

void bt_avrcp_next(void){
    if (avrcp_conn.avrcp_cid == 0) return;
    report("NEXT", avrcp_controller_forward(avrcp_conn.avrcp_cid));
}

void bt_avrcp_prev(void){
    if (avrcp_conn.avrcp_cid == 0) return;
    report("PREV", avrcp_controller_backward(avrcp_conn.avrcp_cid));
}
