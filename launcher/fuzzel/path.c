#include "path.h"
#include "application.h"
#include "tllist.h"

#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdlib.h>
#include <stdio.h>

#define LOG_MODULE "path"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "xdg.h"
#include "char32.h"
#include "xmalloc.h"

void
path_find_programs(struct application_list *applications)
{
    const char *_path = getenv("PATH");

    if (_path == NULL)
        return ;

    char *copy = xstrdup(_path);
    char *ctx = NULL;
    tll(struct application *) entries = tll_init();

    for (const char *tok = strtok_r(copy, ":", &ctx);
         tok != NULL;
         tok = strtok_r(NULL, ":", &ctx))
    {
        DIR *d = opendir(tok);
        if (d == NULL) {
            LOG_WARN("failed to open %s from PATH: %s", tok, strerror(errno));
            continue;
        }
        int fd = dirfd(d);
        if (fd == -1) {
            LOG_WARN("failed to open %s's fd from PATH: %s", tok, strerror(errno));
            closedir(d);
            continue;
        }
        for (const struct dirent *e = readdir(d); e != NULL; e = readdir(d)) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;

            struct stat st;
            if (fstatat(fd, e->d_name, &st, 0) == -1) {
                LOG_WARN("%s: failed to stat: %s", e->d_name, strerror(errno));
                continue;
            }
            if (S_ISREG(st.st_mode) && st.st_mode & S_IXUSR) {
                char32_t *wtitle = ambstoc32(e->d_name);
                if (wtitle == NULL)
                    continue;
                bool already_exist = false;
                tll_foreach(entries, it) {
                    if (c32cmp(wtitle, it->item->title) == 0) {
                        already_exist = true;
                        break;
                    }
                };
                if (already_exist) {
                    free(wtitle);
                    continue;
                }

                char32_t *lowercase = xc32dup(wtitle);
                for (size_t i = 0; i < c32len(lowercase); i++)
                    lowercase[i] = toc32lower(lowercase[i]);

                struct application *app = xmalloc(sizeof(*app));
                *app = (struct application){
                    .index = 0,  /* Not used in application mode */
                    .title = wtitle,
                    .title_lowercase = lowercase,
                    .title_len = c32len(lowercase),
                    .exec = xstrjoin3(tok, "/", e->d_name),
                    .visible = true,
                };

                tll_push_back(entries, app);
            }
        }
        closedir(d);
    }
    free(copy);

    applications->v = xreallocarray(
        applications->v, applications->count + tll_length(entries), sizeof(applications->v[0]));

    tll_foreach(entries, it) {
        applications->v[applications->count++] = it->item;
        applications->visible_count++;
        tll_remove(entries, it);
    }
}
