#include "clipboard.h"

#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include <sys/epoll.h>
#include <sys/timerfd.h>

#include <primary-selection-unstable-v1.h>

#define LOG_MODULE "clipboard"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "macros.h"
#include "prompt.h"
#include "uri.h"
#include "wayland.h"
#include "xmalloc.h"

static const char *const mime_type_map[] = {
    [DATA_OFFER_MIME_UNSET] = NULL,
    [DATA_OFFER_MIME_TEXT_PLAIN] = "text/plain",
    [DATA_OFFER_MIME_TEXT_UTF8] = "text/plain;charset=utf-8",
    [DATA_OFFER_MIME_URI_LIST] = "text/uri-list",

    [DATA_OFFER_MIME_TEXT_TEXT] = "TEXT",
    [DATA_OFFER_MIME_TEXT_STRING] = "STRING",
    [DATA_OFFER_MIME_TEXT_UTF8_STRING] = "UTF8_STRING",
};

typedef void (*data_cb_t)(char *data, size_t size, void *user);
typedef void (*done_cb_t)(void *user);

static void begin_receive_clipboard(
    struct seat *seat, int read_fd,
    enum data_offer_mime_type mime_type,
    data_cb_t data_cb, done_cb_t done_cb, void *user);

static void
data_offer_reset(struct wl_clipboard *clipboard)
{
    if (clipboard->data_offer != NULL) {
        wl_data_offer_destroy(clipboard->data_offer);
        clipboard->data_offer = NULL;
    }

    //clipboard->window = NULL;
    clipboard->mime_type = DATA_OFFER_MIME_UNSET;
}

static void
clipboard_data(char *data, size_t size, void *user)
{
    struct seat *seat = user;
    wayl_clipboard_data(seat->wayl, data, size);
}

static void
clipboard_done(void *user)
{
    struct seat *seat = user;
    wayl_clipboard_done(seat->wayl);
}

static void
select_mime_type_for_offer(const char *_mime_type,
                           enum data_offer_mime_type *type)
{
    enum data_offer_mime_type mime_type = DATA_OFFER_MIME_UNSET;

    /* Translate offered mime type to our mime type enum */
    for (size_t i = 0; i < ALEN(mime_type_map); i++) {
        if (mime_type_map[i] == NULL)
            continue;

        if (strcmp(_mime_type, mime_type_map[i]) == 0) {
            mime_type = i;
            break;
        }
    }

    LOG_DBG("mime-type: %s -> %s (offered type was %s)",
            mime_type_map[*type],
            (mime_type_map[mime_type] == NULL ? "(null)" : mime_type_map[mime_type]),
            _mime_type);

    /* Mime-type transition; if the new mime-type is "better" than
     * previously offered types, use the new type */

    switch (mime_type) {
    case DATA_OFFER_MIME_TEXT_PLAIN:
    case DATA_OFFER_MIME_TEXT_TEXT:
    case DATA_OFFER_MIME_TEXT_STRING:
        /* text/plain is our least preferred type. Only use if current
         * type is unset */
        switch (*type) {
        case DATA_OFFER_MIME_UNSET:
            *type = mime_type;
            break;

        default:
            break;
        }
        break;

    case DATA_OFFER_MIME_TEXT_UTF8:
    case DATA_OFFER_MIME_TEXT_UTF8_STRING:
        /* text/plain;charset=utf-8 is preferred over text/plain */
        switch (*type) {
        case DATA_OFFER_MIME_UNSET:
        case DATA_OFFER_MIME_TEXT_PLAIN:
        case DATA_OFFER_MIME_TEXT_TEXT:
        case DATA_OFFER_MIME_TEXT_STRING:
            *type = mime_type;
            break;

        default:
            break;
        }
        break;

    case DATA_OFFER_MIME_URI_LIST:
        /* text/uri-list is always used when offered */
        *type = mime_type;
        break;

    case DATA_OFFER_MIME_UNSET:
        break;
    }
}

static void
offer(void *data, struct wl_data_offer *wl_data_offer, const char *mime_type)
{
    struct seat *seat = data;
    select_mime_type_for_offer(mime_type, &seat->clipboard.mime_type);
}

static void
primary_offer(void *data,
              struct zwp_primary_selection_offer_v1 *zwp_primary_selection_offer,
              const char *mime_type)
{
    struct seat *seat = data;
    select_mime_type_for_offer(mime_type, &seat->primary.mime_type);
}

static void
source_actions(void *data, struct wl_data_offer *wl_data_offer,
                uint32_t source_actions)
{
}

static void
offer_action(void *data, struct wl_data_offer *wl_data_offer, uint32_t dnd_action)
{
}

static const struct wl_data_offer_listener data_offer_listener = {
    .offer = &offer,
    .source_actions = &source_actions,
    .action = &offer_action,
};

static const struct zwp_primary_selection_offer_v1_listener primary_selection_offer_listener = {
    .offer = &primary_offer,
};


static void
data_offer(void *data, struct wl_data_device *wl_data_device,
           struct wl_data_offer *offer)
{
    struct seat *seat = data;
    data_offer_reset(&seat->clipboard);
    seat->clipboard.data_offer = offer;
    wl_data_offer_add_listener(offer, &data_offer_listener, seat);
}

static void
enter(void *data, struct wl_data_device *wl_data_device, uint32_t serial,
      struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y,
      struct wl_data_offer *offer)
{
    struct seat *seat = data;

    assert(offer == seat->clipboard.data_offer);

    //assert(seat->clipboard.window == NULL);
    if (seat->clipboard.mime_type != DATA_OFFER_MIME_UNSET &&
        !seat->is_pasting)
    {
        wl_data_offer_accept(
            offer, serial, mime_type_map[seat->clipboard.mime_type]);
        wl_data_offer_set_actions(
            offer,
            WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
            WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    } else {
        /* reject offer */
        wl_data_offer_accept(offer, serial, NULL);
        wl_data_offer_set_actions(
            offer,
            WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE,
            WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE);
    }
}

static void
leave(void *data, struct wl_data_device *wl_data_device)
{
}

static void
motion(void *data, struct wl_data_device *wl_data_device, uint32_t time,
       wl_fixed_t x, wl_fixed_t y)
{
}

struct dnd_context {
    struct seat *seat;
    struct wl_data_offer *data_offer;
};

static void
receive_dnd(char *data, size_t size, void *user)
{
    struct dnd_context *ctx = user;
    clipboard_data(data, size, ctx->seat);
}

static void
receive_dnd_done(void *user)
{
    struct dnd_context *ctx = user;

    wl_data_offer_finish(ctx->data_offer);
    wl_data_offer_destroy(ctx->data_offer);
    clipboard_done(ctx->seat);
    free(ctx);
}

static void
drop(void *data, struct wl_data_device *wl_data_device)
{
    struct seat *seat = data;


    struct wl_clipboard *clipboard = &seat->clipboard;

    if (clipboard->mime_type == DATA_OFFER_MIME_UNSET) {
        LOG_WARN("compositor called data_device::drop() "
                 "even though we rejected the drag-and-drop");
        return;
    }

    struct dnd_context *ctx = xmalloc(sizeof(*ctx));
    *ctx = (struct dnd_context){
        .seat = seat,
        .data_offer = clipboard->data_offer,
    };

    /* Prepare a pipe the other client can write its selection to us */
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) == -1) {
        LOG_ERRNO("failed to create pipe");
        free(ctx);
        return;
    }

    int read_fd = fds[0];
    int write_fd = fds[1];

    LOG_DBG("DnD drop: mime-type=%s", mime_type_map[clipboard->mime_type]);

    /* Give write-end of pipe to other client */
    wl_data_offer_receive(
        clipboard->data_offer, mime_type_map[clipboard->mime_type], write_fd);

    /* Don't keep our copy of the write-end open (or we'll never get EOF) */
    close(write_fd);

    seat->is_pasting = true;
    begin_receive_clipboard(
        seat, read_fd, clipboard->mime_type,
        &receive_dnd, &receive_dnd_done, ctx);

    /* data offer is now "owned" by the receive context */
    clipboard->data_offer = NULL;
    clipboard->mime_type = DATA_OFFER_MIME_UNSET;
}

static void
selection(void *data, struct wl_data_device *wl_data_device,
          struct wl_data_offer *offer)
{
    /* Selection offer from other client */
    struct seat *seat = data;
    if (offer == NULL)
        data_offer_reset(&seat->clipboard);
    else
        assert(offer == seat->clipboard.data_offer);
}

static void
primary_offer_reset(struct wl_primary *primary)
{
    if (primary->data_offer != NULL) {
        zwp_primary_selection_offer_v1_destroy(primary->data_offer);
        primary->data_offer = NULL;
    }

    primary->mime_type = DATA_OFFER_MIME_UNSET;
}

static void
primary_data_offer(void *data,
                   struct zwp_primary_selection_device_v1 *zwp_primary_selection_device,
                   struct zwp_primary_selection_offer_v1 *offer)
{
    struct seat *seat = data;
    primary_offer_reset(&seat->primary);
    seat->primary.data_offer = offer;
    zwp_primary_selection_offer_v1_add_listener(
        offer, &primary_selection_offer_listener, seat);
}

static void
primary_selection(void *data,
                  struct zwp_primary_selection_device_v1 *zwp_primary_selection_device,
                  struct zwp_primary_selection_offer_v1 *offer)
{
}

const struct wl_data_device_listener data_device_listener = {
    .data_offer = &data_offer,
    .enter = &enter,
    .leave = &leave,
    .motion = &motion,
    .drop = &drop,
    .selection = &selection,
};

const struct zwp_primary_selection_device_v1_listener primary_selection_device_listener = {
    .data_offer = &primary_data_offer,
    .selection = &primary_selection,
};

struct clipboard_receive {
    struct seat *seat;
    int read_fd;
    int timeout_fd;
    struct itimerspec timeout;
    bool quote_paths;

    void (*decoder)(struct clipboard_receive *ctx, char *data, size_t size);
    void (*finish)(struct clipboard_receive *ctx);

    /* URI state */
    bool add_space;
    struct {
        char *data;
        size_t sz;
        size_t idx;
    } buf;

    /* Callback data */
    data_cb_t data_cb;
    done_cb_t done_cb;
    void *user;
};

static void
clipboard_receive_done(struct fdm *fdm, struct clipboard_receive *ctx)
{
    ctx->seat->is_pasting = false;

    fdm_del(fdm, ctx->timeout_fd);
    fdm_del(fdm, ctx->read_fd);
    ctx->done_cb(ctx->user);
    free(ctx->buf.data);
    free(ctx);
}

static bool
fdm_receive_timeout(struct fdm *fdm, int fd, int events, void *data)
{
    struct clipboard_receive *ctx = data;
    if (events & EPOLLHUP)
        return false;

    assert(events & EPOLLIN);

    uint64_t expire_count;
    ssize_t ret = read(fd, &expire_count, sizeof(expire_count));
    if (ret < 0) {
        if (errno == EAGAIN)
            return true;

        LOG_ERRNO("failed to read clipboard timeout timer");
        return false;
    }

    LOG_WARN("no data received from clipboard in %llu seconds, aborting",
             (unsigned long long)ctx->timeout.it_value.tv_sec);

    clipboard_receive_done(fdm, ctx);
    return true;
}

static void
fdm_receive_decoder_plain(struct clipboard_receive *ctx, char *data, size_t size)
{
    ctx->data_cb(data, size, ctx->user);
}

static void
fdm_receive_finish_plain(struct clipboard_receive *ctx)
{
}

static bool
decode_one_uri(struct clipboard_receive *ctx, char *uri, size_t len)
{
    LOG_DBG("URI: \"%.*s\"", (int)len, uri);

    if (len == 0)
        return false;

    char *scheme, *host, *path;
    if (!uri_parse(uri, len, &scheme, NULL, NULL, &host, NULL, &path, NULL, NULL)) {
        LOG_ERR("drag-and-drop: invalid URI: %.*s", (int)len, uri);
        return false;
    }

    if (ctx->add_space)
        ctx->data_cb(" ", 1, ctx->user);
    ctx->add_space = true;

    if (strcmp(scheme, "file") == 0 && hostname_is_localhost(host)) {
        if (ctx->quote_paths)
            ctx->data_cb("'", 1, ctx->user);

        ctx->data_cb(path, strlen(path), ctx->user);

        if (ctx->quote_paths)
            ctx->data_cb("'", 1, ctx->user);
    } else
        ctx->data_cb(uri, len, ctx->user);

    free(scheme);
    free(host);
    free(path);
    return true;
}

static void
fdm_receive_decoder_uri(struct clipboard_receive *ctx, char *data, size_t size)
{
    while (ctx->buf.idx + size > ctx->buf.sz) {
        size_t new_sz = ctx->buf.sz == 0 ? size : 2 * ctx->buf.sz;
        ctx->buf.data = xrealloc(ctx->buf.data, new_sz);
        ctx->buf.sz = new_sz;
    }

    memcpy(&ctx->buf.data[ctx->buf.idx], data, size);
    ctx->buf.idx += size;

    char *start = ctx->buf.data;
    char *end = NULL;

    while (true) {
        for (end = start; end < &ctx->buf.data[ctx->buf.idx]; end++) {
            if (*end == '\r' || *end == '\n')
                break;
        }

        if (end >= &ctx->buf.data[ctx->buf.idx])
            break;

        decode_one_uri(ctx, start, end - start);
        start = end + 1;
    }

    const size_t ofs = start - ctx->buf.data;
    const size_t left = ctx->buf.idx - ofs;

    memmove(&ctx->buf.data[0], &ctx->buf.data[ofs], left);
    ctx->buf.idx = left;
}

static void
fdm_receive_finish_uri(struct clipboard_receive *ctx)
{
    LOG_DBG("finish: %.*s", (int)ctx->buf.idx, ctx->buf.data);
    decode_one_uri(ctx, ctx->buf.data, ctx->buf.idx);
}

static bool
fdm_receive(struct fdm *fdm, int fd, int events, void *data)
{
    struct clipboard_receive *ctx = data;

    if ((events & EPOLLHUP) && !(events & EPOLLIN))
        goto done;

    /* Reset timeout timer */
    if (timerfd_settime(ctx->timeout_fd, 0, &ctx->timeout, NULL) < 0) {
        LOG_ERRNO("failed to re-arm clipboard timeout timer");
        return false;
    }

    /* Read until EOF */
    while (true) {
        char text[256];
        ssize_t count = read(fd, text, sizeof(text));

        if (count == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return true;

            LOG_ERRNO("failed to read clipboard data");
            break;
        }

        if (count == 0)
            break;

        /*
         * Call cb while at same time replace:
         *   - \r\n -> \r  (non-bracketed paste)
         *   - \n -> \r    (non-bracketed paste)
         *   - C0 -> <nothing>  (strip non-formatting C0 characters)
         *   - \e -> <nothing>  (i.e. strip ESC)
         */
        char *p = text;
        size_t left = count;

#define skip_one()                              \
        do {                                    \
            ctx->decoder(ctx, p, i);            \
            assert(i + 1 <= left);             \
            p += i + 1;                         \
            left -= i + 1;                      \
        } while (0)

    again:
        for (size_t i = 0; i < left; i++) {
            switch (p[i]) {
            default:
                break;

            case '\n':
                skip_one();
                goto again;

            case '\r':
                /* Convert \r\n -> \r */
                if (i + 1 < left && p[i + 1] == '\n') {
                    i++;
                    skip_one();
                    goto again;
                }
                break;

            /* C0 non-formatting control characters (\b \t \n \r excluded) */
            case '\x01': case '\x02': case '\x03': case '\x04': case '\x05':
            case '\x06': case '\x07': case '\x0e': case '\x0f': case '\x10':
            case '\x11': case '\x12': case '\x13': case '\x14': case '\x15':
            case '\x16': case '\x17': case '\x18': case '\x19': case '\x1a':
            case '\x1b': case '\x1c': case '\x1d': case '\x1e': case '\x1f':
            case '\b': case '\x7f': case '\x00':
                skip_one();
                goto again;
            }
        }

        ctx->decoder(ctx, p, left);
        left = 0;
    }

#undef skip_one

done:
    ctx->finish(ctx);
    clipboard_receive_done(fdm, ctx);
    return true;
}

static void
begin_receive_clipboard(struct seat *seat, int read_fd,
                        enum data_offer_mime_type mime_type,
                        data_cb_t data_cb, done_cb_t done_cb, void *user)
{
    int timeout_fd = -1;
    struct clipboard_receive *ctx = NULL;

    int flags;
    if ((flags = fcntl(read_fd, F_GETFL)) < 0 ||
        fcntl(read_fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        LOG_ERRNO("failed to set O_NONBLOCK");
        goto err;
    }

    timeout_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timeout_fd < 0) {
        LOG_ERRNO("failed to create clipboard timeout timer FD");
        goto err;
    }

    const struct itimerspec timeout = {.it_value = {.tv_sec = 2}};
    if (timerfd_settime(timeout_fd, 0, &timeout, NULL) < 0) {
        LOG_ERRNO("failed to arm clipboard timeout timer");
        goto err;
    }

    ctx = xmalloc(sizeof(*ctx));
    *ctx = (struct clipboard_receive) {
        .seat = seat,
        .read_fd = read_fd,
        .timeout_fd = timeout_fd,
        .timeout = timeout,
        .quote_paths = true,
        .decoder = (mime_type == DATA_OFFER_MIME_URI_LIST
                    ? &fdm_receive_decoder_uri
                    : &fdm_receive_decoder_plain),
        .finish = (mime_type == DATA_OFFER_MIME_URI_LIST
                   ? &fdm_receive_finish_uri
                   : &fdm_receive_finish_plain),
        .data_cb = data_cb,
        .done_cb = done_cb,
        .user = user,
    };

    if (!fdm_add(seat->fdm, read_fd, EPOLLIN, &fdm_receive, ctx) ||
        !fdm_add(seat->fdm, timeout_fd, EPOLLIN, &fdm_receive_timeout, ctx))
    {
        goto err;
    }

    return;

err:
    free(ctx);
    fdm_del(seat->fdm, timeout_fd);
    fdm_del(seat->fdm, read_fd);
    done_cb(user);
}

static void
text_from_clipboard(struct seat *seat,
                    void (*cb)(char *data, size_t size, void *user),
                    void (*done)(void *user), void *user)
{
    struct wl_clipboard *clipboard = &seat->clipboard;
    if (clipboard->data_offer == NULL ||
        clipboard->mime_type == DATA_OFFER_MIME_UNSET)
    {
        done(user);
        return;
    }

    /* Prepare a pipe the other client can write its selection to us */
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) == -1) {
        LOG_ERRNO("failed to create pipe");
        done(user);
        return;
    }

    LOG_DBG("receive from clipboard: mime-type=%s",
            mime_type_map[clipboard->mime_type]);

    int read_fd = fds[0];
    int write_fd = fds[1];

    /* Give write-end of pipe to other client */
    wl_data_offer_receive(
        clipboard->data_offer, mime_type_map[clipboard->mime_type], write_fd);

    /* Don't keep our copy of the write-end open (or we'll never get EOF) */
    close(write_fd);
    begin_receive_clipboard(seat, read_fd, clipboard->mime_type, cb, done, user);
}

static void
text_from_primary(
    struct seat *seat,
    void (*cb)(char *data, size_t size, void *user),
    void (*done)(void *user),
    void *user)
{
    struct wl_primary *primary = &seat->primary;
    if (primary->data_offer == NULL ||
        primary->mime_type == DATA_OFFER_MIME_UNSET)
    {
        done(user);
        return;
    }

    /* Prepare a pipe the other client can write its selection to us */
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) == -1) {
        LOG_ERRNO("failed to create pipe");
        done(user);
        return;
    }

    LOG_DBG("receive from primary: mime-type=%s",
            mime_type_map[primary->mime_type]);

    int read_fd = fds[0];
    int write_fd = fds[1];

    /* Give write-end of pipe to other client */
    zwp_primary_selection_offer_v1_receive(
        primary->data_offer, mime_type_map[primary->mime_type], write_fd);

    /* Don't keep our copy of the write-end open (or we'll never get EOF) */
    close(write_fd);
    begin_receive_clipboard(seat, read_fd, primary->mime_type, cb, done, user);
}

void
paste_from_clipboard(struct seat *seat)
{
    struct wl_clipboard *clipboard = &seat->clipboard;
    if (clipboard->data_offer == NULL)
        return;

    if (seat->is_pasting)
        return;

    seat->is_pasting = true;
    text_from_clipboard(seat, &clipboard_data, &clipboard_done, seat);
}

void
paste_from_primary(struct seat *seat)
{
    struct wl_primary *primary = &seat->primary;
    if (primary->data_offer == NULL)
        return;

    if (seat->is_pasting)
        return;

    seat->is_pasting = true;
    text_from_primary(seat, &clipboard_data, &clipboard_done, seat);
}
