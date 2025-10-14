#include "dmenu.h"
#include "column.h"
#include "icon.h"

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <fcntl.h>
#include <poll.h>
#include <uchar.h>
#include <unistd.h>

#include <tllist.h>

#define LOG_MODULE "dmenu"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "char32.h"
#include "event.h"
#include "macros.h"
#include "xmalloc.h"

void
dmenu_load_entries(struct application_list *applications, char delim,
                   const char *with_nth_format, const char *match_nth_format,
                   char nth_delim, int event_fd, int abort_fd)
{
    tll(struct application *) entries = tll_init();

    size_t size = 0;
    size_t alloc_size = 16384;
    char *buffer = xmalloc(alloc_size);

    int flags = fcntl(STDIN_FILENO, F_GETFL);
    if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) {
        LOG_ERRNO("failed to set O_NONBLOCK on stdin");
        goto out;
    }

    size_t app_idx = 0;

    errno = 0;
    while (true) {
        struct pollfd fds[] = {
            {.fd = STDIN_FILENO, .events = POLLIN},

            /* Must be last */
            {.fd = abort_fd, .events = POLLIN},
        };

        size_t count = ALEN(fds);
        if (abort_fd < 0)
            count--;

        int ret = poll(fds, count, -1);
        if (ret < 0) {
            LOG_ERRNO("failed to poll stdin");
            break;
        }

        if (fds[1].revents & (POLLIN | POLLHUP)) {
            LOG_DBG("aborted");
            break;
        }

        /* Increase size of input buffer, if necessary */
        if (size >= alloc_size - 2) {
            LOG_DBG("increasing input buffer size %zu -> %zu",
                    alloc_size, alloc_size * 2);

            alloc_size *= 2;
            buffer = xrealloc(buffer, alloc_size);
        }

        ssize_t bytes_read = read(
            STDIN_FILENO, &buffer[size], alloc_size - size - 1);

        if (bytes_read < 0) {
            if (errno == EINTR)
                continue;
            LOG_ERRNO("failed to read from stdin");
            break;
        } else if (bytes_read == 0 && size == 0) {
            /* No more data on stdin, and all buffered data consumed */
            break;
        }

        size += bytes_read;

        /*
         * Consume as much as possible from the input buffer
         *
         * Input buffer may contain many lines, resulting in multiple
         * applications being added.
         *
         * Input buffer may not even contain a single line, in which
         * case we can't consume anything at all.
         *
         * Input buffer may contain one or more full lines, followed
         * by a final partial line. If stdin has been closed
         * (i.e. there's no more input to read), then treat the last,
         * partial line as a complete line (i.e. pretend it's followed
         * by a delimiter). But, if stdin is still open, don't consume
         * the last, partial line. Instead, wait for more input.
         */
        while (size > 0) {
            char *delim_at = memchr(buffer, delim, size);
            if (delim_at == NULL) {
                if (bytes_read > 0) {
                    /* No delimiter yet, wait for more data */
                    LOG_DBG("no delimiter found, waiting for more data...");
                    break;
                } else {
                    /* No delimiter, but stdin has been closed. Treat
                       the last chunk of data as a complete line. */
                    LOG_DBG("last line: pointing delim_at to the end of the buffer");
                    delim_at = &buffer[size];
                    size++;
                }
            }

            const size_t entry_len = delim_at - buffer;
            *delim_at = '\0';

            /*
             * Support Rofi’s extended dmenu protocol. One can specify
             * an icon by appending ‘\0icon\x1f<icon-name>’ to the
             * entry:
             *
             *  “hello world\0icon\x1ffirefox”
             *
             * We also support fallback icons using comma-separated values.
             * When the primary icon is not found, subsequent icons in the
             * list will be tried until one is successfully loaded:
             *
             *  “hello world\0icon\x1ffirefox,web-browser,application-x-executable”
             */
            char *icon_name = NULL;
            const char *extra = memchr(buffer, '\0', entry_len);

            if (extra != NULL) {
                const size_t extra_len = delim_at - extra;

                /*
                 * 'extra' is "\0icon\x1f" - 6 characters. Require
                 * *more* than 6, since icon *name* cannot be empty
                 */
                if (extra_len > 6 && memcmp(extra, "\0icon\x1f", 6) == 0)
                    icon_name = xstrndup(extra + 6, delim_at - (extra + 6));
            }

            LOG_DBG("%s (icon=%s)", buffer, icon_name);

            char32_t *wline = ambstoc32(buffer);

            /* Consume entry from input buffer */
            const size_t consume = delim_at + 1 - buffer;
            assert(consume <= size);

            memmove(buffer, delim_at + 1, size - consume);
            size -= consume;

            if (wline == NULL) {
                free(icon_name);
                continue;
            }

            char32_t *title = with_nth_format == NULL
                ? xc32dup(wline)
                : nth_column(wline, nth_delim, with_nth_format);

            char32_t *lowercase = xc32dup(title);
            for (size_t i = 0; i < c32len(lowercase); i++)
                lowercase[i] = toc32lower(lowercase[i]);

            char32_t *match_nth = match_nth_format != NULL
                ? nth_column(wline, nth_delim, match_nth_format)
                : NULL;

            if (match_nth != NULL) {
                for (size_t i = 0; i < c32len(match_nth); i++)
                    match_nth[i] = toc32lower(match_nth[i]);
            }

            struct application *app = xmalloc(sizeof(*app));
            *app = (struct application){
                .index = app_idx++,
                .dmenu_input = wline,
                .dmenu_match_nth = match_nth,
                .dmenu_match_nth_len = match_nth != NULL ? c32len(match_nth) : 0,
                .title = title,
                .title_lowercase = lowercase,
                .title_len = c32len(lowercase),
                .icon = {.name = icon_name},
                .visible = true,
            };

            tll_push_back(entries, app);
        }

        if (event_fd >= 0) {
            mtx_lock(&applications->lock);

            const size_t new_count = applications->count + tll_length(entries);
            if (new_count > 0) {
                applications->v = xreallocarray(
                    applications->v, new_count, sizeof(applications->v[0]));

                size_t i = applications->count;
                tll_foreach(entries, it) {
                    applications->v[i++] = it->item;
                    tll_remove(entries, it);
                }

                assert(i == new_count);
                applications->count = applications->visible_count = new_count;
                send_event(event_fd, EVENT_APPS_SOME_LOADED);
            }

            mtx_unlock(&applications->lock);
        }
    }

    free(buffer);

out:

    if (tll_length(entries) == 0)
        return;

    mtx_lock(&applications->lock);

    const size_t new_count = applications->count + tll_length(entries);
    applications->v = xreallocarray(
        applications->v, new_count, sizeof(applications->v[0]));

    size_t i = applications->count;
    tll_foreach(entries, it) {
        applications->v[i++] = it->item;
        tll_remove(entries, it);
    }

    applications->count = applications->visible_count = new_count;
    mtx_unlock(&applications->lock);

    tll_free(entries);
}

bool
dmenu_execute(const struct application *app, ssize_t index,
              const struct prompt *prompt, enum dmenu_mode format,
              const char *nth_format, char nth_delim)
{
    switch (format) {
    case DMENU_MODE_TEXT: {
        const char32_t *output = app != NULL
            ? app->dmenu_input
            : prompt_text(prompt);

        char32_t *column_output = NULL;
        if (nth_format != NULL) {
            column_output = nth_column(output, nth_delim, nth_format);
            output = column_output;
        }

        char *text = ac32tombs(output);
        if (text != NULL)
            printf("%s\n", text);

        free(column_output);
        free(text);
        break;
    }

    case DMENU_MODE_INDEX:
        printf("%zd\n", index);
        break;
    }

    return true;
}

static void
try_icon_list(struct application *app, icon_theme_list_t themes, int icon_size)
{
    if (app->icon.name == NULL || strchr(app->icon.name, ',') == NULL)
        return;

    if (app->icon.type != ICON_NONE) {
        return;
    }

    char *icon_list = xstrdup(app->icon.name);
    char *saveptr = NULL;

    for (char *icon_name = strtok_r(icon_list, ",", &saveptr);
         icon_name != NULL;
         icon_name = strtok_r(NULL, ",", &saveptr))
    {
        free(app->icon.name);
        app->icon.name = xstrdup(icon_name);

        app->icon.type = ICON_NONE;
        app->icon.path = NULL;
        app->icon.svg = NULL;
        app->icon.png = NULL;

        struct application_list temp_list = {
            .v = &app,
            .count = 1,
            .visible_count = 1
        };

        icon_lookup_application_icons(themes, icon_size, &temp_list);

        if (app->icon.type != ICON_NONE)
            break;
    }

    free(icon_list);
}

void
dmenu_try_icon_list(struct application_list *applications, icon_theme_list_t themes, int icon_size)
{
    for (size_t i = 0; i < applications->count; i++) {
        struct application *app = applications->v[i];
        try_icon_list(app, themes, icon_size);
    }
}
