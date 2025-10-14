#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <fcft/fcft.h>

#include "clipboard.h"
#include "config.h"
#include "dmenu.h"
#include "fdm.h"
#include "key-binding.h"
#include "match.h"
#include "prompt.h"
#include "render.h"

struct wayland;

struct repeat {
    int fd;
    int32_t delay;
    int32_t rate;

    bool dont_re_repeat;
    uint32_t key;
};

struct seat {
    struct fdm *fdm;
    struct wayland *wayl;
    struct wl_seat *wl_seat;
    uint32_t wl_name;
    char *name;

    struct wl_keyboard *wl_keyboard;
    struct {
        uint32_t serial;
        struct xkb_context *xkb;
        struct xkb_keymap *xkb_keymap;
        struct xkb_state *xkb_state;
        struct xkb_compose_table *xkb_compose_table;
        struct xkb_compose_state *xkb_compose_state;
        struct repeat repeat;
    } kbd;

    struct wl_pointer *wl_pointer;
    struct {
        uint32_t serial;

        int x;
        int y;
        size_t hovered_row_idx;

        struct wl_surface *surface;
        struct wp_viewport *viewport;
        struct wl_cursor_theme *theme;
        struct wl_cursor *cursor;
        float scale;

        bool discrete_used;
        double accumulated_scroll;
    } pointer;

    struct wl_touch *wl_touch;
    struct {
        uint32_t serial;
        struct {
            int id;
            double start_x, start_y;
            double current_x, current_y;
            double last_y;
            bool scrolling;
            bool is_tap;
            uint32_t start_time;
            double accumulated_scroll;
        } active_touch;
    } touch;

    struct wl_data_device *data_device;
    struct zwp_primary_selection_device_v1 *primary_selection_device;

    bool is_pasting;
    struct wl_clipboard clipboard;
    struct wl_primary primary;
};


typedef void (*font_reloaded_t)(
    struct wayland *wayl, struct fcft_font *font, void *data);

struct wayland *wayl_init(
    const struct config *conf, struct fdm *fdm,
    struct kb_manager *kb_manager, struct render *render,
    struct prompt *prompt, struct matches *matches,
    font_reloaded_t font_reloaded_cb, void *data);

void wayl_destroy(struct wayland *wayl);

void wayl_refresh(struct wayland *wayl);
void wayl_flush(struct wayland *wayl);
void wayl_ready_to_display(struct wayland *wayl);

int wayl_exit_code(const struct wayland *wayl);
bool wayl_update_cache(const struct wayland *wayl);

void wayl_clipboard_data(struct wayland *wayl, char *data, size_t size);
void wayl_clipboard_done(struct wayland *wayl);

bool wayl_do_linear_blending(const struct wayland *wayl);

void wayl_resized(struct wayland *wayl);

bool wayl_check_auto_select(struct wayland *wayl);
