// AVRCP Browsing controller: media players, folder tree and Now Playing list of the phone.
#ifndef BT_BROWSE_H
#define BT_BROWSE_H

#include <stdbool.h>
#include <stdint.h>
#include "btstack.h"

#define BR_MAX_ITEMS 60
#define BR_NAME_LEN  48

typedef enum { BR_ITEM_FOLDER = 1, BR_ITEM_MEDIA = 2 } br_item_type_t;

typedef struct {
    br_item_type_t type;
    uint8_t  uid[8];
    bool     playable;
    char     name[BR_NAME_LEN];     // folder name or track title
    char     artist[BR_NAME_LEN];   // media items only
} br_item_t;

void bt_browse_init(void);
bool bt_browse_connected(void);
bool bt_browse_busy(void);

// queries (one at a time; results are printed on the console and kept in the item list)
void bt_browse_players(void);         // re-list media players, pick one, list its queue
void bt_browse_now_playing(void);     // current queue of the browsed player
void bt_browse_root(void);            // re-select player (resets path) and list the root folder
void bt_browse_folder(void);          // list the current folder
void bt_browse_up(void);              // go up one level, then list
void bt_browse_select(uint16_t index);// folder: go down + list; track: play it

const br_item_t *bt_browse_items(uint16_t *count, uint32_t *total);

// called by bt_avrcp
void bt_browse_on_avrcp_connected(bd_addr_t addr);
void bt_browse_on_avrcp_released(void);

#endif
