#pragma once

#include <stdbool.h>
#include <pixman.h>

#include "char32.h"

#include <wlr-layer-shell-unstable-v1.h>

struct config_font {
    char *pattern;
    float pt_size;
    int px_size;
};

enum scaling_filter {
    SCALING_FILTER_NONE,
    SCALING_FILTER_NEAREST,
    SCALING_FILTER_BILINEAR,
    SCALING_FILTER_CUBIC,
    SCALING_FILTER_LANCZOS3,
};

enum progress_style {
    PROGRESS_STYLE_BAR,
    PROGRESS_STYLE_BACKGROUND
};

struct urgency_config {
    enum zwlr_layer_shell_v1_layer layer;
    pixman_color_t bg;

    struct {
        pixman_color_t color;
        int size;
        int radius;
    } border;

    struct {
        int vertical;
        int horizontal;
    } padding;

    struct {
        struct config_font font;
        pixman_color_t color;
        char32_t *format;
    } app;

    struct {
        struct config_font font;
        pixman_color_t color;
        char32_t *format;
    } summary;

    struct {
        struct config_font font;
        pixman_color_t color;
        char32_t *format;
    } body;

    struct {
        struct config_font font;
        pixman_color_t color;
    } action;

    struct {
        int height;
        pixman_color_t color;
        enum progress_style style;
    } progress;

    int max_timeout_secs;
    int default_timeout_secs;
    int idle_timeout_secs;
    char *sound_file;  /* Path to user-configured sound to play on notification */
    char *icon;
};

struct config_spawn_template {
    char *raw_cmd;
    char **argv;
};

struct config {
    char *output;
    int min_width;
    int max_width;
    int max_height;

    bool dpi_aware;

    char *icon_theme_name;
    int max_icon_size;

    enum {
        STACK_BOTTOM_UP,
        STACK_TOP_DOWN,
    } stacking_order;

    enum {
        ANCHOR_TOP_LEFT,
        ANCHOR_TOP_RIGHT,
        ANCHOR_BOTTOM_LEFT,
        ANCHOR_BOTTOM_RIGHT,
        ANCHOR_CENTER,
    } anchor;

    struct {
        int vertical;
        int horizontal;
        int between;
    } margins;

    struct urgency_config by_urgency[3];

    char *selection_helper;
    bool selection_helper_uses_null_separator;
    struct config_spawn_template play_sound;
    enum scaling_filter scaling_filter;
};

bool config_load(struct config *conf, const char *path);
void config_destroy(struct config conf);
