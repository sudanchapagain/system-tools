#include "ctrl.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/un.h>
#include <sys/types.h>

#include <tllist.h>

#define LOG_MODULE "ctrl"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "ctrl-protocol.h"
#include "dbus.h"

extern volatile sig_atomic_t aborted;

struct ctrl;
struct client {
    struct ctrl *ctrl;
    int fd;

    struct {
        union {
            struct ctrl_request cmd;
            uint8_t raw[sizeof(struct ctrl_request)];
        };
        size_t idx;
    } recv;
};

struct ctrl {
    struct fdm *fdm;
    struct notif_mgr *notif_mgr;
    struct dbus *bus;
    int server_fd;
    char *socket_path;
    tll(struct client) clients;
};


static bool
send_reply(int fd, const struct ctrl_reply *reply)
{
    LOG_DBG("client: FD=%d, reply: result=%s",
            fd, reply->result == CTRL_OK ? "ok" : "fail");

    if (write(fd, reply, sizeof(*reply)) != sizeof(*reply)) {
        LOG_ERRNO("client: FD=%d: failed to send reply", fd);
        return false;
    }

    return true;
}

static void
client_disconnected(struct ctrl *ctrl, int fd)
{
    LOG_DBG("client: FD=%d disconnected", fd);

    fdm_del(ctrl->fdm, fd);

    tll_foreach(ctrl->clients, it) {
        if (it->item.fd == fd) {
            tll_remove(ctrl->clients, it);
            break;
        }
    }
}

struct actions_cb_data {
    struct ctrl *ctrl;
    int client_fd;
};

static void
actions_complete(struct notif *notif, const char *action_id, void *data)
{
    struct actions_cb_data *info = data;
    struct ctrl *ctrl = info->ctrl;
    int fd = info->client_fd;

    struct ctrl_reply reply = {.result = CTRL_INVALID_ID};

    LOG_DBG("actions callback: notif=%u, ID=%s", notif_id, action_id);

    if (action_id == NULL)
        goto out;

    reply.result = notif_signal_action(notif, action_id) ? CTRL_OK : CTRL_ERROR;

out:
    send_reply(fd, &reply);
    client_disconnected(ctrl, fd);
    free(info);
}

static enum ctrl_result
actions_by_id(struct ctrl *ctrl, int fd, uint32_t id)
{
    struct notif *notif = notif_mgr_get_notif(ctrl->notif_mgr, id);
    if (notif == NULL)
        return CTRL_INVALID_ID;

    if (notif_action_count(notif) == 0)
        return CTRL_NO_ACTIONS;

    struct actions_cb_data *info = malloc(sizeof(*info));
    *info = (struct actions_cb_data){.ctrl = ctrl, .client_fd = fd};

    notif_select_action(notif, &actions_complete, info);
    return CTRL_OK;
}

static bool
fdm_client(struct fdm *fdm, int fd, int events, void *data)
{
    struct client *client = data;
    struct ctrl *ctrl = client->ctrl;

    assert(client->fd == fd);

    bool ret = false;
    uint32_t *ids = NULL;
    int64_t id_count = -1;

    size_t left = sizeof(client->recv.cmd) - client->recv.idx;
    ssize_t count = recv(fd, &client->recv.raw[client->recv.idx], left, 0);
    if (count < 0) {
        LOG_ERRNO("client: FD=%d: failed to receive command", fd);
        return false;
    }

    client->recv.idx += count;
    if (client->recv.idx < sizeof(client->recv.cmd)) {
        /* Haven’t received a full command yet */
        goto no_err;
    }

    assert(client->recv.idx == sizeof(client->recv.cmd));

    /* TODO: endianness */

    struct ctrl_reply reply;

    switch (client->recv.cmd.cmd) {
    case CTRL_QUIT:
        LOG_DBG("client: FD=%d, quit", fd);
        aborted = 1;
        reply.result = CTRL_OK;
        break;

    case CTRL_LIST:
        id_count = notif_mgr_get_ids(ctrl->notif_mgr, NULL, 0);
        LOG_INFO("got %"PRIi64" IDs", id_count);
        reply.result = id_count >= 0 ? CTRL_OK : CTRL_ERROR;

        if (id_count >= 0) {
            if (id_count > 0)
                ids = calloc(id_count, sizeof(ids[0]));
            notif_mgr_get_ids(ctrl->notif_mgr, ids, id_count);
        }
        break;

    case CTRL_PAUSE:
        LOG_DBG("client: FD=%d, pause", fd);
        notif_mgr_pause(ctrl->notif_mgr);
        reply.result = CTRL_OK;
        break;

    case CTRL_UNPAUSE:
        LOG_DBG("client: FD=%d, unpause", fd);
        notif_mgr_unpause(ctrl->notif_mgr);
        reply.result = CTRL_OK;
        break;

    case CTRL_DISMISS_BY_ID:
        LOG_DBG("client: FD=%d, dismiss by-id: %u", fd, client->recv.cmd.id);
        reply.result = notif_mgr_dismiss_id(ctrl->notif_mgr, client->recv.cmd.id)
            ? CTRL_OK : CTRL_INVALID_ID;
        break;

    case CTRL_DISMISS_ALL:
        LOG_DBG("client: FD=%d, dismiss all", fd);
        reply.result = notif_mgr_dismiss_all(ctrl->notif_mgr)
            ? CTRL_OK : CTRL_ERROR;
        break;

    case CTRL_ACTIONS_BY_ID:
        LOG_DBG("client: FD=%d, actions by-id: %u", fd, client->recv.cmd.id);
        if ((reply.result = actions_by_id(ctrl, fd, client->recv.cmd.id)) == CTRL_OK) {
            /* Action selection helper successfully started, wait for
             * response before sending reply to fnottctl */
            goto no_reply;
        }
        break;

    case CTRL_DISMISS_WITH_DEFAULT_ACTION_BY_ID: {
        LOG_DBG("client: FD=%d, dismiss-with-default-action: %u", fd, client->recv.cmd.id);

        struct notif *notif =
            notif_mgr_get_notif(ctrl->notif_mgr, client->recv.cmd.id);

        if (notif != NULL) {
            notif_signal_action(notif, "default");
            reply.result =
                notif_mgr_dismiss_id(ctrl->notif_mgr, client->recv.cmd.id)
                    ? CTRL_OK : CTRL_INVALID_ID;
        } else
            reply.result = CTRL_INVALID_ID;
        break;
    }
    }

    if (!send_reply(fd, &reply))
        goto err;

    if (reply.result == CTRL_OK && id_count >= 0) {
        if (write(fd, &id_count, sizeof(id_count)) != sizeof(id_count)) {
            LOG_ERRNO("failed to write 'list' response");
            goto err;
        }

        for (size_t i = 0; i < id_count; i++) {
            char *summary = NULL;
            uint32_t len = 0;

            const struct notif *notif = notif_mgr_get_notif(ctrl->notif_mgr, ids[i]);
            if (notif != NULL) {
                summary = notif_get_summary(notif);
                len = strlen(summary);
            }

            if (write(fd, &ids[i], sizeof(ids[i])) != sizeof(ids[i]) ||
                write(fd, &len, sizeof(len)) != sizeof(len) ||
                write(fd, summary != NULL ? summary : "", len) != len)
            {
                LOG_ERRNO("failed to write 'list' response");
                free(summary);
                goto err;
            }

            free(summary);
        }
    }

no_err:
    ret = true;

err:
    free(ids);

    if ((events & EPOLLHUP) || client->recv.idx >= sizeof(client->recv.cmd)) {
        /* Client disconnected */
        client_disconnected(ctrl, fd);
    }

    return ret;

no_reply:
    free(ids);
    return true;
}

static bool
fdm_server(struct fdm *fdm, int fd, int events, void *data)
{
    if (events & EPOLLHUP) {
        LOG_ERR("disconnected from controller UNIX socket");
        return false;
    }

    struct ctrl *ctrl = data;

    struct sockaddr_un addr;
    socklen_t addr_size = sizeof(addr);
    int client_fd = accept4(ctrl->server_fd, (struct sockaddr *)&addr, &addr_size, SOCK_CLOEXEC);

    if (client_fd == -1) {
        LOG_ERRNO("failed to accept client connection");
        return false;
    }

    LOG_DBG("client FD=%d connected", client_fd);

    tll_push_back(ctrl->clients, ((struct client){.ctrl = ctrl, .fd = client_fd}));
    struct client *client = &tll_back(ctrl->clients)
;
    if (!fdm_add(ctrl->fdm, client_fd, EPOLLIN, &fdm_client, client)) {
        LOG_ERR("failed to register client FD with FDM");
        tll_pop_back(ctrl->clients);
        close(client_fd);
        return false;
    }

    return true;
}

static char *
get_socket_path(void)
{
    const char *xdg_runtime = getenv("XDG_RUNTIME_DIR");
    if (xdg_runtime == NULL)
        return strdup("/tmp/fnott.sock");

    const char *wayland_display = getenv("WAYLAND_DISPLAY");
    if (wayland_display == NULL) {
        char *path = malloc(strlen(xdg_runtime) + 1 + strlen("fnott.sock") + 1);
        sprintf(path, "%s/fnott.sock", xdg_runtime);
        return path;
    }

    char *path = malloc(strlen(xdg_runtime) + 1 + strlen("fnott-.sock") + strlen(wayland_display) + 1);
    sprintf(path, "%s/fnott-%s.sock", xdg_runtime, wayland_display);
    return path;
}

struct ctrl *
ctrl_init(struct fdm *fdm, struct notif_mgr *notif_mgr, struct dbus *bus)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd == -1) {
        LOG_ERRNO("failed to create UNIX socket");
        return NULL;
    }

    char *sock_path = get_socket_path();
    if (sock_path == NULL)
        goto err;

    unlink(sock_path);

    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERRNO("%s: failed to bind", addr.sun_path);
        goto err;
    }

    if (listen(fd, 5) < 0) {
        LOG_ERRNO("%s: failed to listen", addr.sun_path);
        goto err;
    }

    struct ctrl *ctrl = malloc(sizeof(*ctrl));
    *ctrl = (struct ctrl){
        .fdm = fdm,
        .bus = bus,
        .notif_mgr = notif_mgr,
        .server_fd = fd,
        .socket_path = sock_path,
        .clients = tll_init(),
    };

    if (!fdm_add(fdm, fd, EPOLLIN, &fdm_server, ctrl)) {
        LOG_ERR("failed to register with FDM");
        goto err;
    }

    return ctrl;

err:
    if (sock_path)
        free(sock_path);
    if (fd != -1)
        close(fd);
    return NULL;
}

void
ctrl_destroy(struct ctrl *ctrl)
{
    if (ctrl == NULL)
        return;

    fdm_del(ctrl->fdm, ctrl->server_fd);

    if (ctrl->socket_path != NULL)
        unlink(ctrl->socket_path);
    free(ctrl->socket_path);

    free(ctrl);
}

int
ctrl_poll_fd(const struct ctrl *ctrl)
{
    return ctrl->server_fd;
}
