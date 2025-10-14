#pragma once

#include "application.h"
#include "icon.h"
#include "tllist.h"

struct xdg_data_dir {
    char *path;
    int fd;
};

typedef tll(struct xdg_data_dir) xdg_data_dirs_t;

xdg_data_dirs_t xdg_data_dirs(void);
void xdg_data_dirs_destroy(xdg_data_dirs_t dirs);

const char *xdg_cache_dir(void);

void xdg_find_programs(
    const char *terminal, bool include_actions,
    bool filter_desktop, char_list_t *desktops,
    struct application_list *applications);
