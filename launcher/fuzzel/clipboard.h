#pragma once

#include <stdint.h>
#include <primary-selection-unstable-v1.h>

/* Mime-types we support when dealing with data offers (e.g. copy-paste, or DnD) */
enum data_offer_mime_type {
    DATA_OFFER_MIME_UNSET,
    DATA_OFFER_MIME_TEXT_PLAIN,
    DATA_OFFER_MIME_TEXT_UTF8,
    DATA_OFFER_MIME_URI_LIST,

    DATA_OFFER_MIME_TEXT_TEXT,
    DATA_OFFER_MIME_TEXT_STRING,
    DATA_OFFER_MIME_TEXT_UTF8_STRING,
};

struct wl_clipboard {
    //struct wl_window *window;  /* For DnD */
    struct wl_data_source *data_source;
    struct wl_data_offer *data_offer;
    enum data_offer_mime_type mime_type;
    char *text;
    uint32_t serial;
};

struct wl_primary {
    struct zwp_primary_selection_source_v1 *data_source;
    struct zwp_primary_selection_offer_v1 *data_offer;
    enum data_offer_mime_type mime_type;
    char *text;
    uint32_t serial;
};

extern const struct wl_data_device_listener data_device_listener;
extern const struct zwp_primary_selection_device_v1_listener primary_selection_device_listener;

struct seat;
void paste_from_clipboard(struct seat *seat);
void paste_from_primary(struct seat *seat);
