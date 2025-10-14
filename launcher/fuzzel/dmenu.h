#pragma once

#include <sys/types.h>

#include "application.h"
#include "config.h"
#include "icon.h"
#include "prompt.h"

void dmenu_load_entries(
    struct application_list *applications, char delim,
    const char *with_nth_format, const char *match_nth_format,
    char nth_delim, int event_fd, int abort_fd);

bool dmenu_execute(
    const struct application *app, ssize_t index,
    const struct prompt *prompt, enum dmenu_mode format,
    const char *nth_format, char nth_delim);

void dmenu_try_icon_list(
    struct application_list *applications,
    icon_theme_list_t themes, int icon_size);
