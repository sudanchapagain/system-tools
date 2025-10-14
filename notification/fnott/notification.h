#pragma once

#include <stdint.h>
#include <sys/types.h>

#include <pixman.h>
#include <wayland-client.h>

#include "config.h"
#include "fdm.h"
#include "icon.h"
#include "tllist.h"

/* forward declarations, to avoid circular includes */
struct wayland;
struct dbus;

struct notif_mgr;
struct notif_mgr *notif_mgr_new(struct config *conf, struct fdm *fdm,
                                const icon_theme_list_t *icon_theme);
void notif_mgr_destroy(struct notif_mgr *mgr);

void notif_mgr_configure(
    struct notif_mgr *mgr, struct wayland *wayl, struct dbus *bus);

bool notif_mgr_is_paused(struct notif_mgr *mgr);
void notif_mgr_pause(struct notif_mgr *mgr);
void notif_mgr_unpause(struct notif_mgr *mgr);

void notif_mgr_refresh(struct notif_mgr *mgr);
void notif_mgr_notifs_reload_timeout(const struct notif_mgr *mgr);

ssize_t notif_mgr_get_ids(const struct notif_mgr *mgr, uint32_t *ids, size_t max);

bool notif_mgr_expire_id(struct notif_mgr *mgr, uint32_t id);
bool notif_mgr_dismiss_id(struct notif_mgr *mgr, uint32_t id);
bool notif_mgr_dismiss_all(struct notif_mgr *mgr);

struct monitor;
void notif_mgr_monitor_removed(struct notif_mgr *mgr, const struct monitor *mon);
bool notif_mgr_monitor_updated(struct notif_mgr *mgr, const struct monitor *mon);

enum urgency { URGENCY_LOW, URGENCY_NORMAL, URGENCY_CRITICAL };

struct notif;
struct notif *notif_mgr_create_notif(
    struct notif_mgr *mgr, uint32_t replaces_id, const char *sync_tag);
struct notif *notif_mgr_get_notif(struct notif_mgr *mgr, uint32_t id);
struct notif *notif_mgr_get_notif_for_sync_tag(
    struct notif_mgr *mgr, const char *tag);
struct notif *notif_mgr_get_notif_for_surface(
    struct notif_mgr *mgr, const struct wl_surface *surface);
bool notif_mgr_del_notif(struct notif_mgr *mgr, uint32_t id);

void notif_destroy(struct notif *notif);
uint32_t notif_id(const struct notif *notif);
const struct monitor *notif_monitor(const struct notif *notif);
float notif_scale(const struct notif *notif);

void notif_set_application(struct notif *notif, const char *text);
void notif_set_summary(struct notif *notif, const char *text);
void notif_set_body(struct notif *notif, const char *text);
void notif_set_urgency(struct notif *notif, enum urgency urgency);
void notif_set_image(struct notif *notif, pixman_image_t *pix);
void notif_set_timeout(struct notif *notif, int timeout_ms);
void notif_set_progress(struct notif *notif, int8_t progress);
void notif_add_action(struct notif *notif, const char *id, const char *label);
void notif_play_sound(struct notif *notif);

char *notif_get_summary(const struct notif *notif);

typedef void (*notif_select_action_cb)(
    struct notif *notif, const char *action_id, void *data);

size_t notif_action_count(const struct notif *notif);
bool notif_signal_action(const struct notif *notif, const char *action_id);
void notif_select_action(
    struct notif *notif, notif_select_action_cb completion_cb, void *data);
