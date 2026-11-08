// USB serial console: prints now-playing changes and accepts single-key commands.
#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "btstack_stdin.h"

#include "console.h"
#include "bt_conn.h"
#include "bt_avrcp.h"
#include "bt_audio.h"
#include "bt_browse.h"
#include "now_playing.h"

static const char *status_str(np_status_t s){
    switch (s){
        case NP_STATUS_STOPPED: return "STOPPED";
        case NP_STATUS_PLAYING: return "PLAYING";
        case NP_STATUS_PAUSED:  return "PAUSED";
        default:                return "UNKNOWN";
    }
}

static void fmt_time(char *out, size_t n, uint32_t ms){
    uint32_t s = ms / 1000;
    snprintf(out, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

static void print_now_playing(const now_playing_t *np){
    char pos[16], len[16];
    fmt_time(pos, sizeof(pos), now_playing_position_now_ms());
    fmt_time(len, sizeof(len), np->length_ms);
    printf("NOW  : [%s] %s - %s", status_str(np->status),
           np->artist[0] ? np->artist : "?", np->title[0] ? np->title : "?");
    if (np->album[0]) printf(" (%s)", np->album);
    if (np->total_tracks) printf(" #%u/%u", (unsigned) np->track_number, (unsigned) np->total_tracks);
    printf(" %s/%s\n", pos, len);
}

// Machine-readable mirror of the state for tools (e.g. the cockpit web page):
// one JSON object per line, prefixed with "@J ".
static void json_escape(const char *src, char *dst, size_t n){
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *) src; *p && o + 7 < n; p++){
        switch (*p){
            case '"':  dst[o++] = '\\'; dst[o++] = '"';  break;
            case '\\': dst[o++] = '\\'; dst[o++] = '\\'; break;
            default:
                if (*p < 0x20){ o += snprintf(dst + o, n - o, "\\u%04x", *p); }
                else dst[o++] = (char) *p;
        }
    }
    dst[o] = 0;
}

static void print_json_np(const now_playing_t *np){
    char t[2 * NP_STR_LEN], a[2 * NP_STR_LEN], al[2 * NP_STR_LEN];
    json_escape(np->title, t, sizeof(t));
    json_escape(np->artist, a, sizeof(a));
    json_escape(np->album, al, sizeof(al));
    printf("@J {\"np\":{\"status\":\"%s\",\"title\":\"%s\",\"artist\":\"%s\",\"album\":\"%s\","
           "\"track\":%u,\"total\":%u,\"pos\":%u,\"len\":%u}}\n",
           status_str(np->status), t, a, al,
           (unsigned) np->track_number, (unsigned) np->total_tracks,
           (unsigned) now_playing_position_now_ms(), (unsigned) np->length_ms);
}

static void print_json_conn(void){
    bd_addr_t addr;
    char phone[20] = "";
    if (bt_conn_has_bond()){
        bt_conn_get_bonded_addr(addr);
        snprintf(phone, sizeof(phone), "%s", bd_addr_to_str(addr));
    }
    printf("@J {\"conn\":{\"state\":\"%s\",\"phone\":\"%s\",\"a2dp\":%s,\"stream\":%s,\"avrcp\":%s,\"autoplay\":%s}}\n",
           bt_conn_state_str(bt_conn_get_state()), phone,
           bt_audio_a2dp_cid() ? "true" : "false", bt_audio_stream_playing() ? "true" : "false",
           bt_avrcp_connected() ? "true" : "false", bt_conn_get_autoplay() ? "true" : "false");
}

static void conn_state_listener(bt_conn_state_t state){
    UNUSED(state);
    print_json_conn();
}

static void np_listener(np_change_t what, const now_playing_t *np){
    print_json_np(np);
    switch (what){
        case NP_CHANGE_METADATA:
            print_now_playing(np);
            break;
        case NP_CHANGE_STATUS:
            printf("NOW  : status %s\n", status_str(np->status));
            break;
        case NP_CHANGE_POSITION: {
            char pos[16], len[16];
            fmt_time(pos, sizeof(pos), np->position_ms);
            fmt_time(len, sizeof(len), np->length_ms);
            printf("NOW  : position %s/%s\n", pos, len);
            break;
        }
    }
}

static void print_status(void){
    bd_addr_t addr;
    printf("\n--- PicoCarPod status ---\n");
    printf("state      : %s\n", bt_conn_state_str(bt_conn_get_state()));
    if (bt_conn_has_bond()){
        bt_conn_get_bonded_addr(addr);
        printf("phone      : %s\n", bd_addr_to_str(addr));
    } else {
        printf("phone      : none paired\n");
    }
    printf("a2dp       : %s%s\n", bt_audio_a2dp_cid() ? "connected" : "-", bt_audio_stream_playing() ? ", streaming" : "");
    printf("avrcp      : %s\n", bt_avrcp_connected() ? "connected" : "-");
    printf("browsing   : %s%s\n", bt_browse_connected() ? "connected" : "-", bt_browse_busy() ? ", query running" : "");
    printf("autoplay   : %s\n", bt_conn_get_autoplay() ? "on" : "off");
    print_now_playing(now_playing_get());
    printf("\n");
    print_json_conn();
    print_json_np(now_playing_get());
}

static void show_usage(void){
    printf("\n--- PicoCarPod console ---\n");
    printf("h : help           s : status\n");
    printf("p : pairing mode   f : forget all paired phones\n");
    printf("c : connect now    d : disconnect (pauses auto-reconnect)\n");
    printf("SPACE: play/pause  n : next   b : previous\n");
    printf("i : refresh now playing info   a : toggle autoplay\n");
    printf("--- AVRCP browsing (phone library) ---\n");
    printf("l : now playing list   P : re-list media players   r : root folder   u : up one level\n");
    printf("0-9 : open folder / play track n of the last list   < > : page by 10\n\n");
}

static uint16_t page_base;

static void stdin_handler(char c){
    switch (c){
        case 'h': case '?': show_usage(); break;
        case 's': print_status(); break;
        case 'p': bt_conn_set_pairing_mode(true); break;
        case 'f': bt_conn_forget_all(); break;
        case 'c': bt_conn_connect_now(); break;
        case 'd': bt_conn_disconnect(); break;
        case ' ': bt_avrcp_toggle_play_pause(); break;
        case 'n': bt_avrcp_next(); break;
        case 'b': bt_avrcp_prev(); break;
        case 'i': bt_avrcp_refresh_now_playing(); bt_avrcp_refresh_play_status(); break;
        case 'l': bt_browse_now_playing(); break;
        case 'P': bt_browse_players(); break;
        case 'r': bt_browse_root(); break;
        case 'u': bt_browse_up(); break;
        case '<': if (page_base >= 10) page_base -= 10; printf("CONS : items %u-%u\n", page_base, page_base + 9); break;
        case '>': page_base += 10; printf("CONS : items %u-%u\n", page_base, page_base + 9); break;
        case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
            bt_browse_select(page_base + (uint16_t)(c - '0')); break;
        case 'a':
            bt_conn_set_autoplay(!bt_conn_get_autoplay());
            printf("CONN : autoplay %s\n", bt_conn_get_autoplay() ? "on" : "off");
            break;
        case '\n': case '\r': break;
        default: show_usage(); break;
    }
}

void console_init(void){
    now_playing_set_listener(&np_listener);
    bt_conn_set_state_listener(&conn_state_listener);
    btstack_stdin_setup(&stdin_handler);
}
