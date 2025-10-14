#include "icon.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <limits.h>

#include <sys/stat.h>
#include <fcntl.h>

#include <tllist.h>
#include <nanosvg/nanosvg.h>
#include <nanosvg/nanosvgrast.h>


#define LOG_MODULE "icon"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "png-fnott.h"
#include "svg.h"
#include "xdg.h"

enum icon_type { ICON_NONE, ICON_PNG, ICON_SVG };

typedef tll(char *) theme_names_t;

static bool
dir_context_is_allowed(const char *context)
{
    static const char *const allowed_contexts[] = {"applications", "apps"};

    if (context == NULL)
        return NULL;

    for (size_t i = 0; i < sizeof(allowed_contexts) / sizeof(allowed_contexts[0]); i++) {
        if (strcasecmp(context, allowed_contexts[i]) == 0)
            return true;
    }

    return false;
}

static void
parse_theme(FILE *index, struct icon_theme *theme, bool filter_context,
            theme_names_t *themes_to_load)
{
    char *section = NULL;
    int size = -1;
    int min_size = -1;
    int max_size = -1;
    int scale = 1;
    int threshold = 2;
    char *context = NULL;
    enum icon_dir_type type = ICON_DIR_THRESHOLD;

    while (true) {
        char *line = NULL;
        size_t sz = 0;
        ssize_t len = getline(&line, &sz, index);

        if (len == -1) {
            free(line);
            break;
        }

        if (len == 0) {
            free(line);
            continue;
        }

        if (line[len - 1] == '\n') {
            line[len - 1] = '\0';
            len--;
        }

        if (len == 0) {
            free(line);
            continue;
        }

        if (line[0] == '[' && line[len - 1] == ']') {
            if (!filter_context || dir_context_is_allowed(context)) {
                tll_foreach(theme->dirs, it) {
                    struct icon_dir *d = &it->item;

                    if (section == NULL || strcmp(d->path, section) != 0)
                        continue;

                    d->size = size;
                    d->min_size = min_size >= 0 ? min_size : size;
                    d->max_size = max_size >= 0 ? max_size : size;
                    d->scale = scale;
                    d->threshold = threshold;
                    d->type = type;
                }
            }

            free(section);
            free(context);

            size = min_size = max_size = -1;
            scale = 1;
            section = NULL;
            context = NULL;
            type = ICON_DIR_THRESHOLD;
            threshold = 2;

            section = malloc(len - 2 + 1);
            memcpy(section, &line[1], len - 2);
            section[len - 2] = '\0';
            free(line);
            continue;
        }

        char *tok_ctx = NULL;

        const char *key = strtok_r(line, "=", &tok_ctx);
        char *value = strtok_r(NULL, "=", &tok_ctx);

        if (strcasecmp(key, "inherits") == 0) {
            char *ctx = NULL;
            for (const char *theme_name = strtok_r(value, ",", &ctx);
                 theme_name != NULL; theme_name = strtok_r(NULL, ",", &ctx))
            {
                tll_push_back(*themes_to_load, strdup(theme_name));
            }
        }

        if (strcasecmp(key, "directories") == 0) {
            char *save = NULL;
            for (const char *d = strtok_r(value, ",", &save);
                 d != NULL;
                 d = strtok_r(NULL, ",", &save))
            {
                struct icon_dir dir = {.path = strdup(d)};
                tll_push_back(theme->dirs, dir);
            }
        }

        else if (strcasecmp(key, "size") == 0)
            sscanf(value, "%d", &size);

        else if (strcasecmp(key, "minsize") == 0)
            sscanf(value, "%d", &min_size);

        else if (strcasecmp(key, "maxsize") == 0)
            sscanf(value, "%d", &max_size);

        else if (strcasecmp(key, "scale") == 0)
            sscanf(value, "%d", &scale);

        else if (strcasecmp(key, "context") == 0)
            context = strdup(value);

        else if (strcasecmp(key, "threshold") == 0)
            sscanf(value, "%d", &threshold);

        else if (strcasecmp(key, "type") == 0) {
            if (strcasecmp(value, "fixed") == 0)
                type = ICON_DIR_FIXED;
            else if (strcasecmp(value, "scalable") == 0)
                type = ICON_DIR_SCALABLE;
            else if (strcasecmp(value, "threshold") == 0)
                type = ICON_DIR_THRESHOLD;
            else {
                LOG_WARN(
                    "ignoring unrecognized icon theme directory type: %s",
                    value);
            }
        }

        free(line);
    }

    if (!filter_context || dir_context_is_allowed(context)) {
        tll_foreach(theme->dirs, it) {
            struct icon_dir *d = &it->item;

            if (section == NULL || strcmp(d->path, section) != 0)
                continue;

            d->size = size;
            d->min_size = min_size >= 0 ? min_size : size;
            d->max_size = max_size >= 0 ? max_size : size;
            d->scale = scale;
            d->threshold = threshold;
            d->type = type;
        }
    }

    tll_foreach(theme->dirs, it) {
        if (it->item.size == 0) {
            free(it->item.path);
            tll_remove(theme->dirs, it);
        }
    }

    free(section);
    free(context);
}

static bool
load_theme_in(const char *dir, struct icon_theme *theme,
              bool filter_context, theme_names_t *themes_to_load)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/index.theme", dir);

    FILE *index = fopen(path, "re");
    if (index == NULL)
        return false;

    parse_theme(index, theme, filter_context, themes_to_load);
    fclose(index);
    return true;
}

static bool
already_loaded_theme(const char *theme_name, icon_theme_list_t themes)
{
    tll_foreach(themes, it) {
        if (strcasecmp(it->item.name, theme_name) == 0) {
            return true;
        }
    }
    return false;
}

static void
discover_and_load_theme(const char *theme_name, xdg_data_dirs_t dirs,
                        theme_names_t *themes_to_load, bool filter_context,
                        icon_theme_list_t *themes)
{
    tll_foreach(dirs, dir_it) {
        char path[strlen(dir_it->item.path) + 1 +
                  strlen(theme_name) + 1];
        sprintf(path, "%s/%s", dir_it->item.path, theme_name);

        struct icon_theme theme = {0};
        if (load_theme_in(path, &theme, filter_context, themes_to_load)) {
            theme.name = strdup(theme_name);
            tll_push_back(*themes, theme);
        }
    }
}

static xdg_data_dirs_t
get_icon_dirs(void)
{
    /*
     * See https://specifications.freedesktop.org/icon-theme-spec/latest/#directory_layout
     */

    xdg_data_dirs_t dirs = xdg_data_dirs();

    tll_foreach(dirs, it) {
        struct xdg_data_dir *d = &it->item;
        int fd = openat(d->fd, "icons", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd >= 0) {
            char *p;
            if (asprintf(&p, "%s/icons", d->path) < 0) {
                tll_remove(dirs, it);
                continue;
            }

            free(d->path);
            close(d->fd);

            d->path = p;
            d->fd = fd;
        } else {
            free(d->path);
            close(d->fd);
            tll_remove(dirs, it);
        }
    }

    const char *home = getenv("HOME");
    if (home != NULL) {
        char *path;
        if (asprintf(&path, "%s/%s", home, ".icons") >= 0) {
            int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

            if (fd >= 0) {
                struct xdg_data_dir home_icons = {
                    .path = path,
                    .fd = fd,
                };

                tll_push_front(dirs, home_icons);
            } else
                free(path);
        }
    }

    {
        const char *path = "/usr/share/pixmaps";
        int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd >= 0) {
            struct xdg_data_dir home_icons = {
                .path = strdup(path),
                .fd = fd,
            };

            tll_push_back(dirs, home_icons);
        }
    }

    return dirs;
}

icon_theme_list_t
icon_load_theme(const char *name, bool filter_context)
{
    /* List of themes; first item is the primary theme, subsequent
     * items are inherited items (i.e. fallback themes) */
    icon_theme_list_t themes = tll_init();

    /* List of themes to try to load. This list will be appended to as
     * we go, and find 'Inherits' values in the theme index files. */
    theme_names_t themes_to_load = tll_init();
    tll_push_back(themes_to_load, strdup(name));

    xdg_data_dirs_t dirs = get_icon_dirs();

    while (tll_length(themes_to_load) > 0) {
        char *theme_name = tll_pop_front(themes_to_load);

        /*
         * Check if we've already loaded this theme. Example:
         * "Arc" inherits "Moka,Faba,elementary,Adwaita,gnome,hicolor
         * "Moka" inherits "Faba"
         * "Faba" inherits "elementary,gnome,hicolor"
         */
        if (already_loaded_theme(theme_name, themes)) {
            free(theme_name);
            continue;
        }

        discover_and_load_theme(theme_name, dirs, &themes_to_load, filter_context, &themes);
        free(theme_name);
    }

    /*
     * According to the freedesktop.org icon theme spec,
     * implementation are required to always fallback on hicolor, even
     * if it is not explicitly set in inheritance chain.  See
     * https://specifications.freedesktop.org/icon-theme-spec/icon-theme-spec-latest.html
     *
     * Thus, we add it in the end of the list, if it has not been
     * already added as part of the theme inheritance.
     */
    if (!already_loaded_theme("hicolor", themes)) {
        /*
         * hicolor has no dependency, thus the themes_to_load here is
         * assumed to stay empty and will be disregarded.
         */
        discover_and_load_theme("hicolor", dirs, &themes_to_load, filter_context, &themes);
    }

    xdg_data_dirs_destroy(dirs);
    return themes;
}

static void
theme_destroy(struct icon_theme theme)
{
    free(theme.name);

    tll_foreach(theme.dirs, it) {
        free(it->item.path);
        tll_remove(theme.dirs, it);
    }
}

void
icon_themes_destroy(icon_theme_list_t themes)
{
    tll_foreach(themes, it) {
        theme_destroy(it->item);
        tll_remove(themes, it);
    }
}

/*
 * Path is expected to contain the icon’s basename. It doesn’t have to
 * have the extension filled in; it will be filled in by this
 * function.
 *
 * Note that only supported image types are searched for. That is, if
 * PNGs have been disabled, we only search for SVGs.
 *
 * Also note that we only check for the existence of a file; we don’t
 * validate it.
 *
 * Returns true if there exist an icon file (of the specified name) in
 * <dir_fd>. In this case, path has been updated with the extension
 * (.png or .svg).
 */
static bool
icon_file_exists(int dir_fd, char *path, size_t path_len)
{
    path[path_len - 3] = 'p';
    path[path_len - 2] = 'n';
    path[path_len - 1] = 'g';

    if (faccessat(dir_fd, path, R_OK, 0) < 0) {
        path[path_len - 3] = 's';
        path[path_len - 2] = 'v';
        path[path_len - 1] = 'g';
        return faccessat(dir_fd, path, R_OK, 0) == 0;
    }

    return true;
}

pixman_image_t *
icon_load(const char *name, int icon_size, const icon_theme_list_t *themes)
{
    pixman_image_t *pix = NULL;

    struct icon_data {
        char *file_name;
        size_t file_name_len;

        struct {
            int diff;
            const struct xdg_data_dir *xdg_dir;
            const struct icon_theme *theme;
            const struct icon_dir *icon_dir;
            enum icon_type type;
        } min_diff;
    } icon_data = {0};

    if (name[0] == '/') {
        const size_t name_len = strlen(name);
        if (name[name_len - 3] == 's' &&
            name[name_len - 2] == 'v' &&
            name[name_len - 1] == 'g')
        {
            if ((pix = svg_load(name, icon_size)) != NULL) {
                LOG_DBG("%s: absolute path SVG", name);
                return pix;
            }
        } else if (name[name_len - 3] == 'p' &&
                   name[name_len - 2] == 'n' &&
                   name[name_len - 1] == 'g')
        {
            if ((pix = png_load(name)) != NULL) {
                LOG_DBG("%s: abslute path PNG", name);
                return pix;
            }
        }
    } else {
        if (asprintf(&icon_data.file_name, "%s.xxx", name) < 0)
            return NULL;

        icon_data.file_name_len = strlen(icon_data.file_name);
        icon_data.min_diff.diff = INT_MAX;
    }

    /* For details, see
     * https://specifications.freedesktop.org/icon-theme-spec/icon-theme-spec-latest.html#icon_lookup */

    xdg_data_dirs_t xdg_dirs = get_icon_dirs();

    tll_foreach(*themes, theme_it) {
        const struct icon_theme *theme = &theme_it->item;

        /* Fallback icon to use if there aren’t any exact matches */
        /* Assume sorted */
        tll_foreach(theme->dirs, icon_dir_it) {
            const struct icon_dir *icon_dir = &icon_dir_it->item;

            char theme_relative_path[
                strlen(theme->name) + 1 +
                strlen(icon_dir->path) + 1];

            sprintf(theme_relative_path, "%s/%s",
                    theme->name, icon_dir->path);

            tll_foreach(xdg_dirs, xdg_dir_it) {
                const struct xdg_data_dir *xdg_dir = &xdg_dir_it->item;

                if (icon_dir->scale > 1) {
                    /*
                     * Scaled dirs are physically bigger icons (more
                     * pixels), but with less details. See
                     *  - https://codeberg.org/dnkl/fuzzel/issues/459#issuecomment-2574718
                     *  - https://codeberg.org/dnkl/fuzzel/issues/459#issuecomment-2574720
                     * For details on why we're skipping these.
                     */
                    continue;
                }

                const int size = icon_dir->size;
                const int min_size = icon_dir->min_size;
                const int max_size = icon_dir->max_size;
                const int threshold = icon_dir->threshold;
                const enum icon_dir_type type = icon_dir->type;

                bool is_exact_match = false;
                int diff = INT_MAX;

                /* See if this directory is usable for the requested icon size */
                switch (type) {
                case ICON_DIR_FIXED:
                    is_exact_match = size == icon_size;
                    diff = abs(size - icon_size);
                    LOG_DBG(
                        "%s/%s (fixed): "
                        "icon-size=%d, size=%d, exact=%d, diff=%d",
                        xdg_dir->path, theme_relative_path, icon_size, size,
                        is_exact_match, diff);
                    break;

                case ICON_DIR_THRESHOLD:
                    is_exact_match =
                        (size - threshold) <= icon_size &&
                        (size + threshold) >= icon_size;
                    diff = icon_size < (size - threshold)
                        ? min_size - icon_size
                        : (icon_size > (size + threshold)
                           ? icon_size - max_size
                           : 0);
                    LOG_DBG(
                        "%s/%s (threshold): "
                        "icon-size=%d, threshold=%d, exact=%d, diff=%d",
                        xdg_dir->path, theme_relative_path, icon_size, threshold,
                        is_exact_match, diff);
                    break;

                case ICON_DIR_SCALABLE:
                    is_exact_match =
                        min_size <= icon_size &&
                        max_size >= icon_size;
                    diff = icon_size < min_size
                        ? min_size - icon_size
                        : (icon_size > max_size
                           ? icon_size - max_size
                           : 0);
                    LOG_DBG("%s/%s (scalable): "
                            "icon-size=%d, min=%d, max=%d, exact=%d, diff=%d",
                            xdg_dir->path, theme_relative_path, icon_size,
                            min_size, max_size, is_exact_match, diff);
                    break;
                }

                int dir_fd = openat(
                    xdg_dir->fd, theme_relative_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
                if (dir_fd < 0)
                    continue;

                if (!is_exact_match && icon_data.min_diff.diff <= diff) {
                    close(dir_fd);
                    continue;
                }

                size_t len = icon_data.file_name_len;
                char *path = icon_data.file_name;
                path[len - 4] = '.';

                if (icon_file_exists(dir_fd, path, len)) {
                    if (!is_exact_match) {
                        assert(diff < icon_data.min_diff.diff);
                        icon_data.min_diff.diff = diff;
                        icon_data.min_diff.xdg_dir = xdg_dir;
                        icon_data.min_diff.theme = theme;
                        icon_data.min_diff.icon_dir = icon_dir;
                        icon_data.min_diff.type = path[len - 3] == 's'
                            ? ICON_SVG : ICON_PNG;
                    } else {
                        char *full_path;

                        if (asprintf(
                            &full_path,
                            "%s/%s/%s/%s",
                            xdg_dir->path, theme->name, icon_dir->path, path) >= 0)
                        {
                            if ((path[len - 3] == 's' &&
                                 (pix = svg_load(full_path, icon_size)) != NULL) ||
                                (path[len - 3] == 'p' &&
                                 (pix = png_load(full_path)) != NULL))
                            {
                                LOG_DBG("%s: %s", name, full_path);
                                free(full_path);
                                close(dir_fd);
                                goto done;
                            }

                            free(full_path);
                        }
                    }
                }

                close(dir_fd);
           }
       }

        /* Try loading fallbacks for those icons we didn’t find an
         * exact match */
        if (icon_data.min_diff.type != ICON_NONE) {
            size_t path_len =
                strlen(icon_data.min_diff.xdg_dir->path) + 1 +
                strlen(icon_data.min_diff.theme->name) + 1 +
                strlen(icon_data.min_diff.icon_dir->path) + 1 +
                strlen(name) + 4;

            char full_path[path_len + 1];
            sprintf(full_path, "%s/%s/%s/%s.%s",
                    icon_data.min_diff.xdg_dir->path,
                    icon_data.min_diff.theme->name,
                    icon_data.min_diff.icon_dir->path,
                    name,
                    icon_data.min_diff.type == ICON_SVG ? "svg" : "png");

            if ((icon_data.min_diff.type == ICON_SVG &&
                 (pix = svg_load(full_path, icon_size)) != NULL) ||
                (icon_data.min_diff.type == ICON_PNG &&
                 (pix = png_load(full_path)) != NULL))
            {
                LOG_DBG("%s: %s (fallback)", name, full_path);
                goto done;
            } else {
                /* Reset diff data, before checking the parent theme(s) */
                icon_data.min_diff.diff = INT_MAX;
                icon_data.min_diff.xdg_dir = NULL;
                icon_data.min_diff.theme = NULL;
                icon_data.min_diff.icon_dir = NULL;
                icon_data.min_diff.type = ICON_NONE;
            }
        }
    }

    /* Finally, look in XDG_DATA_DIRS/pixmaps */
    tll_foreach(xdg_dirs, it) {
        size_t len = icon_data.file_name_len;
        char *path = icon_data.file_name;

        if (!icon_file_exists(it->item.fd, path, len))
            continue;

        char full_path[strlen(it->item.path) + 1 + len + 1];

        /* Try SVG variant first */
        sprintf(full_path, "%s/%s", it->item.path, path);
        if ((path[len - 3] == 's' && (pix = svg_load(full_path, icon_size)) != NULL) ||
            (path[len - 3] == 'p' && (pix = png_load(full_path)) != NULL))
        {
            LOG_DBG("%s: %s (standalone)", name, full_path);
            goto done;
        }
    }

done:
    free(icon_data.file_name);
    xdg_data_dirs_destroy(xdg_dirs);
    return pix;
}
