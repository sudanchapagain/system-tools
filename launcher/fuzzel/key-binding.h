#pragma once

#include <xkbcommon/xkbcommon.h>
#include <tllist.h>

#include "config.h"

enum bind_action {
    BIND_ACTION_NONE,
    BIND_ACTION_CANCEL,
    BIND_ACTION_CURSOR_HOME,
    BIND_ACTION_CURSOR_END,
    BIND_ACTION_CURSOR_LEFT,
    BIND_ACTION_CURSOR_LEFT_WORD,
    BIND_ACTION_CURSOR_RIGHT,
    BIND_ACTION_CURSOR_RIGHT_WORD,
    BIND_ACTION_DELETE_LINE,
    BIND_ACTION_DELETE_PREV,
    BIND_ACTION_DELETE_PREV_WORD,
    BIND_ACTION_DELETE_LINE_BACKWARD,
    BIND_ACTION_DELETE_NEXT,
    BIND_ACTION_DELETE_NEXT_WORD,
    BIND_ACTION_DELETE_LINE_FORWARD,
    BIND_ACTION_INSERT_SELECTED,
    BIND_ACTION_EXPUNGE,
    BIND_ACTION_CLIPBOARD_PASTE,
    BIND_ACTION_PRIMARY_PASTE,

    BIND_ACTION_MATCHES_EXECUTE,
    BIND_ACTION_MATCHES_EXECUTE_OR_NEXT,
    BIND_ACTION_MATCHES_EXECUTE_INPUT,
    BIND_ACTION_MATCHES_PREV,
    BIND_ACTION_MATCHES_PREV_WITH_WRAP,
    BIND_ACTION_MATCHES_PREV_PAGE,
    BIND_ACTION_MATCHES_NEXT,
    BIND_ACTION_MATCHES_NEXT_WITH_WRAP,
    BIND_ACTION_MATCHES_NEXT_PAGE,
    BIND_ACTION_MATCHES_FIRST,
    BIND_ACTION_MATCHES_LAST,

    BIND_ACTION_CUSTOM_1,
    BIND_ACTION_CUSTOM_2,
    BIND_ACTION_CUSTOM_3,
    BIND_ACTION_CUSTOM_4,
    BIND_ACTION_CUSTOM_5,
    BIND_ACTION_CUSTOM_6,
    BIND_ACTION_CUSTOM_7,
    BIND_ACTION_CUSTOM_8,
    BIND_ACTION_CUSTOM_9,
    BIND_ACTION_CUSTOM_10,
    BIND_ACTION_CUSTOM_11,
    BIND_ACTION_CUSTOM_12,
    BIND_ACTION_CUSTOM_13,
    BIND_ACTION_CUSTOM_14,
    BIND_ACTION_CUSTOM_15,
    BIND_ACTION_CUSTOM_16,
    BIND_ACTION_CUSTOM_17,
    BIND_ACTION_CUSTOM_18,
    BIND_ACTION_CUSTOM_19,

    BIND_ACTION_COUNT,
};

typedef tll(xkb_keycode_t) xkb_keycode_list_t;

struct key_binding {
    int action; /* enum bind_action_* */
    xkb_mod_mask_t mods;

    union {
        struct {
            xkb_keysym_t sym;
            xkb_keycode_list_t key_codes;
        } k;
#if 0
        struct {
            uint32_t button;
            int count;
        } m;
#endif
    };
};
typedef tll(struct key_binding) key_binding_list_t;

struct key_binding_set {
    key_binding_list_t key;
    //key_binding_list_t mouse;
};

struct seat;
struct kb_manager;

struct kb_manager *kb_manager_new(void);
void kb_manager_destroy(struct kb_manager *mgr);

void kb_new_for_seat(struct kb_manager *mgr, const struct config *conf,
                     const struct seat *seat);
void kb_remove_seat(struct kb_manager *mgr, const struct seat *seat);

void kb_load_keymap(struct kb_manager *mgr, const struct seat *seat,
                    struct xkb_state *xkb, struct xkb_keymap *keymap);
void kb_unload_keymap(struct kb_manager *mgr, const struct seat *seat);

struct key_binding_set *key_binding_for(
    struct kb_manager *mgr, const struct seat *seat);
