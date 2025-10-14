#pragma once
#include <stdint.h>

enum ctrl_command {
    CTRL_QUIT,
    CTRL_LIST,
    CTRL_PAUSE,
    CTRL_UNPAUSE,
    CTRL_DISMISS_BY_ID, CTRL_DISMISS_ALL,
    CTRL_ACTIONS_BY_ID,
    CTRL_DISMISS_WITH_DEFAULT_ACTION_BY_ID,
};

struct ctrl_request {
    enum ctrl_command cmd;
    uint32_t id;
} __attribute__((packed));

enum ctrl_result { CTRL_OK, CTRL_INVALID_ID, CTRL_NO_ACTIONS, CTRL_ERROR };
struct ctrl_reply {
    enum ctrl_result result;
} __attribute__((packed));
