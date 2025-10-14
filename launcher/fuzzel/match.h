#pragma once

#include <stdbool.h>
#include <unistd.h>

#include "application.h"
#include "config.h"
#include "fdm.h"
#include "prompt.h"

enum matched_type {
    MATCHED_NONE,
    MATCHED_EXACT,
    MATCHED_FUZZY,
};

struct match_substring {
    ssize_t start;
    size_t len;
};

struct match {
    enum matched_type matched_type;
    struct application *application;
    struct match_substring *pos;
    size_t pos_count;
    size_t score;
};

struct wayland;
struct matches;
struct matches *matches_init(struct fdm *fdm, const struct prompt *prompt,
                             enum match_fields fields, enum match_mode mode,
                             bool sort_result,
                             size_t fuzzy_min_length,
                             size_t fuzzy_max_length_discrepancy,
                             size_t fuzzy_max_distance, uint16_t workers,
                             size_t delay_ms, size_t delay_limit);
void matches_destroy(struct matches *matches);

void matches_set_wayland(struct matches *matches, struct wayland *wayl);
void matches_set_applications(
    struct matches *matches, struct application_list *applications);
void matches_all_applications_loaded(struct matches *matches);
void matches_icons_loaded(struct matches *matches);
bool matches_have_icons(const struct matches *matches);
void matches_lock(struct matches *matches);
void matches_unlock(struct matches *matches);

size_t matches_max_matches_per_page(const struct matches *matches);
void matches_max_matches_per_page_set(
    struct matches *matches, size_t max_matches);

void matches_update(struct matches *matches);
void matches_update_no_delay(struct matches *matches);
void matches_update_incremental(struct matches *matches);

size_t matches_get_page_count(const struct matches *matches);
size_t matches_get_page(const struct matches *matches);

const struct match *matches_get(const struct matches *matches, size_t idx);
const struct match *matches_get_match(const struct matches *matches);
size_t matches_get_application_visible_count(const struct matches *matches);
size_t matches_get_count(const struct matches *matches); /* Matches on current page */
size_t matches_get_total_count(const struct matches *matches);
size_t matches_get_match_index(const struct matches *matches);

bool matches_selected_select(struct matches *matches, const char *string);
bool matches_idx_select(struct matches *matches, size_t idx);

bool matches_selected_set(struct matches *matches, const size_t idx);

bool matches_selected_first(struct matches *matches);
bool matches_selected_last(struct matches *matches);

bool matches_selected_prev(struct matches *matches, bool wrap);
bool matches_selected_next(struct matches *matches, bool wrap);

bool matches_selected_prev_page(struct matches *matches, bool scrolling);
bool matches_selected_next_page(struct matches *matches, bool scrolling);
