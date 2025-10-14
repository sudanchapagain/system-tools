#include "notification.h"
#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <regex.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <fcntl.h>

#include <pixman.h>
#include <wayland-client.h>
#include <wlr-layer-shell-unstable-v1.h>

#define LOG_MODULE "notification"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "char32.h"
#include "dbus.h"
#include "icon.h"
#include "spawn.h"
#include "wayland.h"

#define max(x, y) ((x) > (y) ? (x) : (y))
#define min(x, y) ((x) < (y) ? (x) : (y))

#ifndef CLOCK_BOOTTIME
#ifdef CLOCK_UPTIME
/* DragonFly and FreeBSD */
#define CLOCK_BOOTTIME CLOCK_UPTIME
#else
#define CLOCK_BOOTTIME CLOCK_MONOTONIC
#endif
#endif

struct notif_mgr;

struct font_set {
    struct fcft_font *regular;
    struct fcft_font *bold;
    struct fcft_font *italic;
    struct fcft_font *bold_italic;
};

struct action {
    char *id;
    char *label;
    char32_t *wid;
    char32_t *wlabel;
};

struct text_run_cache {
    struct fcft_text_run *run;
    const struct fcft_font *font;
    uint64_t hash;
    enum fcft_subpixel subpixel;
    size_t ofs;
};

struct notif {
    struct notif_mgr *mgr;

    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wp_fractional_scale_v1 *fractional_scale;
    struct wp_viewport *viewport;
    bool is_configured;

    uint32_t id;
    char *synchronous_tag;  /* x-canonical-private-synchronous */

    char32_t *app;
    char32_t *summary;
    char32_t *body;
    enum urgency urgency;
    tll(struct action) actions;

    int8_t progress;
    int timeout_ms;  /* Timeout provided by the notification itself */
    int timeout_fd;
    enum {DISMISS_IMMEDIATELY, DISMISS_DEFER, DISMISS_DELAYED} deferred_dismissal;
    enum {EXPIRE_IMMEDIATELY, EXPIRE_DEFER, EXPIRE_DELAYED} deferred_expiral;

    struct {
        float dpi;
        bool dpi_aware;
        enum urgency urgency;
        struct font_set app;
        struct font_set summary;
        struct font_set body;
        struct font_set action;
    } fonts;

    pixman_image_t *pix;
    int image_width;
    int image_height;
    bool image_is_custom;
    int preferred_buffer_scale;
    float preferred_fractional_scale;
    float scale;
    enum fcft_subpixel subpixel;

    struct buffer *pending;
    struct wl_callback *frame_callback;

    int y;

    const struct monitor *mon;

    tll(struct text_run_cache) text_run_cache;
};

struct notif_mgr {
    struct config *conf;
    struct fdm *fdm;
    struct wayland *wayl;
    struct dbus *bus;
    const icon_theme_list_t *icon_theme;
    regex_t html_entity_re;

    tll(struct notif *) notifs;

    bool paused;
};

static size_t next_id = 1;

struct notif_mgr *
notif_mgr_new(struct config *conf, struct fdm *fdm,
              const icon_theme_list_t *icon_theme)
{
    struct notif_mgr *mgr = malloc(sizeof(*mgr));
    *mgr = (struct notif_mgr) {
        .conf = conf,
        .fdm = fdm,
        .wayl = NULL,   /* notif_mgr_configure() */
        .bus = NULL,    /* notif_mgr_configure() */
        .icon_theme = icon_theme,
        .notifs = tll_init(),
        .paused = false,
    };

    int r = regcomp(
        &mgr->html_entity_re,
        /* Entity names (there's a *lot* of these - we only support the common ones */
        "&\\(nbsp\\|lt\\|gt\\|amp\\|quot\\|apos\\|cent\\|pound\\|yen\\|euro\\|copy\\|reg\\);\\|"

        /* Decimal entity number: &#39; */
        "&#\\([0-9]\\+\\);\\|"

        /* Hexadecimal entity number: &#x27; */
        "&#x\\([0-9a-fA-F]\\+\\);" , 0);

    if (r != 0) {
        char err[1024];
        regerror(r, &mgr->html_entity_re, err, sizeof(err));

        LOG_ERR("failed to compile HTML entity regex: %s (%d)", err, r);
        regfree(&mgr->html_entity_re);
        free(mgr);
        return NULL;
    }

    return mgr;
}

void
notif_mgr_destroy(struct notif_mgr *mgr)
{
    if (mgr == NULL)
        return;

    regfree(&mgr->html_entity_re);

    notif_mgr_dismiss_all(mgr);

    tll_foreach(mgr->notifs, it)
        notif_destroy(it->item);
    tll_free(mgr->notifs);
    free(mgr);
}

void
notif_mgr_configure(struct notif_mgr *mgr, struct wayland *wayl, struct dbus *bus)
{
    assert(mgr->wayl == NULL);
    assert(mgr->bus == NULL);

    mgr->wayl = wayl;
    mgr->bus = bus;
}

bool
notif_mgr_is_paused(struct notif_mgr *mgr)
{
    return mgr->paused;
}

void
notif_mgr_pause(struct notif_mgr *mgr)
{
    LOG_INFO("pausing");
    mgr->paused = true;
}

void
notif_mgr_unpause(struct notif_mgr *mgr)
{
    LOG_INFO("unpausing");
    mgr->paused = false;
}

static bool notif_reload_default_icon(struct notif *notif);
static bool notif_reload_fonts(struct notif *notif);
static bool notif_reload_timeout(struct notif *notif);
static int notif_show(struct notif *notif, int y);

static void
surface_enter(void *data, struct wl_surface *wl_surface,
              struct wl_output *wl_output)
{
    struct notif *notif = data;

    const struct monitor *mon = wayl_monitor_get(notif->mgr->wayl, wl_output);
    if (notif->mon == mon)
        return;

    notif->mon = mon;
    notif->subpixel = mon != NULL
        ? (enum fcft_subpixel)mon->subpixel
        : wayl_guess_subpixel(notif->mgr->wayl);

    if (notif_reload_fonts(notif))
        notif_show(notif, notif->y);
}

static void
surface_leave(void *data, struct wl_surface *wl_surface,
              struct wl_output *wl_output)
{
    struct notif *notif = data;
    notif->mon = NULL;
}

#if defined(WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION)
static void
surface_preferred_buffer_scale(void *data, struct wl_surface *surface,
                               int32_t scale)
{
    struct notif *notif = data;

    if (notif->preferred_buffer_scale == scale)
        return;

    LOG_DBG("wl_surface preferred scale: %d -> %d", notif->preferred_buffer_scale, scale);

    notif->preferred_buffer_scale = scale;
    if (notif_mgr_monitor_updated(notif->mgr, NULL))
        notif_mgr_refresh(notif->mgr);
}

static void
surface_preferred_buffer_transform(void *data, struct wl_surface *surface,
                                   uint32_t transform)
{
}
#endif

static const struct wl_surface_listener surface_listener = {
    .enter = &surface_enter,
    .leave = &surface_leave,
#if defined(WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION)
    .preferred_buffer_scale = &surface_preferred_buffer_scale,
    .preferred_buffer_transform = &surface_preferred_buffer_transform,
#endif
};

static void frame_callback(
    void *data, struct wl_callback *wl_callback, uint32_t callback_data);

static const struct wl_callback_listener frame_listener = {
    .done = &frame_callback,
};

static void
commit_buffer(struct notif *notif, struct buffer *buf)
{
    struct wayland *wayl = notif->mgr->wayl;

    assert(notif->scale >= 1);
    assert(buf->busy);

    if (notif->preferred_fractional_scale > 0) {
        assert(notif->viewport != NULL);
        assert(notif->fractional_scale != NULL);

        LOG_DBG("scaling by a factor of %.2f using fractional scaling "
                "(width=%d, height=%d)", notif->scale, buf->width, buf->height);

        wl_surface_set_buffer_scale(notif->surface, 1);
        wp_viewport_set_destination(
            notif->viewport, roundf(buf->width / notif->scale), roundf(buf->height / notif->scale));
    }

    else {
        LOG_DBG("scaling by a factor of %.2f using %s (width=%d, height=%d)",
                notif->scale,
                notif->preferred_buffer_scale > 0
                    ? "wl_surface.preferred_buffer_scale"
                    : "legacy mode",
                buf->width, buf->height);

        wl_surface_set_buffer_scale(notif->surface, notif->scale);
    }

    wl_surface_attach(notif->surface, buf->wl_buf, 0, 0);
    wl_surface_damage_buffer(notif->surface, 0, 0, buf->width, buf->height);

    assert(notif->frame_callback == NULL);
    notif->frame_callback = wl_surface_frame(notif->surface);
    wl_callback_add_listener(notif->frame_callback, &frame_listener, notif);

    wl_surface_commit(notif->surface);
    wayl_flush(wayl);
}

static void
layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                        uint32_t serial, uint32_t w, uint32_t h)
{
    LOG_DBG("configure: width=%u, height=%u", w, h);
    struct notif *notif = data;
    notif->is_configured = true;
    zwlr_layer_surface_v1_ack_configure(surface, serial);

    if (notif->pending != NULL && notif->frame_callback == NULL) {
        commit_buffer(notif, notif->pending);
        notif->pending = NULL;
    } else {
        /* ack *must* be followed by a commit */
        notif_show(notif, notif->y);
    }
}

static void notif_destroy_surfaces(struct notif *notif);

static void
layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    struct notif *notif = data;
    notif_destroy_surfaces(notif);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = &layer_surface_configure,
    .closed = &layer_surface_closed,
};

static void
fractional_scale_preferred_scale(
    void *data, struct wp_fractional_scale_v1 *wp_fractional_scale_v1,
    uint32_t scale)
{
    struct notif *notif = data;

    const float new_scale = (float)scale / 120.;

    if (notif->preferred_fractional_scale == new_scale)
        return;

    LOG_DBG("fractional scale: %.2f -> %.2f",
            notif->preferred_fractional_scale, new_scale);

    notif->preferred_fractional_scale = new_scale;
    if (notif_mgr_monitor_updated(notif->mgr, NULL))
        notif_mgr_refresh(notif->mgr);
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = &fractional_scale_preferred_scale,
};

struct notif *
notif_mgr_get_notif(struct notif_mgr *mgr, uint32_t id)
{
    if (id == 0 && tll_length(mgr->notifs) > 0)
        return tll_front(mgr->notifs);

    tll_foreach(mgr->notifs, it) {
        if (it->item->id == id)
            return it->item;
    }

    return NULL;
}

struct notif *
notif_mgr_get_notif_for_sync_tag(struct notif_mgr *mgr, const char *tag)
{
    if (tag == NULL)
        return NULL;

    tll_foreach(mgr->notifs, it) {
        if (it->item->synchronous_tag == NULL)
            continue;

        if (strcmp(it->item->synchronous_tag, tag) == 0)
            return it->item;
    }

    return NULL;
}

struct notif *
notif_mgr_get_notif_for_surface(struct notif_mgr *mgr,
                                const struct wl_surface *surface)
{
    tll_foreach(mgr->notifs, it) {
        if (it->item->surface == surface)
            return it->item;
    }

    return NULL;
}

/* Instantiates a new notification. You *must* call
 * notif_mgr_refresh() "soon" (after configuring the notification). */
struct notif *
notif_mgr_create_notif(struct notif_mgr *mgr, uint32_t replaces_id,
                       const char *sync_tag)
{
    int notif_id;

    {
        struct notif *old_notif = notif_mgr_get_notif_for_sync_tag(mgr, sync_tag);
        if (old_notif != NULL)
            return old_notif;
    }

    if (replaces_id != 0) {
        struct notif *old_notif = notif_mgr_get_notif(mgr, replaces_id);
        if (old_notif != NULL)
            return old_notif;

        notif_id = replaces_id;
    } else
        notif_id = next_id++;

    struct notif *notif = malloc(sizeof(*notif));
    *notif = (struct notif) {
        .mgr = mgr,
        .id = notif_id,
        .synchronous_tag = sync_tag != NULL ? strdup(sync_tag) : NULL,
        .app = c32dup(U""),
        .summary = c32dup(U""),
        .body = c32dup(U""),
        .urgency = URGENCY_NORMAL,
        .actions = tll_init(),
        .timeout_ms = -1,  /* -1, up to us, 0 - never expire */
        .timeout_fd = -1,
        .deferred_dismissal = DISMISS_IMMEDIATELY,
        .deferred_expiral = EXPIRE_IMMEDIATELY,
    };

    notif_reload_default_icon(notif);
    notif_reload_fonts(notif);
    notif_reload_timeout(notif);

    tll_rforeach(mgr->notifs, it) {
        if (it->item->urgency >= notif->urgency) {
            tll_insert_after(mgr->notifs, it, notif);
            return notif;
        }
    }

    tll_push_front(mgr->notifs, notif);
    return notif;
}

bool
notif_mgr_del_notif(struct notif_mgr *mgr, uint32_t id)
{
    if (id == 0)
        return false;

    tll_foreach(mgr->notifs, it) {
        if (it->item->id != id)
            continue;

        notif_destroy(it->item);
        tll_remove(mgr->notifs, it);
        return true;
    }

    return false;
}

static void
notif_destroy_surfaces(struct notif *notif)
{
    if (notif->frame_callback != NULL)
        wl_callback_destroy(notif->frame_callback);

    if (notif->fractional_scale != NULL)
        wp_fractional_scale_v1_destroy(notif->fractional_scale);
    if (notif->viewport != NULL)
        wp_viewport_destroy(notif->viewport);
    if (notif->layer_surface != NULL)
        zwlr_layer_surface_v1_destroy(notif->layer_surface);
    if (notif->surface != NULL)
        wl_surface_destroy(notif->surface);

    notif->is_configured = false;
    notif->surface = NULL;
    notif->layer_surface = NULL;
    notif->frame_callback = NULL;
    notif->mon = NULL;
    notif->scale = 0;
    notif->fonts.dpi = 0;
    notif->subpixel = FCFT_SUBPIXEL_DEFAULT;
}

void
notif_destroy(struct notif *notif)
{
    if (notif == NULL)
        return;

    notif_destroy_surfaces(notif);

    fdm_del(notif->mgr->fdm, notif->timeout_fd);

    if (notif->pix != NULL) {
        free(pixman_image_get_data(notif->pix));
        pixman_image_unref(notif->pix);
    }

    tll_foreach(notif->actions, it) {
        free(it->item.id);
        free(it->item.wid);
        free(it->item.label);
        free(it->item.wlabel);
        tll_remove(notif->actions, it);
    }

    tll_foreach(notif->text_run_cache, it) {
        fcft_text_run_destroy(it->item.run);
        tll_remove(notif->text_run_cache, it);
    }

    free(notif->synchronous_tag);
    free(notif->app);
    free(notif->summary);
    free(notif->body);
    free(notif);
}

static float
get_dpi(const struct notif *notif)
{
    if (notif->mon != NULL)
        return notif->mon->dpi > 0 ? notif->mon->dpi : 96.;
    else
        return wayl_dpi_guess(notif->mgr->wayl);
}

static float
get_scale(const struct notif *notif)
{
    if (notif->preferred_fractional_scale > 0.)
        return notif->preferred_fractional_scale;
    else if (notif->preferred_buffer_scale > 0)
        return notif->preferred_buffer_scale;
    else if (notif->mon != NULL)
        return notif->mon->scale;
    else
        return wayl_guess_scale(notif->mgr->wayl);
}

static void
font_set_destroy(struct font_set *set)
{
    fcft_destroy(set->regular);
    fcft_destroy(set->bold);
    fcft_destroy(set->italic);
    fcft_destroy(set->bold_italic);

    set->regular = set->bold = set->italic = set->bold_italic = NULL;
}

static bool
reload_one_font_set(const struct config_font *font,
                    struct font_set *set,
                    bool dpi_aware, float scale, float dpi)
{

    scale = dpi_aware ? 1 : scale;
    dpi = dpi_aware ? dpi : 96.;

    char size[64];
    if (font->px_size > 0) {
        snprintf(size, sizeof(size), "pixelsize=%d",
                 (int)roundf(font->px_size * scale));
    } else {
        snprintf(size, sizeof(size), "size=%.2f",
                 font->pt_size * (double)scale);
    }

    char attrs0[256], attrs1[256], attrs2[256], attrs3[256];
    snprintf(attrs0, sizeof(attrs0), "dpi=%.2f:%s", dpi, size);
    snprintf(attrs1, sizeof(attrs1), "dpi=%.2f:weight=bold:%s", dpi, size);
    snprintf(attrs2, sizeof(attrs2), "dpi=%.2f:slant=italic:%s", dpi, size);
    snprintf(attrs3, sizeof(attrs3), "dpi=%.2f:weight=bold:slant=italic:%s", dpi, size);

    const char *names[1] = {font->pattern};

    struct fcft_font *regular = fcft_from_name(1, names, attrs0);
    if (regular == NULL) {
        LOG_ERR("%s: failed to load font", font->pattern);
        return false;
    }

    struct fcft_font *bold = fcft_from_name(1, names, attrs1);
    struct fcft_font *italic = fcft_from_name(1, names, attrs2);
    struct fcft_font *bold_italic = fcft_from_name(1, names, attrs3);

    set->regular = regular;
    set->bold = bold;
    set->italic = italic;
    set->bold_italic = bold_italic;
    return true;
}

static bool
notif_reload_fonts(struct notif *notif)
{
    const float old_dpi = notif->fonts.dpi;
    const float new_dpi = get_dpi(notif);

    const float old_scale = notif->scale;
    const float new_scale = get_scale(notif);

    const enum urgency old_urgency = notif->fonts.urgency;
    const enum urgency new_urgency = notif->urgency;

    const bool was_dpi_aware = notif->fonts.dpi_aware;
    const bool is_dpi_aware = notif->mgr->conf->dpi_aware;

    notif->scale = new_scale;
    notif->fonts.dpi = new_dpi;
    notif->fonts.dpi_aware = is_dpi_aware;
    notif->fonts.urgency = notif->urgency;

    /* Skip font reload if none of the parameters affecting font
     * rendering has changed */
    if (notif->fonts.app.regular != NULL &&
        was_dpi_aware == is_dpi_aware &&
        (is_dpi_aware
         ? old_dpi == new_dpi
         : old_scale == new_scale) &&
        old_urgency == new_urgency)
    {
        LOG_DBG("skipping font reloading (DPI-aware: %d/%d, DPI: %.2f/%.2f, "
                "scale: %.2f/%.2f, urgency: %d/%d)",
                was_dpi_aware, is_dpi_aware,
                old_dpi, new_dpi,
                old_scale, new_scale,
                old_urgency, new_urgency);
        return false;
    }

    const struct urgency_config *urgency
        = &notif->mgr->conf->by_urgency[notif->urgency];

    struct font_set app;
    if (reload_one_font_set(
            &urgency->app.font, &app, is_dpi_aware, new_scale, new_dpi))
    {
        font_set_destroy(&notif->fonts.app);
        notif->fonts.app = app;
    }

    struct font_set summary;
    if (reload_one_font_set(
            &urgency->summary.font, &summary, is_dpi_aware, new_scale, new_dpi))
    {
        font_set_destroy(&notif->fonts.summary);
        notif->fonts.summary = summary;
    }

    struct font_set body;
    if (reload_one_font_set(
            &urgency->body.font, &body, is_dpi_aware, new_scale, new_dpi))
    {
        font_set_destroy(&notif->fonts.body);
        notif->fonts.body = body;
    }

    struct font_set action;
    if (reload_one_font_set(
            &urgency->action.font, &action, is_dpi_aware, new_scale, new_dpi))
    {
        font_set_destroy(&notif->fonts.action);
        notif->fonts.action = action;
    }

    return true;
}

static void
notif_reset_image(struct notif *notif)
{
    if (notif->pix == NULL)
        return;

    free(pixman_image_get_data(notif->pix));
    pixman_image_unref(notif->pix);
    notif->pix = NULL;
    notif->image_is_custom = false;
}

static void
notif_set_image_internal(struct notif *notif, pixman_image_t *pix, bool custom)
{
    const int max_size = notif->mgr->conf->max_icon_size;

    notif_reset_image(notif);

    notif->image_is_custom = custom;
    notif->pix = pix;
    notif->image_width = pixman_image_get_width(pix);
    notif->image_height = pixman_image_get_height(pix);

    if (max_size == 0) {
        notif_reset_image(notif);
        return;
    }

    if (notif->image_width <= max_size && notif->image_height <= max_size)
        return;

    double scale_w = notif->image_width / max_size;
    double scale_h = notif->image_height / max_size;
    double scale = scale_w > scale_h ? scale_w : scale_h;

    notif->image_width /= scale;
    notif->image_height /= scale;

    LOG_DBG("image re-scaled: %dx%d -> %dx%d",
            pixman_image_get_width(pix), pixman_image_get_height(pix),
            notif->image_width, notif->image_height);

    struct pixman_f_transform f_scale;
    pixman_f_transform_init_scale(&f_scale, scale, scale);

    struct pixman_transform t;
    pixman_transform_from_pixman_f_transform(&t, &f_scale);
    pixman_image_set_transform(pix, &t);

    const enum scaling_filter filter = notif->mgr->conf->scaling_filter;

    switch (filter) {
    case SCALING_FILTER_NONE:
        break;

    case SCALING_FILTER_NEAREST:
        pixman_image_set_filter(pix, PIXMAN_FILTER_NEAREST, NULL, 0);
        break;

    case SCALING_FILTER_BILINEAR:
        pixman_image_set_filter(pix, PIXMAN_FILTER_BILINEAR, NULL, 0);
        break;

    case SCALING_FILTER_CUBIC:
    case SCALING_FILTER_LANCZOS3: {
        int param_count = 0;
        pixman_kernel_t kernel = filter == SCALING_FILTER_CUBIC
            ? PIXMAN_KERNEL_CUBIC
            : PIXMAN_KERNEL_LANCZOS3;

        pixman_fixed_t *params = pixman_filter_create_separable_convolution(
            &param_count,
            pixman_double_to_fixed(scale),
            pixman_double_to_fixed(scale),
            kernel, kernel,
            kernel, kernel,
            pixman_int_to_fixed(1),
            pixman_int_to_fixed(1));

        pixman_image_set_filter(
            pix, PIXMAN_FILTER_SEPARABLE_CONVOLUTION, params, param_count);
        free(params);
    }
    }
}

static bool
notif_reload_default_icon(struct notif *notif)
{
    if (notif->image_is_custom)
        return true;

    const struct config *conf = notif->mgr->conf;
    const char *icon = conf->by_urgency[notif->urgency].icon;

    if (icon == NULL) {
        notif_reset_image(notif);
        return true;
    }

    pixman_image_t *pix = icon_load(
        icon, conf->max_icon_size, notif->mgr->icon_theme);

    if (pix == NULL) {
        LOG_ERR("failed to load image: %s", icon);
        notif_reset_image(notif);
        return false;
    }

    notif_set_image_internal(notif, pix, false);
    return true;
}

static bool
fdm_timeout(struct fdm *fdm, int fd, int events, void *data)
{
    if (events & EPOLLHUP)
        return false;

    struct notif *notif = data;

    uint64_t unused;
    ssize_t ret = read(fd, &unused, sizeof(unused));

    if (ret < 0) {
        if (errno == EAGAIN)
            return true;

        LOG_ERRNO("failed to read notification timeout timer");
        return false;
    }

    notif_mgr_expire_id(notif->mgr, notif->id);
    return true;
}

static bool
notif_reload_timeout(struct notif *notif)
{
    const struct urgency_config *urgency =
        &notif->mgr->conf->by_urgency[notif->urgency];

    const int notif_timeout_ms = notif->timeout_ms;
    const int max_timeout_ms = urgency->max_timeout_secs * 1000;
    const int default_timeout_ms = urgency->default_timeout_secs * 1000;

    int timeout_ms = notif_timeout_ms == -1
        ? default_timeout_ms
        : notif_timeout_ms;

    assert(timeout_ms >= 0);

    if (max_timeout_ms > 0) {
        if (timeout_ms > 0)
            timeout_ms = min(timeout_ms, max_timeout_ms);
        else
            timeout_ms = max_timeout_ms;
    }

    LOG_DBG("timeout=%dms "
            "(notif-timeout=%dms, max-timeout=%dms, default-timeout=%dms)",
            timeout_ms, notif_timeout_ms, max_timeout_ms, default_timeout_ms);

    /* Remove existing timer */
    if (notif->timeout_fd >= 0) {
        fdm_del(notif->mgr->fdm, notif->timeout_fd);
        notif->timeout_fd = -1;
    }

    if (wayl_is_idle_for_urgency(notif->mgr->wayl, notif->urgency)) {
        LOG_DBG("removed timer for notification with id %d, because urgency level %d is idle",
                notif->id, notif->urgency);
        return true;
    }

    if (timeout_ms == 0) {
        /* No timeout */
        return true;
    }

    notif->timeout_fd = timerfd_create(
        CLOCK_BOOTTIME, TFD_CLOEXEC | TFD_NONBLOCK);

    if (notif->timeout_fd < 0) {
        LOG_ERRNO("failed to create notification timeout timer FD");
        return false;
    }

    long nsecs = (long)timeout_ms * 1000000;
    time_t secs = nsecs / 1000000000l;
    nsecs %= 1000000000l;

    struct itimerspec timeout = {
        .it_value = {.tv_sec = secs, .tv_nsec = nsecs}
    };

    if (timerfd_settime(notif->timeout_fd, 0, &timeout, NULL) < 0) {
        LOG_ERRNO("failed to configure notification timeout timer FD");
        goto fail;
    }

    if (!fdm_add(notif->mgr->fdm, notif->timeout_fd, EPOLLIN,
                 &fdm_timeout, notif))
    {
        LOG_ERR("failed to add notification timeout timer to FDM");
        goto fail;
    }

    return true;

fail:
    if (notif->timeout_fd != -1)
        close(notif->timeout_fd);
    notif->timeout_fd = -1;
    return false;
}

uint32_t
notif_id(const struct notif *notif)
{
    return notif->id;
}

const struct monitor *
notif_monitor(const struct notif *notif)
{
    return notif->mon;
}

float
notif_scale(const struct notif *notif)
{
    return notif->scale;
}

static char32_t *
decode_html_entities(const struct notif_mgr *mgr, const char *s)
{
    /* Guesstimate initial size */
    size_t sz = strlen(s) + 1;
    char32_t *result = malloc(sz * sizeof(char32_t));

    /* Output so far */
    size_t len = 0;
    char32_t *out = result;

#define ensure_size(new_size)                               \
    do {                                                    \
        while (sz < (new_size)) {                           \
            sz *= 2;                                        \
            result = realloc(result, sz * sizeof(char32_t)); \
            out = &result[len];                             \
        }                                                   \
    } while (0)

#define append(wc)                                           \
    do {                                                     \
        ensure_size(len + 1);                                \
        *out++ = wc;                                         \
        len++;                                               \
    } while (0)

#define append_u8(s, s_len)                                      \
    do {                                                         \
        size_t _w_len = mbsntoc32(NULL, s, s_len, 0);            \
        if (_w_len > 0) {                                        \
            ensure_size(len + _w_len + 1);                       \
            mbsntoc32(out, s, s_len, _w_len + 1);                \
            out += _w_len;                                       \
            len += _w_len;                                       \
        }                                                        \
    } while (0)

    while (true) {
        regmatch_t matches[mgr->html_entity_re.re_nsub + 1];
        if (regexec(&mgr->html_entity_re, s, mgr->html_entity_re.re_nsub + 1, matches, 0) == REG_NOMATCH) {
            append_u8(s, strlen(s));
            break;
        }

        const regmatch_t *all = &matches[0];
        const regmatch_t *named = &matches[1];
        const regmatch_t *decimal = &matches[2];
        const regmatch_t *hex = &matches[3];

        append_u8(s, all->rm_so);

        if (named->rm_so >= 0) {
            size_t match_len = named->rm_eo - named->rm_so;
            const char *match = &s[named->rm_so];

            if (strncmp(match, "nbsp", match_len) == 0)       append(U' ');
            else if (strncmp(match, "lt", match_len) == 0)    append(U'<');
            else if (strncmp(match, "gt", match_len) == 0)    append(U'>');
            else if (strncmp(match, "amp", match_len) == 0)   append(U'&');
            else if (strncmp(match, "quot", match_len) == 0)  append(U'"');
            else if (strncmp(match, "apos", match_len) == 0)  append(U'\'');
            else if (strncmp(match, "cent", match_len) == 0)  append(U'¢');
            else if (strncmp(match, "pound", match_len) == 0) append(U'£');
            else if (strncmp(match, "yen", match_len) == 0)   append(U'¥');
            else if (strncmp(match, "euro", match_len) == 0)  append(U'€');
            else if (strncmp(match, "copy", match_len) == 0)  append(U'©');
            else if (strncmp(match, "reg", match_len) == 0)   append(U'®');
            else assert(false);
        }

        else if (decimal->rm_so >= 0 || hex->rm_so >= 0) {
            bool is_hex = hex->rm_so >= 0;
            const char *match = is_hex ? &s[hex->rm_so] : &s[decimal->rm_so];

            /* Convert string to integer */
            errno = 0;

            char *end;
            unsigned long v = strtoul(match, &end, is_hex ? 16 : 10);

            if (errno == 0) {
                assert(*end == ';');
                append((char32_t)v);
            }
        }

        s += all->rm_eo;
    }

#undef append
#undef append_n
#undef ensure_size

    result[len] = U'\0';
    return result;
}

void
notif_set_application(struct notif *notif, const char *text)
{
    free(notif->app);
    notif->app = ambstoc32(text);
}

void
notif_set_summary(struct notif *notif, const char *text)
{
    free(notif->summary);
    notif->summary = decode_html_entities(notif->mgr, text);
}

char *
notif_get_summary(const struct notif *notif)
{
    if (notif->summary == NULL)
        return NULL;

    return ac32tombs(notif->summary);
}

void
notif_set_body(struct notif *notif, const char *text)
{
    free(notif->body);
    notif->body = decode_html_entities(notif->mgr, text);
}

void
notif_set_urgency(struct notif *notif, enum urgency urgency)
{
    if (notif->urgency == urgency)
        return;

    notif->urgency = urgency;
    notif_reload_timeout(notif);
    notif_reload_fonts(notif);
    notif_reload_default_icon(notif);

    if (tll_length(notif->mgr->notifs) <= 1)
        return;

    tll_foreach(notif->mgr->notifs, it) {
        if (it->item == notif) {
            tll_remove(notif->mgr->notifs, it);
            break;
        }
    }

    tll_rforeach(notif->mgr->notifs, it) {
        if (it->item->urgency >= notif->urgency) {
            tll_insert_after(notif->mgr->notifs, it, notif);
            return;
        }
    }

    tll_push_front(notif->mgr->notifs, notif);
}

void
notif_set_progress(struct notif *notif, int8_t progress)
{
    if (notif->progress == progress)
        return;

    notif->progress = progress;
}

void
notif_set_image(struct notif *notif, pixman_image_t *pix)
{
    notif_set_image_internal(notif, pix, true);
}

void
notif_set_timeout(struct notif *notif, int timeout_ms)
{
    /* 0 - never expire */
    notif->timeout_ms = timeout_ms;
    notif_reload_timeout(notif);
}

void
notif_add_action(struct notif *notif, const char *id, const char *label)
{

    char32_t *wid = ambstoc32(id);
    char32_t *wlabel = ambstoc32(label);

    if (wid == NULL || wlabel == NULL) {
        free(wid);
        free(wlabel);
        return;
    }

    tll_push_back(
        notif->actions,
        ((struct action){
            .id = strdup(id), .wid = wid,
            .label = strdup(label), .wlabel = wlabel}));
}

void
notif_play_sound(struct notif *notif)
{
    const struct config *conf = notif->mgr->conf;
    const struct urgency_config *uconf = &conf->by_urgency[notif->urgency];

    if (conf->play_sound.raw_cmd == NULL || uconf->sound_file == NULL)
        return;

    size_t argc;
    char **argv;

    if (!spawn_expand_template(
            &conf->play_sound,
            1,
            (const char *[]){"filename"},
            (const char *[]){uconf->sound_file},
            &argc, &argv))
    {
        return;
    }

    spawn(NULL, argv, -1, -1, -1);
    for (size_t i = 0; i < argc; i++)
        free(argv[i]);
    free(argv);
}

static void
frame_callback(void *data, struct wl_callback *wl_callback, uint32_t callback_data)
{
    struct notif *notif = data;

    LOG_DBG("frame callback");
    assert(notif->frame_callback == wl_callback);
    notif->frame_callback = NULL;
    wl_callback_destroy(wl_callback);

    if (notif->pending != NULL) {
        commit_buffer(notif, notif->pending);
        notif->pending = NULL;
    }
}

static bool
notif_instantiate_surface(struct notif *notif, int *width, int *height)
{
    assert(notif->surface == NULL);
    assert(notif->layer_surface == NULL);

    struct notif_mgr *mgr = notif->mgr;
    struct wayland *wayl = mgr->wayl;
    const struct monitor *mon = wayl_preferred_monitor(wayl);

    /* Will be updated, if necessary, once we’ve been mapped */
    const float scale = mon != NULL ? mon->scale : wayl_guess_scale(wayl);

    struct wl_surface *surface =
        wl_compositor_create_surface(wayl_compositor(wayl));

    if (surface == NULL) {
        LOG_ERR("failed to create wayland surface");
        return false;
    }

    const struct config *conf = mgr->conf;
    const struct urgency_config *urgency = &conf->by_urgency[notif->urgency];

    struct zwlr_layer_surface_v1 *layer_surface =
        zwlr_layer_shell_v1_get_layer_surface(
            wayl_layer_shell(wayl), surface, mon != NULL ? mon->output : NULL,
            urgency->layer, "notifications");

    if (layer_surface == NULL) {
        LOG_ERR("failed to create layer shell surface");
        wl_surface_destroy(surface);
        return false;
    }

    /* Width/height must be divisible with the scale */
    *width = (int)roundf(roundf(*width / scale) * scale);
    *height = (int)roundf(roundf(*height / scale) * scale);

    enum zwlr_layer_surface_v1_anchor anchor
        = conf->anchor == ANCHOR_CENTER
            ? 0
            : (conf->anchor == ANCHOR_TOP_LEFT || conf->anchor == ANCHOR_TOP_RIGHT
                ? ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
                : ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) |
              (conf->anchor == ANCHOR_TOP_LEFT || conf->anchor == ANCHOR_BOTTOM_LEFT
                  ? ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
                  : ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);

    zwlr_layer_surface_v1_set_anchor(layer_surface, anchor);
    zwlr_layer_surface_v1_set_size(
        layer_surface, roundf(*width / scale), roundf(*height / scale));

    wl_surface_add_listener(surface, &surface_listener, notif);

    zwlr_layer_surface_v1_add_listener(
        layer_surface, &layer_surface_listener, notif);
    wl_surface_commit(surface);

    struct wp_fractional_scale_manager_v1 *scale_manager =
        wayl_fractional_scale_manager(wayl);
    struct wp_viewporter *viewporter = wayl_viewporter(wayl);
    struct wp_fractional_scale_v1 *fractional_scale = NULL;
    struct wp_viewport *viewport = NULL;

    if (scale_manager != NULL && viewporter != NULL) {
        viewport = wp_viewporter_get_viewport(viewporter, surface);
        fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
            scale_manager, surface);
        wp_fractional_scale_v1_add_listener(
            fractional_scale, &fractional_scale_listener, notif);
    }

    notif->viewport = viewport;
    notif->fractional_scale = fractional_scale;
    notif->surface = surface;
    notif->layer_surface = layer_surface;
    notif->mon = mon;
    notif->scale = mon != NULL ? mon->scale : wayl_guess_scale(wayl);
    notif->subpixel = mon != NULL
        ? (enum fcft_subpixel)mon->subpixel : wayl_guess_subpixel(wayl);

    return true;
}

struct glyph_run {
    size_t count;
    int *cluster;
    const struct fcft_glyph **glyphs;

    bool underline;
    struct fcft_font *font;

    bool free_arrays;
};

static uint64_t
sdbm_hash(size_t len, const char32_t s[static len])
{
    uint64_t hash = 0;

    for (size_t i = 0; i < len; i++) {
        int c = s[i];
        hash = c + (hash << 6) + (hash << 16) - hash;
    }

    return hash;
}

static struct glyph_run
notify_rasterize_text_run(struct notif *notif, struct fcft_font *font,
                          enum fcft_subpixel subpixel,
                          size_t len, const char32_t text[static len],
                          size_t ofs)
{
    uint64_t hash = sdbm_hash(len, text);

    tll_foreach(notif->text_run_cache, it) {
        if (it->item.hash != hash)
            continue;
        if (it->item.font != font)
            continue;
        if (it->item.subpixel != subpixel)
            continue;
        if (it->item.ofs != ofs) {
            /* TODO: we could still reuse the run, but need to deal
             * with cluster offsets */
            continue;
        }

        return (struct glyph_run){
            .count = it->item.run->count,
            .cluster = it->item.run->cluster,
            .glyphs = it->item.run->glyphs,
            .font = font,
            .free_arrays = false,
        };
    }

    struct fcft_text_run *run = fcft_rasterize_text_run_utf32(
        font, len, text, subpixel);

    if (run == NULL)
        return (struct glyph_run){0};

    for (size_t i = 0; i < run->count; i++)
        run->cluster[i] += ofs;

    struct text_run_cache cache = {
        .run = run,
        .font = font,
        .hash = hash,
        .subpixel = subpixel,
        .ofs = ofs,
    };
    tll_push_front(notif->text_run_cache, cache);

    return (struct glyph_run){
        .count = run->count,
        .cluster = run->cluster,
        .glyphs = run->glyphs,
        .font = font,
        .free_arrays = false,
    };
}

static struct glyph_run
notify_rasterize_glyphs(struct fcft_font *font, enum fcft_subpixel subpixel,
                        size_t len, const char32_t text[static len], size_t ofs)
{
    int *cluster = malloc(len * sizeof(cluster[0]));
    const struct fcft_glyph **glyphs = malloc(len * sizeof(glyphs[0]));

    struct glyph_run run = {
        .count = 0,
        .cluster = cluster,
        .glyphs = glyphs,
        .font = font,
        .free_arrays = true,
    };

    for (size_t i = 0; i < len; i++) {
        const struct fcft_glyph *glyph = fcft_rasterize_char_utf32(
            font, text[i], subpixel);

        if (glyph == NULL)
            continue;

        cluster[run.count] = ofs + i;
        glyphs[run.count] = glyph;
        run.count++;
    }

    return run;
}

static struct glyph_run
notify_rasterize(struct notif *notif, struct fcft_font *font, enum fcft_subpixel subpixel,
                 size_t len, const char32_t text[static len], size_t ofs)
{
    if (len == 0)
        return (struct glyph_run){0};

    return fcft_capabilities() & FCFT_CAPABILITY_TEXT_RUN_SHAPING
        ? notify_rasterize_text_run(notif, font, subpixel, len, text, ofs)
        : notify_rasterize_glyphs(font, subpixel, len, text, ofs);
}

struct glyph_layout {
    const struct fcft_glyph *glyph;
    const pixman_color_t *color;
    int x, y;
    struct {
        bool draw;
        int y;
        int thickness;
    } underline;
};

typedef tll(struct glyph_layout) glyph_list_t;

static void
notif_layout(struct notif *notif, struct font_set *fonts,
             const pixman_color_t *color, enum fcft_subpixel subpixel,
             const char32_t *text, int left_pad, int right_pad,
             int y, int max_y, int *width, int *height,
             glyph_list_t *glyph_list)
{
    *width = 0;
    *height = 0;

    const struct config *conf = notif->mgr->conf;

    bool bold = false;
    bool italic = false;
    bool underline = false;

    tll(struct glyph_run) runs = tll_init();
    size_t total_glyph_count = 0;

    /* Rasterize whole runs, if possible. Need to look for font
     * formatters since we need to use different fonts for those */
    const char32_t *_t = text;
    for (const char32_t *wc = _t; true; wc++) {
        if (!(*wc == U'\0' ||
              c32ncasecmp(wc, U"<b>", 3) == 0 ||
              c32ncasecmp(wc, U"<i>", 3) == 0 ||
              c32ncasecmp(wc, U"<u>", 3) == 0 ||
              c32ncasecmp(wc, U"</b>", 4) == 0 ||
              c32ncasecmp(wc, U"</i>", 4) == 0 ||
              c32ncasecmp(wc, U"</u>", 4) == 0))
        {
            continue;
        }

        /* Select font based on formatters currently enabled */
        struct fcft_font *font = NULL;
        if (bold && italic)
            font = fonts->bold_italic;
        else if (bold)
            font = fonts->bold;
        else if (italic)
            font = fonts->italic;

        if (font == NULL)
            font = fonts->regular;

        size_t len = wc - _t;
        size_t ofs = _t - text;

        struct glyph_run run = notify_rasterize(notif, font, subpixel, len, _t, ofs);
        total_glyph_count += run.count;

        if (run.count > 0) {
            run.underline = underline;
            tll_push_back(runs, run);
        }

        if (*wc == U'\0')
            break;

        /* Update formatter state */
        bool new_value = wc[1] == U'/' ? false : true;
        char32_t formatter = wc[1] == U'/' ? wc[2] : wc[1];

        if (formatter == U'b' || formatter == U'B')
            bold = new_value;
        if (formatter == U'i' || formatter == U'I')
            italic = new_value;
        if (formatter == U'u' || formatter == U'U')
            underline = new_value;

        _t = wc + (wc[1] == U'/' ? 4 : 3);
    }

    /* Distance from glyph to next word boundary. Note: only the
     * *first* glyph in a word has a non-zero distance */
    int distance[total_glyph_count];

    {
        /* Need flat cluster+glyph arrays for this... */
        int cluster[total_glyph_count];
        const struct fcft_glyph *glyphs[total_glyph_count];

        size_t idx = 0;
        tll_foreach(runs, it) {
            const struct glyph_run *run = &it->item;

            for (size_t i = 0; i < run->count; i++, idx++) {
                cluster[idx] = it->item.cluster[i];
                glyphs[idx] = it->item.glyphs[i];
            }
        }

        /* Loop glyph runs, looking for word boundaries */
        idx = 0;
        tll_foreach(runs, it) {
            const struct glyph_run *run = &it->item;

            for (size_t i = 0; i < run->count; i++, idx++) {
                distance[idx] = 0;

                if (!isc32space(text[run->cluster[i]]))
                    continue;

                /* Calculate distance to *this* space for all
                 * preceding glyphs (up til the previous space) */
                for (ssize_t j = idx - 1, dist = 0; j >= 0; j--) {
                    if (isc32space(text[cluster[j]]))
                        break;

                    if (j == 0 || (j > 0 && isc32space(text[cluster[j - 1]]))) {
                        /*
                         * Store non-zero distance only in first character in a word
                         * This ensures the layouting doesn't produce output like:
                         *
                         * x
                         * x
                         * x
                         * x
                         *
                         * for very long words, that doesn't fit at all on single line.
                         */
                        distance[j] = dist;
                    }

                    dist += glyphs[j]->advance.x;
                }
            }
        }

        /* Calculate distance for the last word */
        for (ssize_t j = total_glyph_count - 1, dist = 0; j >= 0; j--) {
            if (isc32space(text[cluster[j]]))
                break;

            if (j == 0 || (j > 0 && isc32space(text[cluster[j - 1]]))) {
                /* Store non-zero distance only in first character in a word */
                distance[j] = dist;
            }

            dist += glyphs[j]->advance.x;
        }
    }

    int x = left_pad;

    if (conf->min_width != 0)
        *width = conf->min_width;

    /*
     * Finally, lay out the glyphs
     *
     * This is done by looping the glyphs, and inserting a newline
     * whenever a word cannot be fitted in the remaining space.
     */
    size_t idx = 0;
    tll_foreach(runs, it) {
        struct glyph_run *run = &it->item;

        for (size_t i = 0; i < run->count; i++, idx++) {

            const char32_t wc = text[run->cluster[i]];
            const struct fcft_glyph *glyph = run->glyphs[i];
            struct fcft_font *font = run->font;
            const int dist = distance[idx];

            if ((x > left_pad && conf->max_width > 0 &&
                 x + glyph->advance.x + dist + right_pad > conf->max_width) ||
                wc == U'\n')
            {
                *width = max(*width, x + right_pad);
                *height += fonts->regular->height;

                x = left_pad;
                y += fonts->regular->height;

                if (isc32space(wc)) {
                    /* Don't render trailing whitespace */
                    continue;
                }
            }

            if (max_y >= 0 && y + fonts->regular->height > max_y)
                break;

            if (glyph->cols <= 0)
                continue;

            struct glyph_layout layout = {
                .glyph = glyph,
                .color = color,
                .x = x,
                .y = y + font->ascent,
                .underline = {
                    .draw = run->underline,
                    .y = y + font->ascent - font->underline.position,
                    .thickness = font->underline.thickness,
                },
            };

            tll_push_back(*glyph_list, layout);
            x += glyph->advance.x;
        }

        if (run->free_arrays) {
            free(run->cluster);
            free(run->glyphs);
        }

        tll_remove(runs, it);
    }

    *width = max(*width, x + right_pad);
    *height += fonts->regular->height;
}

static char32_t *
expand_format_string(const struct notif *notif, const char32_t *fmt)
{
    if (fmt == NULL)
        return NULL;

    const size_t fmt_len = c32len(fmt);
    size_t ret_len = fmt_len;

    char32_t *ret = malloc(ret_len * sizeof(ret[0]));
    if (ret == NULL)
        return NULL;

    enum { ESCAPE_NONE, ESCAPE_PERCENT, ESCAPE_BACKSLASH} escape = ESCAPE_NONE;
    char32_t scratch[16];
    size_t ret_idx = 0;

    for (const char32_t *src = fmt; src < &fmt[fmt_len]; src++) {
        const char32_t *append_str = NULL;
        size_t append_len = 0;

        switch (escape) {
        case ESCAPE_NONE:
            switch (*src) {
            case U'%':
                escape = ESCAPE_PERCENT;
                continue;

            case U'\\':
                escape = ESCAPE_BACKSLASH;
                continue;

            default:
                append_str = src;
                append_len = 1;
                break;
            }
            break;

        case ESCAPE_PERCENT:
            switch (*src) {
            case U'a':
                append_str = notif->app;
                append_len = c32len(append_str);
                break;

            case U's':
                append_str = notif->summary;
                append_len = c32len(append_str);
                break;

            case U'b':
                append_str = notif->body;
                append_len = c32len(append_str);
                break;

            case U'A':
                if (tll_length(notif->actions) > 0) {
                    scratch[0] = U'*';
                    append_str = scratch;
                    append_len = 1;
                }
                break;

            case U'%':
                append_str = src;
                append_len = 1;
                break;
            }
            escape = ESCAPE_NONE;
            break;

        case ESCAPE_BACKSLASH:
            switch (*src) {
            case U'n':
                scratch[0] = U'\n';
                append_str = scratch;
                append_len = 1;
                break;
            }
            escape = ESCAPE_NONE;
            break;
        }

        if (append_str == NULL)
            continue;

        while (ret_idx + append_len  + 1> ret_len) {
            size_t new_ret_len = ret_len * 2;
            char32_t *new_ret = realloc(ret, new_ret_len * sizeof(new_ret[0]));

            if (new_ret == NULL) {
                free(ret);
                return NULL;
            }

            ret = new_ret;
            ret_len = new_ret_len;
        }

        assert(ret_idx + append_len <= ret_len);
        memcpy(&ret[ret_idx], append_str, append_len * sizeof(ret[0]));
        ret_idx += append_len;
    }

    if (ret_idx == 0) {
        LOG_DBG("expand: %ls -> NULL", (const wchar_t *)fmt);
        free(ret);
        return NULL;
    }

    assert(ret_idx + 1 <= ret_len);
    ret[ret_idx] = U'\0';

    LOG_DBG("expand: %ls -> %ls", (const wchar_t *)fmt, (const wchar_t *)ret);
    return ret;
}

static pixman_region32_t
rounded_rectangle_region(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint16_t radius)
{

    int rect_count = ( radius + radius ) + 1;
    pixman_box32_t rects[rect_count];

    for (int i = 0; i <= radius; i++) {
        uint16_t ydist = radius - i;
        uint16_t curve = sqrt(radius * radius - ydist * ydist);

        rects[i] = (pixman_box32_t) {
            x + radius - curve,
            y + i,
            x + width - radius + curve,
            y + i + 1
        };

        rects[radius + i] = (pixman_box32_t) {
            x + radius - curve,
            y + height - i,
            x + width - radius + curve,
            y + height - i + 1
        };
    }

    rects[(radius * 2)] = (pixman_box32_t){
        x,
        y + radius,
        x + width,
        y + height + 1 - radius
    };

    pixman_region32_t region;
    pixman_region32_init_rects(&region, rects, rect_count);
    return region;
}

static inline void
fill_region32(pixman_op_t op, pixman_image_t* dest,
                       const pixman_color_t* color, pixman_region32_t* region)
{
    int rectc;
    pixman_box32_t *rects = pixman_region32_rectangles(region, &rectc);
    pixman_image_fill_boxes(op, dest, color, rectc, rects);

}

static inline void
fill_rounded_rectangle(pixman_op_t op, pixman_image_t* dest,
                       const pixman_color_t* color, int16_t x, int16_t y,
                       uint16_t width, uint16_t height, uint16_t radius)
{
    pixman_region32_t region = rounded_rectangle_region(x, y, width, height, radius);
    fill_region32(op, dest, color, &region);
    pixman_region32_fini(&region);
}

static int
notif_show(struct notif *notif, int y)
{
    struct notif_mgr *mgr = notif->mgr;
    struct config *conf = mgr->conf;
    struct urgency_config *urgency = &conf->by_urgency[notif->urgency];

    struct wayland *wayl = notif->mgr->wayl;

    const enum fcft_subpixel subpixel = urgency->bg.alpha == 0xffff
        ? notif->subpixel : FCFT_SUBPIXEL_NONE;

    const int pad_horizontal = urgency->padding.horizontal;
    const int pad_vertical = urgency->padding.vertical;

    const int pbar_height = urgency->progress.height;
    int pbar_y = -1;

    int width = 0;
    int height = pad_vertical;
    glyph_list_t glyphs = tll_init();

    int indent = pad_horizontal;
    int _w, _h;

    if (notif->pix != NULL)
        indent += notif->image_width + pad_horizontal;

    char32_t *title = expand_format_string(notif, urgency->app.format);
    char32_t *summary = expand_format_string(notif, urgency->summary.format);
    char32_t *body = expand_format_string(notif, urgency->body.format);

    if (title != NULL && title[0] != U'\0') {
        notif_layout(
            notif, &notif->fonts.app, &urgency->app.color, subpixel,
            title, indent, pad_horizontal, height,
            conf->max_height > 0 ? conf->max_height - pad_vertical : -1,
            &_w, &_h, &glyphs);
        width = max(width, _w);
        height += _h;
    }

    if (summary != NULL && summary[0] != U'\0') {
        notif_layout(
            notif, &notif->fonts.summary, &urgency->summary.color, subpixel,
            summary, indent, pad_horizontal, height,
            conf->max_height > 0 ? conf->max_height - pad_vertical : -1,
            &_w, &_h, &glyphs);
        width = max(width, _w);
        height += _h;
    }

    if (body != NULL && body[0] != U'\0') {
        notif_layout(
            notif, &notif->fonts.body, &urgency->body.color, subpixel,
            body, indent, pad_horizontal, height,
            conf->max_height > 0 ? conf->max_height - pad_vertical : -1,
            &_w, &_h, &glyphs);
        width = max(width, _w);
        height += _h;
    }

    free(title);
    free(summary);
    free(body);

#if 0
    /* App name */
    if (notif->app != NULL && notif->app[0] != U'\0') {
        notif_layout(
            notif, &notif->fonts.app, &urgency->app.color, subpixel,
            notif->app, indent, pad_horizontal, height,
            conf->max_height > 0 ? conf->max_height - pad_vertical : -1,
            &_w, &_h, &glyphs);

        if (tll_length(notif->actions) > 0) {
            /* TODO: better 'action' indicator */
            int _a, _b;
            notif_layout(
                notif, &notif->fonts.app, &urgency->app.color,
                subpixel, U"*", _w - pad_horizontal, pad_horizontal,
                height,
                conf->max_height > 0 ? conf->max_height - pad_vertical : -1,
                &_a, &_b, &glyphs);
        }
        width = max(width, _w);
        height += _h;
    }

    /* Summary */
    if (notif->summary != NULL && notif->summary[0] != U'\0') {
        notif_layout(
            notif, &notif->fonts.summary, &urgency->summary.color, subpixel,
            notif->summary, indent, pad_horizontal, height,
            conf->max_height > 0 ? conf->max_height - pad_vertical : -1,
            &_w, &_h, &glyphs);
        width = max(width, _w);
        height += _h;
    }

    /* Body */
    if (notif->body != NULL && notif->body[0] != U'\0') {
        /* Empty line between summary and body */
        height += notif->fonts.body.regular->height;

        notif_layout(
            notif, &notif->fonts.body, &urgency->body.color, subpixel,
            notif->body, indent, pad_horizontal, height,
            conf->max_height > 0 ? conf->max_height - pad_vertical : -1,
            &_w, &_h, &glyphs);
        width = max(width, _w);
        height += _h;
    }
#endif

    if (notif->pix != NULL) {
        height = max(height, pad_vertical + notif->image_height + pad_vertical);
        width = max(width, pad_horizontal + notif->image_width + pad_horizontal);
    }

    if (notif->progress >= 0) {
        if (urgency->progress.style == PROGRESS_STYLE_BAR) {
            const int bar_y = height + notif->fonts.body.regular->height;
            if (conf->max_height == 0 ||
                bar_y + pbar_height <= conf->max_height - pad_vertical)
            {
                pbar_y = bar_y;
                height += notif->fonts.body.regular->height + pbar_height;
                width = max(width, 3 * pad_horizontal);
            }
        }
    }
    height += pad_vertical;

    if (conf->max_height > 0)
        height = min(height, conf->max_height);

    bool top_anchored
        = conf->anchor == ANCHOR_TOP_LEFT || conf->anchor == ANCHOR_TOP_RIGHT;

    float scale;

    /* Resize and position */
    if (notif->surface == NULL) {
        if (!notif_instantiate_surface(notif, &width, &height))
            return 0;
        scale = notif->scale;
    } else {
        scale = notif->scale;

        width = (int)roundf(roundf(width / scale) * scale);
        height = (int)roundf(roundf(height / scale) * scale);
        zwlr_layer_surface_v1_set_size(
            notif->layer_surface, roundf(width / scale), roundf(height / scale));
    }

    LOG_DBG("show: y = %d, width = %d, height = %d (scale = %.2f)",
            y, width, height, scale);

    zwlr_layer_surface_v1_set_margin(
        notif->layer_surface,
        (top_anchored
            ? roundf(y / scale)
            : roundf(conf->margins.vertical) / scale), /* top */
        roundf(conf->margins.horizontal / scale),   /* right */
        (!top_anchored
            ? roundf(y / scale)
            : roundf(conf->margins.between) / scale),  /* bottom */
        roundf(conf->margins.horizontal / scale));  /* left */

    struct buffer *buf = wayl_get_buffer(wayl, width, height);
    const int brd_sz = urgency->border.size;
    const int brd_rad = min(min(urgency->border.radius, buf->width*0.5), buf->height*0.5);

    pixman_region32_t clip;
    pixman_region32_init_rect(&clip, 0, 0, width, height);
    pixman_image_set_clip_region32(buf->pix, &clip);
    pixman_region32_fini(&clip);

    if (brd_rad == 0){
        /* Border */
        pixman_image_fill_rectangles(
            PIXMAN_OP_SRC, buf->pix, &urgency->border.color,
            4, (pixman_rectangle16_t []){
                {0, 0, buf->width, brd_sz},                     /* top */
                {buf->width - brd_sz, 0, brd_sz, buf->height},  /* right */
                {0, buf->height - brd_sz, buf->width, brd_sz},  /* bottom */
                {0, 0, brd_sz, buf->height},                    /* left */
            });

        /* Background */
        pixman_image_fill_rectangles(
            PIXMAN_OP_SRC, buf->pix, &urgency->bg,
            1, &(pixman_rectangle16_t){
                brd_sz, brd_sz,
                buf->width - 2 * brd_sz, buf->height - 2 * brd_sz}
          );

        /* Progress */
        if (notif->progress > 0 && urgency->progress.style == PROGRESS_STYLE_BACKGROUND) {
            pixman_image_fill_rectangles(
                PIXMAN_OP_SRC, buf->pix, &urgency->progress.color,
                1, &(pixman_rectangle16_t){
                brd_sz,
                brd_sz,
                (buf->width - 2 * brd_sz) * notif->progress / 100,
                buf->height - 2 * brd_sz}
            );
        }
    } else {
        const int msaa_scale = 2;
        const double brd_sz_scaled = brd_sz * msaa_scale;
        const double brd_rad_scaled = brd_rad * msaa_scale;
        int w = buf->width * msaa_scale;
        int h = buf->height * msaa_scale;
        int bg_w = w - (brd_sz_scaled * 2);
        int bg_h = h - (brd_sz_scaled * 2);
        int bg_rad = brd_rad_scaled * (1.0 - (float)brd_sz_scaled / (float)brd_rad_scaled);

        pixman_image_t *bg;
        if (msaa_scale != 1){
            bg = pixman_image_create_bits(PIXMAN_a8r8g8b8, w, h, NULL, w*4);
        } else {
            bg = buf->pix;
        }

        /* Border */
        fill_rounded_rectangle(
            PIXMAN_OP_SRC, bg, &urgency->border.color,
            0, 0, w, h, brd_rad_scaled
        );

        /* Background */
        pixman_region32_t bg_reg = rounded_rectangle_region(
            brd_sz_scaled,
            brd_sz_scaled,
            bg_w,
            bg_h,
            bg_rad
        );
        fill_region32(PIXMAN_OP_SRC, bg, &urgency->bg, &bg_reg);

        /* Progress */
        if (notif->progress > 0 && urgency->progress.style == PROGRESS_STYLE_BACKGROUND) {
            int progress_width = (w - (brd_sz_scaled * 2)) * notif->progress / 100;
            pixman_region32_t progress_reg;
            pixman_region32_init_rect(&progress_reg,
                brd_sz_scaled,
                brd_sz_scaled,
                progress_width,
                h - (brd_sz_scaled * 2)
            );

            pixman_region32_t out_reg;
            pixman_region32_init(&out_reg);
            pixman_region32_intersect(&out_reg, &bg_reg, &progress_reg);

            int rectc;
            pixman_box32_t *rects = pixman_region32_rectangles(&out_reg, &rectc);
            pixman_image_fill_boxes(PIXMAN_OP_SRC, bg, &urgency->progress.color, rectc, rects);
        }

        if (msaa_scale != 1) {
            pixman_f_transform_t ftrans;
            pixman_transform_t trans;
            pixman_f_transform_init_scale(&ftrans, msaa_scale, msaa_scale);
            pixman_transform_from_pixman_f_transform(&trans, &ftrans);
            pixman_image_set_transform(bg, &trans);
            pixman_image_set_filter(bg, PIXMAN_FILTER_BILINEAR, NULL, 0);

            pixman_image_composite32(
                PIXMAN_OP_SRC, bg, NULL, buf->pix, 0, 0, 0, 0, 0, 0, buf->width, buf->height);
            pixman_image_unref(bg);
        }

        pixman_region32_fini(&bg_reg);
    }

    /* Image */
    if (notif->pix != NULL) {
        pixman_image_composite32(
            PIXMAN_OP_OVER, notif->pix, NULL, buf->pix, 0, 0, 0, 0,
            pad_horizontal,
            (height - notif->image_height - (pbar_y >= 0 ? pbar_height : 0)) / 2,
            notif->image_width, notif->image_height);
    }

    /* Text */
    tll_foreach(glyphs, it) {
        const struct fcft_glyph *glyph = it->item.glyph;
        if (pixman_image_get_format(glyph->pix) == PIXMAN_a8r8g8b8) {
            /* Glyph surface is a fully rendered bitmap */
            pixman_image_composite32(
                PIXMAN_OP_OVER, glyph->pix, NULL, buf->pix, 0, 0, 0, 0,
                it->item.x + glyph->x, it->item.y - glyph->y,
                glyph->width, glyph->height);
        } else {
            /* Glyph surface is an alpha mask */
            pixman_image_t *src = pixman_image_create_solid_fill(it->item.color);
            pixman_image_composite32(
                PIXMAN_OP_OVER, src, glyph->pix, buf->pix, 0, 0, 0, 0,
                it->item.x + glyph->x, it->item.y - glyph->y,
                glyph->width, glyph->height);
            pixman_image_unref(src);
        }

        if (it->item.underline.draw) {
            pixman_image_fill_rectangles(
                PIXMAN_OP_OVER, buf->pix, it->item.color,
                1, &(pixman_rectangle16_t){
                    it->item.x, it->item.underline.y,
                    glyph->advance.x, it->item.underline.thickness});
        }

        tll_remove(glyphs, it);
    }

    /* Progress bar */
    if (pbar_y >= 0 && urgency->progress.style == PROGRESS_STYLE_BAR) {
        const int full_width = buf->width - pad_horizontal * 2;
        const int width = full_width * notif->progress / 100;

        const int border =
            pbar_height > 2 * scale && width > 2 * scale ? 1 * scale : 0;

        pixman_image_fill_rectangles(
            PIXMAN_OP_OVER, buf->pix, &urgency->progress.color,
            5, (pixman_rectangle16_t []){
                /* Edges: left, top, bottom, right */
                {pad_horizontal, pbar_y, border, pbar_height},
                {pad_horizontal + border, pbar_y, full_width - border * 2, border},
                {pad_horizontal + border, pbar_y + pbar_height - border, full_width - border * 2, border},
                {pad_horizontal + full_width - border, pbar_y, border, pbar_height},

                /* The bar */
                {pad_horizontal + border, pbar_y + border, width - border * 2, pbar_height - border * 2}
            });
    }

    if (!notif->is_configured || notif->frame_callback != NULL) {
        if (notif->pending != NULL)
            notif->pending->busy = false;
        notif->pending = buf;

        /* Commit size+margins, but not the new buffer */
        wl_surface_commit(notif->surface);
    } else {
        assert(notif->pending == NULL);
        commit_buffer(notif, buf);
    }

    notif->y = y;
    return height;
}

void
notif_mgr_refresh(struct notif_mgr *mgr)
{
    int y = mgr->conf->margins.vertical;

    switch (mgr->conf->stacking_order) {
    case STACK_BOTTOM_UP:
        tll_rforeach(mgr->notifs, it)
            y += notif_show(it->item, y) + mgr->conf->margins.between;
        break;

    case STACK_TOP_DOWN:
        tll_foreach(mgr->notifs, it)
            y += notif_show(it->item, y) + mgr->conf->margins.between;
        break;
    }
}

void
notif_mgr_notifs_reload_timeout(const struct notif_mgr *mgr)
{
    tll_foreach(mgr->notifs, it)
        notif_reload_timeout(it->item);
}

ssize_t
notif_mgr_get_ids(const struct notif_mgr *mgr, uint32_t *ids, size_t max)
{
    size_t count = 0;
    tll_foreach(mgr->notifs, it) {
        if (++count <= max && ids != NULL)
            ids[count - 1] = it->item->id;
    }

    return count;
}

static bool
notif_dismiss(struct notif *notif)
{
    dbus_signal_dismissed(notif->mgr->bus, notif->id);
    notif_destroy(notif);
    return true;
}

static bool
notif_expire(struct notif *notif)
{
    dbus_signal_expired(notif->mgr->bus, notif->id);
    notif_destroy(notif);
    return true;
}

static bool
notif_mgr_expire_current(struct notif_mgr *mgr)
{
    if (tll_length(mgr->notifs) == 0)
        return false;

    struct notif *notif = tll_pop_front(mgr->notifs);
    switch (notif->deferred_expiral) {
    case EXPIRE_IMMEDIATELY:
        break;

    case EXPIRE_DEFER:
        notif->deferred_expiral = EXPIRE_DELAYED;
        tll_push_front(mgr->notifs, notif);
        return true;

    case EXPIRE_DELAYED:
        /* Already marked for expiration */
        return true;
    }

    bool ret = notif_expire(notif);
    notif_mgr_refresh(mgr);
    return ret;
}

bool
notif_mgr_expire_id(struct notif_mgr *mgr, uint32_t id)
{
    if (id == 0)
        return notif_mgr_expire_current(mgr);

    tll_foreach(mgr->notifs, it) {
        if (it->item->id != id)
            continue;

        struct notif *notif = it->item;

        switch (notif->deferred_expiral) {
        case EXPIRE_IMMEDIATELY:
            break;

        case EXPIRE_DEFER:
            notif->deferred_expiral = EXPIRE_DELAYED;
            return true;

        case EXPIRE_DELAYED:
            /* Already marked for expiration */
            return true;
        }

        tll_remove(mgr->notifs, it);

        bool ret = notif_expire(notif);
        notif_mgr_refresh(mgr);
        return ret;
    }

    return false;
}

static bool
notif_mgr_dismiss_current(struct notif_mgr *mgr)
{
    if (tll_length(mgr->notifs) == 0)
        return false;

    struct notif *notif = tll_pop_front(mgr->notifs);
    switch (notif->deferred_dismissal) {
    case DISMISS_IMMEDIATELY:
        break;

    case DISMISS_DEFER:
        notif->deferred_dismissal = DISMISS_DELAYED;
        tll_push_front(mgr->notifs, notif);
        return true;

    case DISMISS_DELAYED:
        /* Already marked for dismissal */
        return true;
    }

    bool ret = notif_dismiss(notif);
    notif_mgr_refresh(mgr);
    return ret;
}

static bool
notif_mgr_dismiss_id_internal(struct notif_mgr *mgr, uint32_t id, bool refresh)
{
    if (id == 0)
        return notif_mgr_dismiss_current(mgr);

    tll_foreach(mgr->notifs, it) {
        if (it->item->id != id)
            continue;

        struct notif *notif = it->item;
        switch (notif->deferred_dismissal) {
        case DISMISS_IMMEDIATELY:
            break;

        case DISMISS_DEFER:
            notif->deferred_dismissal = DISMISS_DELAYED;
            return true;

        case DISMISS_DELAYED:
            /* Already marked for dismissal */
            return true;
        }

        tll_remove(mgr->notifs, it);

        bool ret = notif_dismiss(notif);
        if (refresh)
            notif_mgr_refresh(mgr);
        return ret;
    }

    return false;
}

bool
notif_mgr_dismiss_id(struct notif_mgr *mgr, uint32_t id)
{
    return notif_mgr_dismiss_id_internal(mgr, id, true);
}

bool
notif_mgr_dismiss_all(struct notif_mgr *mgr)
{
    bool ret = true;
    tll_foreach(mgr->notifs, it) {
        struct notif *notif = it->item;
        bool do_dismiss = true;

        switch (notif->deferred_dismissal) {
        case DISMISS_IMMEDIATELY:
            break;

        case DISMISS_DEFER:
            notif->deferred_dismissal = DISMISS_DELAYED;
            do_dismiss = false;
            break;

        case DISMISS_DELAYED:
            /* Already marked for dismissal */
            do_dismiss = false;
            break;
        }

        if (do_dismiss) {
            if (!notif_dismiss(notif))
                ret = false;
            tll_remove(mgr->notifs, it);
        }
    }

    notif_mgr_refresh(mgr);
    return ret;
}

void
notif_mgr_monitor_removed(struct notif_mgr *mgr, const struct monitor *mon)
{
    tll_foreach(mgr->notifs, it) {
        if (it->item->mon == mon)
            it->item->mon = NULL;
    }
}

/* Returns true if the update is a reason to refresh */
bool
notif_mgr_monitor_updated(struct notif_mgr *mgr, const struct monitor *mon)
{
    bool refresh_needed = false;

    tll_foreach(mgr->notifs, it) {
        struct notif *notif = it->item;

        if (notif->surface == NULL)
            refresh_needed = true;

        const float old_notif_scale = notif->scale;
        if (notif_reload_fonts(notif))
            refresh_needed = true;
        else if (old_notif_scale != notif->scale) {
            /* for set_buffer_scale() */
            refresh_needed = true;
        }

        if (notif->mon != NULL && notif->mon == mon &&
            notif->subpixel != (enum fcft_subpixel)mon->subpixel)
        {
            notif->subpixel = (enum fcft_subpixel)mon->subpixel;
            refresh_needed = true;
        }
    }

    return refresh_needed;
}

struct action_async {
    struct fdm *fdm;
    struct notif_mgr *mgr;

    /* The notification may be dismissed while we're waiting for the
     * action selection. So, store the ID, and re-retreieve the
     * notification when we're done */
    uint32_t notif_id;

    pid_t pid;
    int to_child;      /* Child's stdin */
    int from_child;    /* Child's stdout */

    char *input;       /* Data to be sent to child (action labels) */
    size_t input_idx;  /* Where to start next write() */
    size_t input_len;  /* Total amount of data */

    char *output;      /* Output from child */
    size_t output_len; /* Amount of output received (so far) */

    notif_select_action_cb completion_cb;
    void *data;
};

static bool
fdm_action_writer(struct fdm *fdm, int fd, int events, void *data)
{
    struct action_async *async = data;

    ssize_t count = write(
        async->to_child,
        &async->input[async->input_idx],
        async->input_len - async->input_idx);

    if (count < 0) {
        if (errno == EINTR)
            return true;

        LOG_ERRNO("could not write actions to actions selection helper");
        goto done;
    }

    async->input_idx += count;
    if (async->input_idx >= async->input_len) {
        /* Close child's stdin, to signal there are no more labels */
        LOG_DBG("all input sent to child");
        goto done;
    }

    return true;

done:
    fdm_del(async->fdm, async->to_child);
    async->to_child = -1;
    return true;
}

static bool
fdm_action_reader(struct fdm *fdm, int fd, int events, void *data)
{
    struct action_async *async = data;

    const size_t chunk_sz = 1024;
    char buf[chunk_sz];

    ssize_t count = read(async->from_child, buf, chunk_sz);
    if (count < 0) {
        if (errno == EINTR)
            return true;

        LOG_ERRNO("failed to read from actions selection helper");

        /* FIXME: leaks memory (the async context) */
        return false;
    }

    if (count > 0) {
        /* Append to previously received response */
        size_t new_len = async->output_len + count;
        async->output = realloc(async->output, new_len);
        memcpy(&async->output[async->output_len], buf, count);
        async->output_len = new_len;

        /* There may be more data to read */
        return true;
    }

    /* No more data to read */
    assert(count == 0);

    /* Strip trailing spaces/newlines */
    while (async->output_len > 0
           && isspace(async->output[async->output_len - 1]))
    {
        async->output[--async->output_len] = '\0';
    }

    /* Extract the data we need from the info struct, then free it */
    struct notif_mgr *mgr = async->mgr;
    pid_t pid = async->pid;
    uint32_t notif_id = async->notif_id;
    notif_select_action_cb completion_cb = async->completion_cb;
    void *cb_data = async->data;
    char *chosen = async->output;
    size_t chosen_len = async->output_len;

    if (async->to_child != -1) {
        /* This is an error case - normally, the writer should have
         * completed and closed this already */
        fdm_del(async->fdm, async->to_child);
    }

    fdm_del(async->fdm, async->from_child);

    free(async->input);
    free(async);

    /* Wait for child to die */
    int status;
    waitpid(pid, &status, 0);
    LOG_DBG("child exited with status 0x%08x", status);

    const char *action_id = NULL;
    struct notif *notif = notif_mgr_get_notif(mgr, notif_id);

    if (!WIFEXITED(status)) {
        LOG_ERR("child did not exit normally");
        goto done;
    }

    if (WEXITSTATUS(status) != 0) {
        uint8_t code = WEXITSTATUS(status);
        if (code >> 1)
            LOG_ERRNO_P("failed to execute action selection helper", code & 0x7f);

        goto done;
    }

    if (notif == NULL) {
        LOG_WARN("notification was dismissed before we could signal action: %.*s",
                 (int)chosen_len, chosen);
        goto done;
    }

    /* Map returned label to action ID */
    tll_foreach(notif->actions, it) {
        if (strncmp(it->item.label, chosen, chosen_len) == 0) {
            action_id = it->item.id;
            goto done;
        }
    }

    LOG_WARN("could not map chosen action label to action ID: %.*s", (int)chosen_len, chosen);

done:
    completion_cb(notif, action_id, cb_data);
    free(chosen);

    if (notif->deferred_expiral == EXPIRE_DELAYED) {
        notif->deferred_expiral = EXPIRE_IMMEDIATELY;
        notif_mgr_expire_id(mgr, notif->id);
    } else {
        notif->deferred_expiral = EXPIRE_IMMEDIATELY;

        if (notif->deferred_dismissal == DISMISS_DELAYED) {
            notif->deferred_dismissal = DISMISS_IMMEDIATELY;
            notif_mgr_dismiss_id(mgr, notif->id);
        } else
            notif->deferred_dismissal = DISMISS_IMMEDIATELY;
    }
    return true;
}

static bool
push_argv(char ***argv, size_t *size, char *arg, size_t *argc)
{
    if (arg != NULL && arg[0] == '%')
        return true;

    if (*argc >= *size) {
        size_t new_size = *size > 0 ? 2 * *size : 10;
        char **new_argv = realloc(*argv, new_size * sizeof(new_argv[0]));

        if (new_argv == NULL)
            return false;

        *argv = new_argv;
        *size = new_size;
    }

    (*argv)[(*argc)++] = arg;
    return true;
}

static bool
tokenize_cmdline(char *cmdline, char ***argv)
{
    *argv = NULL;
    size_t argv_size = 0;

    bool first_token_is_quoted = cmdline[0] == '"' || cmdline[0] == '\'';
    char delim = first_token_is_quoted ? cmdline[0] : ' ';

    char *p = first_token_is_quoted ? &cmdline[1] : &cmdline[0];

    size_t idx = 0;
    while (*p != '\0') {
        char *end = strchr(p, delim);
        if (end == NULL) {
            if (delim != ' ') {
                LOG_ERR("unterminated %s quote\n", delim == '"' ? "double" : "single");
                free(*argv);
                return false;
            }

            if (!push_argv(argv, &argv_size, p, &idx) ||
                !push_argv(argv, &argv_size, NULL, &idx))
            {
                goto err;
            } else
                return true;
        }

        *end = '\0';

        if (!push_argv(argv, &argv_size, p, &idx))
            goto err;

        p = end + 1;
        while (*p == delim)
            p++;

        while (*p == ' ')
            p++;

        if (*p == '"' || *p == '\'') {
            delim = *p;
            p++;
        } else
            delim = ' ';
    }

    if (!push_argv(argv, &argv_size, NULL, &idx))
        goto err;

    return true;

err:
    free(*argv);
    return false;
}

size_t
notif_action_count(const struct notif *notif)
{
    return tll_length(notif->actions);
}

bool
notif_signal_action(const struct notif *notif, const char *action_id)
{
    if (action_id == NULL)
        return false;

    tll_foreach(notif->actions, it) {
        if (strcmp(it->item.id, action_id) == 0) {
            char *activation_token =
                wayl_get_activation_token(notif->mgr->wayl, notif->surface);

            if (activation_token != NULL)
                dbus_signal_token(notif->mgr->bus, notif->id, activation_token);
            free(activation_token);

            return dbus_signal_action(notif->mgr->bus, notif->id, action_id);
        }
    }

    return false;
}

void
notif_select_action(
    struct notif *notif, notif_select_action_cb completion_cb, void *data)
{
    char *copy = strdup(notif->mgr->conf->selection_helper);
    char **argv = NULL;
    int to_child[2] = {-1, -1};    /* Pipe to child's STDIN */
    int from_child[2] = {-1, -1};  /* Pipe to child's STDOUT */

    if (tll_length(notif->actions) == 0)
        goto err_before_fork;

    if (!tokenize_cmdline(copy, &argv))
        goto err_before_fork;

    if (pipe(to_child) < 0 || pipe(from_child) < 0) {
        LOG_ERRNO("failed to create pipe");
        goto err_before_fork;
    }

    int pid = fork();
    if (pid == -1) {
        LOG_ERRNO("failed to fork");
        goto err_before_fork;
    }

    notif->deferred_dismissal = DISMISS_DEFER;
    notif->deferred_expiral = EXPIRE_DEFER;

    if (pid == 0) {
        /*
         * Child
         */

        close(to_child[1]);
        close(from_child[0]);

        /* Rewire pipes to child's STDIN/STDOUT */
        if (dup2(to_child[0], STDIN_FILENO) < 0 ||
            dup2(from_child[1], STDOUT_FILENO) < 0)
        {
            goto child_exit;
        }

        close(to_child[0]);
        close(from_child[1]);

        execvp(argv[0], argv);

    child_exit:
        _exit(1 << 7 | errno);
    }

    assert(pid > 0);

    /*
     * Parent
     */

    free(copy);
    free(argv);
    close(to_child[0]);
    close(from_child[1]);

    /*
     * Writing the action labels and waiting for the response can take
     * a *very* long time, and we can't block execution.
     *
     * Make our pipe ends non-blocking, and use the FDM to write/read
     * them asynchronously.
     */

    struct action_async *async = NULL;
    size_t input_len = 0;
    char *input = NULL;

    if (fcntl(to_child[1], F_SETFL, fcntl(to_child[1], F_GETFL) | O_NONBLOCK) < 0 ||
        fcntl(from_child[0], F_SETFL, fcntl(from_child[0], F_GETFL) | O_NONBLOCK) < 0)
    {
        LOG_ERRNO("failed to make pipes non blocking");
        goto err_in_parent;
    }

    /* Construct a single string consisting of all the action labels
     * separated by newlines, or NULL */
    tll_foreach(notif->actions, it)
        input_len += strlen(it->item.label) + 1;

    const bool use_null =
        notif->mgr->conf->selection_helper_uses_null_separator;

    input = malloc(input_len + 1);
    size_t idx = 0;
    tll_foreach(notif->actions, it) {
        const size_t label_len = strlen(it->item.label);

        memcpy(&input[idx], it->item.label, label_len);
        idx += label_len;

        input[idx] = use_null ? '\0' : '\n';
        idx++;
    }

    assert(idx == input_len);

    /* FDM callback data. Shared by both the write and read callback,
     * but *only* freed by the *read* handler. */
    async = malloc(sizeof(*async));
    *async = (struct action_async) {
        .fdm = notif->mgr->fdm,
        .mgr = notif->mgr,
        .notif_id = notif->id,
        .to_child = to_child[1],
        .from_child = from_child[0],
        .input = input,
        .input_len = input_len,
        .input_idx = 0,
        .output = NULL,
        .output_len = 0,
        .completion_cb = completion_cb,
        .data = data,
    };

    if (!fdm_add(notif->mgr->fdm, to_child[1], EPOLLOUT, &fdm_action_writer, async) ||
        !fdm_add(notif->mgr->fdm, from_child[0], EPOLLIN, &fdm_action_reader, async))
    {
        goto err_in_parent;
    }

    return;

err_before_fork:
    if (to_child[0] != -1) close(to_child[0]);
    if (to_child[1] != -1) close(to_child[1]);
    if (from_child[0] != -1) close(from_child[0]);
    if (from_child[1] != -1) close(from_child[1]);

    free(copy);
    free(argv);

    completion_cb(notif, NULL, data);
    notif->deferred_dismissal = DISMISS_IMMEDIATELY;
    notif->deferred_expiral = EXPIRE_IMMEDIATELY;
    return;

err_in_parent:
    free(async);
    free(input);
    fdm_del(notif->mgr->fdm, to_child[1]);
    fdm_del(notif->mgr->fdm, from_child[0]);
    completion_cb(notif, NULL, data);
    notif->deferred_dismissal = DISMISS_IMMEDIATELY;
    notif->deferred_expiral = EXPIRE_IMMEDIATELY;
    return;
}
