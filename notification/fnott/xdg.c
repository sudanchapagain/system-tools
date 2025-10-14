#include "xdg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <limits.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>

#define LOG_MODULE "xdg"
#define LOG_ENABLE_DBG 0
#include "log.h"

xdg_data_dirs_t
xdg_data_dirs(void)
{
    xdg_data_dirs_t ret = tll_init();

    const char *xdg_data_home = getenv("XDG_DATA_HOME");
    if (xdg_data_home != NULL && xdg_data_home[0] != '\0') {
        int fd = open(xdg_data_home, O_RDONLY | O_DIRECTORY);
        if (fd >= 0) {
            struct xdg_data_dir d = {.fd = fd, .path = strdup(xdg_data_home)};
            tll_push_back(ret, d);
        }
    } else {
        static const char *const local = ".local/share";
        const struct passwd *pw = getpwuid(getuid());

        char *path = malloc(strlen(pw->pw_dir) + 1 + strlen(local) + 1);
        sprintf(path, "%s/%s", pw->pw_dir, local);

        int fd = open(path, O_RDONLY | O_DIRECTORY);
        if (fd >= 0) {
            struct xdg_data_dir d = {.fd = fd, .path = path};
            tll_push_back(ret, d);
        } else
            free(path);
    }

    const char *_xdg_data_dirs = getenv("XDG_DATA_DIRS");

    if (_xdg_data_dirs != NULL) {

        char *ctx = NULL;
        char *copy = strdup(_xdg_data_dirs);

        for (const char *tok = strtok_r(copy, ":", &ctx);
             tok != NULL;
             tok = strtok_r(NULL, ":", &ctx))
        {
            int fd = open(tok, O_RDONLY | O_DIRECTORY);
            if (fd >= 0) {
                struct xdg_data_dir d = {.fd = fd, .path = strdup(tok)};
                tll_push_back(ret, d);
            }
        }

        free(copy);
    } else {
        int fd1 = open("/usr/local/share", O_RDONLY | O_DIRECTORY);
        int fd2 = open("/usr/share", O_RDONLY | O_DIRECTORY);

        if (fd1 >= 0) {
            struct xdg_data_dir d = {.fd = fd1, .path = strdup("/usr/local/share")};
            tll_push_back(ret, d);
        }

        if (fd2 >= 0) {
            struct xdg_data_dir d = {.fd = fd2, .path = strdup("/usr/share")};
            tll_push_back(ret, d);;
        }
    }

    return ret;
}

void
xdg_data_dirs_destroy(xdg_data_dirs_t dirs)
{
    tll_foreach(dirs, it) {
        close(it->item.fd);
        free(it->item.path);
        tll_remove(dirs, it);
    }
}

const char *
xdg_cache_dir(void)
{
    const char *xdg_cache_home = getenv("XDG_CACHE_HOME");
    if (xdg_cache_home != NULL && xdg_cache_home[0] != '\0')
        return xdg_cache_home;

    static char path[PATH_MAX];
    const struct passwd *pw = getpwuid(getuid());
    snprintf(path, sizeof(path), "%s/.cache", pw->pw_dir);
    return path;
}
