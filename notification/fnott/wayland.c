#include "wayland.h"

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/epoll.h>

#include <cursor-shape-v1.h>
#include <ext-idle-notify-v1.h>
#include <idle.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <wlr-layer-shell-unstable-v1.h>
#include <xdg-activation-v1.h>
#include <xdg-output-unstable-v1.h>

#include <tllist.h>

#define LOG_MODULE "wayland"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "notification.h"

#define min(x, y) ((x) < (y) ? (x) : (y))

struct idle_timer {
    struct notif_mgr *notif_mgr;
    struct org_kde_kwin_idle_timeout *kde_idle_timeout;
    struct ext_idle_notification_v1 *idle_notification;
    enum urgency urgency;
    struct seat *seat;
};

struct seat {
    struct wayland *wayl;
    struct wl_seat *wl_seat;
    uint32_t wl_name;
    char *name;
    /* One for each urgency level */
    struct idle_timer idle_timer[3];
    bool is_idle[3];

    struct wl_pointer *wl_pointer;
    struct {
        uint32_t serial;

        /* Current location */
        int x;
        int y;
        struct wl_surface *on_surface;

        /* Server side */
        struct wp_cursor_shape_device_v1 *shape_device;

        /* Client side */
        struct wl_surface *surface;
        struct wl_cursor_theme *theme;
        struct wl_cursor *cursor;

        /* Cursor theme info */
        float scale;
    } pointer;
};

struct wayland {
    const struct config *conf;
    struct fdm *fdm;
    struct org_kde_kwin_idle *kde_idle_manager;
    struct ext_idle_notifier_v1 *idle_notifier;
    struct notif_mgr *notif_mgr;
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct zxdg_output_manager_v1 *xdg_output_manager;
    struct wl_shm *shm;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct wp_viewporter *viewporter;
    struct wp_fractional_scale_manager_v1 *fractional_scale_manager;
    struct wp_cursor_shape_manager_v1 *cursor_shape_manager;
    struct xdg_activation_v1 *xdg_activation;

    bool have_argb8888;

    tll(struct seat) seats;
    tll(struct monitor) monitors;
    const struct monitor *monitor;
};

static void
seat_destroy(struct seat *seat)
{
    if (seat == NULL)
        return;

    for (int i = 0; i < 3; i++) {
        if (seat->idle_timer[i].kde_idle_timeout != NULL)
            org_kde_kwin_idle_timeout_release(seat->idle_timer[i].kde_idle_timeout);
        if (seat->idle_timer[i].idle_notification != NULL)
            ext_idle_notification_v1_destroy(seat->idle_timer[i].idle_notification);
    }

    if (seat->pointer.shape_device != NULL)
        wp_cursor_shape_device_v1_destroy(seat->pointer.shape_device);
    if (seat->pointer.theme != NULL)
        wl_cursor_theme_destroy(seat->pointer.theme);
    if (seat->pointer.surface != NULL)
        wl_surface_destroy(seat->pointer.surface);
    if (seat->wl_pointer != NULL)
        wl_pointer_release(seat->wl_pointer);
    if (seat->wl_seat != NULL)
        wl_seat_release(seat->wl_seat);

    free(seat->name);
}

static void
update_cursor_surface(struct seat *seat)
{
    if (seat->pointer.serial == 0)
        return;

    if (seat->pointer.shape_device != NULL) {
        LOG_DBG("using server-side cursor");
        wp_cursor_shape_device_v1_set_shape(
            seat->pointer.shape_device, seat->pointer.serial,
            WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
        return;
    }

    if (seat->pointer.cursor == NULL)
        return;

    if (seat->wl_pointer == NULL)
        return;

    LOG_DBG("using client-side cursor");

    float scale = seat->pointer.scale;
    wl_surface_set_buffer_scale(seat->pointer.surface, scale);

    struct wl_cursor_image *image = seat->pointer.cursor->images[0];

    wl_surface_attach(
        seat->pointer.surface, wl_cursor_image_get_buffer(image), 0, 0);

    wl_pointer_set_cursor(
        seat->wl_pointer, seat->pointer.serial,
        seat->pointer.surface,
        roundf(image->hotspot_x / scale), roundf(image->hotspot_y / scale));

    wl_surface_damage_buffer(
        seat->pointer.surface, 0, 0, INT32_MAX, INT32_MAX);

    wl_surface_commit(seat->pointer.surface);
}

static bool
reload_cursor_theme(struct seat *seat, float new_scale)
{
    if (seat->pointer.theme != NULL && seat->pointer.scale == new_scale)
        return true;

    if (seat->pointer.theme != NULL) {
        wl_cursor_theme_destroy(seat->pointer.theme);
        seat->pointer.theme = NULL;
        seat->pointer.cursor = NULL;
    }

    if (seat->pointer.shape_device != NULL) {
        /* Server side cursors */
        return true;
    }

    /* Cursor */
    unsigned cursor_size = 24;
    const char *cursor_theme = getenv("XCURSOR_THEME");

    {
        const char *env_cursor_size = getenv("XCURSOR_SIZE");
        if (env_cursor_size != NULL) {
            unsigned size;
            if (sscanf(env_cursor_size, "%u", &size) == 1)
                cursor_size = size;
        }
    }

    /* Note: theme is (re)loaded on scale and output changes */
    LOG_INFO("cursor theme: %s, size: %u, scale: %.2f",
             cursor_theme, cursor_size, new_scale);

    struct wayland *wayl = seat->wayl;

    seat->pointer.theme = wl_cursor_theme_load(
        cursor_theme, cursor_size * new_scale, wayl->shm);

    if (seat->pointer.theme == NULL) {
        LOG_ERR("%s: failed to load cursor theme", cursor_theme);
        return false;
    }

    seat->pointer.cursor = wl_cursor_theme_get_cursor(
        seat->pointer.theme, "left_ptr");

    if (seat->pointer.cursor == NULL) {
        LOG_ERR("%s: failed to load cursor 'left_ptr'", seat->name);
        return false;
    }

    seat->pointer.scale = new_scale;
    return true;
}

static void
shm_format(void *data, struct wl_shm *wl_shm, uint32_t format)
{
    struct wayland *wayl = data;
    if (format == WL_SHM_FORMAT_ARGB8888)
        wayl->have_argb8888 = true;

}

static void
output_update_ppi(struct monitor *mon)
{
    if (mon->dim.mm.width == 0 || mon->dim.mm.height == 0)
        return;

    int x_inches = mon->dim.mm.width * 0.03937008;
    int y_inches = mon->dim.mm.height * 0.03937008;
    mon->ppi.real.x = mon->dim.px_real.width / x_inches;
    mon->ppi.real.y = mon->dim.px_real.height / y_inches;

    /* The *logical* size is affected by the transform */
    switch (mon->transform) {
    case WL_OUTPUT_TRANSFORM_90:
    case WL_OUTPUT_TRANSFORM_270:
    case WL_OUTPUT_TRANSFORM_FLIPPED_90:
    case WL_OUTPUT_TRANSFORM_FLIPPED_270: {
        int swap = x_inches;
        x_inches = y_inches;
        y_inches = swap;
        break;
    }

    case WL_OUTPUT_TRANSFORM_NORMAL:
    case WL_OUTPUT_TRANSFORM_180:
    case WL_OUTPUT_TRANSFORM_FLIPPED:
    case WL_OUTPUT_TRANSFORM_FLIPPED_180:
        break;
    }

    mon->ppi.scaled.x = mon->dim.px_scaled.width / x_inches;
    mon->ppi.scaled.y = mon->dim.px_scaled.height / y_inches;

    float px_diag = sqrt(
        pow(mon->dim.px_scaled.width, 2) +
        pow(mon->dim.px_scaled.height, 2));

    mon->dpi = px_diag / mon->inch * mon->scale;
}

static void
output_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y,
                int32_t physical_width, int32_t physical_height,
                int32_t subpixel, const char *make, const char *model,
                int32_t transform)
{
    struct monitor *mon = data;

    free(mon->make);
    free(mon->model);

    mon->dim.mm.width = physical_width;
    mon->dim.mm.height = physical_height;
    mon->inch = sqrt(pow(mon->dim.mm.width, 2) + pow(mon->dim.mm.height, 2)) * 0.03937008;
    mon->make = make != NULL ? strdup(make) : NULL;
    mon->model = model != NULL ? strdup(model) : NULL;
    mon->subpixel = subpixel;
    mon->transform = transform;

    output_update_ppi(mon);
}

static void
output_mode(void *data, struct wl_output *wl_output, uint32_t flags,
            int32_t width, int32_t height, int32_t refresh)
{
    if ((flags & WL_OUTPUT_MODE_CURRENT) == 0)
        return;

    struct monitor *mon = data;
    mon->refresh = (float)refresh / 1000;
    mon->dim.px_real.width = width;
    mon->dim.px_real.height = height;
    output_update_ppi(mon);
}

static void
output_done(void *data, struct wl_output *wl_output)
{
    struct monitor *mon = data;

    if (notif_mgr_monitor_updated(mon->wayl->notif_mgr, mon))
        notif_mgr_refresh(mon->wayl->notif_mgr);
}

static void
output_scale(void *data, struct wl_output *wl_output, int32_t factor)
{
    struct monitor *mon = data;
    mon->scale = factor;
}

static void
xdg_output_handle_logical_position(
    void *data, struct zxdg_output_v1 *xdg_output, int32_t x, int32_t y)
{
    struct monitor *mon = data;
    mon->x = x;
    mon->y = y;
}

static void
xdg_output_handle_logical_size(void *data, struct zxdg_output_v1 *xdg_output,
                               int32_t width, int32_t height)
{
    struct monitor *mon = data;
    mon->dim.px_scaled.width = width;
    mon->dim.px_scaled.height = height;
    output_update_ppi(mon);
}

static void
xdg_output_handle_done(void *data, struct zxdg_output_v1 *xdg_output)
{
    struct monitor *mon = data;
    if (notif_mgr_monitor_updated(mon->wayl->notif_mgr, mon))
        notif_mgr_refresh(mon->wayl->notif_mgr);
}

static void
xdg_output_handle_name(void *data, struct zxdg_output_v1 *xdg_output,
                       const char *name)
{
    struct monitor *mon = data;
    struct wayland *wayl = mon->wayl;

    free(mon->name);
    mon->name = name != NULL ? strdup(name) : NULL;

    if (wayl->conf->output != NULL &&
        mon->name != NULL &&
        strcmp(mon->name, wayl->conf->output) == 0)
    {
        wayl->monitor = mon;
    }
}

static void
xdg_output_handle_description(void *data, struct zxdg_output_v1 *xdg_output,
                              const char *description)
{
    struct monitor *mon = data;
    free(mon->description);
    mon->description = description != NULL ? strdup(description) : NULL;
}

static void
wl_pointer_enter(void *data, struct wl_pointer *wl_pointer,
                 uint32_t serial, struct wl_surface *surface,
                 wl_fixed_t surface_x, wl_fixed_t surface_y)
{
    struct seat *seat = data;
    const struct notif *notif = notif_mgr_get_notif_for_surface(seat->wayl->notif_mgr, surface);

    if (notif == NULL) {
        /*
         * Seen on Sway-1.5 when cursor is hovering over the area
         * where a notification is later shown, and that notification
         * is then dismissed without moving the mouse (i.e. either
         * with fnottctl, or a timeout).
         */
        return;
    }

    const float scale = notif_scale(notif);

    seat->pointer.serial = serial;
    seat->pointer.x = wl_fixed_to_int(surface_x) * scale;
    seat->pointer.y = wl_fixed_to_int(surface_y) * scale;
    seat->pointer.on_surface = surface;
    reload_cursor_theme(seat, scale);
    update_cursor_surface(seat);
}

static void
wl_pointer_leave(void *data, struct wl_pointer *wl_pointer,
                 uint32_t serial, struct wl_surface *surface)
{
    struct seat *seat = data;
    seat->pointer.serial = 0;
    seat->pointer.x = seat->pointer.y = 0;
    seat->pointer.on_surface = NULL;
}

static void
wl_pointer_motion(void *data, struct wl_pointer *wl_pointer,
                  uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y)
{
    struct seat *seat = data;

    const struct notif *notif = notif_mgr_get_notif_for_surface(
        seat->wayl->notif_mgr, seat->pointer.on_surface);

    if (notif == NULL)
        return;

    const struct monitor *mon = notif_monitor(notif);

    assert(mon != NULL);
    const int scale = mon->scale;

    seat->pointer.x = wl_fixed_to_int(surface_x) * scale;
    seat->pointer.y = wl_fixed_to_int(surface_y) * scale;
}

static void
xdg_activation_token_done(
    void *data, struct xdg_activation_token_v1 *xdg_activation_token_v1,
    const char *token)
{
    char **out = data;
    *out = token != NULL ? strdup(token) : NULL;
}


static const struct xdg_activation_token_v1_listener xdg_activation_token_listener = {
    .done = &xdg_activation_token_done,
};

char *
wayl_get_activation_token(struct wayland *wayl, struct wl_surface *surface)
{
    if (wayl->xdg_activation == NULL)
        return NULL;

    struct seat *seat = NULL;
    tll_foreach(wayl->seats, it) {
        if (it->item.pointer.on_surface == surface) {
            seat = &it->item;
            break;
        }
    }

    if (seat == NULL ||
        seat->pointer.serial == 0 ||
        seat->pointer.on_surface == NULL)
    {
        return NULL;
    }

    struct xdg_activation_token_v1 *token =
        xdg_activation_v1_get_activation_token(wayl->xdg_activation);

    if (token == NULL)
        return NULL;

    char *token_str = NULL;
    xdg_activation_token_v1_add_listener(
        token, &xdg_activation_token_listener, &token_str);

    xdg_activation_token_v1_set_serial(token, seat->pointer.serial, seat->wl_seat);
    xdg_activation_token_v1_set_surface(token, seat->pointer.on_surface);
    xdg_activation_token_v1_commit(token);
    wl_display_flush(wayl->display);

    while (token_str == NULL) {
        while (wl_display_prepare_read(wayl->display) != 0) {
            if (wl_display_dispatch_pending(wayl->display) < 0) {
                LOG_ERRNO("failed to dispatch pending Wayland events");
                goto out;
            }
        }

        if (wl_display_read_events(wayl->display) < 0) {
            LOG_ERRNO("failed to read events from the Wayland socket");
            goto out;
        }

        wl_display_dispatch_pending(wayl->display);
    }

out:
    xdg_activation_token_v1_destroy(token);

    LOG_DBG("XDG activation token: %s", token_str != NULL ? token_str : "<failed>");
    return token_str;
}

static void
wl_pointer_button(void *data, struct wl_pointer *wl_pointer,
                  uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    LOG_DBG("BUTTON: button=%x, state=%u", button, state);

    struct seat *seat = data;
    struct wayland *wayl = seat->wayl;

    seat->pointer.serial = serial;

    switch (state) {
    case WL_POINTER_BUTTON_STATE_PRESSED: {
        struct notif *notif = notif_mgr_get_notif_for_surface(
            wayl->notif_mgr, seat->pointer.on_surface);

        if (notif != NULL) {
            if (button == BTN_LEFT) {
                notif_signal_action(notif, "default");
                notif_mgr_dismiss_id(wayl->notif_mgr, notif_id(notif));
            }

            else if (button == BTN_RIGHT) {
                /* Dismiss without triggering the default action */
                notif_mgr_dismiss_id(wayl->notif_mgr, notif_id(notif));
            }
        }
        break;
    }

    case WL_POINTER_BUTTON_STATE_RELEASED:
        break;
    }
}

static void
wl_pointer_axis(void *data, struct wl_pointer *wl_pointer,
                uint32_t time, uint32_t axis, wl_fixed_t value)
{
}

static void
wl_pointer_axis_discrete(void *data, struct wl_pointer *wl_pointer,
                         uint32_t axis, int32_t discrete)
{
}

static void
wl_pointer_frame(void *data, struct wl_pointer *wl_pointer)
{
}

static void
wl_pointer_axis_source(void *data, struct wl_pointer *wl_pointer,
                       uint32_t axis_source)
{
}

static void
wl_pointer_axis_stop(void *data, struct wl_pointer *wl_pointer,
                     uint32_t time, uint32_t axis)
{
}

const struct wl_pointer_listener pointer_listener = {
    .enter = wl_pointer_enter,
    .leave = wl_pointer_leave,
    .motion = wl_pointer_motion,
    .button = wl_pointer_button,
    .axis = wl_pointer_axis,
    .frame = wl_pointer_frame,
    .axis_source = wl_pointer_axis_source,
    .axis_stop = wl_pointer_axis_stop,
    .axis_discrete = wl_pointer_axis_discrete,
};

static void
seat_capabilities(void *data, struct wl_seat *wl_seat,
                  enum wl_seat_capability caps)
{
    struct seat *seat = data;

    if (caps & WL_SEAT_CAPABILITY_POINTER) {
        if (seat->wl_pointer == NULL) {
            assert(seat->pointer.surface == NULL);
            seat->pointer.surface = wl_compositor_create_surface(
                seat->wayl->compositor);

            if (seat->pointer.surface == NULL) {
                LOG_ERR("%s: failed to create cursor surface", seat->name);
                return;
            }

            seat->wl_pointer = wl_seat_get_pointer(wl_seat);
            wl_pointer_add_listener(seat->wl_pointer, &pointer_listener, seat);

            if (seat->wayl->cursor_shape_manager != NULL) {
                seat->pointer.shape_device = wp_cursor_shape_manager_v1_get_pointer(
                    seat->wayl->cursor_shape_manager, seat->wl_pointer);
            }
        }
    } else {
        if (seat->wl_pointer != NULL) {
            if (seat->pointer.shape_device != NULL) {
                wp_cursor_shape_device_v1_destroy(seat->pointer.shape_device);
                seat->pointer.shape_device = NULL;
            }

            wl_surface_destroy(seat->pointer.surface);
            wl_pointer_release(seat->wl_pointer);

            if (seat->pointer.theme != NULL)
                wl_cursor_theme_destroy(seat->pointer.theme);

            seat->wl_pointer = NULL;
            seat->pointer.surface = NULL;
            seat->pointer.theme = NULL;
            seat->pointer.cursor = NULL;
        }
    }
}

static void
seat_name(void *data, struct wl_seat *wl_seat, const char *name)
{
    struct seat *seat = data;
    free(seat->name);
    seat->name = strdup(name);
}

static void
idle_idled(struct idle_timer *timer)
{
    LOG_DBG("idle notify for urgency level %d", timer->urgency);
    timer->seat->is_idle[timer->urgency] = true;
    notif_mgr_notifs_reload_timeout(timer->notif_mgr);
}

static void
idle_resumed(struct idle_timer *timer)
{
    LOG_DBG("resume notify for urgency level %d", timer->urgency);
    timer->seat->is_idle[timer->urgency] = false;
    notif_mgr_notifs_reload_timeout(timer->notif_mgr);
}

static void
idle_notify_idled(void *data, struct ext_idle_notification_v1 *notification)
{
    struct idle_timer *timer = data;
    assert(timer->idle_notification == notification);
    idle_idled(timer);
}

static void
idle_notify_resumed(void *data, struct ext_idle_notification_v1 *notification)
{
    struct idle_timer *timer = data;
    assert(timer->idle_notification == notification);
    idle_resumed(timer);
}

static const struct ext_idle_notification_v1_listener idle_notify_listener = {
    .idled = &idle_notify_idled,
    .resumed = &idle_notify_resumed,
};

static void
kde_idled(void *data, struct org_kde_kwin_idle_timeout *timeout)
{
    struct idle_timer *timer = data;
    assert(timer->kde_idle_timeout == timeout);
    idle_idled(timer);
}

static void
kde_resumed(void *data, struct org_kde_kwin_idle_timeout *timeout)
{
    struct idle_timer *timer = data;
    assert(timer->kde_idle_timeout == timeout);
    idle_resumed(timer);
}

static const struct org_kde_kwin_idle_timeout_listener kde_idle_listener = {
    .idle = kde_idled,
    .resumed = kde_resumed,
};

static const struct wl_shm_listener shm_listener = {
    .format = &shm_format,
};

static const struct wl_output_listener output_listener = {
    .geometry = &output_geometry,
    .mode = &output_mode,
    .done = &output_done,
    .scale = &output_scale,
};

static struct zxdg_output_v1_listener xdg_output_listener = {
    .logical_position = xdg_output_handle_logical_position,
    .logical_size = xdg_output_handle_logical_size,
    .done = xdg_output_handle_done,
    .name = xdg_output_handle_name,
    .description = xdg_output_handle_description,
};

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};


static bool
verify_iface_version(const char *iface, uint32_t version, uint32_t wanted)
{
    if (version >= wanted)
        return true;

    LOG_ERR("%s: need interface version %u, but compositor only implements %u",
            iface, wanted, version);
    return false;
}

static void
seat_register_idle(struct seat *seat)
{
    struct wayland *wayl = seat->wayl;
    const struct config *conf = wayl->conf;

    if (wayl->idle_notifier == NULL && wayl->kde_idle_manager == NULL) {
        /* No idle notification interfaces available (yet) */
        return;
    }

    for (int i = 0; i < 3; i++) {
        const struct urgency_config *urg_conf = &conf->by_urgency[i];
        if (urg_conf->idle_timeout_secs <= 0)
            continue;

        struct idle_timer *timer = &seat->idle_timer[i];

        LOG_DBG("registering a new idle timer for urgency level %d: %ds",
                i, urg_conf->idle_timeout_secs);

        timer->notif_mgr = wayl->notif_mgr;
        timer->urgency = i;
        timer->seat = seat;

        if (wayl->idle_notifier != NULL) {
            /* We prefer the newer ext-idle-notify interface */
            if (timer->kde_idle_timeout != NULL) {
                org_kde_kwin_idle_timeout_release(timer->kde_idle_timeout);
                timer->kde_idle_timeout = NULL;
            }

            assert(timer->kde_idle_timeout == NULL);
            timer->idle_notification = ext_idle_notifier_v1_get_idle_notification(
                wayl->idle_notifier, urg_conf->idle_timeout_secs * 1000, seat->wl_seat);

            ext_idle_notification_v1_add_listener(
                timer->idle_notification, &idle_notify_listener, timer);
        } else
        if (wayl->kde_idle_manager != NULL) {
            assert(timer->idle_notification == NULL);

            timer->kde_idle_timeout = org_kde_kwin_idle_get_idle_timeout(
                wayl->kde_idle_manager, seat->wl_seat,
                urg_conf->idle_timeout_secs * 1000);

            org_kde_kwin_idle_timeout_add_listener(
                timer->kde_idle_timeout, &kde_idle_listener, timer);
        }
    }
}

static void
wayl_register_idle_for_all_seats(struct wayland *wayl)
{
    assert(wayl->idle_notifier != NULL ||
           wayl->kde_idle_manager != NULL);

    tll_foreach(wayl->seats, it)
    seat_register_idle(&it->item);
}

static void
handle_global(void *data, struct wl_registry *registry,
              uint32_t name, const char *interface, uint32_t version)
{
    LOG_DBG("global: 0x%08x, interface=%s, version=%u", name, interface, version);
    struct wayland *wayl = data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        const uint32_t required = 4;
        if (!verify_iface_version(interface, version, required))
            return;

#if defined (WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION)
        const uint32_t preferred = WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION;
#else
        const uint32_t preferred = required;
#endif
        wayl->compositor = wl_registry_bind(
            wayl->registry, name, &wl_compositor_interface, min(version, preferred));
    }

    else if (strcmp(interface, wl_shm_interface.name) == 0) {
        const uint32_t required = 1;
        if (!verify_iface_version(interface, version, required))
            return;

        wayl->shm = wl_registry_bind(
            wayl->registry, name, &wl_shm_interface, required);
        wl_shm_add_listener(wayl->shm, &shm_listener, wayl);
    }

    else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        const uint32_t required = 1;
        if (!verify_iface_version(interface, version, required))
            return;

        wayl->layer_shell = wl_registry_bind(
            wayl->registry, name, &zwlr_layer_shell_v1_interface, required);
    }

    else if (strcmp(interface, wl_output_interface.name) == 0) {
        const uint32_t required = 3;
        if (!verify_iface_version(interface, version, required))
            return;

        struct wl_output *output = wl_registry_bind(
            wayl->registry, name, &wl_output_interface, required);

        tll_push_back(wayl->monitors, ((struct monitor){
            .wayl = wayl, .output = output,
            .wl_name = name,}
        ));

        struct monitor *mon = &tll_back(wayl->monitors);
        wl_output_add_listener(output, &output_listener, mon);

        /*
         * The "output" interface doesn't give us the monitors'
         * identifiers (e.g. "LVDS-1"). Use the XDG output interface
         * for that.
         */

        assert(wayl->xdg_output_manager != NULL);
        if (wayl->xdg_output_manager != NULL) {
            mon->xdg = zxdg_output_manager_v1_get_xdg_output(
                wayl->xdg_output_manager, mon->output);

            zxdg_output_v1_add_listener(mon->xdg, &xdg_output_listener, mon);
        }
    }

    else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
        const uint32_t required = 2;
        if (!verify_iface_version(interface, version, required))
            return;

        wayl->xdg_output_manager = wl_registry_bind(
            wayl->registry, name, &zxdg_output_manager_v1_interface, required);
    }

    else if (strcmp(interface, wl_seat_interface.name) == 0) {
        const uint32_t required = 4;
        if (!verify_iface_version(interface, version, required))
            return;

        struct wl_seat *wl_seat = wl_registry_bind(
            wayl->registry, name, &wl_seat_interface, required);

        tll_push_back(wayl->seats, ((struct seat){
                    .wayl = wayl,
                    .wl_seat = wl_seat,
                    .wl_name = name}));

        struct seat *seat = &tll_back(wayl->seats);
        wl_seat_add_listener(wl_seat, &seat_listener, seat);
        seat_register_idle(seat);
    }

    else if (strcmp(interface, org_kde_kwin_idle_interface.name) == 0) {
        const uint32_t required = 1;
        if (!verify_iface_version(interface, version, required))
            return;

        wayl->kde_idle_manager = wl_registry_bind(
            wayl->registry, name, &org_kde_kwin_idle_interface, required);
        wayl_register_idle_for_all_seats(wayl);
    }

    else if (strcmp(interface, ext_idle_notifier_v1_interface.name) == 0) {
        const uint32_t required = 1;
        if (!verify_iface_version(interface, version, required))
            return;

        wayl->idle_notifier = wl_registry_bind(
            wayl->registry, name, &ext_idle_notifier_v1_interface, required);
        wayl_register_idle_for_all_seats(wayl);
    }

    else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
        const uint32_t required = 1;
        if (!verify_iface_version(interface, version, required))
            return;

        wayl->viewporter = wl_registry_bind(
            wayl->registry, name, &wp_viewporter_interface, required);
    }

    else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
        const uint32_t required = 1;
        if (!verify_iface_version(interface, version, required))
            return;

        wayl->fractional_scale_manager = wl_registry_bind(
            wayl->registry, name,
            &wp_fractional_scale_manager_v1_interface, required);
    }

    else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
        const uint32_t required = 1;
        if (!verify_iface_version(interface, version, required))
            return;

        wayl->cursor_shape_manager = wl_registry_bind(
            wayl->registry, name, &wp_cursor_shape_manager_v1_interface, required);
    }

    else if (strcmp(interface, xdg_activation_v1_interface.name) == 0) {
        const uint32_t required = 1;
        if (!verify_iface_version(interface, version, required))
            return;

        wayl->xdg_activation = wl_registry_bind(
            wayl->registry, name, &xdg_activation_v1_interface, required);
    }
}

static void
monitor_destroy(struct monitor *mon)
{
    free(mon->name);
    if (mon->xdg != NULL)
        zxdg_output_v1_destroy(mon->xdg);
    if (mon->output != NULL)
        wl_output_release(mon->output);
    free(mon->make);
    free(mon->model);
    free(mon->description);
}

static void
handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    LOG_DBG("global removed: 0x%08x", name);

    struct wayland *wayl = data;

    tll_foreach(wayl->monitors, it) {
        struct monitor *mon = &it->item;

        if (mon->wl_name != name)
            continue;

        LOG_INFO("monitor disabled: %s", mon->name);

        if (wayl->monitor == mon)
            wayl->monitor = NULL;

        notif_mgr_monitor_removed(wayl->notif_mgr, mon);

        monitor_destroy(mon);
        tll_remove(wayl->monitors, it);
        return;
    }

    tll_foreach(wayl->seats, it) {
        struct seat *seat = &it->item;

        if (seat->wl_name != name)
            continue;

        LOG_INFO("seat removed: %s", seat->name);
        seat_destroy(seat);
        tll_remove(wayl->seats, it);
    }

    LOG_WARN("unknown global removed: 0x%08x", name);
}

static const struct wl_registry_listener registry_listener = {
    .global = &handle_global,
    .global_remove = &handle_global_remove,
};

static bool
fdm_handler(struct fdm *fdm, int fd, int events, void *data)
{
    struct wayland *wayl = data;
    int event_count = 0;

    if (events & EPOLLIN) {
        if (wl_display_read_events(wayl->display) < 0) {
            LOG_ERRNO("failed to read events from the Wayland socket");
            return false;
        }

        wl_display_dispatch_pending(wayl->display);

        while (wl_display_prepare_read(wayl->display) != 0) {
            if (wl_display_dispatch_pending(wayl->display)  < 0) {
                LOG_ERRNO("failed to dispatch pending Wayland events");
                return false;
            }
        }
    }

    if (events & EPOLLHUP) {
        LOG_WARN("disconnected from Wayland");
        // wl_display_cancel_read(wayl->display);
        return false;
    }

    wl_display_flush(wayl->display);
    return event_count != -1;
}

struct wayland *
wayl_init(const struct config *conf, struct fdm *fdm, struct notif_mgr *notif_mgr)
{
    struct wayland *wayl = calloc(1, sizeof(*wayl));
    wayl->conf = conf;
    wayl->notif_mgr = notif_mgr;

    wayl->display = wl_display_connect(NULL);
    if (wayl->display == NULL) {
        LOG_ERR("failed to connect to wayland; no compositor running?");
        goto err;
    }

    wayl->registry = wl_display_get_registry(wayl->display);
    if (wayl->registry == NULL) {
        LOG_ERR("failed to get wayland registry");
        goto err;
    }

    wl_registry_add_listener(wayl->registry, &registry_listener, wayl);
    wl_display_roundtrip(wayl->display);

    if (wayl->compositor == NULL) {
        LOG_ERR("no compositor");
        goto err;
    }
    if (wayl->shm == NULL) {
        LOG_ERR("no shared memory buffers interface");
        goto err;
    }
    if (wayl->layer_shell == NULL) {
        LOG_ERR("compositor does not support layer shells");
        goto err;
    }

    if ((wayl->idle_notifier == NULL && wayl->kde_idle_manager == NULL) &&
        (conf->by_urgency[0].idle_timeout_secs > 0 ||
         conf->by_urgency[1].idle_timeout_secs > 0 ||
         conf->by_urgency[2].idle_timeout_secs > 0))
    {
        LOG_WARN("compositor does not support idle protocol, ignoring 'idle-timeout' setting");
    }

    wl_display_roundtrip(wayl->display);

    if (!wayl->have_argb8888) {
        LOG_ERR("compositor does not support ARGB surfaces");
        goto err;
    }

    if (tll_length(wayl->monitors) == 0) {
        LOG_ERR("no outputs found");
        goto err;
    }

    tll_foreach(wayl->monitors, it) {
        const struct monitor *mon = &it->item;
        LOG_INFO(
            "%s: %dx%d+%dx%d@%dHz %s %.2f\" scale=%d PPI=%dx%d (physical) PPI=%dx%d (logical), DPI=%.2f",
            mon->name, mon->dim.px_real.width, mon->dim.px_real.height,
            mon->x, mon->y, (int)round(mon->refresh),
            mon->model != NULL ? mon->model : mon->description,
            mon->inch, mon->scale,
            mon->ppi.real.x, mon->ppi.real.y,
            mon->ppi.scaled.x, mon->ppi.scaled.y, mon->dpi);
    }

    if (wl_display_prepare_read(wayl->display) != 0) {
        LOG_ERRNO("failed to prepare for reading wayland events");
        goto err;
    }

    if (!fdm_add(fdm, wl_display_get_fd(wayl->display), EPOLLIN, &fdm_handler, wayl)) {
        LOG_ERR("failed to register with FDM");
        goto err;
    }
    wayl->fdm = fdm;
    return wayl;

err:
    wayl_destroy(wayl);
    return NULL;
}

void
wayl_destroy(struct wayland *wayl)
{
    if (wayl == NULL)
        return;

    if (wayl->fdm != NULL)
        fdm_del_no_close(wayl->fdm, wl_display_get_fd(wayl->display));

    tll_foreach(wayl->monitors, it)
        monitor_destroy(&it->item);
    tll_free(wayl->monitors);

    tll_foreach(wayl->seats, it)
        seat_destroy(&it->item);
    tll_free(wayl->seats);

    if (wayl->xdg_activation != NULL)
        xdg_activation_v1_destroy(wayl->xdg_activation);
    if (wayl->cursor_shape_manager != NULL)
        wp_cursor_shape_manager_v1_destroy(wayl->cursor_shape_manager);
    if (wayl->fractional_scale_manager != NULL)
        wp_fractional_scale_manager_v1_destroy(wayl->fractional_scale_manager);
    if (wayl->viewporter != NULL)
        wp_viewporter_destroy(wayl->viewporter);
    if (wayl->idle_notifier != NULL)
        ext_idle_notifier_v1_destroy(wayl->idle_notifier);
    if (wayl->kde_idle_manager != NULL)
        org_kde_kwin_idle_destroy(wayl->kde_idle_manager);
    if (wayl->layer_shell != NULL)
        zwlr_layer_shell_v1_destroy(wayl->layer_shell);
    if (wayl->xdg_output_manager != NULL)
        zxdg_output_manager_v1_destroy(wayl->xdg_output_manager);
    if (wayl->shm != NULL)
        wl_shm_destroy(wayl->shm);
    if (wayl->compositor != NULL)
        wl_compositor_destroy(wayl->compositor);
    if (wayl->registry != NULL)
        wl_registry_destroy(wayl->registry);
    if (wayl->display != NULL) {
        wayl_flush(wayl);
        wl_display_disconnect(wayl->display);
    }

    free(wayl);
}

bool
wayl_is_idle_for_urgency(const struct wayland *wayl, const enum urgency urgency)
{
    bool idle = true;
    assert(urgency >= 0 && urgency < 3);
    tll_foreach(wayl->seats, it) {
        struct seat *seat = &it->item;
        idle &= seat->is_idle[urgency];
    }

    return idle;
}

struct wl_compositor *
wayl_compositor(const struct wayland *wayl)
{
    return wayl->compositor;
}

struct zwlr_layer_shell_v1 *
wayl_layer_shell(const struct wayland *wayl)
{
    return wayl->layer_shell;
}

struct wp_fractional_scale_manager_v1 *
wayl_fractional_scale_manager(const struct wayland *wayl)
{
    return wayl->fractional_scale_manager;
}

struct wp_viewporter *
wayl_viewporter(const struct wayland *wayl)
{
    return wayl->viewporter;
}

struct buffer *
wayl_get_buffer(const struct wayland *wayl, int width, int height)
{
    return shm_get_buffer(wayl->shm, width, height);
}

const struct monitor *
wayl_preferred_monitor(const struct wayland *wayl)
{
    return wayl->monitor;
}

const struct monitor *
wayl_monitor_get(const struct wayland *wayl, struct wl_output *output)
{
    tll_foreach(wayl->monitors, it) {
        if (it->item.output == output)
            return &it->item;
    }

    return NULL;
}

float
wayl_guess_scale(const struct wayland *wayl)
{
    if (wayl->monitor != NULL)
        return wayl->monitor->scale;

    if (tll_length(wayl->monitors) == 0)
        return 1;

    bool all_have_same_scale = true;
    float last_scale = -1.;

    tll_foreach(wayl->monitors, it) {
        if (last_scale == -1.)
            last_scale = it->item.scale;
        else if (last_scale != it->item.scale) {
            all_have_same_scale = false;
            break;
        }
    }

    if (all_have_same_scale) {
        assert(last_scale >= 1);
        return last_scale;
    }

    return 1;
}

bool
wayl_all_monitors_have_scale_one(const struct wayland *wayl)
{
    tll_foreach(wayl->monitors, it) {
        if (it->item.scale > 1)
            return false;
    }

    return true;
}

enum fcft_subpixel
wayl_guess_subpixel(const struct wayland *wayl)
{
    if (wayl->monitor != NULL)
        return (enum fcft_subpixel)wayl->monitor->subpixel;

    if (tll_length(wayl->monitors) == 0)
        return FCFT_SUBPIXEL_DEFAULT;

    return (enum fcft_subpixel)tll_front(wayl->monitors).subpixel;
}

float
wayl_dpi_guess(const struct wayland *wayl)
{
    const struct monitor *mon = NULL;

    if (wayl->monitor != NULL)
        mon = wayl->monitor;
    else if (tll_length(wayl->monitors) > 0)
        mon = &tll_front(wayl->monitors);

    if (mon != NULL && mon->dpi > 0)
        return mon->dpi;

    return 96.;
}

int
wayl_poll_fd(const struct wayland *wayl)
{
    return wl_display_get_fd(wayl->display);
}

void
wayl_flush(struct wayland *wayl)
{
    while (true) {
        int r = wl_display_flush(wayl->display);
        if (r >= 0) {
            /* Most likely code path - the flush succeed */
            return;
        }

        if (errno == EINTR) {
            /* Unlikely */
            continue;
        }

        if (errno != EAGAIN) {
            const int saved_errno = errno;

            if (errno == EPIPE) {
                wl_display_read_events(wayl->display);
                wl_display_dispatch_pending(wayl->display);
            }

            LOG_ERRNO_P("failed to flush wayland socket", saved_errno);
            return;
        }

        /* Socket buffer is full - need to wait for it to become
           writeable again */
        assert(errno == EAGAIN);

        while (true) {
            const int wayl_fd = wl_display_get_fd(wayl->display);
            struct pollfd fds[] = {{.fd = wayl_fd, .events = POLLOUT}};

            r = poll(fds, sizeof(fds) / sizeof(fds[0]), -1);

            if (r < 0) {
                if (errno == EINTR)
                    continue;

                LOG_ERRNO("failed to poll");
                return;
            }

            if (fds[0].revents & POLLHUP)
                return;

            assert(fds[0].revents & POLLOUT);
            break;
        }
    }
}

void
wayl_roundtrip(struct wayland *wayl)
{
    wl_display_cancel_read(wayl->display);
    if (wl_display_roundtrip(wayl->display) < 0) {
        LOG_ERRNO("failed to roundtrip Wayland display");
        return;
    }

    wl_display_dispatch_pending(wayl->display);

    while (wl_display_prepare_read(wayl->display) != 0) {
        if (wl_display_dispatch_pending(wayl->display) < 0) {
            LOG_ERRNO("failed to dispatch pending Wayland events");
            return;
        }
    }
    wl_display_flush(wayl->display);
}
