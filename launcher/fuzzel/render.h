#pragma once

#include <threads.h>
#include <pixman.h>

#include <fcft/fcft.h>

#include "config.h"
#include "match.h"
#include "shm.h"
#include "tllist.h"

struct render;
struct render *render_init(const struct config *conf, mtx_t *icon_lock);
void render_destroy(struct render *render);

void render_initialize_colors(
    struct render *render, const struct config *conf, bool gamma_correct);

void render_set_subpixel(struct render *render, enum fcft_subpixel subpixel);
bool render_set_font_and_update_sizes(
    struct render *render, struct fcft_font *font, struct fcft_font *font_bold,
    float scale, float dpi, bool size_font_by_dpi,
    int *new_width, int *new_height);
void render_resized(struct render *render, int *new_width, int *new_height);
void render_flush_text_run_cache(struct render *render);

void render_background(const struct render *render, struct buffer *buf);

void render_prompt(
    struct render *render, struct buffer *buf,
    const struct prompt *prompt, const struct matches *matches);

void render_match_list(
    struct render *render, struct buffer *buf,
    const struct prompt *prompt, const struct matches *matches);

int render_icon_size(const struct render *render);

ssize_t render_get_row_num(
    const struct render *render, int window_width, int x, int y,
    const struct matches *matches);
