#pragma once

enum event_type {
    EVENT_APPS_SOME_LOADED,
    EVENT_APPS_ALL_LOADED,
    EVENT_ICONS_LOADED,

    EVENT_INVALID,
};

int send_event(int fd, enum event_type event);
