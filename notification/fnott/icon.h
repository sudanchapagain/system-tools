#pragma once

#include <stdbool.h>
#include <pixman.h>

#include "tllist.h"

enum icon_dir_type {
    ICON_DIR_FIXED,
    ICON_DIR_SCALABLE,
    ICON_DIR_THRESHOLD,
};

struct icon_dir {
    char *path;  /* Relative to theme's base path */
    int size;
    int min_size;
    int max_size;
    int scale;
    int threshold;
    enum icon_dir_type type;
};

struct icon_theme {
    char *name;
    tll(struct icon_dir) dirs;
};

typedef tll(struct icon_theme) icon_theme_list_t;

icon_theme_list_t icon_load_theme(const char *name, bool filter_context);
void icon_themes_destroy(icon_theme_list_t themes);

pixman_image_t *icon_load(
    const char *name, int icon_size, const icon_theme_list_t *themes);
