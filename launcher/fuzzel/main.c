#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <threads.h>

#include <locale.h>
#include <libgen.h>

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/time.h>
#include <fcntl.h>
#include <dirent.h>

#include <tllist.h>
#include <fcft/fcft.h>

#define LOG_MODULE "fuzzel"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "application.h"
#include "char32.h"
#include "config.h"
#include "dmenu.h"
#include "event.h"
#include "fdm.h"
#include "key-binding.h"
#include "match.h"
#include "path.h"
#include "render.h"
#include "shm.h"
#include "version.h"
#include "wayland.h"
#include "xdg.h"
#include "xmalloc.h"
#include "xsnprintf.h"

#include "timing.h"

#define max(x, y) ((x) > (y) ? (x) : (y))
#define min(x, y) ((x)  < (y) ? (x) : (y))

struct context {
    struct config *conf;
    const char *cache_path;
    struct wayland *wayl;
    struct render *render;
    struct matches *matches;
    struct application_list *apps;

    icon_theme_list_t *themes;
    int icon_size;
    mtx_t *icon_lock;

    const char *select_initial;
    const size_t select_initial_idx;

    int event_fd;
    int dmenu_abort_fd;

    struct {
        struct {
            struct timespec *start;
            struct timespec *stop;
        } apps;
        struct {
            struct timespec *start;
            struct timespec *stop;
        } icons_theme;
        struct {
            struct timespec *start;
            struct timespec *stop;
        } icons;
    } timing;
};

struct cache_entry {
    char *id;
    char32_t *title;
    size_t count;
};

static void
read_cache(const char *path, struct application_list *apps, bool dmenu)
{
    if (path == NULL && dmenu) {
        /* Don't thrash "normal" cache in dmenu mode */
        return;
    }

    struct stat st;
    FILE *f = NULL;

    if (path == NULL) {
        path = xdg_cache_dir();
        if (path == NULL) {
            LOG_WARN("failed to get cache directory: not saving popularity cache");
            return;
        }

        int cache_dir_fd = open(path, O_DIRECTORY | O_CLOEXEC);
        if (cache_dir_fd == -1) {
            LOG_ERRNO("%s: failed to open", path);
            return;
        }

        int fd = -1;
        if (fstatat(cache_dir_fd, "fuzzel", &st, 0) < 0 ||
            (fd = openat(cache_dir_fd, "fuzzel", O_RDONLY | O_CLOEXEC)) < 0 ||
            (f = fdopen(fd, "r")) == NULL)
        {
            close(cache_dir_fd);
            if (fd >= 0)
                close(fd);

            if (errno != ENOENT)
                LOG_ERRNO("%s/fuzzel: failed to open", path);
            return;
        }
        close(cache_dir_fd);
    } else {
        if (stat(path, &st) < 0 || (f = fopen(path, "re")) == NULL)
        {
            if (errno != ENOENT)
                LOG_ERRNO("%s: failed to open", path);
            return;
        }
    }

    tll(struct cache_entry) cache_entries = tll_init();

    size_t line_sz = 0;
    char *line = NULL;

    while (true) {
        errno = 0;
        ssize_t bytes = getline(&line, &line_sz, f);

        if (bytes < 0) {
            if (errno != 0) {
                LOG_ERRNO("failed to read cache");
                fclose(f);
                free(line);
                return;
            }

            /* Done */
            break;
        }

        if (line[bytes - 1] == '\n')
            line[bytes - 1] = '\0';

        /* Parse each line ("<title>|<count>") */
        char *ptr = NULL;
        const char *id = strtok_r(line, "|", &ptr);
        const char *count_str = strtok_r(NULL, "|", &ptr);

        if (id == NULL || count_str == NULL) {
            LOG_ERR("invalid cache entry (cache corrupt?): %s", line);
            continue;
        }

        int count;
        sscanf(count_str, "%u", &count);

        struct cache_entry entry = {
            .id = dmenu ? NULL : xstrdup(id),
            .title = dmenu ? ambstoc32(id) : NULL,
            .count = count,
        };
        tll_push_back(cache_entries, entry);
    }

    free(line);
    fclose(f);

    /* Loop all applications, and search for a matching cache entry */
    for (size_t i = 0; i < apps->count; i++) {
        struct application *app = apps->v[i];

        if ((!dmenu && app->id == NULL) || (dmenu && app->title == NULL)) {
            continue;
        }

        tll_foreach(cache_entries, it) {
            const struct cache_entry *e = &it->item;
            if ((!dmenu && e->id == NULL) || (dmenu && e->title == NULL))
                continue;

            if ((!dmenu && strcmp(app->id, e->id) == 0) ||
                (dmenu && c32cmp(app->title, e->title) == 0))
            {
                app->count = e->count;

                free(e->id);
                free(e->title);
                tll_remove(cache_entries, it);
                break;
            }
        }
    }

    /* Free all un-matched cache entries */
    tll_foreach(cache_entries, it) {
        free(it->item.id);
        tll_remove(cache_entries, it);
    }
}

static void
write_cache(const char *path, const struct application_list *apps, bool dmenu)
{
    if (path == NULL && dmenu) {
        /* Don't thrash "normal" cache in dmenu mode */
        return;
    }

    int fd = -1;

    if (path == NULL) {
        path = xdg_cache_dir();
        if (path == NULL) {
            LOG_WARN("failed to get cache directory: not saving popularity cache");
            return;
        }

        int cache_dir_fd = open(path, O_DIRECTORY | O_CLOEXEC);
        if (cache_dir_fd == -1) {
            LOG_ERRNO("%s: failed to open", path);
            return;
        }

        fd = openat(cache_dir_fd, "fuzzel", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        close(cache_dir_fd);
    } else {
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    }

    if (fd == -1) {
        LOG_ERRNO("%s/fuzzel: failed to open", path);
        return;
    }

    for (size_t i = 0; i < apps->count; i++) {
        if (apps->v[i]->count == 0)
            continue;

        if (!apps->v[i]->visible)
            continue;

        if (!dmenu && apps->v[i]->id == NULL)
            continue;

        if (dmenu && apps->v[i]->title == NULL)
            continue;

        if (!dmenu) {
            const char *id = apps->v[i]->id;
            const size_t id_len = strlen(id);

            if (write(fd, id, id_len) != id_len) {
                LOG_ERRNO("failed to write cache");
                break;
            }
        } else {
            const char32_t *title = apps->v[i]->title;
            char *u8_title = ac32tombs(title);

            if (u8_title != NULL) {
                const size_t title_len = strlen(u8_title);
                if (write(fd, u8_title, title_len) != title_len) {
                    LOG_ERRNO("failed to write cache");
                    break;
                }

                free(u8_title);
            }
        }

        char count_as_str[11];
        const size_t count_len = xsnprintf(
            count_as_str, sizeof(count_as_str), "%u", apps->v[i]->count);

        if (write(fd, "|", 1) != 1 ||
            write(fd, count_as_str, count_len) != count_len ||
            write(fd, "\n", 1) != 1)
        {
            LOG_ERRNO("failed to write cache");
            break;
        }
    }

    close(fd);
}

static const char *
version_and_features(void)
{
    static char buf[256];
    xsnprintf(buf, sizeof(buf), "version: %s %ccairo %cpng %csvg%s %cassertions",
             FUZZEL_VERSION,
#if defined(FUZZEL_ENABLE_CAIRO)
             '+',
#else
             '-',
#endif
#if defined(FUZZEL_ENABLE_PNG_LIBPNG)
             '+',
#else
             '-',
#endif
#if defined(FUZZEL_ENABLE_SVG_NANOSVG)
             '+', "(nanosvg)",
#elif defined(FUZZEL_ENABLE_SVG_LIBRSVG)
             '+', "(librsvg)",
#elif defined(FUZZEL_ENABLE_SVG_RESVG)
             '+', "(resvg)",
#else
             '-', "",
#endif
#if !defined(NDEBUG)
             '+'
#else
             '-'
#endif
        );
    return buf;
}

static void
print_usage(const char *prog_name)
{
    printf("Usage: %s [OPTION]...\n", prog_name);
    printf("\n");
    printf("Options:\n");
    printf("     --config=PATH               load configuration from PATH\n"
           "                                 (XDG_CONFIG_HOME/fuzzel/fuzzel.ini)\n"
           "     --check-config              verify configuration, exit with 0 if ok,\n"
           "                                 otherwise exit with 1\n"
           "     --cache=PATH                load most recently launched applications from\n"
           "                                 PATH (XDG_CACHE_HOME/fuzzel)\n"
           "  -n,--namespace=NAMESPACE       layer shell surface namespace\n"
           "  -o,--output=OUTPUT             output (monitor) to display on (none)\n"
           "  -f,--font=FONT                 font name and style, in FontConfig format\n"
           "                                 (monospace)\n"
           "     --use-bold                  allow fuzzel to use bold fonts\n"
           "  -D,--dpi-aware=no|yes|auto     enable or disable DPI aware rendering (auto)\n"
           "     --icon-theme=NAME           icon theme name (\"default\")\n"
           "  -I,--no-icons                  do not render any icons\n"
           "     --hide-before-typing    hide application list until something is typed\n"
           "  -F,--fields=FIELDS             comma separated list of XDG Desktop entry\n"
           "                                 fields to match\n"
           "  -p,--prompt=PROMPT             string to use as input prompt (\"> \")\n"
           "     --prompt-only=PROMPT        same as --prompt, but in dmenu mode it does not\n"
           "                                 wait for STDIN, and implies --lines=0\n"
           "     --placeholder=TEXT          placeholder text in input box\n"
           "     --search=TEXT               initial search/filter string\n"
           "     --password=[CHARACTER]      render all input as CHARACTER ('*' by default)\n"
           "  -T,--terminal                  terminal command to use when launching\n"
           "                                 'terminal' programs, e.g. \"xterm -e\".\n"
           "                                 Not used in dmenu mode (not set)\n"
           "  -a,--anchor                    window anchor (center)\n"
           "     --x-margin=MARGIN           horizontal margin, in pixels (0)\n"
           "     --y-margin=MARGIN           vertical margin, in pixels (0)\n"
           "     --select=STRING             select first entry that matches the given\n"
           "                                 string\n"
           "     --auto-select               automatically select when only one match\n"
           "                                 remains\n"
           "     --select-index=INDEX        select the entry with index, not compatible with --select\n"
           "  -l,--lines                     number of matches to show\n"
           "     --minimal-lines             adjust the number of lines to the\n"
           "                                 minimum of --lines and the number\n"
           "                                 of input lines (dmenu mode only)\n"
           "     --hide-prompt               hide the prompt line, making the window\n"
           "                                 smaller (input still accepted)\n"
           "  -w,--width                     window width, in characters (margins and\n"
           "                                 borders not included)\n"
           "     --tabs=INT                  number of spaces a tab is expanded to (8)\n"
           "  -x,--horizontal-pad=PAD        horizontal padding, in pixels (40)\n"
           "  -y,--vertical-pad=PAD          vertical padding, in pixels (8)\n"
           "  -P,--inner-pad=PAD             vertical padding between prompt and match list,\n"
           "                                 in pixels (0)\n"
           "  -b,--background-color=HEX      background color (fdf6e3ff)\n"
           "  -t,--text-color=HEX            text color of matched entries (657b83ff)\n"
           "     --prompt-color=HEX          text color of the prompt (586e75ff)\n"
           "     --placeholder-color=HEX     text color of the placeholder text (93a1a1)\n"
           "     --input-color=HEX           text color of the input string (657b83ff)\n"
           "  -m,--match-color=HEX           color of matched substring (cb4b16ff)\n"
           "  -s,--selection-color=HEX       background color of selected item (eee8d5ff)\n"
           "  -S,--selection-text-color=HEX  text color of selected item (586e75ff)\n"
           "  -M,--selection-match-color=HEX color of matched substring in selection\n"
           "                                 (cb4b16ff)\n"
           "     --selection-radius          border radius of the selected entry (0)\n"
           "     --counter-color             color of the match count (93a1a1)\n"
           "  -B,--border-width=INT          width of border, in pixels (1)\n"
           "  -r,--border-radius=INT         amount of corner \"roundness\" (10)\n"
           "  -C,--border-color=HEX          border color (002b36ff)\n"
           "     --show-actions              include desktop actions in the list\n"
           "     --match-mode=exact|fzf|fuzzy how to match what you type against the entries\n"
           "     --filter-desktop            filter desktop entries based on XDG_CURRENT\n"
           "     --fuzzy-min-length=VALUE    search strings shorter than this will not be\n"
           "                                 fuzzy matched (3)\n"
           "     --fuzzy-max-length-discrepancy=VALUE  maximum allowed length discrepancy\n"
           "                                           between a fuzzy match and the search\n"
           "                                           criteria. Larger values mean more\n"
           "                                           fuzzy matches (2)\n"
           "     --fuzzy-max-distance=VALUE  maximum levenshtein distance between a fuzzy\n"
           "                                 match and the search criteria. Larger values\n"
           "                                 mean more fuzzy matches\n"
           "     --line-height=HEIGHT        override line height from font metrics\n"
           "     --letter-spacing=AMOUNT     additional letter spacing\n"
           "     --layer=top|overlay         which layer to render the fuzzel window on\n"
           "                                 (top)\n"
           "     --keyboard-focus=exclusive|on-demand  keyboard focus mode (exclusive)\n"
           "     --no-exit-on-keyboard-focus-loss  do not exit when losing keyboard focus\n"
           "     --launch-prefix=COMMAND     prefix to add before argv of executed program\n"
           "     --list-executables-in-path  include executables from PATH in the list\n"
           "     --render-workers=N          number of threads to use for rendering\n"
           "     --match-workers=N           number of threads to use for matching\n"
           "     --no-sort                   do not sort the result\n"
           "     --counter                   display the match count\n"
           "     --delayed-filter-ms=TIME_MS time in ms before refiltering after a keystroke\n"
           "                                 (300)\n"
           "     --delayed-filter-limit=N    used delayed refiltering when the number of\n"
           "                                 matches exceeds this number (10000)\n"
           "     --scaling-filter=FILTER     filter to use when down scaling PNGs\n"
           "  -d,--dmenu                     dmenu compatibility mode\n"
           "     --dmenu0                    like --dmenu, but input is NUL separated\n"
           "                                 instead of newline separated\n"
           "     --index                     print selected entry's index instead of of the \n"
           "                                 entry's text (dmenu mode only)\n"
           "     --with-nth=N|FMT            display the N:th column (tab separated by\n"
           "                                 default) of each input line (dmenu only)\n"
           "     --accept-nth=N|FMT          output the N:th column (tab separated by\n"
           "                                 default) of each input line (dmenu only)\n"
           "     --match-nth=N|FMT           match against the N:th column (tab separated by\n"
           "                                 default) of each input line, instead of what is\n"
           "                                 being displayed (dmenu only)\n"
           "     --nth-delimiter=CHARACTER   field (column) character, for --with-nth and\n"
           "                                 --accept-nth\n"
           "     --only-match                do not allow custom entries, only return a\n"
           "                                 selected item\n"
           "  -R,--no-run-if-empty           exit immediately without showing UI if stdin\n"
           "                                 is empty (dmenu mode only)\n"
           "     --log-level={info|warning|error|none}\n"
           "                                 log level (warning)\n"
           "     --log-colorize=[never|always|auto]\n"
           "                                 enable/disable colorization of log output on\n"
           "                                 stderr\n"
           "     --log-no-syslog             disable syslog logging\n"
           "     --print-timing-info         print timing information, to help debug\n"
           "                                 performance issues\n"
           "     --no-mouse                  disable mouse input\n"
           "  -v,--version                   show the version number and quit\n");
    printf("\n");
    printf("All colors are RGBA - i.e. 8-digit hex values, without prefix.\n");
}

static void
font_reloaded(struct wayland *wayl, struct fcft_font *font, void *data)
{
    struct context *ctx = data;
    const struct config *conf = ctx->conf;

    applications_flush_text_run_cache(ctx->apps);
    render_flush_text_run_cache(ctx->render);

    mtx_lock(ctx->icon_lock);
    {
        ctx->icon_size = render_icon_size(ctx->render);

        if (conf->icons_enabled) {
            icon_lookup_application_icons(
                *ctx->themes, ctx->icon_size, ctx->apps);

            if (conf->dmenu.enabled) {
                dmenu_try_icon_list(ctx->apps, *ctx->themes, ctx->icon_size);
            }
        }
    }
    mtx_unlock(ctx->icon_lock);
}

static bool
pt_or_px_from_string(const char  *s, struct pt_or_px *res)
{
    const size_t len = strlen(s);
    if (len >= 2 && s[len - 2] == 'p' && s[len - 1] == 'x') {
        errno = 0;
        char *end = NULL;

        long value = strtol(s, &end, 10);
        if (!(errno == 0 && end == s + len - 2)) {
            fprintf(stderr, "%s: not a valid pixel value\n", s);
            return false;
        }

        res->pt = 0;
        res->px = value;
    } else {
        errno = 0;
        char *end = NULL;

        float value = strtof(s, &end);
        if (!(errno == 0 && *end == '\0')) {
            fprintf(stderr, "%s: not a valid point value\n", s);
            return false;
        }
        res->pt = value;
        res->px = 0;
    }

    return true;
}

static char *
lock_file_name(void)
{
    const char *xdg_runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (xdg_runtime_dir == NULL)
        xdg_runtime_dir = "/tmp";

    const char *wayland_display = getenv("WAYLAND_DISPLAY");
    if (wayland_display == NULL)
        return NULL;

    char *path = xasprintf("%s/fuzzel-%s.lock", xdg_runtime_dir, wayland_display);
    LOG_DBG("lock file: %s", path);
    return path;
}

static bool
acquire_file_lock(const char *path, int *fd)
{
    *fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (*fd < 0) {
        /* Warn, but allow running anyway */
        LOG_WARN("%s: failed to create lock file: %s", path, strerror(errno));
        return true;
    }

    if (flock(*fd, LOCK_EX | LOCK_NB) < 0) {
        if (errno == EWOULDBLOCK) {
            /* The file is locked and the LOCK_NB flag was selected */
            LOG_ERR("%s: failed to acquire lock: fuzzel already running?", path);
            return false;
        }
    }

    return true;
}

/* THREAD */
static int
populate_apps(void *_ctx)
{
    struct context *ctx = _ctx;
    const char *cache_path = ctx->cache_path;
    struct application_list *apps = ctx->apps;
    const struct config *conf = ctx->conf;
    const char *icon_theme = conf->icon_theme;
    const char *terminal = conf->terminal;
    bool actions_enabled = conf->actions_enabled;
    bool dmenu_enabled = conf->dmenu.enabled;
    bool icons_enabled = conf->icons_enabled;
    char dmenu_delim = conf->dmenu.delim;
    char dmenu_nth_delim = conf->dmenu.nth_delim;
    const char *dmenu_with_nth_format = conf->dmenu.with_nth_format;
    const char *dmenu_match_nth_format = conf->dmenu.match_nth_format;
    bool filter_desktop = conf->filter_desktop;
    bool list_exec_in_path = conf->list_executables_in_path;
    char_list_t desktops = tll_init();
    char *saveptr = NULL;

    if (filter_desktop) {
        char *xdg_current_desktop = getenv("XDG_CURRENT_DESKTOP");
        if (xdg_current_desktop && strlen(xdg_current_desktop) != 0) {
            xdg_current_desktop = xstrdup(xdg_current_desktop);
            for (char *desktop = strtok_r(xdg_current_desktop, ":", &saveptr);
                 desktop != NULL;
                 desktop = strtok_r(NULL, ":", &saveptr))
                {
                    tll_push_back(desktops, xstrdup(desktop));
                }
            free(xdg_current_desktop);
        }
    }

    ctx->timing.apps.start = time_begin();

    if (dmenu_enabled) {
        if (!conf->prompt_only) {
            dmenu_load_entries(
                apps, dmenu_delim, dmenu_with_nth_format, dmenu_match_nth_format,
                dmenu_nth_delim, ctx->event_fd, ctx->dmenu_abort_fd);
            read_cache(cache_path, apps, true);
        }
    } else {
        xdg_find_programs(terminal, actions_enabled, filter_desktop, &desktops, apps);
        if (list_exec_in_path)
            path_find_programs(apps);
        read_cache(cache_path, apps, false);
    }
    tll_free_and_free(desktops, free);

    ctx->timing.apps.stop = time_end();

    int r = send_event(ctx->event_fd, EVENT_APPS_ALL_LOADED);
    if (r != 0)
        return r;

    if (icons_enabled) {
        ctx->timing.icons_theme.start = time_begin();
        icon_theme_list_t icon_themes = icon_load_theme(icon_theme, !dmenu_enabled);
        if (tll_length(icon_themes) > 0)
            LOG_INFO("theme: %s", tll_front(icon_themes).name);
        else
            LOG_WARN("%s: icon theme not found", icon_theme);
        ctx->timing.icons_theme.stop = time_end();

        ctx->timing.icons.start = time_begin();
        mtx_lock(ctx->icon_lock);
        {
            *ctx->themes = icon_themes;
            if (ctx->icon_size > 0) {
                icon_lookup_application_icons(
                    *ctx->themes, ctx->icon_size, apps);

                if (dmenu_enabled) {
                    dmenu_try_icon_list(apps, *ctx->themes, ctx->icon_size);
                }
            }
        }
        mtx_unlock(ctx->icon_lock);
        ctx->timing.icons.stop = time_end();

        r = send_event(ctx->event_fd, EVENT_ICONS_LOADED);
        if (r != 0)
            return r;
    }

    return 0;
}

static bool
process_event(struct context *ctx, enum event_type event)
{
    const struct config *conf = ctx->conf;
    struct wayland *wayl = ctx->wayl;
    struct application_list *apps = ctx->apps;
    struct matches *matches = ctx->matches;
    const char *select = ctx->select_initial;
    const size_t select_idx = ctx->select_initial_idx;

    switch (event) {
    case EVENT_APPS_SOME_LOADED:
    case EVENT_APPS_ALL_LOADED: {
        if (event == EVENT_APPS_ALL_LOADED) {
            time_finish(
                ctx->timing.apps.start, ctx->timing.apps.stop, "apps loaded");
        }
        /* Update matches list, then refresh the GUI */
        matches_set_applications(matches, apps);

        if (event == EVENT_APPS_ALL_LOADED) {
            if (conf->dmenu.exit_immediately_if_empty && apps->count == 0)
                return false;

            matches_all_applications_loaded(matches);

            if (conf->dmenu.enabled && conf->minimal_lines) {
                const size_t effective_lines = min(apps->count, conf->lines);
                matches_max_matches_per_page_set(matches, effective_lines);
                ctx->conf->lines = effective_lines;
                wayl_resized(wayl);
            }

            wayl_ready_to_display(wayl);
        }

        const size_t match_count = matches_get_count(matches);
        const size_t matches_per_page = matches_max_matches_per_page(matches);

        if (conf->match_counter ||
            event == EVENT_APPS_ALL_LOADED ||
            match_count < matches_per_page)
        {
            /*
             * Only update matches if
             *   a) we're displaying a match counter
             *   b) all applications have been loaded
             *   c) a partial load will cause more matches to be displayed
             */
            matches_update_no_delay(matches);
            if (select_idx != 0) {
                if (!matches_selected_set(matches, select_idx)) {
                    LOG_ERR("couldn't select entry at index %zu", select_idx);
                    return false;
                }
            }
            else
                matches_selected_select(matches, select);
        }

        /*
         * Allow displaying the menu if:
         *   --no-run-if-empty is NOT enabled, OR we have at least one entry
         * AND
         *   --minimal-lines is NOT enabled, OR we have at least --lines number of entries
         *
         * In short, display the window as soon as we are sure we
         * won't exit automatically (due to --no-run-if-empty), and we
         * know its final size.
         */
        if ((!conf->dmenu.exit_immediately_if_empty || apps->count > 0) &&
            (!(conf->dmenu.enabled && conf->minimal_lines) ||
             apps->count >= conf->lines))
        {
            wayl_ready_to_display(wayl);
        }
        break;
    }

    case EVENT_ICONS_LOADED:
        /* Just need to refresh the GUI */
        time_finish(ctx->timing.icons_theme.start, ctx->timing.icons_theme.stop,
                    "icon themes loaded");
        time_finish(ctx->timing.icons.start, ctx->timing.icons.stop,
                    "icon paths resolved");
        matches_icons_loaded(matches);
        break;

    default:
        LOG_ERR("unknown event: %llx", (long long)event);
        return false;
    }

    wayl_refresh(wayl);

    if (event == EVENT_APPS_ALL_LOADED)
        wayl_check_auto_select(wayl);

    return true;
}

/*
 * Called when the application list has been populated
 */
static bool
fdm_apps_populated(struct fdm *fdm, int fd, int events, void *data)
{
    struct context *ctx = data;
    uint64_t last_event = EVENT_INVALID;
    uint64_t event = EVENT_INVALID;

    while (true) {
        ssize_t bytes = read(fd, &event, sizeof(event));
        if (bytes == 0) {
            if (event != EVENT_INVALID) {
                if (!process_event(ctx, event))
                    return false;
            }
            break;
        }

        if (bytes != (ssize_t)sizeof(event)) {
            if (bytes < 0) {
                if (errno == EAGAIN) {
                    if (event != EVENT_INVALID) {
                        if (!process_event(ctx, event))
                            return false;
                    }
                    break;
                }
                LOG_ERRNO("failed to read event FD");
            } else
                LOG_ERR("partial read from event FD");
            return false;
        }

        /* Coalesce multiple, identical events */
        if (last_event != EVENT_INVALID) {
            if (event != last_event) {
                if (!process_event(ctx, last_event))
                    return false;
            }
        }

        last_event = event;
    }

    return true;
}

int
main(int argc, char *const *argv)
{
    time_init();

    #define OPT_LETTER_SPACING               256
    #define OPT_LAUNCH_PREFIX                257
    #define OPT_SHOW_ACTIONS                 258
    #define OPT_MATCH_MODE                   259
    #define OPT_FUZZY_MIN_LENGTH             260
    #define OPT_FUZZY_MAX_LENGTH_DISCREPANCY 261
    #define OPT_FUZZY_MAX_DISTANCE           262
    #define OPT_DMENU_INDEX                  263
    #define OPT_LOG_LEVEL                    264
    #define OPT_LOG_COLORIZE                 265
    #define OPT_LOG_NO_SYSLOG                266
    #define OPT_PASSWORD                     267
    #define OPT_CONFIG                       268
    #define OPT_LAYER                        269
    #define OPT_ICON_THEME                   270
    #define OPT_KEYBOARD_FOCUS               271
    #define OPT_NO_EXIT_ON_KB_LOSS           272
    #define OPT_TABS                         273
    #define OPT_DMENU_NULL                   274
    #define OPT_FILTER_DESKTOP               275
    #define OPT_CHECK_CONFIG                 276
    #define OPT_SELECT                       277
    #define OPT_SELECT_INDEX                 278
    #define OPT_LIST_EXECS_IN_PATH           279
    #define OPT_X_MARGIN                     280
    #define OPT_Y_MARGIN                     281
    #define OPT_CACHE                        282
    #define OPT_RENDER_WORKERS               283
    #define OPT_PROMPT_COLOR                 284
    #define OPT_INPUT_COLOR                  285
    #define OPT_PROMPT_ONLY                  286
    #define OPT_COUNTER_COLOR                287
    #define OPT_USE_BOLD                     288
    #define OPT_MATCH_WORKERS                289
    #define OPT_NO_SORT                      290
    #define OPT_DELAYED_FILTER_MS            291
    #define OPT_DELAYED_FILTER_LIMIT         292
    #define OPT_PLACEHOLDER                  293
    #define OPT_PLACEHOLDER_COLOR            294
    #define OPT_SEARCH_TEXT                  295
    #define OPT_COUNTER                      296
    #define OPT_HIDE_WHEN_PROMPT_EMPTY       297
    #define OPT_DMENU_WITH_NTH               298
    #define OPT_DMENU_ACCEPT_NTH             299
    #define OPT_GAMMA_CORRECT                300
    #define OPT_PRINT_TIMINGS                301
    #define OPT_SCALING_FILTER               302
    #define OPT_MINIMAL_LINES                303
    #define OPT_HIDE_PROMPT                  304
    #define OPT_AUTO_SELECT                  305
    #define OPT_SELECTION_RADIUS             306
    #define OPT_NO_MOUSE                     307
    #define OPT_DMENU_NTH_DELIM              308
    #define OPT_DMENU_MATCH_NTH              309
    #define OPT_DMENU_ONLY_MATCH             310

    static const struct option longopts[] = {
        {"config",               required_argument, 0, OPT_CONFIG},
        {"check-config",         no_argument,       0, OPT_CHECK_CONFIG},
        {"namespace",            required_argument, 0, 'n'},
        {"cache",                required_argument, 0, OPT_CACHE},
        {"output"  ,             required_argument, 0, 'o'},
        {"font",                 required_argument, 0, 'f'},
        {"use-bold",             no_argument,       0, OPT_USE_BOLD},
        {"dpi-aware",            required_argument, 0, 'D'},
        {"gamma-correct",        no_argument,       0, OPT_GAMMA_CORRECT},
        {"icon-theme",           required_argument, 0, OPT_ICON_THEME},
        {"no-icons",             no_argument,       0, 'I'},
        {"hide-before-typing", no_argument,     0, OPT_HIDE_WHEN_PROMPT_EMPTY},
        {"fields",               required_argument, 0, 'F'},
        {"password",             optional_argument, 0, OPT_PASSWORD},
        {"anchor",               required_argument, 0, 'a'},
        {"x-margin",             required_argument, 0, OPT_X_MARGIN},
        {"y-margin",             required_argument, 0, OPT_Y_MARGIN},
        {"select",               required_argument, 0, OPT_SELECT},
        {"select-index",         required_argument, 0, OPT_SELECT_INDEX},
        {"lines",                required_argument, 0, 'l'},
        {"minimal-lines",        no_argument,       0, OPT_MINIMAL_LINES},
        {"hide-prompt",          no_argument,       0, OPT_HIDE_PROMPT},
        {"width",                required_argument, 0, 'w'},
        {"tabs",                 required_argument, 0, OPT_TABS},
        {"horizontal-pad",       required_argument, 0, 'x'},
        {"vertical-pad",         required_argument, 0, 'y'},
        {"inner-pad",            required_argument, 0, 'P'},
        {"background-color",     required_argument, 0, 'b'},
        {"text-color",           required_argument, 0, 't'},
        {"prompt-color",         required_argument, 0, OPT_PROMPT_COLOR},
        {"placeholder-color",    required_argument, 0, OPT_PLACEHOLDER_COLOR},
        {"input-color",          required_argument, 0, OPT_INPUT_COLOR},
        {"match-color",          required_argument, 0, 'm'},
        {"selection-color",      required_argument, 0, 's'},
        {"selection-text-color", required_argument, 0, 'S'},
        {"selection-match-color",required_argument, 0, 'M'},
        {"selection-radius",     required_argument, 0, OPT_SELECTION_RADIUS},
        {"counter-color",        required_argument, 0, OPT_COUNTER_COLOR},
        {"border-width",         required_argument, 0, 'B'},
        {"border-radius",        required_argument, 0, 'r'},
        {"border-color",         required_argument, 0, 'C'},
        {"prompt",               required_argument, 0, 'p'},
        {"prompt-only",          required_argument, 0, OPT_PROMPT_ONLY},
        {"placeholder",          required_argument, 0, OPT_PLACEHOLDER},
        {"search",               required_argument, 0, OPT_SEARCH_TEXT},
        {"terminal",             required_argument, 0, 'T'},
        {"show-actions",         no_argument,       0, OPT_SHOW_ACTIONS},
        {"match-mode",           required_argument, 0, OPT_MATCH_MODE},
        {"filter-desktop",       optional_argument, 0, OPT_FILTER_DESKTOP},
        {"fuzzy-min-length",     required_argument, 0, OPT_FUZZY_MIN_LENGTH},
        {"fuzzy-max-length-discrepancy", required_argument, 0, OPT_FUZZY_MAX_LENGTH_DISCREPANCY},
        {"fuzzy-max-distance",   required_argument, 0, OPT_FUZZY_MAX_DISTANCE},
        {"line-height",          required_argument, 0, 'H'},
        {"letter-spacing",       required_argument, 0, OPT_LETTER_SPACING},
        {"launch-prefix",        required_argument, 0, OPT_LAUNCH_PREFIX},
        {"layer",                required_argument, 0, OPT_LAYER},
        {"keyboard-focus",       required_argument, 0, OPT_KEYBOARD_FOCUS},
        {"no-exit-on-keyboard-focus-loss",   no_argument, 0, OPT_NO_EXIT_ON_KB_LOSS},
        {"list-executables-in-path",         no_argument, 0, OPT_LIST_EXECS_IN_PATH},
        {"render-workers",       required_argument, 0, OPT_RENDER_WORKERS},
        {"match-workers",        required_argument, 0, OPT_MATCH_WORKERS},
        {"no-sort",              no_argument,       0, OPT_NO_SORT},
        {"counter",              no_argument,       0, OPT_COUNTER},
        {"delayed-filter-ms",    required_argument, 0, OPT_DELAYED_FILTER_MS},
        {"delayed-filter-limit", required_argument, 0, OPT_DELAYED_FILTER_LIMIT},
        {"scaling-filter",       required_argument, 0, OPT_SCALING_FILTER},
        {"auto-select",          no_argument,       0, OPT_AUTO_SELECT},
        {"no-mouse",             no_argument,       0, OPT_NO_MOUSE},

        /* dmenu mode */
        {"dmenu",                no_argument,       0, 'd'},
        {"dmenu0",               no_argument,       0, OPT_DMENU_NULL},
        {"no-run-if-empty",      no_argument,       0, 'R'},
        {"index",                no_argument,       0, OPT_DMENU_INDEX},
        {"nth-delimiter",        required_argument, 0, OPT_DMENU_NTH_DELIM},
        {"with-nth",             required_argument, 0, OPT_DMENU_WITH_NTH},
        {"accept-nth",           required_argument, 0, OPT_DMENU_ACCEPT_NTH},
        {"match-nth",            required_argument, 0, OPT_DMENU_MATCH_NTH},
        {"only-match",           no_argument,       0, OPT_DMENU_ONLY_MATCH},

        /* Misc */
        {"log-level",            required_argument, 0, OPT_LOG_LEVEL},
        {"log-colorize",         optional_argument, 0, OPT_LOG_COLORIZE},
        {"log-no-syslog",        no_argument,       0, OPT_LOG_NO_SYSLOG},
        {"version",              no_argument,       0, 'v'},
        {"print-timing-info",    no_argument,       0, OPT_PRINT_TIMINGS},
        {"help",                 no_argument,       0, 'h'},
        {NULL,                   no_argument,       0, 0},
    };

    bool check_config = false;
    const char *config_path = NULL;
    enum log_class log_level = LOG_CLASS_WARNING;
    enum log_colorize log_colorize = LOG_COLORIZE_AUTO;
    bool log_syslog = true;
    const char *select = NULL;
    size_t select_idx = 0;

    struct {
        struct config conf;

        bool dpi_aware_set:1;
        bool gamma_correct_set:1;
        bool match_fields_set:1;
        bool icons_disabled_set:1;
        bool hide_when_prompt_empty_set:1;
        bool anchor_set:1;
        bool x_margin_set : 1;
        bool y_margin_set : 1;
        bool lines_set:1;
        bool minimal_lines_set:1;
        bool hide_prompt_set:1;
        bool chars_set:1;
        bool tabs_set:1;
        bool pad_x_set:1;
        bool pad_y_set:1;
        bool pad_inner_set:1;
        bool background_color_set:1;
        bool text_color_set:1;
        bool prompt_color_set:1;
        bool input_color_set:1;
        bool match_color_set:1;
        bool selection_color_set:1;
        bool selection_text_color_set:1;
        bool selection_match_color_set:1;
        bool selection_border_radius_set:1;
        bool counter_color_set:1;
        bool border_color_set:1;
        bool border_size_set:1;
        bool border_radius_set:1;
        bool actions_enabled_set:1;
        bool filter_desktop_set:1;
        bool match_mode_set:1;
        bool fuzzy_min_length_set:1;
        bool fuzzy_max_length_discrepancy_set:1;
        bool fuzzy_max_distance_set:1;
        bool line_height_set:1;
        bool letter_spacing_set:1;
        bool dmenu_enabled_set:1;
        bool dmenu_mode_set:1;
        bool dmenu_exit_immediately_if_empty_set:1;
        bool dmenu_delim_set:1;
        bool dmenu_nth_delim_set:1;
        bool dmenu_with_nth_set:1;
        bool dmenu_accept_nth_set:1;
        bool dmenu_match_nth_set:1;
        bool dmenu_only_match_set:1;
        bool layer_set:1;
        bool keyboard_focus_set:1;
        bool no_exit_on_keyboard_focus_loss_set:1;
        bool render_workers_set:1;
        bool match_workers_set:1;
        bool prompt_only_set:1;
        bool use_bold_set:1;
        bool no_sort_set:1;
        bool delayed_filter_ms_set:1;
        bool delayed_filter_limit_set:1;
        bool placeholder_color_set:1;
        bool counter_set:1;
        bool print_timing_info_set:1;
        bool scaling_filter_set:1;
        bool no_mouse_set:1;
    } cmdline_overrides = {{0}};

    setlocale(LC_CTYPE, "");
    setlocale(LC_MESSAGES, "");

    /* Auto-enable dmenu mode if invoked through a ‘dmenu’ symlink */
    if (argv[0] != NULL) {
        char *copy = xstrdup(argv[0]);
        const char *name = basename(copy);
        if (name != NULL && strcmp(name, "dmenu") == 0) {
            cmdline_overrides.conf.dmenu.enabled = true;
            cmdline_overrides.dmenu_enabled_set = true;
        }

        free(copy);
    }

    while (true) {
        int c = getopt_long(argc, argv, ":n:o:f:D:IF:ia:l:w:x:y:p:P:b:t:m:s:S:M:B:r:C:T:dRvh", longopts, NULL);
        if (c == -1)
            break;

        switch (c) {
        case OPT_CONFIG:
            config_path = optarg;
            break;

        case OPT_CHECK_CONFIG:
            check_config = true;
            break;

        case 'n':
            cmdline_overrides.conf.namespace = optarg;
            break;

        case OPT_CACHE:
            cmdline_overrides.conf.cache_path = optarg;
            break;

        case 'o':
            cmdline_overrides.conf.output = optarg;
            break;

        case 'f':
            cmdline_overrides.conf.font = optarg;
            break;

        case OPT_USE_BOLD:
            cmdline_overrides.conf.use_bold = true;
            cmdline_overrides.use_bold_set = true;
            break;

        case 'D':
            if (strcmp(optarg, "auto") == 0)
                cmdline_overrides.conf.dpi_aware = DPI_AWARE_AUTO;
            else if (strcmp(optarg, "no") == 0)
                cmdline_overrides.conf.dpi_aware = DPI_AWARE_NO;
            else if (strcmp(optarg, "yes") == 0)
                cmdline_overrides.conf.dpi_aware = DPI_AWARE_YES;
            else {
                fprintf(
                    stderr,
                    "%s: invalid value for dpi-aware: "
                    "must be one of 'auto', 'no', or 'yes'\n",
                    optarg);
                return EXIT_FAILURE;
            }
            cmdline_overrides.dpi_aware_set = true;
            break;

        case OPT_GAMMA_CORRECT:
            cmdline_overrides.conf.gamma_correct = true;
            cmdline_overrides.gamma_correct_set = true;
            break;

        case 'i':
            /* Ignore (this flag means case insensitive search in
             * other fuzzel-like utilities) */
            break;

        case OPT_ICON_THEME:
            cmdline_overrides.conf.icon_theme = optarg;
            break;

        case 'I':
            cmdline_overrides.conf.icons_enabled = false;
            cmdline_overrides.icons_disabled_set = true;
            break;

        case OPT_HIDE_WHEN_PROMPT_EMPTY:
            cmdline_overrides.conf.hide_when_prompt_empty = true;
            cmdline_overrides.hide_when_prompt_empty_set = true;
            break;

        case 'F': {
            static const struct {
                const char *name;
                enum match_fields value;
            } map[] = {
                {"filename", MATCH_FILENAME},
                {"name", MATCH_NAME},
                {"generic", MATCH_GENERIC},
                {"exec", MATCH_EXEC},
                {"categories", MATCH_CATEGORIES},
                {"keywords", MATCH_KEYWORDS},
                {"comment", MATCH_COMMENT},
            };

            cmdline_overrides.conf.match_fields = 0;
            for (const char *f = strtok(optarg, ", ");
                 f != NULL;
                 f = strtok(NULL, ", "))
            {
                enum match_fields field = 0;

                for (size_t i = 0; i < ALEN(map); i++) {
                    if (strcmp(f, map[i].name) == 0) {
                        field = map[i].value;
                        break;
                    }
                }

                if (field > 0)
                    cmdline_overrides.conf.match_fields |= field;
                else {
                    char valid_names[128] = {0};
                    size_t idx = 0;
                    for (size_t i = 0; i < ALEN(map); i++) {
                        idx += xsnprintf(
                            &valid_names[idx], sizeof(valid_names) - idx,
                            "'%s', ", map[i].name);
                    }

                    valid_names[idx - 2] = '\0';

                    fprintf(
                        stderr,
                        "%s: invalid field name: must be one of %s\n",
                        f, valid_names);
                    return EXIT_FAILURE;
                }
            }
            cmdline_overrides.match_fields_set = true;
            break;
        }

        case OPT_PASSWORD: {
            char32_t password_char = U'\0';
            if (optarg != NULL) {
                char32_t *wide_optarg = ambstoc32(optarg);
                if (wide_optarg == NULL){
                    fprintf(stderr, "%s: invalid password character\n", optarg);
                    return EXIT_FAILURE;
                }

                if (c32len(wide_optarg) > 1) {
                    fprintf(
                        stderr,
                        "%s: password character must be a single character\n",
                        optarg);
                    free(wide_optarg);
                    return EXIT_FAILURE;
                }

                password_char = wide_optarg[0];
                free(wide_optarg);
                cmdline_overrides.conf.password_mode.character_set = true;
            }

            cmdline_overrides.conf.password_mode.enabled = true;
            cmdline_overrides.conf.password_mode.character = password_char;
            break;
        }

        case 'T':
            cmdline_overrides.conf.terminal = optarg;
            break;

        case 'a': {
            enum anchors anchor;
            bool valid_anchor = false;

            for (size_t i = 0; anchors_map[i].name != NULL; i++) {
                if (strcmp(optarg, anchors_map[i].name) == 0) {
                    anchor = anchors_map[i].value;
                    valid_anchor = true;
                    break;
                }
            }

            if (!valid_anchor) {
                fprintf(stderr, "%s: invalid anchor\n", optarg);
                return EXIT_FAILURE;
            }

            cmdline_overrides.conf.anchor = anchor;
            cmdline_overrides.anchor_set = true;
            break;
        }

        case OPT_X_MARGIN:
          if (sscanf(optarg, "%u", &cmdline_overrides.conf.margin.x) != 1) {
            fprintf(stderr, "%s: invalid horizontal margin\n", optarg);
            return EXIT_FAILURE;
          }
          cmdline_overrides.x_margin_set = true;
          break;

        case OPT_Y_MARGIN:
          if (sscanf(optarg, "%u", &cmdline_overrides.conf.margin.y) != 1) {
            fprintf(stderr, "%s: invalid vertical margin\n", optarg);
            return EXIT_FAILURE;
          }
          cmdline_overrides.y_margin_set = true;
          break;

        case OPT_SELECT:
            select = optarg;
            break;

        case OPT_SELECT_INDEX:
            if (sscanf(optarg, "%zu", &select_idx) != 1) {
                fprintf(
                    stderr,
                    "%s: invalid selected index (must be an integer)\n",
                    optarg);
                return EXIT_FAILURE;
            }
            break;

        case 'l':
            if (sscanf(optarg, "%u", &cmdline_overrides.conf.lines) != 1) {
                fprintf(stderr, "%s: invalid line count\n", optarg);
                return EXIT_FAILURE;
            }
            cmdline_overrides.lines_set = true;
            break;

        case OPT_MINIMAL_LINES:
            cmdline_overrides.conf.minimal_lines = true;
            cmdline_overrides.minimal_lines_set = true;
            break;

        case OPT_HIDE_PROMPT:
            cmdline_overrides.conf.hide_prompt = true;
            cmdline_overrides.hide_prompt_set = true;
            break;

        case 'w':
            if (sscanf(optarg, "%u", &cmdline_overrides.conf.chars) != 1) {
                fprintf(stderr, "%s: invalid width\n", optarg);
                return EXIT_FAILURE;
            }
            cmdline_overrides.chars_set = true;
            break;

        case OPT_TABS:
            if (sscanf(optarg, "%u", &cmdline_overrides.conf.tabs) != 1) {
                fprintf(stderr, "%s: invalid tab count\n", optarg);
                return EXIT_FAILURE;
            }
            cmdline_overrides.tabs_set = true;
            break;

        case 'x':
            if (sscanf(optarg, "%u", &cmdline_overrides.conf.pad.x) != 1) {
                fprintf(stderr, "%s: invalid padding\n", optarg);
                return EXIT_FAILURE;
            }
            cmdline_overrides.pad_x_set = true;
            break;

        case 'y':
            if (sscanf(optarg, "%u", &cmdline_overrides.conf.pad.y) != 1) {
                fprintf(stderr, "%s: invalid padding\n", optarg);
                return EXIT_FAILURE;
            }
            cmdline_overrides.pad_y_set = true;
            break;

        case 'P':
            if (sscanf(optarg, "%u", &cmdline_overrides.conf.pad.inner) != 1) {
                fprintf(stderr, "%s: invalid padding\n", optarg);
                return EXIT_FAILURE;
            }
            cmdline_overrides.pad_inner_set = true;
            break;

        case 'p':
            free(cmdline_overrides.conf.prompt);
            cmdline_overrides.conf.prompt = ambstoc32(optarg);

            if (cmdline_overrides.conf.prompt == NULL) {
                fprintf(stderr, "%s: invalid prompt\n", optarg);
                return EXIT_FAILURE;
            }

            break;

        case OPT_PROMPT_ONLY:
            free(cmdline_overrides.conf.prompt);
            cmdline_overrides.conf.prompt = ambstoc32(optarg);

            if (cmdline_overrides.conf.prompt == NULL) {
                fprintf(stderr, "%s: invalid prompt\n", optarg);
                return EXIT_FAILURE;
            }

            cmdline_overrides.prompt_only_set = true;
            cmdline_overrides.conf.prompt_only = true;
            break;

        case OPT_PLACEHOLDER:
            free(cmdline_overrides.conf.placeholder);
            cmdline_overrides.conf.placeholder = ambstoc32(optarg);

            if (cmdline_overrides.conf.placeholder == NULL) {
                fprintf(stderr, "%s: invalid placeholder\n", optarg);
                return EXIT_FAILURE;
            }

            break;

        case OPT_SEARCH_TEXT:
            free(cmdline_overrides.conf.search_text);
            cmdline_overrides.conf.search_text = ambstoc32(optarg);

            if (cmdline_overrides.conf.search_text == NULL) {
                fprintf(stderr, "%s: invalid search text\n", optarg);
                return EXIT_FAILURE;
            }

            break;

        case 'b': {
            const char *clr_start = optarg;
            if (clr_start[0] == '#')
                clr_start++;

            errno = 0;
            char *end = NULL;
            uint32_t background = strtoul(clr_start, &end, 16);
            if (errno != 0 || end == NULL || *end != '\0' || (end - clr_start) != 8) {
                fprintf(stderr, "background-color: %s: invalid color\n",
                        optarg);
                return EXIT_FAILURE;
            }
            cmdline_overrides.conf.colors.background =
                conf_hex_to_rgba(background);
            cmdline_overrides.background_color_set = true;
            break;
        }

        case 't': {
            const char *clr_start = optarg;
            if (clr_start[0] == '#')
                clr_start++;

            errno = 0;
            char *end = NULL;
            uint32_t text_color = strtoul(clr_start, &end, 16);
            if (errno != 0 || end == NULL || *end != '\0' || (end - clr_start) != 8) {
                fprintf(stderr, "text-color: %s: invalid color\n", optarg);
                return EXIT_FAILURE;
            }
            cmdline_overrides.conf.colors.text = conf_hex_to_rgba(text_color);
            cmdline_overrides.text_color_set = true;
            break;
        }

        case OPT_PROMPT_COLOR: {
            const char *clr_start = optarg;
            if (clr_start[0] == '#')
                clr_start++;

            errno = 0;
            char *end = NULL;
            uint32_t prompt_color = strtoul(clr_start, &end, 16);
            if (errno != 0 || end == NULL || *end != '\0' || (end - clr_start) != 8) {
                fprintf(stderr, "prompt-color: %s: invalid color\n", optarg);
                return EXIT_FAILURE;
            }
            cmdline_overrides.conf.colors.prompt = conf_hex_to_rgba(prompt_color);
            cmdline_overrides.prompt_color_set = true;
            break;
        }

        case OPT_PLACEHOLDER_COLOR: {
            const char *clr_start = optarg;
            if (clr_start[0] == '#')
                clr_start++;

            errno = 0;
            char *end = NULL;
            uint32_t placeholder_color = strtoul(clr_start, &end, 16);
            if (errno != 0 || end == NULL || *end != '\0' || (end - clr_start) != 8) {
                fprintf(stderr, "placeholder-color: %s: invalid color\n", optarg);
                return EXIT_FAILURE;
            }
            cmdline_overrides.conf.colors.placeholder = conf_hex_to_rgba(placeholder_color);
            cmdline_overrides.placeholder_color_set = true;
            break;
        }

        case OPT_INPUT_COLOR: {
            const char *clr_start = optarg;
            if (clr_start[0] == '#')
                clr_start++;

            errno = 0;
            char *end = NULL;
            uint32_t input_color = strtoul(clr_start, &end, 16);
            if (errno != 0 || end == NULL || *end != '\0' || (end - clr_start) != 8) {
                fprintf(stderr, "prompt-color: %s: invalid color\n", optarg);
                return EXIT_FAILURE;
            }
            cmdline_overrides.conf.colors.input = conf_hex_to_rgba(input_color);
            cmdline_overrides.input_color_set = true;
            break;
        }

        case 'm': {
            const char *clr_start = optarg;
            if (clr_start[0] == '#')
                clr_start++;

            errno = 0;
            char *end = NULL;
            uint32_t match_color = strtoul(clr_start, &end, 16);
            if (errno != 0 || end == NULL || *end != '\0' || (end - clr_start) != 8) {
                fprintf(stderr, "match-color: %s: invalid color\n", optarg);
                return EXIT_FAILURE;
            }
            cmdline_overrides.conf.colors.match =
                conf_hex_to_rgba(match_color);
            cmdline_overrides.match_color_set = true;
            break;
        }

        case 's': {
            const char *clr_start = optarg;
            if (clr_start[0] == '#')
                clr_start++;

            errno = 0;
            char *end = NULL;
            uint32_t selection_color = strtoul(clr_start, &end, 16);
            if (errno != 0 || end == NULL || *end != '\0' || (end - clr_start) != 8) {
                fprintf(stderr, "selection-color: %s: invalid color\n", optarg);
                return EXIT_FAILURE;
            }
            cmdline_overrides.conf.colors.selection =
                conf_hex_to_rgba(selection_color);
            cmdline_overrides.selection_color_set = true;
            break;
        }

        case 'S': {
            const char *clr_start = optarg;
            if (clr_start[0] == '#')
                clr_start++;

            errno = 0;
            char *end = NULL;
            uint32_t selection_text_color = strtoul(clr_start, &end, 16);
            if (errno != 0 || end == NULL || *end != '\0' || (end - clr_start) != 8) {
                fprintf(stderr, "selection-text-color: %s: invalid color\n",
                        optarg);
                return EXIT_FAILURE;
            }
            cmdline_overrides.conf.colors.selection_text =
                conf_hex_to_rgba(selection_text_color);
            cmdline_overrides.selection_text_color_set = true;
            break;
        }

        case OPT_SELECTION_RADIUS: {
            if (sscanf(optarg, "%u", &cmdline_overrides.conf.selection_border.radius) != 1) {
                fprintf(stderr, "%s: invalid selection border radius (must be an integer)\n",
                        optarg);
                return EXIT_FAILURE;
            }
            cmdline_overrides.selection_border_radius_set = true;
            break;
        }

        case 'M': {
            const char *clr_start = optarg;
            if (clr_start[0] == '#')
                clr_start++;

            errno = 0;
            char *end = NULL;
            uint32_t selection_match_color = strtoul(clr_start, &end, 16);
            if (errno != 0 || end == NULL || *end != '\0' || (end - clr_start) != 8) {
                fprintf(stderr, "selection-match-color: %s: invalid color\n",
                        optarg);
                return EXIT_FAILURE;
            }
            cmdline_overrides.conf.colors.selection_match =
                conf_hex_to_rgba(selection_match_color);
            cmdline_overrides.selection_match_color_set = true;
            break;
        }

        case OPT_COUNTER_COLOR: {
            const char *clr_start = optarg;
            if (clr_start[0] == '#')
                clr_start++;

            errno = 0;
            char *end = NULL;
            uint32_t count_color = strtoul(clr_start, &end, 16);
            if (errno != 0 || end == NULL || *end != '\0' || (end - clr_start) != 8) {
                fprintf(stderr, "counter-color: %s: invalid color\n",
                        optarg);
                return EXIT_FAILURE;
            }
            cmdline_overrides.conf.colors.counter = conf_hex_to_rgba(count_color);
            cmdline_overrides.counter_color_set = true;
            break;
        }


        case 'B':
            if (sscanf(optarg, "%u", &cmdline_overrides.conf.border.size) != 1) {
                fprintf(
                    stderr,
                    "%s: invalid border width (must be an integer)\n", optarg);
                return EXIT_FAILURE;
            }
            cmdline_overrides.border_size_set = true;
            break;

        case 'r':
            if (sscanf(optarg, "%u", &cmdline_overrides.conf.border.radius) != 1) {
                fprintf(stderr, "%s: invalid border radius (must be an integer)\n",
                        optarg);
                return EXIT_FAILURE;
            }
            cmdline_overrides.border_radius_set = true;
            break;

        case 'C': {
            const char *clr_start = optarg;
            if (clr_start[0] == '#')
                clr_start++;

            errno = 0;
            char *end = NULL;
            uint32_t border_color = strtoul(clr_start, &end, 16);
            if (errno != 0 || end == NULL || *end != '\0' || (end - clr_start) != 8) {
                fprintf(stderr, "border-color: %s: invalid color\n", optarg);
                return EXIT_FAILURE;
            }
            cmdline_overrides.conf.colors.border =
                conf_hex_to_rgba(border_color);
            cmdline_overrides.border_color_set = true;
            break;
        }

        case OPT_SHOW_ACTIONS:
            cmdline_overrides.actions_enabled_set = true;
            cmdline_overrides.conf.actions_enabled = true;
            break;

        case OPT_MATCH_MODE:
            if (strcmp(optarg, "exact") == 0)
                cmdline_overrides.conf.match_mode = MATCH_MODE_EXACT;
            else if (strcmp(optarg, "fzf") == 0)
                cmdline_overrides.conf.match_mode = MATCH_MODE_FZF;
            else if (strcmp(optarg, "fuzzy") == 0)
                cmdline_overrides.conf.match_mode = MATCH_MODE_FUZZY;
            else {
                fprintf(stderr, "%s: invalid match-mode. Must be 'exact', 'fuzzy' or 'fzf'\n", optarg);
                return EXIT_FAILURE;
            }

            cmdline_overrides.match_mode_set = true;
            break;

        case OPT_FILTER_DESKTOP:
            cmdline_overrides.filter_desktop_set = true;
            if (optarg != NULL && strcasecmp(optarg, "no") == 0)
                cmdline_overrides.conf.filter_desktop = false;
            else if (optarg != NULL) {
                fprintf(stderr, "%s: invalid filter-desktop option\n", optarg);
                return EXIT_FAILURE;
            }
            else
                cmdline_overrides.conf.filter_desktop = true;
            break;

        case OPT_FUZZY_MIN_LENGTH:
            if (sscanf(optarg, "%zu", &cmdline_overrides.conf.fuzzy.min_length) != 1) {
                fprintf(
                    stderr,
                    "%s: invalid fuzzy min length (must be an integer)\n",
                    optarg);
                return EXIT_FAILURE;
            }
            cmdline_overrides.fuzzy_min_length_set = true;
            break;

        case OPT_FUZZY_MAX_LENGTH_DISCREPANCY:
            if (sscanf(optarg, "%zu", &cmdline_overrides.conf.fuzzy.max_length_discrepancy) != 1) {
                fprintf(
                    stderr,
                    "%s: invalid fuzzy max length discrepancy "
                    "(must be an integer)\n", optarg);
                return EXIT_FAILURE;
            }
            cmdline_overrides.fuzzy_max_length_discrepancy_set = true;
            break;

        case OPT_FUZZY_MAX_DISTANCE:
            if (sscanf(optarg, "%zu", &cmdline_overrides.conf.fuzzy.max_distance) != 1) {
                fprintf(
                    stderr,
                    "%s: invalid fuzzy max distance (must be an integer)\n",
                    optarg);
                return EXIT_FAILURE;
            }
            cmdline_overrides.fuzzy_max_distance_set = true;
            break;

        case OPT_LIST_EXECS_IN_PATH:
            cmdline_overrides.conf.list_executables_in_path = true;
            break;

        case 'H': { /* line-height */
            if (!pt_or_px_from_string(optarg, &cmdline_overrides.conf.line_height))
                return EXIT_FAILURE;
            cmdline_overrides.line_height_set = true;
            break;
        }

        case OPT_LETTER_SPACING: {
            if (!pt_or_px_from_string(optarg, &cmdline_overrides.conf.letter_spacing))
                return EXIT_FAILURE;
            cmdline_overrides.letter_spacing_set = true;
            break;
        }

        case OPT_LAUNCH_PREFIX: {
            cmdline_overrides.conf.launch_prefix = optarg;
            break;
        }

        case OPT_LAYER:
            if (strcasecmp(optarg, "top") == 0)
                cmdline_overrides.conf.layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
            else if (strcasecmp(optarg, "overlay") == 0)
                cmdline_overrides.conf.layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
            else {
                fprintf(
                    stderr,
                    "%s: invalid layer. Must be one of 'top', 'overlay'\n",
                    optarg);
                return EXIT_FAILURE;
            }
            cmdline_overrides.layer_set = true;
            break;

        case OPT_KEYBOARD_FOCUS:
            if (strcasecmp(optarg, "exclusive") == 0)
                cmdline_overrides.conf.keyboard_focus = ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;
            else if (strcasecmp(optarg, "on-demand") == 0)
                cmdline_overrides.conf.keyboard_focus = ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND;
            else {
                fprintf(
                    stderr,
                    "%s: invalid keyboard-focus. Must be one of 'exclusive', 'on-demand'\n",
                    optarg);
                return EXIT_FAILURE;
            }
            cmdline_overrides.keyboard_focus_set = true;
            break;

        case OPT_NO_EXIT_ON_KB_LOSS:
            cmdline_overrides.conf.exit_on_kb_focus_loss = false;
            cmdline_overrides.no_exit_on_keyboard_focus_loss_set = true;
            break;

        case 'd':
            cmdline_overrides.conf.dmenu.enabled = true;
            cmdline_overrides.dmenu_enabled_set = true;
            break;

        case OPT_DMENU_NULL:
            cmdline_overrides.conf.dmenu.enabled = true;
            cmdline_overrides.conf.dmenu.delim = '\0';
            cmdline_overrides.dmenu_enabled_set = true;
            cmdline_overrides.dmenu_delim_set = true;
            break;

        case 'R':
            cmdline_overrides.conf.dmenu.exit_immediately_if_empty = true;
            cmdline_overrides.dmenu_exit_immediately_if_empty_set = true;
            break;

        case OPT_DMENU_INDEX:
            cmdline_overrides.conf.dmenu.mode = DMENU_MODE_INDEX;
            cmdline_overrides.dmenu_mode_set = true;
            break;

        case OPT_DMENU_NTH_DELIM:
            if (strlen(optarg) != 1) {
                fprintf(
                    stderr,
                    "%s: invalid nth-delimiter. Must be a single ASCII character",
                    optarg);
                return EXIT_FAILURE;
            }

            cmdline_overrides.conf.dmenu.nth_delim = optarg[0];
            cmdline_overrides.dmenu_nth_delim_set = true;
            break;

        case OPT_DMENU_WITH_NTH: {
            free(cmdline_overrides.conf.dmenu.with_nth_format);
            cmdline_overrides.conf.dmenu.with_nth_format = NULL;

            unsigned int with_nth_idx;
            if (sscanf(optarg, "%u", &with_nth_idx) == 1) {
                if (with_nth_idx == 0) {
                    /* Do nothing more - i.e. leave it unset */
                } else {
                    cmdline_overrides.conf.dmenu.with_nth_format =
                        xasprintf("{%u}", with_nth_idx);
                }
            } else if (optarg[0] != '\0')
                cmdline_overrides.conf.dmenu.with_nth_format = xstrdup(optarg);
            cmdline_overrides.dmenu_with_nth_set = true;
            break;
        }

        case OPT_DMENU_ACCEPT_NTH: {
            free(cmdline_overrides.conf.dmenu.accept_nth_format);
            cmdline_overrides.conf.dmenu.accept_nth_format = NULL;

            unsigned int accept_nth_idx;
            if (sscanf(optarg, "%u", &accept_nth_idx) == 1) {
                if (accept_nth_idx == 0) {
                    /* Do nothing more - i.e. leave it unset */
                } else {
                    cmdline_overrides.conf.dmenu.accept_nth_format =
                        xasprintf("{%u}", accept_nth_idx);
                }
            } else if (optarg[0] != '\0')
                cmdline_overrides.conf.dmenu.accept_nth_format = xstrdup(optarg);

            cmdline_overrides.dmenu_accept_nth_set = true;
            break;
        }

        case OPT_DMENU_MATCH_NTH: {
            free(cmdline_overrides.conf.dmenu.match_nth_format);
            cmdline_overrides.conf.dmenu.match_nth_format = NULL;

            unsigned int match_nth_idx;
            if (sscanf(optarg, "%u", &match_nth_idx) == 1) {
                if (match_nth_idx == 0) {
                    /* Do nothing more - i.e. leave it unset */
                } else {
                    cmdline_overrides.conf.dmenu.match_nth_format =
                        xasprintf("{%u}", match_nth_idx);
                }
            } else if (optarg[0] != '\0')
                cmdline_overrides.conf.dmenu.match_nth_format = xstrdup(optarg);

            cmdline_overrides.dmenu_match_nth_set = true;
            break;
        }

        case OPT_DMENU_ONLY_MATCH:
            cmdline_overrides.conf.dmenu.only_match = true;
            cmdline_overrides.dmenu_only_match_set = true;
            break;

        case OPT_LOG_LEVEL: {
            int lvl = log_level_from_string(optarg);
            if (lvl < 0) {
                fprintf(
                    stderr,
                    "--log-level: %s: argument must be one of %s\n",
                    optarg, log_level_string_hint());
                return EXIT_FAILURE;
            }
            log_level = lvl;
            break;
        }

        case OPT_LOG_COLORIZE:
            if (optarg == NULL || strcmp(optarg, "auto") == 0)
                log_colorize = LOG_COLORIZE_AUTO;
            else if (strcmp(optarg, "never") == 0)
                log_colorize = LOG_COLORIZE_NEVER;
            else if (strcmp(optarg, "always") == 0)
                log_colorize = LOG_COLORIZE_ALWAYS;
            else {
                fprintf(stderr, "--log-colorize: %s: argument must be one of 'never', 'always' or 'auto'\n", optarg);
                return EXIT_FAILURE;
            }
            break;

        case OPT_LOG_NO_SYSLOG:
            log_syslog = false;
            break;

        case OPT_RENDER_WORKERS:
            if (sscanf(optarg, "%hu", &cmdline_overrides.conf.render_worker_count) != 1) {
                fprintf(
                    stderr,
                    "%s: invalid value for render-workers (must be an integer)\n",
                    optarg);
                return EXIT_FAILURE;
            }

            cmdline_overrides.render_workers_set = true;
            break;

        case OPT_MATCH_WORKERS:
            if (sscanf(optarg, "%hu", &cmdline_overrides.conf.match_worker_count) != 1) {
                fprintf(
                    stderr,
                    "%s: invalid value for match-workers (must be an integer)\n",
                    optarg);
                return EXIT_FAILURE;
            }

            cmdline_overrides.match_workers_set = true;
            break;

        case OPT_NO_SORT:
            cmdline_overrides.conf.sort_result = false;
            cmdline_overrides.no_sort_set = true;
            break;

        case OPT_COUNTER:
            cmdline_overrides.conf.match_counter = true;
            cmdline_overrides.counter_set = true;
            break;

        case OPT_DELAYED_FILTER_MS:
            if (sscanf(optarg, "%u", &cmdline_overrides.conf.delayed_filter_ms) != 1) {
                fprintf(stderr, "%s: invalid delayed-filter-ms (must be an integer)\n",
                        optarg);
                return EXIT_FAILURE;
            }
            cmdline_overrides.delayed_filter_ms_set = true;
            break;

        case OPT_DELAYED_FILTER_LIMIT:
            if (sscanf(optarg, "%u", &cmdline_overrides.conf.delayed_filter_limit) != 1) {
                fprintf(stderr, "%s: invalid delayed-filter-limit (must be an integer)\n",
                        optarg);
                return EXIT_FAILURE;
            }
            cmdline_overrides.delayed_filter_limit_set = true;
            break;

        case OPT_SCALING_FILTER:
            if (strcmp(optarg, "none") == 0)
                cmdline_overrides.conf.png_scaling_filter = SCALING_FILTER_NONE;
            else if (strcmp(optarg, "nearest") == 0)
                cmdline_overrides.conf.png_scaling_filter = SCALING_FILTER_NEAREST;
            else if (strcmp(optarg, "bilinear") == 0)
                cmdline_overrides.conf.png_scaling_filter = SCALING_FILTER_BILINEAR;
            else if (strcmp(optarg, "box") == 0)
                cmdline_overrides.conf.png_scaling_filter = SCALING_FILTER_BOX;
            else if (strcmp(optarg, "linear") == 0)
                cmdline_overrides.conf.png_scaling_filter = SCALING_FILTER_LINEAR;
            else if (strcmp(optarg, "cubic") == 0)
                cmdline_overrides.conf.png_scaling_filter = SCALING_FILTER_CUBIC;
            else if (strcmp(optarg, "lanczos2") == 0)
                cmdline_overrides.conf.png_scaling_filter = SCALING_FILTER_LANCZOS2;
            else if (strcmp(optarg, "lanczos3") == 0)
                cmdline_overrides.conf.png_scaling_filter = SCALING_FILTER_LANCZOS3;
            else if (strcmp(optarg, "lanczos3-stretched") == 0)
                cmdline_overrides.conf.png_scaling_filter = SCALING_FILTER_LANCZOS3_STRETCHED;
            else {
                fprintf(stderr, "%s: invalid scaling-filter", optarg);
                return EXIT_FAILURE;
            }
            cmdline_overrides.scaling_filter_set = true;
            break;

        case OPT_PRINT_TIMINGS:
            cmdline_overrides.conf.print_timing_info = true;
            cmdline_overrides.print_timing_info_set = true;
            break;

        case OPT_AUTO_SELECT:
            cmdline_overrides.conf.auto_select = true;
            break;

        case OPT_NO_MOUSE:
            cmdline_overrides.conf.enable_mouse = false;
            cmdline_overrides.no_mouse_set = true;
            break;

        case 'v':
            printf("fuzzel %s\n", version_and_features());
            return EXIT_SUCCESS;

        case 'h':
            print_usage(argv[0]);
            return EXIT_SUCCESS;

        case ':':
            fprintf(stderr, "error: %s: missing required argument\n", argv[optind-1]);
            return EXIT_FAILURE;

        case '?':
            fprintf(stderr, "error: %s: invalid option\n", argv[optind-1]);
            return EXIT_FAILURE;
        }
    }


    log_init(log_colorize, log_syslog, LOG_FACILITY_USER, log_level);
    LOG_INFO("%s", version_and_features());

    int ret = EXIT_FAILURE;

    if (select != NULL && select_idx) {
        LOG_ERRNO("--select and --select-index cannot be used at the same time");
        return ret;
    }

    if (cmdline_overrides.prompt_only_set && cmdline_overrides.conf.hide_prompt) {
        LOG_ERRNO("--prompt-only and --hide-prompt cannot be used at the same time");
        config_free(&cmdline_overrides.conf);
        return ret;
    }

    struct config conf = {0};
    bool conf_successful = config_load(&conf, config_path, NULL, check_config);
    if (!conf_successful) {
        config_free(&conf);
        config_free(&cmdline_overrides.conf);
        return ret;
    }

    if (check_config) {
        config_free(&conf);
        config_free(&cmdline_overrides.conf);
        return EXIT_SUCCESS;
    }

    /* Apply command line overrides */
    if (cmdline_overrides.conf.output != NULL) {
        free(conf.output);
        conf.output = xstrdup(cmdline_overrides.conf.output);
    }
    if (cmdline_overrides.conf.namespace != NULL) {
        free(conf.namespace);
        conf.namespace = xstrdup(cmdline_overrides.conf.namespace);
    }
    if (cmdline_overrides.conf.prompt != NULL) {
        free(conf.prompt);
        conf.prompt = cmdline_overrides.conf.prompt;
        if (cmdline_overrides.prompt_only_set)
            conf.prompt_only = cmdline_overrides.conf.prompt_only;
    }
    if (cmdline_overrides.conf.placeholder != NULL) {
        free(conf.placeholder);
        conf.placeholder = cmdline_overrides.conf.placeholder;
    }
    if (cmdline_overrides.conf.search_text != NULL) {
        free(conf.search_text);
        conf.search_text = cmdline_overrides.conf.search_text;
    }
    if (cmdline_overrides.conf.password_mode.enabled) {
        conf.password_mode.enabled = true;
        conf.password_mode.character =
            cmdline_overrides.conf.password_mode.character_set
                ? cmdline_overrides.conf.password_mode.character
                : conf.password_mode.character_set
                    ? conf.password_mode.character
                    : U'*';
    }
    if (cmdline_overrides.conf.terminal != NULL) {
        free(conf.terminal);
        conf.terminal = xstrdup(cmdline_overrides.conf.terminal);
    }
    if (cmdline_overrides.conf.launch_prefix != NULL) {
        free(conf.launch_prefix);
        conf.launch_prefix = xstrdup(cmdline_overrides.conf.launch_prefix);
    }
    if (cmdline_overrides.conf.font != NULL) {
        free(conf.font);
        conf.font = xstrdup(cmdline_overrides.conf.font);
    }
    if (cmdline_overrides.use_bold_set)
        conf.use_bold = cmdline_overrides.conf.use_bold;
    if (cmdline_overrides.conf.icon_theme != NULL) {
        free(conf.icon_theme);
        conf.icon_theme = xstrdup(cmdline_overrides.conf.icon_theme);
    }
    if (cmdline_overrides.dpi_aware_set)
        conf.dpi_aware = cmdline_overrides.conf.dpi_aware;
    if (cmdline_overrides.gamma_correct_set)
        conf.gamma_correct = cmdline_overrides.conf.gamma_correct;
    if (cmdline_overrides.match_fields_set)
        conf.match_fields = cmdline_overrides.conf.match_fields;
    if (cmdline_overrides.icons_disabled_set)
        conf.icons_enabled = cmdline_overrides.conf.icons_enabled;
    if (cmdline_overrides.hide_when_prompt_empty_set)
        conf.hide_when_prompt_empty = cmdline_overrides.conf.hide_when_prompt_empty;
    if (cmdline_overrides.hide_prompt_set)
        conf.hide_prompt = cmdline_overrides.conf.hide_prompt;
    if (cmdline_overrides.anchor_set)
        conf.anchor = cmdline_overrides.conf.anchor;
    if (cmdline_overrides.x_margin_set)
        conf.margin.x = cmdline_overrides.conf.margin.x;
    if (cmdline_overrides.y_margin_set)
        conf.margin.y = cmdline_overrides.conf.margin.y;
    if (cmdline_overrides.lines_set)
        conf.lines = cmdline_overrides.conf.lines;
    if (cmdline_overrides.minimal_lines_set)
        conf.minimal_lines = cmdline_overrides.conf.minimal_lines;
    if (cmdline_overrides.chars_set)
        conf.chars = cmdline_overrides.conf.chars;
    if (cmdline_overrides.tabs_set)
        conf.tabs = cmdline_overrides.conf.tabs;
    if (cmdline_overrides.pad_x_set)
        conf.pad.x = cmdline_overrides.conf.pad.x;
    if (cmdline_overrides.pad_y_set)
        conf.pad.y = cmdline_overrides.conf.pad.y;
    if (cmdline_overrides.pad_inner_set)
        conf.pad.inner = cmdline_overrides.conf.pad.inner;
    if (cmdline_overrides.background_color_set)
        conf.colors.background = cmdline_overrides.conf.colors.background;
    if (cmdline_overrides.text_color_set)
        conf.colors.text = cmdline_overrides.conf.colors.text;
    if (cmdline_overrides.prompt_color_set)
        conf.colors.prompt = cmdline_overrides.conf.colors.prompt;
    if (cmdline_overrides.placeholder_color_set)
        conf.colors.placeholder = cmdline_overrides.conf.colors.placeholder;
    if (cmdline_overrides.input_color_set)
        conf.colors.input = cmdline_overrides.conf.colors.input;
    if (cmdline_overrides.match_color_set)
        conf.colors.match = cmdline_overrides.conf.colors.match;
    if (cmdline_overrides.selection_color_set)
        conf.colors.selection = cmdline_overrides.conf.colors.selection;
    if (cmdline_overrides.selection_text_color_set)
        conf.colors.selection_text = cmdline_overrides.conf.colors.selection_text;
    if (cmdline_overrides.selection_match_color_set)
        conf.colors.selection_match = cmdline_overrides.conf.colors.selection_match;
    if (cmdline_overrides.selection_border_radius_set)
        conf.selection_border.radius = cmdline_overrides.conf.selection_border.radius;
    if (cmdline_overrides.counter_color_set)
        conf.colors.counter = cmdline_overrides.conf.colors.counter;
    if (cmdline_overrides.border_color_set)
        conf.colors.border = cmdline_overrides.conf.colors.border;
    if (cmdline_overrides.border_size_set)
        conf.border.size = cmdline_overrides.conf.border.size;
    if (cmdline_overrides.border_radius_set)
        conf.border.radius = cmdline_overrides.conf.border.radius;
    if (cmdline_overrides.filter_desktop_set)
        conf.filter_desktop = cmdline_overrides.conf.filter_desktop;
    if (cmdline_overrides.actions_enabled_set)
        conf.actions_enabled = cmdline_overrides.conf.actions_enabled;
    if (cmdline_overrides.match_mode_set)
        conf.match_mode = cmdline_overrides.conf.match_mode;
    if (cmdline_overrides.fuzzy_min_length_set)
        conf.fuzzy.min_length = cmdline_overrides.conf.fuzzy.min_length;
    if (cmdline_overrides.fuzzy_max_length_discrepancy_set)
        conf.fuzzy.max_length_discrepancy = cmdline_overrides.conf.fuzzy.max_length_discrepancy;
    if (cmdline_overrides.fuzzy_max_distance_set)
        conf.fuzzy.max_distance = cmdline_overrides.conf.fuzzy.max_distance;
    if (cmdline_overrides.line_height_set)
        conf.line_height = cmdline_overrides.conf.line_height;
    if (cmdline_overrides.letter_spacing_set)
        conf.letter_spacing = cmdline_overrides.conf.letter_spacing;
    if (cmdline_overrides.layer_set)
        conf.layer = cmdline_overrides.conf.layer;
    if (cmdline_overrides.keyboard_focus_set)
        conf.keyboard_focus = cmdline_overrides.conf.keyboard_focus;
    if (cmdline_overrides.no_exit_on_keyboard_focus_loss_set)
        conf.exit_on_kb_focus_loss = cmdline_overrides.conf.exit_on_kb_focus_loss;
    if (cmdline_overrides.dmenu_enabled_set)
        conf.dmenu.enabled = cmdline_overrides.conf.dmenu.enabled;
    if (cmdline_overrides.dmenu_delim_set)
        conf.dmenu.delim = cmdline_overrides.conf.dmenu.delim;
    if (cmdline_overrides.dmenu_mode_set)
        conf.dmenu.mode = cmdline_overrides.conf.dmenu.mode;
    if (cmdline_overrides.dmenu_exit_immediately_if_empty_set)
        conf.dmenu.exit_immediately_if_empty = cmdline_overrides.conf.dmenu.exit_immediately_if_empty;
    if (cmdline_overrides.dmenu_nth_delim_set)
        conf.dmenu.nth_delim = cmdline_overrides.conf.dmenu.nth_delim;
    if (cmdline_overrides.dmenu_with_nth_set) {
        free(conf.dmenu.with_nth_format);
        conf.dmenu.with_nth_format = cmdline_overrides.conf.dmenu.with_nth_format;
    }
    if (cmdline_overrides.dmenu_accept_nth_set) {
        free(conf.dmenu.accept_nth_format);
        conf.dmenu.accept_nth_format = cmdline_overrides.conf.dmenu.accept_nth_format;
    }
    if (cmdline_overrides.dmenu_match_nth_set) {
        free(conf.dmenu.match_nth_format);
        conf.dmenu.match_nth_format = cmdline_overrides.conf.dmenu.match_nth_format;
    }
    if (cmdline_overrides.dmenu_only_match_set)
        conf.dmenu.only_match = cmdline_overrides.conf.dmenu.only_match;
    if (cmdline_overrides.conf.list_executables_in_path)
        conf.list_executables_in_path = cmdline_overrides.conf.list_executables_in_path;
    if (cmdline_overrides.render_workers_set)
        conf.render_worker_count = cmdline_overrides.conf.render_worker_count;
    if (cmdline_overrides.match_workers_set)
        conf.match_worker_count = cmdline_overrides.conf.match_worker_count;
    if (cmdline_overrides.no_sort_set)
        conf.sort_result = cmdline_overrides.conf.sort_result;
    if (cmdline_overrides.counter_set)
        conf.match_counter = cmdline_overrides.conf.match_counter;
    if (cmdline_overrides.delayed_filter_ms_set)
        conf.delayed_filter_ms = cmdline_overrides.conf.delayed_filter_ms;
    if (cmdline_overrides.delayed_filter_limit_set)
        conf.delayed_filter_limit = cmdline_overrides.conf.delayed_filter_limit;
    if (cmdline_overrides.conf.cache_path != NULL) {
        free(conf.cache_path);
        conf.cache_path = xstrdup(cmdline_overrides.conf.cache_path);
    }
    if (cmdline_overrides.print_timing_info_set)
        conf.print_timing_info = cmdline_overrides.conf.print_timing_info;
    if (cmdline_overrides.scaling_filter_set)
        conf.png_scaling_filter = cmdline_overrides.conf.png_scaling_filter;
    if (cmdline_overrides.conf.auto_select)
        conf.auto_select = cmdline_overrides.conf.auto_select;
    if (cmdline_overrides.no_mouse_set)
        conf.enable_mouse = cmdline_overrides.conf.enable_mouse;

    if (conf.dmenu.enabled) {
        /* We don't have any meta data in dmenu mode */
        conf.match_fields = conf.dmenu.match_nth_format != NULL
            ? MATCH_NTH : MATCH_NAME;

        if (conf.prompt_only) {
            conf.lines = 0;
            conf.match_counter = false;
            conf.dmenu.exit_immediately_if_empty = false;
            close(STDIN_FILENO);  /* To catch reads */
        }
    }

    if (conf.print_timing_info)
        time_enable();

    _Static_assert((int)LOG_CLASS_ERROR == (int)FCFT_LOG_CLASS_ERROR,
                   "fcft log level enum offset");
    _Static_assert((int)LOG_COLORIZE_ALWAYS == (int)FCFT_LOG_COLORIZE_ALWAYS,
                   "fcft colorize enum mismatch");
    fcft_init((enum fcft_log_colorize)log_colorize, log_syslog, (enum fcft_log_class)log_level);

#if !defined(FUZZEL_ENABLE_SVG_LIBRSVG)
    /* Skip fcft cleanup if we’re using the librsvg backend
     * (https://codeberg.org/dnkl/fuzzel/issues/87) */
    atexit(&fcft_fini);
#endif

    mtx_t icon_lock;
    if (mtx_init(&icon_lock, mtx_plain) != thrd_success) {
        LOG_ERR("failed to create icon lock");
        return EXIT_FAILURE;
    }

    struct application_list *apps = NULL;
    struct fdm *fdm = NULL;
    struct prompt *prompt = NULL;
    struct matches *matches = NULL;
    struct render *render = NULL;
    struct wayland *wayl = NULL;
    struct kb_manager *kb_manager = NULL;

    thrd_t app_thread_id;
    bool join_app_thread = false;
    int event_pipe[2] = {-1, -1};
    int dmenu_abort_fd = -1;

    char *lock_file = NULL;
    int file_lock_fd = -1;
    bool unlink_lock_file = true;

    icon_theme_list_t themes = tll_init();

    /* Don’t allow multiple instances (in the same Wayland session) */
    lock_file = lock_file_name();
    if (lock_file != NULL) {
        if (!acquire_file_lock(lock_file, &file_lock_fd)) {
            unlink_lock_file = false;
            goto out;
        }
    }

    if ((fdm = fdm_init()) == NULL)
        goto out;

    if ((render = render_init(&conf, &icon_lock)) == NULL)
        goto out;

    if ((prompt = prompt_init(conf.prompt, conf.placeholder, conf.search_text)) == NULL)
        goto out;

    if ((matches = matches_init(
        fdm, prompt,
        conf.match_fields, conf.match_mode, conf.sort_result,
        conf.fuzzy.min_length,
        conf.fuzzy.max_length_discrepancy,
        conf.fuzzy.max_distance,
        conf.match_worker_count,
        conf.delayed_filter_ms, conf.delayed_filter_limit)) == NULL)
    {
        goto out;
    }
    matches_max_matches_per_page_set(matches, conf.lines);

    if ((apps = applications_init()) == NULL)
        goto out;

    matches_set_applications(matches, apps);

    if (conf.dmenu.enabled) {
        dmenu_abort_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (dmenu_abort_fd < 0) {
            LOG_ERRNO("failed to create event FD for dmenu mode");
            goto out;
        }
    }

    struct context ctx = {
        .conf = &conf,
        .cache_path = conf.cache_path,
        .render = render,
        .matches = matches,
        .apps = apps,
        .themes = &themes,
        .icon_lock = &icon_lock,
        .select_initial = select,
        .select_initial_idx = select_idx,
        .event_fd = -1,
        .dmenu_abort_fd = dmenu_abort_fd,
    };

    if ((kb_manager = kb_manager_new()) == NULL)
        goto out;

    if ((wayl = wayl_init(
             &conf, fdm, kb_manager, render, prompt, matches,
             &font_reloaded, &ctx)) == NULL)
        goto out;

    render_initialize_colors(render, &conf, wayl_do_linear_blending(wayl));

    matches_set_wayland(matches, wayl);
    ctx.wayl = wayl;

    /* Create thread that will populate the application list */
    if (pipe2(event_pipe, O_CLOEXEC | O_NONBLOCK) < 0) {
        LOG_ERRNO("failed to create event pipe");
        goto out;
    }

    ctx.event_fd = event_pipe[1];

    if (!fdm_add(fdm, event_pipe[0], EPOLLIN, &fdm_apps_populated, &ctx))
        goto out;

    if (thrd_create(&app_thread_id, &populate_apps, &ctx) != thrd_success) {
        LOG_ERR("failed to create thread");
        goto out;
    }

    join_app_thread = true;

    /*
     * Render immediately, even if empty
     *
     * We do this in dmenu mode only; we assume there's at least one
     * .desktop file in application mode, and we don't want to waste
     * resources on rendering an empty window while loading the
     * applications.
     *
     * Even if we're in dmenu mode, we don't always render immediately
     *  - exit-immediately-if-empty: we don't want to display anything
     *    if the final list is empty
     *  - minimal-lines: we need to know how many items we're going to
     *    display
     */
    if (conf.dmenu.enabled) {
        if (!conf.dmenu.exit_immediately_if_empty && !conf.minimal_lines)
            wayl_ready_to_display(wayl);
    }

    wayl_refresh(wayl);

    while (true) {
        wayl_flush(wayl);
        if (!fdm_poll(fdm))
            break;
    }

    if (wayl_update_cache(wayl))
        write_cache(conf.cache_path, apps, conf.dmenu.enabled);

    ret = wayl_exit_code(wayl);

out:
    if (join_app_thread) {
        if (dmenu_abort_fd >= 0) {
            if (write(dmenu_abort_fd, &(uint64_t){1}, sizeof(uint64_t)) < 0)
                LOG_ERRNO("failed to signal abort");
        }

        int res;
        thrd_join(app_thread_id, &res);

        if (res != 0) {
            if (res < 0)
                LOG_ERRNO_P("populate application list thread failed", res);
            else
                LOG_ERRNO("populate application list thread failed: "
                          "failed to signal done event");
        }
    }

    if (event_pipe[0] >= 0) {
        fdm_del_no_close(fdm, event_pipe[0]);
        close(event_pipe[0]);
    }
    if (event_pipe[1] >= 0)
        close(event_pipe[1]);

    if (dmenu_abort_fd >= 0)
        close(dmenu_abort_fd);

    mtx_destroy(&icon_lock);

    shm_fini();

    wayl_destroy(wayl);
    kb_manager_destroy(kb_manager);
    render_destroy(render);
    matches_destroy(matches);
    prompt_destroy(prompt);
    fdm_destroy(fdm);
    applications_destroy(apps);
    icon_themes_destroy(themes);
    //free(prompt_allocated);
    config_free(&conf);

#if defined(FUZZEL_ENABLE_CAIRO) && defined(_DEBUG)
    cairo_debug_reset_static_data();
#endif
    log_deinit();

    if (file_lock_fd >= 0)
        close(file_lock_fd);
    if (lock_file != NULL) {
        if (unlink_lock_file)
            unlink(lock_file);
        free(lock_file);
    }
    return ret;
}
