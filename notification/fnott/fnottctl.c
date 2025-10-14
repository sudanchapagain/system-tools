#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>

#include <sys/socket.h>
#include <sys/un.h>

#define LOG_MODULE "main"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "ctrl-protocol.h"
#include "version.h"

static void
print_usage(const char *prog)
{
    printf("Usage: %s dismiss | actions | dismiss-with-default-action [<id>]\n"
           "       %s list | pause | unpause | quit\n"
           "       %s --version\n"
           "\n"
           "Options:\n"
           "  id                          notification ID to dismiss or show actions for\n"
           "  -v,--version                show the version number and quit\n",
           prog, prog, prog);
}

int
main(int argc, char *const *argv)
{
    const char *const prog = argv[0];

    static const struct option longopts[] = {
        {"version", no_argument, 0, 'v'},
        {"help",    no_argument, 0, 'h'},
        {NULL,      no_argument, 0,   0},
    };

    while (true) {
        int c = getopt_long(argc, argv, "+:vh", longopts, NULL);
        if (c == -1)
            break;

        switch (c) {
        case 'v':
            printf("fnottctl version %s\n", FNOTT_VERSION);
            return EXIT_SUCCESS;

        case 'h':
            print_usage(prog);
            return EXIT_SUCCESS;

        case ':':
            fprintf(stderr, "error: -%c: missing required argument\n", optopt);
            return EXIT_FAILURE;

        case '?':
            fprintf(stderr, "error: -%c: invalid option\n", optopt);
            return EXIT_FAILURE;
        }
    }

    argc -= optind;
    argv += optind;

    if (argc < 1) {
        print_usage(prog);
        return EXIT_FAILURE;
    }

    log_init(LOG_COLORIZE_AUTO, false, LOG_FACILITY_USER, LOG_CLASS_DEBUG);

    bool have_id = argc >= 2;
    const char *cmd_word = argv[0];
    const char *id_str = have_id ? argv[1] : NULL;

    /* Which command should we execute? */
    enum ctrl_command cmd_type;
    if (strcmp(cmd_word, "quit") == 0)
        cmd_type = CTRL_QUIT;
    else if (strcmp(cmd_word, "dismiss") == 0) {
        cmd_type = have_id && strcmp(id_str, "all") == 0
            ? CTRL_DISMISS_ALL : CTRL_DISMISS_BY_ID;
    } else if (strcmp(cmd_word, "actions") == 0)
        cmd_type = CTRL_ACTIONS_BY_ID;
    else if (strcmp(cmd_word, "dismiss-with-default-action") == 0)
        cmd_type = CTRL_DISMISS_WITH_DEFAULT_ACTION_BY_ID;
    else if (strcmp(cmd_word, "list") == 0)
        cmd_type = CTRL_LIST;
    else if (strcmp(cmd_word, "pause") == 0)
        cmd_type = CTRL_PAUSE;
    else if (strcmp(cmd_word, "unpause") == 0)
        cmd_type = CTRL_UNPAUSE;
    else {
        LOG_ERR("%s: invalid command", cmd_word);
        return EXIT_FAILURE;
    }

    /* With which ID? */
    uint32_t id;
    switch (cmd_type) {
    case CTRL_DISMISS_BY_ID:
    case CTRL_ACTIONS_BY_ID:
    case CTRL_DISMISS_WITH_DEFAULT_ACTION_BY_ID:
        if (have_id) {
            char *end = NULL;
            errno = 0;

            id = strtoul(id_str, &end, 0);

            if (errno != 0 || *end != '\0') {
                LOG_ERR(
                    "%s: invalid notification ID (expected an integer)",
                    id_str);
                return EXIT_FAILURE;
            }
        } else
            id = 0;
        break;

    case CTRL_QUIT:
    case CTRL_LIST:
    case CTRL_PAUSE:
    case CTRL_UNPAUSE:
    case CTRL_DISMISS_ALL:
        id  = 0;
        break;
    }

    int ret = EXIT_FAILURE;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1) {
        LOG_ERRNO("failed to create socket");
        goto err;
    }

    bool connected = false;
    struct sockaddr_un addr = {.sun_family = AF_UNIX};

    const char *xdg_runtime = getenv("XDG_RUNTIME_DIR");
    if (xdg_runtime != NULL) {
        const char *wayland_display = getenv("WAYLAND_DISPLAY");
        if (wayland_display != NULL)
            snprintf(addr.sun_path, sizeof(addr.sun_path),
                     "%s/fnott-%s.sock", xdg_runtime, wayland_display);
        else
            snprintf(addr.sun_path, sizeof(addr.sun_path),
                     "%s/fnott.sock", xdg_runtime);

        if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) == 0)
            connected = true;
        else
            LOG_WARN("%s: failed to connect, will now try /tmp/fnott.sock", addr.sun_path);
    }

    if (!connected) {
        strncpy(addr.sun_path, "/tmp/fnott.sock", sizeof(addr.sun_path) - 1);
        if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
            LOG_ERRNO("failed to connect; is fnott running?");
            goto err;
        }
    }

    /* TODO: endianness */
    struct ctrl_request cmd = {
        .cmd = cmd_type,
        .id = id,
    };

    ssize_t sent = send(fd, &cmd, sizeof(cmd), 0);
    if (sent == -1 || sent != sizeof(cmd)) {
        LOG_ERRNO("failed to send command");
        goto err;
    }

    struct ctrl_reply reply;
    ssize_t rcvd = read(fd, &reply, sizeof(reply));
    if (rcvd != sizeof(reply)) {
        LOG_ERRNO("failed to read reply");
        goto err;
    }

    if (reply.result == CTRL_OK && cmd_type == CTRL_LIST) {
        uint64_t count;
        if (read(fd, &count, sizeof(count)) != sizeof(count)) {
            LOG_ERRNO("failed to read 'list' response");
            goto err;
        }

        for (size_t i = 0; i < count; i++) {
            uint32_t notif_id;
            uint32_t summary_len;
            if (read(fd, &notif_id, sizeof(notif_id)) != sizeof(notif_id) ||
                read(fd, &summary_len, sizeof(summary_len)) != sizeof(summary_len))
            {
                LOG_ERRNO("failed to read 'list' response");
                goto err;
            }

            char *summary = malloc(summary_len + 1);
            if (read(fd, summary, summary_len) != summary_len) {
                LOG_ERRNO("failed to read 'list' response");
                free(summary);
                goto err;
            }
            printf("%u: %.*s\n", notif_id, summary_len, summary);
        }
    }

    switch (reply.result) {
    case CTRL_OK:
        break;

    case CTRL_INVALID_ID:
        fprintf(stderr, "%u: invalid ID\n", id);
        break;

    case CTRL_NO_ACTIONS:
        fprintf(stderr, "%u: no actions\n", id);
        break;

    case CTRL_ERROR:
        fprintf(stderr, "unknown error\n");
        break;
    }

    ret = reply.result == CTRL_OK ? EXIT_SUCCESS : EXIT_FAILURE;

err:
    if (fd != -1)
        close(fd);
    return ret;
}
