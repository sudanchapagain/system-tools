#include "match.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <string.h>
#include <threads.h>

#include <sys/epoll.h>
#include <sys/timerfd.h>

#include "macros.h"
#if HAS_INCLUDE(<pthread_np.h>)
#include <pthread_np.h>
#define pthread_setname_np(thread, name) (pthread_set_name_np(thread, name), 0)
#elif defined(__NetBSD__)
#define pthread_setname_np(thread, name) pthread_setname_np(thread, "%s", (void *)name)
#endif

#include <tllist.h>

#define LOG_MODULE "match"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "char32.h"
#include "wayland.h"
#include "xmalloc.h"
#include "xsnprintf.h"

#define min(x, y) ((x < y) ? (x) : (y))
#define max(x, y) ((x > y) ? (x) : (y))

enum delayed_update_type {
    DELAYED_NO_UPDATE,
    DELAYED_FULL_UPDATE,
    DELAYED_INCREMENTAL_UPDATE,
    DELAYED_UPDATE_IN_PROGRESS,
};

struct matches {
    struct fdm *fdm;
    struct wayland *wayl;
    const struct prompt *prompt;
    struct application_list *applications;
    enum match_fields fields;
    bool all_apps_loaded;

    enum match_mode mode;
    bool sort_result;

    struct match *matches;
    _Atomic size_t match_count;  /* Number of matched applications */
    size_t matches_size;         /* Number of elements in 'matches' */

    size_t page_count;
    size_t selected;
    size_t max_matches_per_page;
    size_t fuzzy_min_length;
    size_t fuzzy_max_length_discrepancy;
    size_t fuzzy_max_distance;
    bool have_icons;

    size_t delay_ms;
    size_t delay_limit;
    int delay_fd;
    enum delayed_update_type delayed_update_type;

    /* Thread synchronization */
    struct {
        uint16_t count;
        sem_t start;
        sem_t done;
        mtx_t lock;
        tll(uint64_t) queue;
        thrd_t *threads;

        /* Set before feeding thread with sorting data */
        bool incremental;
        const char32_t *const *tokens;
        size_t *tok_lengths;
        size_t tok_count;

        struct match *old_matches;
    } workers;
};

struct thread_context {
    struct matches *matches;
    int my_id;
};

struct levenshtein_matrix {
    size_t distance;
    enum choice { UNSET, FIRST, SECOND, THIRD } choice;
};

static int match_thread(void *_ctx);

static char32_t *
match_exact(const char32_t *haystack, size_t haystack_len,
            const char32_t *needle, size_t needle_len)
{
    return memmem(haystack, haystack_len * sizeof(char32_t),
                  needle, needle_len * sizeof(char32_t));
}

static void
match_fzf(const char32_t *haystack, size_t haystack_len,
          const char32_t *needle, size_t needle_len,
          struct match_substring **pos, size_t *pos_count,
          enum matched_type *match_type)
{
    const char32_t *const needle_end = &needle[needle_len];
    const char32_t *const haystack_end = &haystack[haystack_len];

    const char32_t *haystack_search_start = haystack;

    while (needle < needle_end) {
        if (haystack_search_start >= haystack_end) {
            if (pos != NULL) {
                free(*pos);
                *pos = NULL;
                *pos_count = 0;
            }

            if (match_type != NULL)
                *match_type = MATCHED_NONE;

            return;
        }

        size_t longest_match_len = 0;
        size_t longest_match_ofs = 0;

        for (const char32_t *start = haystack_search_start;
             start < haystack_end;
             start++)
        {
            const char32_t *n = needle;
            const char32_t *h = start;

            size_t match_len = 0;
            while (n < needle_end &&
                   h < haystack_end && *n == *h)
            {
                match_len++;
                n++;
                h++;
            }

            if (match_len > longest_match_len) {
                longest_match_len = match_len;
                longest_match_ofs = start - haystack;
            }

            if (n >= needle_end) {
                /* We've matched all of the search string; no need to
                   look for any longer matches */
                break;
            }
        }

        if (longest_match_len == 0) {
            if (match_type != NULL)
                *match_type = MATCHED_NONE;
            return;
        }

        if (pos != NULL) {
            assert(pos_count != NULL);
            (*pos_count)++;
            *pos = xreallocarray(*pos, *pos_count, sizeof((*pos)[0]));
            (*pos)[*pos_count - 1].start = longest_match_ofs;
            (*pos)[*pos_count - 1].len = longest_match_len;
        }

        if (match_type != NULL)
            *match_type = MATCHED_EXACT;

        needle += longest_match_len;
        haystack_search_start = haystack + longest_match_ofs + longest_match_len;
    }
}

static void
levenshtein_distance(const char32_t *a, size_t alen,
                     const char32_t *b, size_t blen,
                     struct levenshtein_matrix **m)
{
    for (size_t i = 1; i <= blen; i++) {
        m[i][0].distance = i;
        m[i][0].choice = FIRST;
    }

    for (size_t i = 1; i <= blen; i++) {
        for (size_t j = 1; j <= alen; j++) {
            const size_t cost = a[j - 1] == b[i - 1] ? 0 : 1;
            const size_t first = m[i - 1][j].distance + 1;
            const size_t second = m[i][j - 1].distance + 1;
            const size_t third = m[i - 1][j - 1].distance + cost;

            const size_t shortest = min(min(first, second), third);
            m[i][j].distance = shortest;

            if (shortest == first)
                m[i][j].choice = FIRST;
            else if (shortest == second)
                m[i][j].choice = SECOND;
            else
                m[i][j].choice = THIRD;
        }
    }
}

static const char32_t *
match_levenshtein(struct matches *matches,
                  const char32_t *src, size_t src_len,
                  const char32_t *pat, size_t pat_len, size_t *_match_len)
{
    if (matches->mode != MATCH_MODE_FUZZY)
        return NULL;

    if (pat_len < matches->fuzzy_min_length)
        return NULL;

    if (src_len < pat_len)
        return NULL;

    struct levenshtein_matrix **m = xcalloc(pat_len + 1, sizeof(m[0]));
    for (size_t i = 0; i < pat_len + 1; i++)
        m[i] = xcalloc(src_len + 1, sizeof(m[0][0]));

    levenshtein_distance(src, src_len, pat, pat_len, m);

#if 0
    /* Dump levenshtein table */
    printf("     ");
    for (size_t j = 0; j < src_len; j++)
        printf("%lc  ", src[j]);
    printf("\n");
    for (size_t i = 0; i < pat_len + 1; i++) {
        if (i > 0)
            printf("%lc ", pat[i - 1]);
        else
            printf("  ");

        for (size_t j = 0; j < src_len + 1; j++)
            printf("%zu%c ", m[i][j].distance,
                   m[i][j].choice == FIRST ? 'f' :
                   m[i][j].choice == SECOND ? 's' :
                   m[i][j].choice == THIRD ? 't' : 'u');
        printf("\n");
    }
#endif

    size_t c = 0;
    size_t best = m[pat_len][0].distance;

    for (ssize_t j = src_len; j >= 0; j--) {
        if (m[pat_len][j].distance < best) {
            best = m[pat_len][j].distance;
            c = j;
        }
    }

    const size_t end = c;
    size_t r = pat_len;
    // printf("starting at r=%zu, c=%zu\n", r, c);

    while (r > 0) {
        switch (m[r][c].choice) {
        case UNSET: assert(false); break;
        case FIRST: r--; break;
        case SECOND: c--; break;
        case THIRD: r--; c--; break;
        }
    }

#if 0
    *match = (struct levenshtein_match){
        .start = c,
        .len = end - c,
        .distance = m[pat_len][end].distance,
    };
#endif
    const size_t match_ofs = c;
    const size_t match_distance = m[pat_len][end].distance;
    const size_t match_len = end - c;

    LOG_DBG("%ls vs. %ls: sub-string: %.*ls, (distance=%zu, row=%zu, col=%zu)",
            (const wchar_t *)src, (const wchar_t *)pat,
            (int)match_len, (const wchar_t *)&src[match_ofs], match_distance, r, c);

    for (size_t i = 0; i < pat_len + 1; i++)
        free(m[i]);
    free(m);

    const size_t len_diff = match_len > pat_len
        ? match_len - pat_len
        : pat_len - match_len;

    if (len_diff <= matches->fuzzy_max_length_discrepancy &&
        match_distance <= matches->fuzzy_max_distance)
    {
        if (_match_len != NULL)
            *_match_len = match_len;
        return &src[match_ofs];
    }

    return NULL;
}

static bool
fdm_delayed_timer(struct fdm *fdm, int fd, int events, void *data)
{
    if (events & EPOLLHUP)
        return false;

    struct matches *matches = data;

    uint64_t expiration_count;
    ssize_t ret = read(
        matches->delay_fd, &expiration_count, sizeof(expiration_count));

    if (ret < 0) {
        if (errno == EAGAIN)
            return true;

        LOG_ERRNO("failed to read blink timer");
        return false;
    }

    const enum delayed_update_type update_type = matches->delayed_update_type;
    matches->delayed_update_type = DELAYED_UPDATE_IN_PROGRESS;

    switch (update_type) {
    case DELAYED_NO_UPDATE:
        break;

    case DELAYED_INCREMENTAL_UPDATE:
        matches_update_incremental(matches);
        break;

    case DELAYED_FULL_UPDATE:
        matches_update(matches);
        break;

    case DELAYED_UPDATE_IN_PROGRESS:
        assert(false);
        break;
    }

    matches->delayed_update_type = DELAYED_NO_UPDATE;
    wayl_refresh(matches->wayl);
    return true;
}

struct matches *
matches_init(struct fdm *fdm, const struct prompt *prompt,
             enum match_fields fields, enum match_mode mode, bool sort_result,
             size_t fuzzy_min_length, size_t fuzzy_max_length_discrepancy,
             size_t fuzzy_max_distance, uint16_t workers,
             size_t delay_ms, size_t delay_limit)
{
    struct matches *matches = xmalloc(sizeof(*matches));
    *matches = (struct matches) {
        .fdm = fdm,
        .prompt = prompt,
        .applications = NULL,
        .fields = fields,
        .mode = mode,
        .sort_result = sort_result,
        .matches = NULL,
        .matches_size = 0,
        .match_count = 0,
        .page_count = 0,
        .selected = 0,
        .max_matches_per_page = 0,
        .fuzzy_min_length = fuzzy_min_length,
        .fuzzy_max_length_discrepancy = fuzzy_max_length_discrepancy,
        .fuzzy_max_distance = fuzzy_max_distance,
        .delay_fd = -1,
        .delay_ms = delay_ms,
        .delay_limit = delay_limit,
    };

    if (workers > 0) {
        if (sem_init(&matches->workers.start, 0, 0) < 0 ||
            sem_init(&matches->workers.done, 0, 0) < 0)
        {
            LOG_ERRNO("failed to instantiate match worker semaphores");
            goto err_free_matches;
        }

        int err;
        if ((err = mtx_init(&matches->workers.lock, mtx_plain)) != thrd_success) {
            LOG_ERR("failed to instantiate match worker mutex: %d", err);
            goto err_free_semaphores;
        }

        matches->workers.threads =
            xcalloc(workers, sizeof(matches->workers.threads[0]));

        for (size_t i = 0; i < workers; i++) {
            struct thread_context *ctx = xmalloc(sizeof(*ctx));
            *ctx = (struct thread_context){
                .matches = matches,
                .my_id = 1 + i,
            };

            int ret = thrd_create(
                &matches->workers.threads[i], &match_thread, ctx);
            if (ret != thrd_success) {
                LOG_ERR("failed to create match worker thread: %d", ret);
                matches->workers.threads[i] = 0;
                goto err_free_semaphores_and_lock;
            }

            matches->workers.count++;
        }

        LOG_INFO("using %hu match worker threads", matches->workers.count);
    }

    int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (timer_fd < 0) {
        LOG_ERRNO("failed to create timerfd");
        goto err_free_semaphores_and_lock;
    }

    if (!fdm_add(fdm, timer_fd, EPOLLIN, &fdm_delayed_timer, matches)) {
        close(timer_fd);
        goto err_free_semaphores_and_lock;
    }

    matches->delay_fd = timer_fd;
    return matches;

err_free_semaphores_and_lock:
    mtx_destroy(&matches->workers.lock);
err_free_semaphores:
    sem_destroy(&matches->workers.start);
    sem_destroy(&matches->workers.done);
err_free_matches:
    free(matches);
    return NULL;
}

void
matches_destroy(struct matches *matches)
{
    if (matches == NULL)
        return;

    fdm_del(matches->fdm, matches->delay_fd);

    mtx_lock(&matches->workers.lock);
    {
        assert(tll_length(matches->workers.queue) == 0);

        for (size_t i = 0; i < matches->workers.count; i++) {
            assert(matches->workers.threads[i] != 0);
            sem_post(&matches->workers.start);
            tll_push_back(matches->workers.queue, (uint64_t)-2);
        }
    }
    mtx_unlock(&matches->workers.lock);

    for (size_t i = 0; i < matches->workers.count; i++)
        thrd_join(matches->workers.threads[i], NULL);

    mtx_lock(&matches->applications->lock);
    if (matches->applications != NULL) {
        for (size_t i = 0; i < matches->matches_size; i++)
            free(matches->matches[i].pos);
    }
    mtx_unlock(&matches->applications->lock);

    free(matches->workers.threads);
    free(matches->matches);
    free(matches->workers.old_matches);
    mtx_destroy(&matches->workers.lock);
    sem_destroy(&matches->workers.start);
    sem_destroy(&matches->workers.done);
    free(matches);
}

void
matches_set_wayland(struct matches *matches, struct wayland *wayl)
{
    assert(matches->wayl == NULL);
    matches->wayl = wayl;
}

void
matches_all_applications_loaded(struct matches *matches)
{
    matches->all_apps_loaded = true;
#if defined(_DEBUG)
    matches_lock(matches);
    assert(matches->matches_size == matches->applications->count);
    matches_unlock(matches);
#endif
}

void
matches_set_applications(struct matches *matches,
                         struct application_list *applications)
{
    mtx_lock(&applications->lock);

    assert(matches->matches == NULL || matches->matches_size > 0);
    assert(applications->count >= matches->matches_size);

    matches->applications = applications;

    if (matches->matches == NULL && applications->count == 0) {
        mtx_unlock(&applications->lock);
        return;
    }

    matches->matches = xreallocarray(
        matches->matches, applications->count, sizeof(matches->matches[0]));

    const size_t old_size = matches->matches_size;
    const size_t diff = applications->count - old_size;

    memset(&matches->matches[old_size], 0, diff * sizeof(matches->matches[0]));

    matches->matches_size = applications->count;
    mtx_unlock(&applications->lock);
    matches_icons_loaded(matches);
}

void
matches_icons_loaded(struct matches *matches)
{
    if (matches->have_icons)
        return;

    mtx_lock(&matches->applications->lock);
    for (size_t i = 0; i < matches->applications->count; i++) {
        if (matches->applications->v[i]->icon.name != NULL) {
            matches->have_icons = true;
            break;
        }
    }
    mtx_unlock(&matches->applications->lock);
}

bool
matches_have_icons(const struct matches *matches)
{
    return matches->have_icons;
}

void
matches_lock(struct matches *matches)
{
    mtx_lock(&matches->applications->lock);
}

void
matches_unlock(struct matches *matches)
{
    mtx_unlock(&matches->applications->lock);
}

size_t
matches_max_matches_per_page(const struct matches *matches)
{
    return matches->max_matches_per_page;
}

void
matches_max_matches_per_page_set(struct matches *matches, size_t max_matches)
{
    matches->max_matches_per_page = max_matches;
}

size_t
matches_get_page_count(const struct matches *matches)
{
    return matches->max_matches_per_page != 0
        ? matches->match_count / matches->max_matches_per_page
        : matches->match_count;
}

size_t
matches_get_page(const struct matches *matches)
{
    return matches->max_matches_per_page != 0
        ? matches->selected / matches->max_matches_per_page
        : matches->selected;
}

size_t
match_get_idx(const struct matches *matches, size_t idx)
{
    const size_t page_no = matches_get_page(matches);
    const size_t items_on_page UNUSED = matches_get_count(matches);

    LOG_DBG(
        "page-count: %zu, page-no: %zu, items-on-page: %zu, idx: %zu, max: %zu, "
        "match-count: %zu",
        matches->page_count, page_no, items_on_page, idx,
        matches->max_matches_per_page, matches->match_count);

    assert(idx < items_on_page);
    idx += page_no * matches->max_matches_per_page;

    assert(idx < matches->match_count);
    return idx;
}

const struct match *
matches_get(const struct matches *matches, size_t idx)
{
    return &matches->matches[match_get_idx(matches, idx)];
}

const struct match *
matches_get_match(const struct matches *matches)
{
    return matches->match_count > 0
        ? &matches->matches[matches->selected]
        : NULL;
}

size_t
matches_get_application_visible_count(const struct matches *matches)
{
    return matches->applications->visible_count;
}

size_t
matches_get_count(const struct matches *matches)
{
    const size_t total = matches->match_count;
    const size_t page_no = matches_get_page(matches);

    if (matches->max_matches_per_page == 0)
        return 0;

    if (total == 0)
        return 0;
    else if (page_no + 1 >= matches->page_count) {
        size_t remainder = total % matches->max_matches_per_page;
        return remainder == 0 ? matches->max_matches_per_page : remainder;
    } else
        return matches->max_matches_per_page;
}

size_t
matches_get_total_count(const struct matches *matches)
{
    return matches->match_count;
}

size_t
matches_get_match_index(const struct matches *matches)
{
    return matches->max_matches_per_page != 0
        ? matches->selected % matches->max_matches_per_page
        : 0;
}

bool
matches_selected_select(struct matches *matches, const char *_string)
{
    if (_string == NULL)
        return false;

    char32_t *string = ambstoc32(_string);
    if (string == NULL)
        return false;

    for (size_t i = 0; i < matches->match_count; i++) {
        if (match_exact(matches->matches[i].application->title,
                       matches->matches[i].application->title_len,
                       string, c32len(string)) != NULL)
        {
            matches->selected = i;
            free(string);
            return true;
        }
    }

    free(string);
    return false;
}

bool
matches_idx_select(struct matches *matches, size_t idx)
{
    if (idx == (size_t)-1)
        return false;

    matches->selected = match_get_idx(matches, idx);
    return true;
}

bool
matches_selected_first(struct matches *matches)
{
    if (matches->match_count <= 0 || matches->selected <= 0)
        return false;

    matches->selected = 0;
    return true;
}

bool
matches_selected_set(struct matches *matches, size_t idx)
{
    if (matches == NULL || matches->match_count <= 0 || idx >= matches->match_count)
        return false;

    matches->selected = idx;
    return true;
}

bool
matches_selected_last(struct matches *matches)
{
    if (matches->match_count <= 0 ||
        matches->selected >= matches->match_count - 1)
    {
        return false;
    }

    matches->selected = matches->match_count - 1;
    return true;
}

bool
matches_selected_prev(struct matches *matches, bool wrap)
{
    if (matches->selected > 0) {
        matches->selected--;
        return true;
    } else if (wrap && matches->match_count > 1) {
        matches->selected = matches->match_count - 1;
        return true;
    }

    return false;
}

bool
matches_selected_next(struct matches *matches, bool wrap)
{
    if (matches->selected + 1 < matches->match_count) {
        matches->selected++;
        return true;
    } else if (wrap && matches->match_count > 1) {
        matches->selected = 0;
        return true;
    }

    return false;
}

bool
matches_selected_prev_page(struct matches *matches, bool scrolling)
{
    const size_t page_no = matches_get_page(matches);
    if (page_no > 0) {
        assert(matches->selected >= matches->max_matches_per_page);
        matches->selected -= matches->max_matches_per_page;
        return true;
    } else if (!scrolling && matches->selected > 0) {
        matches->selected = 0;
        return true;
    }

    return false;
}

bool
matches_selected_next_page(struct matches *matches, bool scrolling)
{
    const size_t page_no = matches_get_page(matches);
    if (page_no + 1 < matches->page_count) {
        matches->selected = min(
            matches->selected + matches->max_matches_per_page,
            matches->match_count - 1);
        return true;
    } else if (!scrolling && matches->selected < matches->match_count - 1) {
        matches->selected = matches->match_count - 1;
        return true;
    }

    return false;
}

static int
match_compar(const void *_a, const void *_b)
{
    const struct match *a = _a;
    const struct match *b = _b;

    if (a->matched_type != b->matched_type) {
        if (a->matched_type == MATCHED_EXACT)
            return -1;
        else
            return 1;
    }

    else if (a->score > b->score)
        return -1;
    else if (a->score < b->score)
        return 1;
    else if (a->pos_count < b->pos_count)
        return -1;
    else if (a->pos_count > b->pos_count)
        return 1;
    else if (a->application->count > b->application->count)
        return -1;
    else if (a->application->count < b->application->count)
        return 1;
    else if (a->pos_count > 0 && b->pos_count > 0 && a->pos[0].start < b->pos[0].start)
        return -1;
    else if (a->pos_count > 0 && b->pos_count > 0 && a->pos[0].start > b->pos[0].start)
        return 1;
    else if (a->pos_count > 0 && b->pos_count > 0 &&
             a->application->title != NULL && b->application->title != NULL)
    {
        return a->application->title_len > b->application->title_len;
    } else
        return 0;
}

static void
match_app(struct matches *matches, struct application *app,
          size_t tok_count, const char32_t *const tokens[static tok_count],
          const size_t tok_lengths[static tok_count],
          bool match_name,
          bool match_filename,
          bool match_generic,
          bool match_exec,
          bool match_comment,
          bool match_keywords,
          bool match_categories,
          bool match_nth)
{
    size_t pos_count = 0;
    struct match_substring *pos = NULL;

    enum matched_type match_type_name = MATCHED_NONE;
    enum matched_type match_type_filename = MATCHED_NONE;
    enum matched_type match_type_generic = MATCHED_NONE;
    enum matched_type match_type_exec = MATCHED_NONE;
    enum matched_type match_type_comment = MATCHED_NONE;
    enum matched_type match_type_nth = MATCHED_NONE;
    enum matched_type match_type_keywords[tll_length(app->keywords)];
    enum matched_type match_type_categories[tll_length(app->categories)];

    if (match_keywords) {
        for (size_t k = 0; k < tll_length(app->keywords); k++)
            match_type_keywords[k] = MATCHED_NONE;
    }
    if (match_categories) {
        for (size_t k = 0; k < tll_length(app->categories); k++)
            match_type_categories[k] = MATCHED_NONE;
    }

    for (size_t t = 0; t < tok_count; t++) {
        const char32_t *const tok = tokens[t];
        const size_t tok_len = tok_lengths[t];

        if (match_name && (t == 0 || match_type_name != MATCHED_NONE)) {
            const char32_t *m = NULL;
            size_t match_len = 0;
            enum matched_type match_type = MATCHED_NONE;

            switch (matches->mode) {
            case MATCH_MODE_EXACT:
                m = match_exact(app->title_lowercase, app->title_len, tok, tok_len);
                if (m != NULL) {
                    match_type = MATCHED_EXACT;
                    match_len = tok_len;
                }
                break;

            case MATCH_MODE_FZF:
                match_fzf(app->title_lowercase, app->title_len,
                          tok, tok_len, &pos, &pos_count, &match_type);
                break;

            case MATCH_MODE_FUZZY:
                m = match_exact(app->title_lowercase, app->title_len, tok, tok_len);
                if (m != NULL) {
                    match_type = MATCHED_EXACT;
                    match_len = tok_len;
                } else {
                    m = match_levenshtein(
                        matches, app->title_lowercase, app->title_len,
                        tok, tok_len, &match_len);
                    if (m != NULL)
                        match_type = MATCHED_FUZZY;
                }
                break;
            }

            if (match_len > 0) {
                assert(matches->mode != MATCH_MODE_FZF);

                if (pos_count > 0 && m == &app->title_lowercase[pos[pos_count - 1].start +
                                                                pos[pos_count - 1].len]) {
                    /* Extend last match position */
                    pos[pos_count - 1].len += match_len;
                } else {
                    pos_count += 1;
                    pos = xreallocarray(pos, pos_count, sizeof(pos[0]));

                    pos[pos_count - 1].start = m - app->title_lowercase;
                    pos[pos_count - 1].len = match_len;
                }
            }

            if (t == 0)
                match_type_name = match_type;
            else if (match_type == MATCHED_NONE)
                match_type_name = MATCHED_NONE;
            else if (match_type_name == MATCHED_EXACT)
                match_type_name = match_type;
        }

        if (match_nth && app->dmenu_match_nth != NULL &&
            (t == 0 || match_type_nth != MATCHED_NONE))
        {
            const char32_t *m = NULL;
            size_t match_len = 0;
            enum matched_type match_type = MATCHED_NONE;

            switch (matches->mode) {
            case MATCH_MODE_EXACT:
                m = match_exact(
                    app->dmenu_match_nth, app->dmenu_match_nth_len, tok, tok_len);

                if (m != NULL) {
                    match_type = MATCHED_EXACT;
                    match_len = tok_len;
                }
                break;

            case MATCH_MODE_FZF:
                match_fzf(app->dmenu_match_nth, app->dmenu_match_nth_len,
                          tok, tok_len, NULL, NULL, &match_type);
                break;

            case MATCH_MODE_FUZZY:
                m = match_exact(
                    app->dmenu_match_nth, app->dmenu_match_nth_len, tok, tok_len);

                if (m != NULL) {
                    match_type = MATCHED_EXACT;
                    match_len = tok_len;
                } else {
                    m = match_levenshtein(
                        matches, app->dmenu_match_nth, app->dmenu_match_nth_len,
                        tok, tok_len, &match_len);
                    if (m != NULL)
                        match_type = MATCHED_FUZZY;
                }
                break;
            }

            if (t == 0)
                match_type_nth = match_type;
            else if (match_type == MATCHED_NONE)
                match_type_nth = MATCHED_NONE;
            else if (match_type_nth == MATCHED_EXACT)
                match_type_nth = match_type;
        }

        if (match_filename && app->basename != NULL &&
            (t == 0 || match_type_filename != MATCHED_NONE))
        {
            const char32_t *m = NULL;
            size_t match_len = 0;
            enum matched_type match_type = MATCHED_NONE;

            switch (matches->mode) {
            case MATCH_MODE_EXACT:
                m = match_exact(app->basename, app->basename_len, tok, tok_len);
                if (m != NULL) {
                    match_type = MATCHED_EXACT;
                    match_len = tok_len;
                }
                break;

            case MATCH_MODE_FZF:
                match_fzf(app->basename, app->basename_len, tok, tok_len,
                          NULL, NULL, &match_type);
                break;

            case MATCH_MODE_FUZZY:
                m = match_exact(app->basename, app->basename_len, tok, tok_len);
                if (m != NULL) {
                    match_type = MATCHED_EXACT;
                    match_len = tok_len;
                } else {
                    m = match_levenshtein(
                        matches, app->basename, app->basename_len,
                        tok, tok_len, &match_len);
                    if (m != NULL)
                        match_type = MATCHED_FUZZY;
                }
                break;
            }

            if (t == 0)
                match_type_filename = match_type;
            else if (match_type == MATCHED_NONE)
                match_type_filename = MATCHED_NONE;
            else if (match_type_filename == MATCHED_EXACT)
                match_type_filename = match_type;
        }

        if (match_generic && app->generic_name != NULL &&
            (t == 0 || match_type_generic != MATCHED_NONE))
        {
            const char32_t *m = NULL;
            size_t match_len = 0;
            enum matched_type match_type = MATCHED_NONE;

            switch (matches->mode) {
            case MATCH_MODE_EXACT:
                m = match_exact(app->generic_name, app->generic_name_len, tok, tok_len);
                if (m != NULL) {
                    match_type = MATCHED_EXACT;
                    match_len = tok_len;
                }
                break;

            case MATCH_MODE_FZF:
                match_fzf(app->generic_name, app->generic_name_len,
                          tok, tok_len, NULL, NULL, &match_type);
                break;

            case MATCH_MODE_FUZZY:
                m = match_exact(app->generic_name, app->generic_name_len, tok, tok_len);
                if (m != NULL) {
                    match_type = MATCHED_EXACT;
                    match_len = tok_len;
                } else {
                    m = match_levenshtein(
                        matches, app->generic_name, app->generic_name_len,
                        tok, tok_len, &match_len);
                    if (m != NULL)
                        match_type = MATCHED_FUZZY;
                }
                break;
            }

            if (t == 0)
                match_type_generic = match_type;
            else if (match_type == MATCHED_NONE)
                match_type_generic = MATCHED_NONE;
            else if (match_type_generic == MATCHED_EXACT)
                match_type_generic = match_type;
        }

        if (match_exec && app->wexec != NULL &&
            (t == 0 || match_type_exec != MATCHED_NONE))
        {
            const char32_t *m = NULL;
            size_t match_len = 0;
            enum matched_type match_type = MATCHED_NONE;

            switch (matches->mode) {
            case MATCH_MODE_EXACT:
                m = match_exact(app->wexec, app->wexec_len, tok, tok_len);
                if (m != NULL) {
                    match_type = MATCHED_EXACT;
                    match_len = tok_len;
                }
                break;

            case MATCH_MODE_FZF:
                match_fzf(app->wexec, app->wexec_len, tok, tok_len,
                          NULL, NULL, &match_type);
                break;

            case MATCH_MODE_FUZZY:
                m = match_exact(app->wexec, app->wexec_len, tok, tok_len);
                if (m != NULL) {
                    match_type = MATCHED_EXACT;
                    match_len = tok_len;
                } else {
                    m = match_levenshtein(
                        matches, app->wexec, app->wexec_len,
                        tok, tok_len, &match_len);
                    if (m != NULL)
                        match_type = MATCHED_FUZZY;
                }
                break;
            }

            if (t == 0)
                match_type_exec = match_type;
            else if (match_type == MATCHED_NONE)
                match_type_exec = MATCHED_NONE;
            else if (match_type_exec == MATCHED_EXACT)
                match_type_exec = match_type;
        }

        if (match_comment && app->comment != NULL &&
            (t == 0 || match_type_comment != MATCHED_NONE))
        {
            const char32_t *m = NULL;
            size_t match_len = 0;
            enum matched_type match_type = MATCHED_NONE;

            switch (matches->mode) {
            case MATCH_MODE_EXACT:
                m = match_exact(app->comment, app->comment_len, tok, tok_len);
                if (m != NULL) {
                    match_type = MATCHED_EXACT;
                    match_len = tok_len;
                }
                break;

            case MATCH_MODE_FZF:
                match_fzf(app->comment, app->comment_len, tok, tok_len,
                          NULL, NULL, &match_type);
                break;

            case MATCH_MODE_FUZZY:
                m = match_exact(app->comment, app->comment_len, tok, tok_len);
                if (m != NULL) {
                    match_type = MATCHED_EXACT;
                    match_len = tok_len;
                } else {
                    m = match_levenshtein(
                        matches, app->comment, app->comment_len,
                        tok, tok_len, &match_len);
                    if (m != NULL)
                        match_type = MATCHED_FUZZY;
                }
                break;
            }

            if (t == 0)
                match_type_comment = match_type;
            else if (match_type == MATCHED_NONE)
                match_type_comment = MATCHED_NONE;
            else if (match_type_comment == MATCHED_EXACT)
                match_type_comment = match_type;
        }

        if (match_keywords) {
            size_t k = 0;
            tll_foreach(app->keywords, it) {
                if (!(t == 0 || match_type_keywords[k] != MATCHED_NONE))
                    continue;

                const char32_t *m = NULL;
                size_t match_len = 0;
                enum matched_type match_type = MATCHED_NONE;

                switch (matches->mode) {
                case MATCH_MODE_EXACT:
                    m = match_exact(it->item, c32len(it->item), tok, tok_len);
                    if (m != NULL) {
                        match_type = MATCHED_EXACT;
                        match_len = tok_len;
                    }
                    break;

                case MATCH_MODE_FZF:
                    match_fzf(it->item, c32len(it->item), tok, tok_len,
                              NULL, NULL, &match_type);
                    break;

                case MATCH_MODE_FUZZY:
                    m = match_exact(it->item, c32len(it->item), tok, tok_len);
                    if (m != NULL) {
                        match_type = MATCHED_EXACT;
                        match_len = tok_len;
                    } else {
                        m = match_levenshtein(
                            matches, it->item, c32len(it->item),
                            tok, tok_len, &match_len);
                        if (m != NULL)
                            match_type = MATCHED_FUZZY;
                    }
                    break;
                }

                if (t == 0)
                    match_type_keywords[k] = match_type;
                else if (match_type == MATCHED_NONE)
                    match_type_keywords[k] = MATCHED_NONE;
                else if (match_type_keywords[k] == MATCHED_EXACT)
                    match_type_keywords[k] = match_type;

                k++;
            }
        }

        if (match_categories) {
            size_t k = 0;
            tll_foreach(app->categories, it) {
                if (!(t == 0 || match_type_categories[k] != MATCHED_NONE))
                    continue;

                const char32_t *m = NULL;
                size_t match_len = 0;
                enum matched_type match_type = MATCHED_NONE;

                switch (matches->mode) {
                case MATCH_MODE_EXACT:
                    m = match_exact(it->item, c32len(it->item), tok, tok_len);
                    if (m != NULL) {
                        match_type = MATCHED_EXACT;
                        match_len = tok_len;
                    }
                    break;

                case MATCH_MODE_FZF:
                    match_fzf(it->item, c32len(it->item), tok, tok_len,
                              NULL, NULL, &match_type);
                    break;

                case MATCH_MODE_FUZZY:
                    m = match_exact(it->item, c32len(it->item), tok, tok_len);
                    if (m != NULL) {
                        match_type = MATCHED_EXACT;
                        match_len = tok_len;
                    } else {
                        m = match_levenshtein(
                            matches, it->item, c32len(it->item),
                            tok, tok_len, &match_len);
                        if (m != NULL)
                            match_type = MATCHED_FUZZY;
                    }
                    break;
                }

                if (t == 0)
                    match_type_categories[k] = match_type;
                else if (match_type == MATCHED_NONE)
                    match_type_categories[k] = MATCHED_NONE;
                else if (match_type_categories[k] == MATCHED_EXACT)
                    match_type_categories[k] = match_type;

                k++;
            }
        }
    }

    enum matched_type match_type_keywords_final = MATCHED_NONE;

    if (match_keywords) {
        for (size_t k = 0; k < tll_length(app->keywords); k++) {
            if (match_type_keywords[k] != MATCHED_NONE) {
                /* match_type_keywords_final represents the combined
                   result of all keywords; if a single keyword matched
                   exactly, treat the entire keywords field as an
                   exact match */
                if (match_type_keywords_final != MATCHED_EXACT)
                    match_type_keywords_final = match_type_keywords[k];
            }
        }
    }

    enum matched_type match_type_categories_final = MATCHED_NONE;

    if (match_categories) {
        for (size_t k = 0; k < tll_length(app->categories); k++) {
            if (match_type_categories[k] != MATCHED_NONE) {
                /* match_type_categories_final represents the combined
                   result of all keywords; if a single keyword matched
                   exactly, treat the entire keywords field as an
                   exact match */
                if (match_type_categories_final != MATCHED_EXACT)
                    match_type_categories_final = match_type_categories[k];
            }
        }
    }

    enum matched_type app_match_type = MATCHED_NONE;

    /*
     * For now, only track score for the application title. The longer
     * consecutive match, the higher the score.
     *
     * This means searching for 'oo' will sort 'foot' before 'firefox
     * browser', for example.
     */
    size_t score = 0;
    if (match_name) {
        size_t longest_match = 0;
        for (size_t k = 0; k < pos_count; k++) {
            if (pos[k].len > longest_match)
                longest_match = pos[k].len;
        }

        score = longest_match;
    }

    if (match_type_name != MATCHED_NONE)
        app_match_type = match_type_name;
    else if (match_type_filename != MATCHED_NONE)
        app_match_type = match_type_filename;
    else if (match_type_generic != MATCHED_NONE)
        app_match_type = match_type_generic;
    else if (match_type_exec != MATCHED_NONE)
        app_match_type = match_type_exec;
    else if (match_type_comment != MATCHED_NONE)
        app_match_type = match_type_comment;
    else if (match_type_keywords_final != MATCHED_NONE)
        app_match_type = match_type_keywords_final;
    else if (match_type_categories_final != MATCHED_NONE)
        app_match_type = match_type_categories_final;
    else if (match_type_nth != MATCHED_NONE)
        app_match_type = match_type_nth;

    if (app_match_type == MATCHED_NONE) {
        free(pos);
        return;
    }

    struct match m = {
        .matched_type = app_match_type,
        .application = app,
        .pos = pos,
        .pos_count = pos_count,
        .score = score,
    };

    const size_t idx = matches->match_count++;

    free(matches->matches[idx].pos);
    matches->matches[idx] = m;
}

/* THREAD */
static int
match_thread(void *_ctx)
{
    struct thread_context *ctx = _ctx;
    struct matches *matches = ctx->matches;
    const int my_id = ctx->my_id;
    free(ctx);

    sigset_t mask;
    sigfillset(&mask);
    pthread_sigmask(SIG_SETMASK, &mask, NULL);

    /*
     * Note: using 'mtch' instead of 'match', to allow for core IDs >
     * 99.
     */
    char proc_title[16];
    xsnprintf(proc_title, sizeof(proc_title), "fuzzel:mtch:%d", my_id);

    if (pthread_setname_np(pthread_self(), proc_title) < 0)
        LOG_ERRNO("render worker %d: failed to set process title", my_id);

    const enum match_fields fields = matches->fields;
    const bool match_name = fields & MATCH_NAME;
    const bool match_filename = fields & MATCH_FILENAME;
    const bool match_generic = fields & MATCH_GENERIC;
    const bool match_exec = fields & MATCH_EXEC;
    const bool match_comment = fields & MATCH_COMMENT;
    const bool match_keywords = fields & MATCH_KEYWORDS;
    const bool match_categories = fields & MATCH_CATEGORIES;
    const bool match_nth = fields & MATCH_NTH;

    sem_t *start = &matches->workers.start;
    sem_t *done = &matches->workers.done;
    mtx_t *lock = &matches->workers.lock;

    while (true) {
        sem_wait(start);

        bool incremental = matches->workers.incremental;
        const size_t tok_count = matches->workers.tok_count;
        const char32_t *const *tokens = matches->workers.tokens;
        const size_t *tok_lengths = matches->workers.tok_lengths;
        struct match *prev_matches = matches->workers.old_matches;

        bool match_done = false;

        while (!match_done) {
            mtx_lock(lock);
            assert(tll_length(matches->workers.queue) > 0);

            const uint64_t slice_start_and_end =
                tll_pop_front(matches->workers.queue);

            const uint32_t slice_start = slice_start_and_end;
            const uint32_t slice_end = slice_start_and_end >> 32;

            mtx_unlock(lock);

            if (slice_start == (uint32_t)-1) {
                match_done = true;
                sem_post(done);
            } else if (slice_start == (uint32_t)-2) {
                /* Exit */
                return 0;
            } else {
                struct application **apps = matches->applications->v;
                struct application **a = &apps[slice_start];
                struct match *m = &prev_matches[slice_start];

                for (size_t i = slice_start; i < slice_end; i++) {
                    struct application *app;

                    if (incremental) {
                        app = m->application;
                        m++;
                        assert(app->visible);
                    } else {
                        app = *a;
                        a++;

                        if (!app->visible)
                            continue;
                    }

                    match_app(matches, app, tok_count, tokens, tok_lengths,
                              match_name, match_filename, match_generic, match_exec,
                              match_comment, match_keywords, match_categories,
                              match_nth);
                }
            }
        }
    }

    return -1;
}


static void
matches_update_internal(struct matches *matches, bool incremental)
{
    if (matches->applications == NULL)
        return;

    matches_lock(matches);

    const char32_t *ptext = prompt_text(matches->prompt);

    /* Nothing entered; all programs found matches */
    if (ptext[0] == '\0') {
        matches->match_count = 0;
        for (size_t i = 0; i < matches->matches_size; i++) {
            if (!matches->applications->v[i]->visible)
                continue;

            free(matches->matches[matches->match_count].pos);

            matches->matches[matches->match_count++] = (struct match){
                .matched_type = MATCHED_NONE,
                .application = matches->applications->v[i],
                .pos = NULL,
                .pos_count = 0,
            };
        }

        /* Sort */
        if (matches->sort_result && matches->all_apps_loaded) {
            qsort(matches->matches, matches->match_count,
                  sizeof(matches->matches[0]), &match_compar);
        }

        if (matches->selected >= matches->match_count && matches->selected > 0)
            matches->selected = matches->match_count - 1;

        matches->page_count = matches->max_matches_per_page > 0
            ? ((matches->match_count + (matches->max_matches_per_page - 1)) /
               matches->max_matches_per_page)
            : 1;
        goto unlock_and_return;
    }

    const enum match_fields fields = matches->fields;
    const bool match_name = fields & MATCH_NAME;
    const bool match_filename = fields & MATCH_FILENAME;
    const bool match_generic = fields & MATCH_GENERIC;
    const bool match_exec = fields & MATCH_EXEC;
    const bool match_comment = fields & MATCH_COMMENT;
    const bool match_keywords = fields & MATCH_KEYWORDS;
    const bool match_categories = fields & MATCH_CATEGORIES;
    const bool match_nth = fields & MATCH_NTH;

    LOG_DBG(
        "matching: filename=%s, name=%s, generic=%s, exec=%s, categories=%s, "
        "keywords=%s, comment=%s, nth=%d",
        match_filename ? "yes" : "no",
        match_name ? "yes" : "no",
        match_generic ? "yes" : "no",
        match_exec ? "yes" : "no",
        match_categories ? "yes" : "no",
        match_keywords ? "yes" : "no",
        match_comment ? "yes" : "no",
        match_nth ? "yes" : "no");

    LOG_DBG("match update begin");

    char32_t *copy = xc32dup(ptext);
    char32_t **tokens = xmalloc(sizeof(tokens[0]));
    size_t *tok_lengths = xmalloc(sizeof(tok_lengths[0]));
    size_t tok_count = 1;
    tokens[0] = copy;
    tok_lengths[0] = 0;

    for (char32_t *p = copy; *p != U'\0'; p++) {
        if (*p != U' ') {
            *p = toc32lower(*p);
            tok_lengths[tok_count - 1]++;
            continue;
        }

        *p = U'\0';
        if ((ptrdiff_t)(p - tokens[tok_count - 1]) == 0) {
            /* Collapse multiple spaces */
            tokens[tok_count - 1] = p + 1;
        } else {
            tok_count++;

            tokens = xreallocarray(tokens, tok_count, sizeof(tokens[0]));

            tok_lengths = xreallocarray(
                tok_lengths, tok_count, sizeof(tok_lengths[0]));

            tokens[tok_count - 1] = p + 1;
            tok_lengths[tok_count - 1] = 0;
        }
    }

    if (tokens[tok_count - 1][0] == U'\0') {
        /* Don’t count trailing spaces as a token */
        tok_count--;
    }

#if defined(_DEBUG)
    for (size_t i = 0; i < tok_count; i++)
        assert(c32len(tokens[i]) == tok_lengths[i]);
#endif

    const size_t search_count =
        incremental ? matches->match_count : matches->matches_size;

    const size_t slice_size = 4096;
    const size_t slice_count = (search_count + slice_size - 1) / slice_size;
    assert(search_count == 0 || slice_count >= 1);

    const bool use_threads = /*!incremental && */matches->workers.count > 0 && slice_count > 1;

    if (use_threads) {
        mtx_lock(&matches->workers.lock);
        matches->workers.incremental = incremental;
        matches->workers.tok_count = tok_count;
        matches->workers.tokens = (const char32_t *const *)tokens;
        matches->workers.tok_lengths = tok_lengths;

        if (incremental) {
            matches->workers.old_matches =
                xreallocarray(matches->workers.old_matches,
                             matches->match_count,
                             sizeof(matches->workers.old_matches[0]));

            memcpy(matches->workers.old_matches,
                   matches->matches,
                   matches->match_count * sizeof(matches->matches[0]));
        }

        for (size_t i = 0; i < matches->workers.count; i++)
            sem_post(&matches->workers.start);
    }

    matches->match_count = 0;

    for (size_t i = 0, slice_start_idx = 0; i < slice_count; i++, slice_start_idx += slice_size) {
        const size_t count = min(search_count - slice_start_idx, slice_size);

        if (use_threads) {
            tll_push_back(
                matches->workers.queue,
                (uint64_t)(slice_start_idx + count) << 32 | slice_start_idx);
        } else {
            for (size_t j = 0; j < count; j++) {
                struct application *app = NULL;
                const size_t idx = slice_start_idx + j;

                if (incremental) {
                    app = matches->matches[idx].application;
                    assert(app->visible);
                } else {
                    app = matches->applications->v[idx];

                    if (!app->visible)
                        continue;
                }

                match_app(matches, app,
                          tok_count, (const char32_t *const *)tokens, tok_lengths,
                          match_name, match_filename, match_generic, match_exec,
                          match_comment, match_keywords, match_categories, match_nth);
            }
        }
    }

    if (use_threads) {
        for (size_t i = 0; i < matches->workers.count; i++)
            tll_push_back(matches->workers.queue, (uint64_t)-1);

        mtx_unlock(&matches->workers.lock);

        for (size_t i = 0; i < matches->workers.count; i++)
            sem_wait(&matches->workers.done);

        matches->workers.tok_count = 0;
        matches->workers.tokens = NULL;
        matches->workers.tok_lengths = NULL;
    }

    LOG_DBG("match update done");

    /* Sort */
    if (matches->sort_result) {
        qsort(matches->matches, matches->match_count,
              sizeof(matches->matches[0]), &match_compar);
    }

    matches->page_count = matches->max_matches_per_page
        ? ((matches->match_count + (matches->max_matches_per_page - 1)) /
           matches->max_matches_per_page)
        : 1;

    if (matches->selected >= matches->match_count && matches->selected > 0)
        matches->selected = matches->match_count - 1;

    free(tok_lengths);
    free(tokens);
    free(copy);

unlock_and_return:
    matches_unlock(matches);
}

static bool
arm_delayed_timer(struct matches *matches)
{
    struct itimerspec timeout = {
        .it_value = {
            .tv_sec = matches->delay_ms / 1000,
            .tv_nsec = (matches->delay_ms % 1000) * 1000000,
        },
    };

    if (timerfd_settime(matches->delay_fd, 0, &timeout, NULL) < 0) {
        LOG_ERRNO("failed to arm delayed filter timer");
        return false;
    }

    return true;
}

void
matches_update(struct matches *matches)
{
    if (matches->delay_ms > 0 &&
        matches->applications->count > matches->delay_limit &&
        matches->delayed_update_type != DELAYED_UPDATE_IN_PROGRESS)
    {
        matches->delayed_update_type = DELAYED_FULL_UPDATE;
        if (arm_delayed_timer(matches))
            return;
    }

    matches_update_internal(matches, false);
}

void
matches_update_no_delay(struct matches *matches)
{
    matches_update_internal(matches, false);
}

void
matches_update_incremental(struct matches *matches)
{
    const bool need_full_update = matches->mode == MATCH_MODE_FUZZY;

    if (matches->delay_ms > 0 &&
        matches->match_count > matches->delay_limit &&
        matches->delayed_update_type != DELAYED_UPDATE_IN_PROGRESS)
    {
        switch (matches->delayed_update_type) {
        case DELAYED_NO_UPDATE:
        case DELAYED_INCREMENTAL_UPDATE:
            matches->delayed_update_type = need_full_update
                ? DELAYED_FULL_UPDATE : DELAYED_INCREMENTAL_UPDATE;
            break;

        case DELAYED_FULL_UPDATE:
            break;

        case DELAYED_UPDATE_IN_PROGRESS:
            assert(false);
            break;
        }

        if (arm_delayed_timer(matches))
            return;
    }

    matches_update_internal(matches, !need_full_update);
}
