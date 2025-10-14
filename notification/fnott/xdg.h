#pragma once

#include "tllist.h"

struct xdg_data_dir {
    char *path;
    int fd;
};

typedef tll(struct xdg_data_dir) xdg_data_dirs_t;

xdg_data_dirs_t xdg_data_dirs(void);
void xdg_data_dirs_destroy(xdg_data_dirs_t dirs);

const char *xdg_cache_dir(void);
