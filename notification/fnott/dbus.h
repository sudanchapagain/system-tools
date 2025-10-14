#pragma once

#include <stdbool.h>

#include "notification.h"
#include "wayland.h"
#include "fdm.h"
#include "icon.h"

struct dbus;

struct dbus *dbus_init(
    const struct config *conf, struct fdm *fdm, struct wayland *wayl,
    struct notif_mgr *notif_mgr, const icon_theme_list_t *icon_theme);
void dbus_destroy(struct dbus *bus);

bool dbus_signal_expired(struct dbus *bus, uint32_t id);
bool dbus_signal_dismissed(struct dbus *bus, uint32_t id);
bool dbus_signal_closed(struct dbus *bus, uint32_t id);
bool dbus_signal_token(struct dbus *bus, uint32_t id, const char *token);
bool dbus_signal_action(struct dbus *bus, uint32_t id, const char *action_id);

void dbus_dispatch_initial_pending(struct dbus *bus);
int dbus_poll_fd(const struct dbus *bus);
