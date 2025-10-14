#pragma once

#include <stdbool.h>

#include "notification.h"
#include "fdm.h"
#include "dbus.h"

struct ctrl;
struct ctrl *ctrl_init(
    struct fdm *fdm, struct notif_mgr *notif_mgr, struct dbus *bus);
void ctrl_destroy(struct ctrl *ctrl);

int ctrl_poll_fd(const struct ctrl *ctrl);
