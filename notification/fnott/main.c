#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include <poll.h>
#include <assert.h>
#include <errno.h>
#include <locale.h>
#include <getopt.h>
#include <unistd.h>

#include <sys/stat.h>
#include <fcntl.h>

#define LOG_MODULE "main"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "ctrl.h"
#include "dbus.h"
#include "fdm.h"
#include "icon.h"
#include "wayland.h"
#include "version.h"

volatile sig_atomic_t aborted = 0;

static void
sig_handler(int signo)
{
    aborted = 1;
}

static void
print_usage(const char *prog)
{
    printf("Usage: %s\n"
           "       %s --version\n"
           "\n"
           "Options:\n"
           "  -c,--config=PATH                      load configuration from PATH ($XDG_CONFIG_HOME/fnott/fnott.ini)\n"
           "  -p,--print-pid=FILE|FD                print PID to file or FD\n"
           "  -l,--log-colorize=[never|always|auto] enable/disable colorization of log output on stderr\n"
           "  -s,--log-no-syslog                    disable syslog logging\n"
           "  -v,--version                          show the version number and quit\n",
           prog, prog);
}

static bool
print_pid(const char *pid_file, bool *unlink_at_exit)
{
    LOG_DBG("printing PID to %s", pid_file);

    errno = 0;
    char *end;
    int pid_fd = strtoul(pid_file, &end, 10);

    if (errno != 0 || *end != '\0') {
        if ((pid_fd = open(pid_file,
                           O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                           S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)) < 0) {
            LOG_ERRNO("%s: failed to open", pid_file);
            return false;
        } else
            *unlink_at_exit = true;
    }

    if (pid_fd >= 0) {
        char pid[32];
        snprintf(pid, sizeof(pid), "%u\n", getpid());

        ssize_t bytes = write(pid_fd, pid, strlen(pid));
        close(pid_fd);

        if (bytes < 0) {
            LOG_ERRNO("failed to write PID to FD=%u", pid_fd);
            return false;
        }

        LOG_DBG("wrote %zd bytes to FD=%d", bytes, pid_fd);
        return true;
    } else
        return false;
}

int
main(int argc, char *const *argv)
{
    static const struct option longopts[] = {
        {"config",        required_argument, 0, 'c'},
        {"print-pid",     required_argument, 0, 'p'},
        {"log-colorize",  optional_argument, 0, 'l'},
        {"log-no-syslog", no_argument,       0, 's'},
        {"version",       no_argument,       0, 'v'},
        {"help",          no_argument,       0, 'h'},
        {NULL,            no_argument,       0, 0},
    };

    bool unlink_pid_file = false;
    const char *pid_file = NULL;
    const char *conf_path = NULL;

    enum log_colorize log_colorize = LOG_COLORIZE_AUTO;
    bool log_syslog = true;

    while (true) {
        int c = getopt_long(argc, argv, ":c:p:l::svh", longopts, NULL);
        if (c == -1)
            break;

        switch (c) {
        case 'c':
            conf_path = optarg;
            break;

        case 'p':
            pid_file = optarg;
            break;

        case 'l':
            if (optarg == NULL || strcmp(optarg, "auto") == 0)
                log_colorize = LOG_COLORIZE_AUTO;
            else if (strcmp(optarg, "never") == 0)
                log_colorize = LOG_COLORIZE_NEVER;
            else if (strcmp(optarg, "always") == 0)
                log_colorize = LOG_COLORIZE_ALWAYS;
            else {
                fprintf(stderr, "%s: argument must be one of 'never', 'always' or 'auto'\n", optarg);
                return EXIT_FAILURE;
            }
            break;

        case 's':
            log_syslog = false;
            break;

        case 'v':
            printf("fnott version %s\n", FNOTT_VERSION);
            return EXIT_SUCCESS;

        case 'h':
            print_usage(argv[0]);
            return EXIT_SUCCESS;

        case ':':
            fprintf(stderr, "error: -%c: missing required argument\n", optopt);
            return EXIT_FAILURE;

        case '?':
            fprintf(stderr, "error: -%c: invalid option\n", optopt);
            return EXIT_FAILURE;
        }
    }

    log_init(log_colorize, log_syslog, LOG_FACILITY_DAEMON, LOG_CLASS_DEBUG);
    fcft_init((enum fcft_log_colorize)log_colorize, log_syslog,
              FCFT_LOG_CLASS_DEBUG);
    atexit(&fcft_fini);

    int ret = EXIT_FAILURE;

    struct config conf = {};
    struct fdm *fdm = NULL;
    struct ctrl *ctrl = NULL;
    struct dbus *bus = NULL;
    struct wayland *wayl = NULL;
    struct notif_mgr *mgr = NULL;
    icon_theme_list_t icon_theme = tll_init();

    if (!config_load(&conf, conf_path))
        goto err;

    icon_theme = icon_load_theme(conf.icon_theme_name, true);

    setlocale(LC_CTYPE, "");

    if ((fdm = fdm_init()) == NULL)
        goto err;

    if ((mgr = notif_mgr_new(&conf, fdm, &icon_theme)) == NULL)
        goto err;

    if ((wayl = wayl_init(&conf, fdm, mgr)) == NULL)
        goto err;

    if ((bus = dbus_init(&conf, fdm, wayl, mgr, &icon_theme)) == NULL)
        goto err;

    if ((ctrl = ctrl_init(fdm, mgr, bus)) == NULL)
        goto err;

    notif_mgr_configure(mgr, wayl, bus);

    const struct sigaction sigact = {
        .sa_handler = &sig_handler,
    };
    sigaction(SIGINT, &sigact, NULL);
    sigaction(SIGTERM, &sigact, NULL);

    if (pid_file != NULL) {
        if (!print_pid(pid_file, &unlink_pid_file))
            goto err;
    }

    dbus_dispatch_initial_pending(bus);

    while (!aborted) {
        wayl_flush(wayl);
        if (!fdm_poll(fdm))
            break;
    }

    if (aborted)
        ret = EXIT_SUCCESS;

err:
    icon_themes_destroy(icon_theme);
    ctrl_destroy(ctrl);
    notif_mgr_destroy(mgr);
    dbus_destroy(bus);
    wayl_destroy(wayl);
    fdm_destroy(fdm);
    config_destroy(conf);

    if (unlink_pid_file)
        unlink(pid_file);

    log_deinit();
    return ret;
}
