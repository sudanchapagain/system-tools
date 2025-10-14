#include "xdg.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>

#define LOG_MODULE "xdg"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "char32.h"
#include "icon.h"
#include "macros.h"
#include "xmalloc.h"
#include "xsnprintf.h"

typedef tll(struct application *) application_llist_t;

struct locale_variants {
    char *lang_country_modifier;
    char *lang_country;
    char *lang_modifier;
    char *lang;
};

struct action {
    char32_t *name;
    char32_t *generic_name;
    char *app_id;
    char32_t *comment;
    char32_list_t keywords;
    char32_list_t categories;
    char_list_t onlyshowin;
    char_list_t notshowin;

    int name_locale_score;
    int generic_name_locale_score;
    int comment_locale_score;
    int keywords_locale_score;
    int categories_locale_score;

    char *icon;
    char *exec;
    char32_t *wexec;

    char *path;
    bool visible;
    bool use_terminal;
    bool no_startup_notify;
};

static bool
filter_desktop_entry(const struct action *act, const char_list_t *desktops)
{
    /* If a matching entry is found in OnlyShowIn then the desktop file is
     * shown. If an entry is found in NotShowIn then the desktop file is not
     * shown. */
    tll_foreach(*desktops, current) {
        tll_foreach(act->onlyshowin, desktop) {
            if (strcmp(current->item, desktop->item) == 0)
                return true;
        }

        tll_foreach(act->notshowin, desktop) {
            if (strcmp(current->item, desktop->item) == 0)
                return false;
        }
    }

    /* By default, a desktop file should be shown, unless an OnlyShowIn key is
     * present, in which case, the default is for the file not to be shown. */
    return tll_length(act->onlyshowin) == 0;
}

static void
parse_desktop_file(int fd, char *id, const char32_t *file_basename_lowercase,
                   const char *terminal, bool include_actions,
                   bool filter_desktops, char_list_t *desktops,
                   const struct locale_variants *lc_messages,
                   application_llist_t *applications)
{
    FILE *f = fdopen(fd, "re");
    if (f == NULL) {
        close(fd);
        return;
    }

    bool is_desktop_entry = false;

    tll(char *) action_names = tll_init();

    tll(struct action) actions = tll_init();
    tll_push_back(actions, ((struct action){.visible = true}));
    struct action *action = &tll_back(actions);
    struct action *default_action = action;

    while (true) {
        char *raw_line = NULL;
        size_t sz = 0;
        ssize_t len = getline(&raw_line, &sz, f);

        if (len == -1) {
            free(raw_line);
            break;
        }

        if (len == 0) {
            free(raw_line);
            continue;
        }

        if (raw_line[len - 1] == '\n') {
            raw_line[len - 1] = '\0';
            len--;
        }

        char *line = raw_line;

        /* Strip leading whitespace */
        while (len > 0 && isspace(line[0])) {
            line++;
            len--;
        }

        /* Strip trailing whitespace */
        while (len > 0 && isspace(line[len - 1])) {
            line[len - 1] = '\0';
            len--;
        }

        if (len == 0) {
            free(raw_line);
            continue;
        }

        if (line[0] == '[' && line[len - 1] == ']') {
            if (strncasecmp(&line[1], "desktop entry", len - 2) == 0) {
                is_desktop_entry = true;
                free(raw_line);
                continue;
            }

            else if (include_actions &&
                     strncasecmp(&line[1], "desktop action ", 15) == 0)
            {
                const char *action_name = &line[16];
                const size_t name_len = len - 1 - 16;

                bool action_is_valid = false;
                tll_foreach(action_names, it) {
                    if (strlen(it->item) == name_len &&
                        strncmp(it->item, action_name, name_len) == 0)
                    {
                        tll_push_back(actions, ((struct action){0}));
                        action = &tll_back(actions);

                        action->generic_name =
                            default_action->generic_name != NULL
                            ? xc32dup(default_action->generic_name) : NULL;
                        action->comment = default_action->comment != NULL
                            ? xc32dup(default_action->comment) : NULL;
                        action->path = default_action->path != NULL
                            ? xstrdup(default_action->path) : NULL;
                        action->icon = default_action->icon != NULL
                            ? xstrdup(default_action->icon) : NULL;
                        action->visible = default_action->visible;
                        action->use_terminal = default_action->use_terminal;

                        tll_foreach(default_action->keywords, it)
                            tll_push_back(action->keywords, xc32dup(it->item));
                        tll_foreach(default_action->categories, it)
                            tll_push_back(action->categories, xc32dup(it->item));

                        action_is_valid = true;
                        break;
                    }
                }

                free(raw_line);
                if (action_is_valid)
                    continue;
                else
                    break;
            }

            else {
                free(raw_line);
                break;
            }
        }

        char *ctx;
        char *key = strtok_r(line, "=", &ctx);
        char *value = strtok_r(NULL, "\n", &ctx);
        int locale_score = 1;  /* Default, locale not specified in key */

        if (key != NULL && value != NULL) {
            size_t key_len = strlen(key);

            /* Strip trailing whitespace from the key name */
            while (key_len > 1 && isspace(key[key_len - 1])) {
                key[key_len - 1] = '\0';
                key_len--;
            }

            /* Strip leading whitespace from value */
            while (value[0] != '\0' && isspace(value[0]))
                value++;

            if (key[key_len - 1] == ']') {
                char *locale = strchr(key, '[');
                if (locale != NULL) {
                    /* NULL terminate key */
                    *locale = '\0';

                    /* NULL terminate locale */
                    key[key_len - 1] = '\0';
                    locale++;  /* Skip past ‘[’ */

                    if (lc_messages->lang_country_modifier != NULL &&
                        strcmp(locale, lc_messages->lang_country_modifier) == 0)
                    {
                        locale_score = 5;
                    } else if (lc_messages->lang_country != NULL &&
                               strcmp(locale, lc_messages->lang_country) == 0)
                    {
                        locale_score = 4;
                    } else if (lc_messages->lang_modifier != NULL &&
                               strcmp(locale, lc_messages->lang_modifier) == 0)
                    {
                        locale_score = 3;
                    } else if (lc_messages->lang != NULL &&
                               strcmp(locale, lc_messages->lang) == 0)
                    {
                        locale_score = 2;
                    } else {
                        /* Key has locale, but didn’t match. Make sure
                         * we don’t use its value */
                        locale_score = -1;
                    }
                }
            }

            if (strcmp(key, "Name") == 0) {
                if (locale_score > action->name_locale_score) {
                    free(action->name);
                    action->name = ambstoc32(value);
                    action->name_locale_score = locale_score;
                }
            }

            else if (strcmp(key, "Exec") == 0) {
                free(action->exec);
                free(action->wexec);
                action->exec = xstrdup(value);
                action->wexec = ambstoc32(value);
            }

            else if (strcmp(key, "Path") == 0) {
                free(action->path);
                action->path = xstrdup(value);
            }

            else if (strcmp(key, "GenericName") == 0) {
                if (locale_score > action->generic_name_locale_score) {
                    free(action->generic_name);
                    action->generic_name = ambstoc32(value);
                    action->generic_name_locale_score = locale_score;
                }
            }

            else if (strcmp(key, "StartupNotify") == 0) {
                if (strcmp(value, "false"))
                    action->no_startup_notify = true;
            }

            else if (strcmp(key, "StartupWMClass") == 0) {
                free(action->app_id);
                action->app_id = xstrdup(value);
            }

            else if (strcmp(key, "Comment") == 0) {
                if (locale_score > action->comment_locale_score) {
                    free(action->comment);
                    action->comment = ambstoc32(value);
                    action->comment_locale_score = locale_score;
                }
            }

            else if (strcmp(key, "Keywords") == 0) {
                if (locale_score > action->keywords_locale_score) {
                    for (const char *kw = strtok_r(value, ";", &ctx);
                         kw != NULL;
                         kw = strtok_r(NULL, ";", &ctx))
                    {
                        char32_t *wide_kw = ambstoc32(kw);
                        if (wide_kw != NULL)
                            tll_push_back(action->keywords, wide_kw);
                    }

                    action->keywords_locale_score = locale_score;
                }
            }

            else if (strcmp(key, "Categories") == 0) {
                if (locale_score > action->categories_locale_score) {
                    for (const char *category = strtok_r(value, ";", &ctx);
                         category != NULL;
                         category = strtok_r(NULL, ";", &ctx))
                    {
                        char32_t *wide_category = ambstoc32(category);
                        if (wide_category != NULL)
                            tll_push_back(action->categories, wide_category);
                    }

                    action->categories_locale_score = locale_score;
                }
            }

            else if (strcmp(key, "Actions") == 0) {
                for (const char *action = strtok_r(value, ";", &ctx);
                     action != NULL;
                     action = strtok_r(NULL, ";", &ctx))
                {
                    tll_push_back(action_names, xstrdup(action));
                }
            }

            else if (strcmp(key, "OnlyShowIn") == 0) {
                for (const char *desktop = strtok_r(value, ";", &ctx);
                     desktop != NULL;
                     desktop = strtok_r(NULL, ";", &ctx))
                {
                    tll_push_back(action->onlyshowin, xstrdup(desktop));
                }
            }

            else if (strcmp(key, "NotShowIn") == 0) {
                for (const char *desktop = strtok_r(value, ";", &ctx);
                     desktop != NULL;
                     desktop = strtok_r(NULL, ";", &ctx))
                {
                    tll_push_back(action->notshowin, xstrdup(desktop));
                }
            }

            else if (strcmp(key, "Icon") == 0) {
                free(action->icon);
                action->icon = xstrdup(value);
            }

            else if (strcmp(key, "Hidden") == 0 ||
                     strcmp(key, "NoDisplay") == 0)
            {
                if (strcmp(value, "true") == 0)
                    action->visible = false;
            }

            else if (strcmp(key, "Terminal") == 0) {
                if (strcmp(value, "true") == 0)
                    action->use_terminal = true;
            }
        }
        free(raw_line);
    }

    fclose(f);

    tll_foreach(actions, it) {
        struct action *a = &it->item;

        if (!is_desktop_entry) {
            free(a->name);
            free(a->generic_name);
            free(a->app_id);
            free(a->comment);
            free(a->icon);
            free(a->exec);
            free(a->wexec);
            free(a->path);

            tll_free_and_free(a->keywords, free);
            tll_free_and_free(a->categories, free);
            tll_free_and_free(a->onlyshowin, free);
            tll_free_and_free(a->notshowin, free);

            continue;
        }

        if (a->name == NULL)
            a->name = ambstoc32("<no title>");

        if (a->use_terminal && terminal != NULL && a->exec != NULL) {
            char *exec_with_terminal;

            // Check if terminal command contains {cmd} placeholder
            if (strstr(terminal, "{cmd}") != NULL) {
                // Replace all occurrences of {cmd} with the actual command
                const char *placeholder = "{cmd}";
                const size_t placeholder_len = 5; // strlen("{cmd}")
                const size_t exec_len = strlen(a->exec);
                const size_t terminal_len = strlen(terminal);

                // Count occurrences of {cmd}
                size_t occurrences = 0;
                const char *pos = terminal;
                while ((pos = strstr(pos, placeholder)) != NULL) {
                    occurrences++;
                    pos += placeholder_len;
                }

                // Calculate final string size
                assert(terminal_len >= occurrences * placeholder_len); // should be true, so let's assert it is
                size_t final_len = terminal_len - occurrences * placeholder_len + occurrences * exec_len + 1;
                exec_with_terminal = xmalloc(final_len);

                // Replace all {cmd} occurrences
                const char *src = terminal;
                char *dst = exec_with_terminal;
                while ((pos = strstr(src, placeholder)) != NULL) {
                    // Copy part before {cmd}
                    size_t prefix_len = pos - src;
                    memcpy(dst, src, prefix_len);
                    dst += prefix_len;

                    // Copy the exec command
                    memcpy(dst, a->exec, exec_len);
                    dst += exec_len;

                    // Move past {cmd}
                    src = pos + placeholder_len;
                }

                // Copy remaining part
                strcpy(dst, src);
            } else {
                // Use the old behavior: "terminal command exec"
                exec_with_terminal = xstrjoin3(terminal, " ", a->exec);
            }

            free(a->exec);
            a->exec = exec_with_terminal;
        }

        char32_t *title = a->name;
        if (a != default_action) {
            title = xc32join3(default_action->name, U" — ", a->name);
            free(a->name);
        }

        const size_t title_len = c32len(title);
        const size_t basename_len = file_basename_lowercase != NULL
            ? c32len(file_basename_lowercase)
            : 0;
        const size_t wexec_len = action->wexec != NULL
            ? c32len(action->wexec)
            : 0;
        const size_t generic_name_len = action->generic_name != NULL
            ? c32len(action->generic_name)
            : 0;
        const size_t comment_len = action->comment != NULL
            ? c32len(action->comment)
            : 0;

        char32_t *title_lowercase = xc32dup(title);
        for (size_t i = 0; i < title_len; i++)
            title_lowercase[i] = toc32lower(title_lowercase[i]);

        for (size_t i = 0; i < wexec_len; i++)
            action->wexec[i] = toc32lower(action->wexec[i]);
        for (size_t i = 0; i < generic_name_len; i++)
            action->generic_name[i] = toc32lower(action->generic_name[i]);
        for (size_t i = 0; i < comment_len; i++)
            action->comment[i] = toc32lower(action->comment[i]);
        tll_foreach(a->keywords, it) {
            for (size_t i = 0; i < c32len(it->item); i++)
                it->item[i] = toc32lower(it->item[i]);
        }
        tll_foreach(a->categories, it) {
            for (size_t i = 0; i < c32len(it->item); i++)
                it->item[i] = toc32lower(it->item[i]);
        }

        struct application *app = xmalloc(sizeof(*app));
        *app = (struct application){
            .id = xstrdup(id),
            .index = 0,  /* Not used in application mode */
            .path = a->path,
            .exec = a->exec,
            .app_id = a->app_id,
            .title = title,
            .title_lowercase = title_lowercase,
            .basename = xc32dup(file_basename_lowercase),
            .wexec = a->wexec,
            .generic_name = a->generic_name,
            .comment = a->comment,
            .keywords = a->keywords,
            .categories = a->categories,
            .title_len = title_len,
            .basename_len = basename_len,
            .wexec_len = wexec_len,
            .generic_name_len = generic_name_len,
            .comment_len = comment_len,
            .icon = {.name = a->icon},
            .visible = a->visible && (!filter_desktops || filter_desktop_entry(a, desktops)),
            .startup_notify = !a->no_startup_notify,
            .count = 0,
        };
        tll_push_back(*applications, app);
        tll_free_and_free(a->onlyshowin, free);
        tll_free_and_free(a->notshowin, free);
    }

    free(id);
    tll_free(actions);
    tll_free_and_free(action_names, free);
}

static char *
new_id(const char *base_id, const char *new_part)
{
    if (base_id == NULL)
        return xstrdup(new_part);

    return xstrjoin3(base_id, "-", new_part);
}

static void
scan_dir(int base_fd, const char *terminal, bool include_actions,
         bool filter_desktop, char_list_t *desktops,
         application_llist_t *applications, const char *base_id)
{
    DIR *d = fdopendir(base_fd);
    if (d == NULL) {
        LOG_ERRNO("failed to open directory");
        return;
    }

    const char *locale = setlocale(LC_MESSAGES, NULL);
    struct locale_variants lc_messages = {NULL};

    if (locale != NULL) {
        char *lang = xstrdup(locale);
        char *country_start = strchr(lang, '_');
        char *encoding_start = strchr(lang, '.');
        char *modifier_start = strchr(lang, '@');

        // Replace found delimiters with null terminators and skip past them
        if (country_start != NULL)
            *country_start++ = '\0';
        if (encoding_start != NULL)
            *encoding_start++ = '\0';
        if (modifier_start != NULL)
            *modifier_start++ = '\0';

        lc_messages.lang = lang;
        if (country_start != NULL)
            lc_messages.lang_country = xstrjoin3(lang, "_", country_start);

        if (modifier_start != NULL)
            lc_messages.lang_modifier = xstrjoin3(lang, "@", modifier_start);

        if (country_start != NULL && modifier_start != NULL)
            lc_messages.lang_country_modifier = xstrjoin3(
                    lc_messages.lang_country, "@", modifier_start);

        LOG_DBG("lang=%s, lang_country=%s, lang@modifier=%s, lang_country@modifier=%s",
                lc_messages.lang,
                loggable_str(lc_messages.lang_country),
                loggable_str(lc_messages.lang_modifier),
                loggable_str(lc_messages.lang_country_modifier));
    }


    for (const struct dirent *e = readdir(d); e != NULL; e = readdir(d)) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;

        struct stat st;
        if (fstatat(base_fd, e->d_name, &st, 0) == -1) {
            LOG_WARN("%s: failed to stat: %s", e->d_name, strerror(errno));
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            int dir_fd = openat(base_fd, e->d_name, O_RDONLY | O_CLOEXEC);
            if (dir_fd == -1) {
                LOG_ERRNO("%s: failed to open", e->d_name);
                continue;
            }

            char *nested_base_id = new_id(base_id, e->d_name);
            scan_dir(dir_fd, terminal, include_actions,
                     filter_desktop, desktops,
                     applications, nested_base_id);
            free(nested_base_id);
            close(dir_fd);
        } else if (S_ISREG(st.st_mode)) {
            /* Skip files not ending with ".desktop" */
            const size_t desktop_len = strlen(".desktop");
            const size_t name_len = strlen(e->d_name);
            if (name_len < desktop_len)
                continue;

            if (strcmp(&e->d_name[name_len - desktop_len], ".desktop") != 0)
                continue;

            //LOG_DBG("%s", e->d_name);
            int fd = openat(base_fd, e->d_name, O_RDONLY | O_CLOEXEC);
            if (fd == -1)
                LOG_WARN("%s: failed to open: %s", e->d_name, strerror(errno));
            else {
                const char *file_basename = strrchr(e->d_name, '/');
                if (file_basename == NULL)
                    file_basename = e->d_name;
                else
                    file_basename++;

                const char *extension = strrchr(file_basename, '.');
                if (extension == NULL)
                    extension = file_basename + strlen(file_basename);

                int chars = mbsntoc32(NULL, file_basename, extension - file_basename, 0);
                assert(chars >= 0);

                char32_t wfile_basename[chars + 1];
                mbsntoc32(wfile_basename, file_basename, extension - file_basename, chars);
                wfile_basename[chars] = U'\0';

                for (size_t i = 0; i < chars; i++)
                    wfile_basename[i] = toc32lower(wfile_basename[i]);

                char *id = new_id(base_id, e->d_name);

                bool already_added = false;
                tll_foreach(*applications, it) {
                    if (strcmp(it->item->id, id) == 0) {
                        already_added = true;
                        break;
                    }
                }

                if (!already_added) {
                    parse_desktop_file(
                        fd, id, wfile_basename, terminal, include_actions,
                        filter_desktop, desktops,
                        &lc_messages, applications);
                    /* fd closed by parse_desktop_file() */
                } else {
                    free(id);
                    close(fd);
                }
            }
        }
    }

    free(lc_messages.lang);
    free(lc_messages.lang_country);
    free(lc_messages.lang_modifier);
    free(lc_messages.lang_country_modifier);
    closedir(d);
}

static int
sort_application_by_title(const void *_a, const void *_b)
{
    const struct application *const *a = _a;
    const struct application *const *b = _b;
    return c32casecmp((*a)->title, (*b)->title);
}

void
xdg_find_programs(const char *terminal, bool include_actions,
                  bool filter_desktop, char_list_t *desktops,
                  struct application_list *applications)
{
    application_llist_t apps = tll_init();

    xdg_data_dirs_t dirs = xdg_data_dirs();
    tll_foreach(dirs, it) {
        char path[strlen(it->item.path) + 1 + strlen("applications") + 1];
        sprintf(path, "%s/applications", it->item.path);

        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd != -1) {
            scan_dir(fd, terminal, include_actions, filter_desktop, desktops, &apps, NULL);
            close(fd);
        }
    }

    mtx_lock(&applications->lock);
    applications->count = tll_length(apps);
    if (applications->count > 0) {
        applications->v = xmalloc(tll_length(apps) * sizeof(applications->v[0]));
    } else {
        LOG_WARN("No applications found. See SEARCH PATHS in `man fuzzel` for details.");
        applications->v = NULL;
    }

    size_t i = 0;
    tll_foreach(apps, it) {
        applications->v[i++] = it->item;
        if (it->item->visible)
            applications->visible_count++;
        tll_remove(apps, it);
    }
    tll_free(apps);

    qsort(applications->v, applications->count, sizeof(applications->v[0]),
          &sort_application_by_title);
    mtx_unlock(&applications->lock);

    xdg_data_dirs_destroy(dirs);

#if defined(_DEBUG) && LOG_ENABLE_DBG && 0
    for (size_t i = 0; i < applications->count; i++) {
        const struct application *app = &applications->v[i];

        char32_t keywords[1024];
        char32_t categories[1024];

        int idx = 0;
        tll_foreach(app->keywords, it) {
            idx += swprintf(&keywords[idx],
                            ALEN(keywords) - idx,
                            L"%ls, ", (const wchar_t *)it->item);
        }

        if (idx > 0)
            keywords[idx - 2] = U'\0';

        idx = 0;
        tll_foreach(app->categories, it) {
            idx += swprintf(&categories[idx],
                            ALEN(categories) - idx,
                            L"%ls, ", (const wchar_t *)it->item);
        }

        if (idx > 0)
            categories[idx - 2] = U'\0';

        LOG_DBG("%s:\n"
                "  name/title:   %ls\n"
                "  path:         %s\n"
                "  exec:         %s\n"
                "  basename:     %ls\n"
                "  generic-name: %ls\n"
                "  comment:      %ls\n"
                "  keywords:     %ls\n"
                "  categories:   %ls\n"
                "  icon:\n"
                "    name: %s\n"
                "    type: %s",
                app->id, (const wchar_t *)app->title, app->path, app->exec,
                (const wchar_t *)app->basename,
                (const wchar_t *)app->generic_name,
                (const wchar_t *)app->comment,
                (const wchar_t *)keywords,
                (const wchar_t *)categories,
                app->icon.name,
                app->icon.type == ICON_PNG ? "PNG" :
                app->icon.type == ICON_SVG ? "SVG" : "<none>");


    }
#endif
}

xdg_data_dirs_t
xdg_data_dirs(void)
{
    xdg_data_dirs_t ret = tll_init();

    const char *xdg_data_home = getenv("XDG_DATA_HOME");
    const char *home;
    if (xdg_data_home != NULL && xdg_data_home[0] != '\0') {
        int fd = open(xdg_data_home, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd >= 0) {
            struct xdg_data_dir d = {.fd = fd, .path = xstrdup(xdg_data_home)};
            tll_push_back(ret, d);
        }
    } else if ((home = getenv("HOME")) != NULL && home[0] != '\0') {
        char *path = xstrjoin(home, "/.local/share");
        int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd >= 0) {
            struct xdg_data_dir d = {.fd = fd, .path = path};
            tll_push_back(ret, d);
        } else
            free(path);
    }

    const char *_xdg_data_dirs = getenv("XDG_DATA_DIRS");

    if (_xdg_data_dirs != NULL) {

        char *ctx = NULL;
        char *copy = xstrdup(_xdg_data_dirs);

        for (const char *tok = strtok_r(copy, ":", &ctx);
             tok != NULL;
             tok = strtok_r(NULL, ":", &ctx))
        {
            int fd = open(tok, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (fd >= 0) {
                struct xdg_data_dir d = {.fd = fd, .path = xstrdup(tok)};
                tll_push_back(ret, d);
            }
        }

        free(copy);
    } else {
        int fd1 = open("/usr/local/share", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        int fd2 = open("/usr/share", O_RDONLY | O_DIRECTORY | O_CLOEXEC);

        if (fd1 >= 0) {
            struct xdg_data_dir d = {.fd = fd1, .path = xstrdup("/usr/local/share")};
            tll_push_back(ret, d);
        }

        if (fd2 >= 0) {
            struct xdg_data_dir d = {.fd = fd2, .path = xstrdup("/usr/share")};
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

    const char *home = getenv("HOME");
    if (home != NULL && home[0] != '\0') {
        static char path[PATH_MAX];
        xsnprintf(path, sizeof(path), "%s/.cache", home);
        return path;
    }

    return NULL;
}
