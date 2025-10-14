#include "render.h"

#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <uchar.h>
#include <sys/time.h>

#include "column.h"
#include "macros.h"
#include "timing.h"

#if HAS_INCLUDE(<pthread_np.h>)
#include <pthread_np.h>
#define pthread_setname_np(thread, name) (pthread_set_name_np(thread, name), 0)
#elif defined(__NetBSD__)
#define pthread_setname_np(thread, name) pthread_setname_np(thread, "%s", (void *)name)
#endif

#if defined(FUZZEL_ENABLE_CAIRO)
#include <cairo.h>
#else
#define cairo_t void
#endif

#include <fcft/fcft.h>

#if defined(FUZZEL_ENABLE_SVG_NANOSVG)
 #include <nanosvg/nanosvgrast.h>
#endif

#if defined(FUZZEL_ENABLE_SVG_RESVG)
 #include <resvg.h>
#endif

#define LOG_MODULE "render"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "char32.h"
#include "icon.h"
#include "srgb.h"
#include "stride.h"
#include "xmalloc.h"
#include "xsnprintf.h"

#define min(x, y) ((x) < (y) ? (x) : (y))
#define max(x, y) ((x) > (y) ? (x) : (y))

struct render;
struct thread_context {
    struct render *render;
    int my_id;
};

struct render {
    const struct config *conf;
    struct fcft_font *font;
    struct fcft_font *font_bold;
    enum fcft_subpixel subpixel;

    /* Cached fcft text runs */
    struct fcft_text_run *prompt_text_run;
    struct fcft_text_run *placeholder_text_run;

    /* Cached selection corners */
    pixman_image_t *selection_corners;

    float scale;
    float dpi;
    bool size_font_by_dpi;

    bool gamma_correct;
    pixman_color_t pix_background_color;
    pixman_color_t pix_border_color;
    pixman_color_t pix_text_color;
    pixman_color_t pix_prompt_color;
    pixman_color_t pix_input_color;
    pixman_color_t pix_match_color;
    pixman_color_t pix_selection_color;
    pixman_color_t pix_selection_text_color;
    pixman_color_t pix_selection_match_color;
    pixman_color_t pix_counter_color;
    pixman_color_t pix_placeholder_color;

    unsigned x_margin;
    unsigned y_margin;
    unsigned inner_pad;
    unsigned border_size;
    unsigned border_radius;
    unsigned selection_border_radius;
    unsigned row_height;
    unsigned icon_height;

    unsigned input_glyph_offset; /* At which glyph to start rendering input */

    struct {
        uint16_t count;
        sem_t start;
        sem_t done;
        mtx_t lock;
        tll(int) queue;
        thrd_t *threads;

        const struct matches *matches;
        struct buffer *buf;
        bool render_icons;
    } workers;

    mtx_t *icon_lock;
};

static pixman_color_t
rgba2pixman(bool gamma_correct, struct rgba rgba)
{
    pixman_color_t pix_color = {
        .alpha = rgba.a * 65535. + 0.5,
        .red = gamma_correct ? srgb_decode_8_to_16(rgba.r * 255 + 0.5) : rgba.r * 65535. + 0.5,
        .green = gamma_correct ? srgb_decode_8_to_16(rgba.g * 255 + 0.5) : rgba.g * 65535. + 0.5,
        .blue = gamma_correct ? srgb_decode_8_to_16(rgba.b * 255 + 0.5) : rgba.b * 65535. + 0.5,
    };

    pix_color.red = (uint32_t)pix_color.red * pix_color.alpha / 0xffff;
    pix_color.green = (uint32_t)pix_color.green * pix_color.alpha / 0xffff;
    pix_color.blue = (uint32_t)pix_color.blue * pix_color.alpha / 0xffff;

    return pix_color;
}

static int
pt_or_px_as_pixels(const struct render *render, const struct pt_or_px *pt_or_px)
{
    double scale = !render->size_font_by_dpi ? render->scale : 1.;
    double dpi = render->size_font_by_dpi  ? render->dpi : 96.;

    return pt_or_px->px == 0
        ? round(pt_or_px->pt * scale * dpi / 72.)
        : pt_or_px->px;
}

static pixman_region32_t
rounded_rectangle_region(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint16_t radius)
{

    int rect_count = ( radius + radius ) + 1;
    pixman_box32_t rects[rect_count];

    for (int i = 0; i <= radius; i++) {
        uint16_t ydist = radius - i;
        uint16_t curve = sqrt(radius * radius - ydist * ydist);

        rects[i] = (pixman_box32_t) {
            x + radius - curve,
            y + i,
            x + width - radius + curve,
            y + i + 1
        };

        rects[radius + i] = (pixman_box32_t) {
            x + radius - curve,
            y + height - i,
            x + width - radius + curve,
            y + height - i + 1
        };
    }

    rects[(radius * 2)] = (pixman_box32_t){
        x,
        y + radius,
        x + width,
        y + height + 1 - radius
    };

    pixman_region32_t region;
    pixman_region32_init_rects(&region, rects, rect_count);
    return region;
}

static inline void
fill_region32(pixman_op_t op, pixman_image_t* dest,
                       const pixman_color_t* color, pixman_region32_t* region)
{
    int rectc;
    pixman_box32_t *rects = pixman_region32_rectangles(region, &rectc);
    pixman_image_fill_boxes(op, dest, color, rectc, rects);

}

static inline void
fill_rounded_rectangle(pixman_op_t op, pixman_image_t* dest,
                       const pixman_color_t* color, int16_t x, int16_t y,
                       uint16_t width, uint16_t height, uint16_t radius)
{
    pixman_region32_t region = rounded_rectangle_region(x, y, width, height, radius);
    fill_region32(op, dest, color, &region);
    pixman_region32_fini(&region);
}

static void
render_rounded_rectangle(pixman_image_t* dest, pixman_color_t* background,
                         pixman_color_t* border, unsigned int radius, unsigned bw,
                         int16_t x, int16_t y, uint16_t width, uint16_t height)
{
    if (radius == 0) {
        pixman_image_fill_rectangles(
            PIXMAN_OP_SRC, dest, background,
            1, &(pixman_rectangle16_t){
            x + bw, y + bw, width - 2 * bw, height - 2 * bw});

        pixman_image_fill_rectangles(
            PIXMAN_OP_SRC, dest, border,
            4, (pixman_rectangle16_t[]){
                {x, y, width, bw},                              /* top */
                {x, y + bw, bw, height - 2 * bw},               /* left */
                {x + width - bw, y + bw, bw, height - 2 * bw},  /* right */
                {x, y + height - bw, width, bw}                 /* bottom */
            });
    } else {
        const int msaa_scale = 2;
        const double brd_sz_scaled = bw * msaa_scale;
        const double brd_rad_scaled = radius * msaa_scale;
        int w = width * msaa_scale;
        int h = height * msaa_scale;
        int bg_rad = brd_rad_scaled * (1.0 - (float)brd_sz_scaled / (float)brd_rad_scaled);

        pixman_image_t *bg_img;
        if (msaa_scale != 1) {
            bg_img = pixman_image_create_bits(
                pixman_image_get_format(dest), w, h, NULL, w*4);
        } else {
            bg_img = dest;
        }

        /* Border */
        fill_rounded_rectangle(
            PIXMAN_OP_SRC, bg_img, border, 0, 0, w, h, brd_rad_scaled);

        /* Background */
        fill_rounded_rectangle(
            PIXMAN_OP_SRC, bg_img, background, brd_sz_scaled, brd_sz_scaled,
            w-(brd_sz_scaled*2),
            h-(brd_sz_scaled*2),
            bg_rad);

        if (msaa_scale != 1) {
            pixman_f_transform_t ftrans;
            pixman_transform_t trans;
            pixman_f_transform_init_scale(&ftrans, msaa_scale, msaa_scale);
            pixman_transform_from_pixman_f_transform(&trans, &ftrans);
            pixman_image_set_transform(bg_img, &trans);
            pixman_image_set_filter(bg_img, PIXMAN_FILTER_BILINEAR, NULL, 0);

            pixman_image_composite32(
                PIXMAN_OP_SRC, bg_img, NULL, dest, 0, 0, 0, 0, x, y,
                width, height);
            pixman_image_unref(bg_img);
        }
    }
}

void
render_background(const struct render *render, struct buffer *buf)
{
    unsigned bw = render->border_size;

    pixman_color_t bg = render->pix_background_color;
    pixman_color_t border_color = render->pix_border_color;

    if (buf->age == 0) {
        /* Each sub-part of the window erases itself */
        return;
    }

    /* Limit radius if the margins are very small, to prevent e.g. the
       selection "box" from overlapping the corners */
    const unsigned int radius =
        min(render->border_radius,
            max(render->x_margin,
                render->y_margin));

    render_rounded_rectangle(buf->pix[0], &bg, &border_color, radius, bw,
                             0, 0, buf->width, buf->height);
}

static void
render_glyph(pixman_image_t *pix, const struct fcft_glyph *glyph, int x, int y,
             const pixman_color_t *color)
{
    if (pixman_image_get_format(glyph->pix) == PIXMAN_a8r8g8b8) {
        /* Glyph surface is a pre-rendered image (typically a color emoji...) */
        pixman_image_composite32(
            PIXMAN_OP_OVER, glyph->pix, NULL, pix, 0, 0, 0, 0,
            x + glyph->x, y - glyph->y,
            glyph->width, glyph->height);
    } else {
        /* Glyph surface is an alpha mask */
        pixman_image_t *src = pixman_image_create_solid_fill(color);
        pixman_image_composite32(
            PIXMAN_OP_OVER, src, glyph->pix, pix, 0, 0, 0, 0,
            x + glyph->x, y - glyph->y,
            glyph->width, glyph->height);
        pixman_image_unref(src);
    }
}

static int
render_baseline(const struct render *render)
{
    const struct fcft_font *font = render->font;
    const int line_height = render->row_height;
    const int font_height = font->ascent + font->descent;

    /*
     * Center glyph on the line *if* using a custom line height,
     * otherwise the baseline is simply 'descent' pixels above the
     * bottom of the cell
     */
    const int glyph_top_y =
        render->conf->line_height.px >= 0 && line_height >= font_height
            ? round((line_height - font_height) / 2.)
            : 0;

    return line_height - glyph_top_y - font->descent;
}

static int
render_match_count(const struct render *render, struct buffer *buf,
                   const struct prompt *prompt, const struct matches *matches)
{
    struct fcft_font *font = render->font;
    assert(font != NULL);

    const struct config *conf = render->conf;

    const enum fcft_subpixel subpixel =
        conf->colors.background.a == 1. && conf->colors.selection.a == 1.
            ? render->subpixel
            : FCFT_SUBPIXEL_NONE;

    size_t total_count = matches_get_application_visible_count(matches);
    size_t match_count = matches_get_total_count(matches);
    const char32_t *ptext = prompt_text(prompt);

    char text[64];
    size_t count = (ptext[0] == U'\0') ? total_count : match_count;
    size_t chars = xsnprintf(text, sizeof(text), "%zu/%zu", count, total_count);

    /* fcft wants UTF-32. Since we only use ASCII... */
    uint32_t wtext[64];
    for (size_t i = 0; i < chars; i++)
        wtext[i] = (uint32_t)(unsigned char)text[i];

    int width = -1;
    int x = buf->width - render->border_size - render->x_margin;
    int y = render->border_size + render->y_margin +
            (render->row_height + font->height) / 2 - font->descent;

    if (fcft_capabilities() & FCFT_CAPABILITY_TEXT_RUN_SHAPING) {
        struct fcft_text_run *run = fcft_rasterize_text_run_utf32(
            font, chars, wtext, subpixel);

        if (run != NULL) {
            width = 0;

            for (size_t i = 0; i < run->count; i++) {
                const struct fcft_glyph *glyph = run->glyphs[i];
                width += glyph->advance.x;
            }

            x -= width;
            for (size_t i = 0; i < run->count; i++) {
                const struct fcft_glyph *glyph = run->glyphs[i];
                render_glyph(buf->pix[0], glyph, x, y, &render->pix_counter_color);
                x += glyph->advance.x;
            }

            fcft_text_run_destroy(run);
            return width;
        }
    }

    const struct fcft_glyph **glyphs = xmalloc(chars * sizeof(glyphs[0]));
    long *x_kern = xmalloc(chars * sizeof(x_kern[0]));
    char32_t prev = 0;

    width = 0;
    for (size_t i = 0; i < (size_t)chars; i++) {
        x_kern[i] = 0;

        const struct fcft_glyph *glyph =
            fcft_rasterize_char_utf32(font, wtext[i], subpixel);
        glyphs[i] = glyph;

        if (glyph == NULL)
            continue;

        if (i > 0) {
            if (fcft_kerning(font, prev, wtext[i], &x_kern[i], NULL))
                width += x_kern[i];
        }
        width += glyph->advance.x;
        prev = wtext[i];
    }

    x -= width;

    for (size_t i = 0; i < (size_t)chars; i++) {
        const struct fcft_glyph *glyph = glyphs[i];
        if (glyph == NULL)
            continue;

        x += x_kern[i];
        render_glyph(buf->pix[0], glyph, x, y, &render->pix_counter_color);
        x += glyph->advance.x;
        prev = wtext[i];
    }

    free(x_kern);
    free(glyphs);
    return width;
}

static void
render_cursor(const struct render *render, int x, int baseline, pixman_image_t *pix)
{
    struct fcft_font *font = render->font;

    if (true) {
        /* Bar cursor */
        const int height = min(font->ascent + font->descent, render->row_height);

        pixman_image_fill_rectangles(
            PIXMAN_OP_SRC, pix, &render->pix_input_color,
            1, &(pixman_rectangle16_t){
                x,
                baseline + render->font->descent - height,
                font->underline.thickness,
                height});
    } else {
        /* TODO: future: underline cursor */
        pixman_image_fill_rectangles(
            PIXMAN_OP_SRC, pix, &render->pix_input_color,
            1, &(pixman_rectangle16_t){
                x, baseline - font->underline.position,
                font->max_advance.x,
                font->underline.thickness});
    }
}

static void
adjust_input_glyph_offset(struct render *render, const struct prompt *prompt,
                          size_t count,
                          const struct fcft_glyph *glyphs[static count],
                          long kerning[static count],
                          int start_x, int max_x)
{
    struct fcft_font *font = render->font;
    const size_t cursor_location = prompt_cursor(prompt);

    if (cursor_location == 0)
        render->input_glyph_offset = 0;

    /* Clamp to input string's length */
    render->input_glyph_offset = min(render->input_glyph_offset, count - 1);

    /* If cursor position is before the current offset, adjust the
       offset backward */
    for (size_t i = 0; i < render->input_glyph_offset; i++) {
        if (cursor_location == i + 1) {
            render->input_glyph_offset = i + 1;
            break;
        }
    }

    /* If cursor position is after the current visible portion, adjust the offset forward */
    int pos = start_x;

    for (size_t i = render->input_glyph_offset; i < count; i++) {

        const struct fcft_glyph *glyph = glyphs[i];
        if (glyph == NULL)
            continue;

        const int pixels_needed = max(glyph->x + glyph->width, glyph->advance.x);

        /* Cursor is after this glyph? */
        if (cursor_location == i + 1) {
            /* If current glyph doesn't fit, adjust offset so that it does */
            if (pos + kerning[i] + pixels_needed > max_x) {
                int needed = (pos + kerning[i] + pixels_needed) - max_x;

                /* Font may be variable width, meaning we may have to scroll multiple characters */
                while (needed > 0 && render->input_glyph_offset + 1 < count) {
                    long scrolled_kerning;
                    fcft_kerning(
                        font,
                        glyphs[render->input_glyph_offset]->cp,
                        glyphs[render->input_glyph_offset + 1]->cp,
                        &scrolled_kerning, NULL);

                    const int gained =
                        scrolled_kerning +
                        glyphs[render->input_glyph_offset]->advance.x;

                    needed -= gained;
                    pos -= gained;
                    render->input_glyph_offset++;
                }
            }
        }

        pos += kerning[i] + glyph->advance.x;
    }

    /* Pull in glyphs from the left, if there's space left */
    while (render->input_glyph_offset > 0 && max_x > pos) {
        int total_width = start_x;

        for (size_t i = render->input_glyph_offset; i < count; i++) {
            const struct fcft_glyph *g = glyphs[i];
            if (g == NULL)
                continue;

            const int w = max(g->width, g->advance.x);

            if (total_width + kerning[i] + w > max_x) {
                break;
            }

            total_width += kerning[i] + (i + 1 == count ? w : g->advance.x);
        }

        const struct fcft_glyph *glyph = glyphs[render->input_glyph_offset - 1];
        const int pixels_needed = max(glyph->x + glyph->width, glyph->advance.x);

        assert(render->input_glyph_offset > 0);
        assert(render->input_glyph_offset < count);

        const long kern = kerning[render->input_glyph_offset];
        const int available = max_x - total_width;
        if (kern + pixels_needed > available)
            break;

        render->input_glyph_offset--;
        pos += kern + glyph->advance.x;
    }
}

void
render_prompt(struct render *render, struct buffer *buf,
              const struct prompt *prompt, const struct matches *matches)
{
    struct fcft_font *font = render->font;
    assert(font != NULL);

    const struct config *conf = render->conf;

    const char32_t *pprompt = prompt_prompt(prompt);
    size_t prompt_len = c32len(pprompt);
    const size_t cursor_location = prompt_cursor(prompt);

    const char32_t *ptext = prompt_text(prompt);
    size_t text_len = c32len(ptext);
    bool use_placeholder = text_len == 0;

    const enum fcft_subpixel subpixel =
        (render->conf->colors.background.a == 1. &&
         render->conf->colors.selection.a == 1.)
        ? render->subpixel : FCFT_SUBPIXEL_NONE;

    const struct fcft_glyph *input_glyphs[text_len];
    long input_kerning[text_len];

    {
        const bool use_password = conf->password_mode.enabled && !use_placeholder;

        for (size_t i = 0; i < text_len; i++) {
            char32_t wc = use_password ? conf->password_mode.character : ptext[i];
            input_glyphs[i] = fcft_rasterize_char_utf32(font, wc, subpixel);
            if (i == 0 || use_password)
                input_kerning[i] = 0;
            else
                fcft_kerning(font, ptext[i - 1], ptext[i], &input_kerning[i], NULL);
        }
    }

    if (use_placeholder) {
        ptext = prompt_placeholder(prompt);
        text_len = c32len(ptext);
    }

    int x = render->border_size + render->x_margin;
    int y = render->border_size + render->y_margin + render_baseline(render);

    /* Erase background */
    pixman_color_t bg = render->pix_background_color;
    //pixman_color_t bg = (pixman_color_t){0xffff, 0, 0, 0xffff};
    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, buf->pix[0], &bg, 1,
        &(pixman_rectangle16_t){
            render->border_size + render->x_margin - render->x_margin / 3,
            render->border_size + render->y_margin,
            buf->width - 2 * (render->border_size + render->x_margin - render->x_margin / 3),
            render->row_height});

#if 0
    bg = (pixman_color_t){0, 0xffff, 0, 0xffff};
    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, buf->pix[0], &bg, 1,
        &(pixman_rectangle16_t){
            render->border_size + render->x_margin,
            render->border_size + render->y_margin,
            buf->width - 2 * (render->border_size + render->x_margin),
            render->row_height});
#endif

    int stats_width = conf->match_counter
        ? render_match_count(render, buf, prompt, matches)
        : 0;

    const int max_x =
        buf->width - render->border_size - render->x_margin - stats_width;

    struct fcft_text_run *prompt_run = render->prompt_text_run;
    struct fcft_text_run *input_run =
        use_placeholder ? render->placeholder_text_run : NULL;

    if (fcft_capabilities() & FCFT_CAPABILITY_TEXT_RUN_SHAPING) {
        if (prompt_run == NULL) {
            prompt_run = fcft_rasterize_text_run_utf32(
                font, prompt_len, pprompt, subpixel);
        }
        if (input_run == NULL && use_placeholder) {
            input_run = fcft_rasterize_text_run_utf32(
                font, text_len, ptext, subpixel);
        }
    }

    if (prompt_run != NULL) {
        for (size_t i = 0; i < prompt_run->count; i++) {
            const struct fcft_glyph *glyph = prompt_run->glyphs[i];
            const int pixels_needed = max(glyph->x + glyph->width, glyph->advance.x);

            if (x + pixels_needed > max_x)
                goto out;

            render_glyph(buf->pix[0], glyph, x, y, &render->pix_prompt_color);
            x += glyph->advance.x;
        }
    } else {
        char32_t prev = 0;
        for (size_t i = 0; i < prompt_len; i++) {
            const char32_t wc = pprompt[i];
            const struct fcft_glyph *glyph = fcft_rasterize_char_utf32(font, wc, subpixel);

            if (glyph == NULL) {
                prev = wc;
                continue;
            }

            long x_kern;
            fcft_kerning(font, prev, wc, &x_kern, NULL);

            x += x_kern;

            const int pixels_needed = max(glyph->x + glyph->width, glyph->advance.x);
            if (x + pixels_needed > max_x)
                goto out;

            render_glyph(
                buf->pix[0], glyph, x, y, &render->pix_prompt_color);
            x += glyph->advance.x;
            x += pt_or_px_as_pixels(render, &render->conf->letter_spacing);

            prev = wc;
        }
    }

    /*
     * Adjust glyph offset (i.e. where in the input string we start
     * rendering), to ensure the portion containing the cursor is
     * always visible.
     */
    adjust_input_glyph_offset(
        render, prompt, !use_placeholder ? text_len : 0,
        input_glyphs, input_kerning, x, max_x);

    /* Cursor, if right after the prompt. In all other cases, the
     * cursor will be rendered by the loop below */
    if (cursor_location == render->input_glyph_offset
        || (conf->password_mode.enabled &&
            conf->password_mode.character == U'\0'))
    {
        render_cursor(render, x, y, buf->pix[0]);
    }

    if (input_run != NULL && !(conf->password_mode.enabled && !use_placeholder)) {
        /* We only shape the placeholder string */
        assert(use_placeholder);

        for (size_t i = render->input_glyph_offset; i < input_run->count; i++) {
            const struct fcft_glyph *glyph = input_run->glyphs[i];
            const int pixels_needed = max(glyph->x + glyph->width, glyph->advance.x);

            if (x + pixels_needed > max_x) {
                goto out;
            }

            render_glyph(buf->pix[0], glyph, x, y,
                         use_placeholder
                             ? &render->pix_placeholder_color
                             : &render->pix_input_color);
            x += glyph->advance.x;

            /* Cursor */
            const int cur_cluster = input_run->cluster[i];
            const int next_cluster = i + 1 < input_run->count
                ? input_run->cluster[i + 1] : INT_MAX;

            if (cursor_location > cur_cluster &&
                cursor_location <= next_cluster)
            {
                render_cursor(render, x, y, buf->pix[0]);
            }
        }
    } else {
        char32_t prev = 0;

        for (size_t i = render->input_glyph_offset; i < text_len; i++) {
            if (conf->password_mode.enabled &&
                conf->password_mode.character == U'\0' &&
                !use_placeholder)
            {
                continue;
            }

            const struct fcft_glyph *glyph = input_glyphs[i];

            if (glyph == NULL) {
                prev = 0;
                continue;
            }

            long x_kern;
            fcft_kerning(font, prev, glyph->cp, &x_kern, NULL);

            x += x_kern;

            const int pixels_needed = max(glyph->x + glyph->width, glyph->advance.x);

            if (x + pixels_needed > max_x)
                goto out;

            render_glyph(
                buf->pix[0], glyph, x, y,
                use_placeholder
                    ? &render->pix_placeholder_color
                    : &render->pix_input_color);
            x += glyph->advance.x;
            x += pt_or_px_as_pixels(render, &render->conf->letter_spacing);

            /* Cursor */
            if (cursor_location > 0 && cursor_location - 1 == i)
                render_cursor(render, x, y, buf->pix[0]);

            prev = glyph->cp;
        }
    }

out:
    if (render->prompt_text_run == NULL)
        render->prompt_text_run = prompt_run;
    else if (prompt_run != render->prompt_text_run)
        fcft_text_run_destroy(prompt_run);
    if (use_placeholder && render->placeholder_text_run == NULL)
        render->placeholder_text_run = input_run;
    else if (!use_placeholder || input_run != render->placeholder_text_run)
        fcft_text_run_destroy(input_run);
}

static void
render_match_text(pixman_image_t *pix, double *_x, double _y, double max_x,
                  const char32_t *text, size_t match_count,
                  const struct match_substring matches[static match_count],
                  struct fcft_font *font, enum fcft_subpixel subpixel,
                  int letter_spacing, int tabs,
                  pixman_color_t regular_color, pixman_color_t match_color,
                  struct fcft_text_run **run)
{
    int x = *_x;
    int y = _y;

    const struct fcft_glyph **glyphs = NULL;
    int *clusters = NULL;
    long *kern = NULL;
    size_t count = 0;

    if (*run == NULL &&
        (fcft_capabilities() & FCFT_CAPABILITY_TEXT_RUN_SHAPING))
    {
        *run = fcft_rasterize_text_run_utf32(
            font, c32len(text), text, subpixel);
    }

    if (*run != NULL) {
        glyphs = (*run)->glyphs;
        clusters = (*run)->cluster;
        count = (*run)->count;
    } else {
        count = c32len(text);
        glyphs = xmalloc(count * sizeof(glyphs[0]));
        clusters = xmalloc(count * sizeof(clusters[0]));
        kern = xmalloc(count * sizeof(kern[0]));

        for (size_t i = 0; i < count; i++) {
            const struct fcft_glyph *glyph = fcft_rasterize_char_utf32(
                font, text[i], subpixel);
            if (glyph == NULL) {
                glyphs[i] = NULL;
                continue;
            }

            if (i > 0)
                fcft_kerning(font, text[i - 1], text[i], &kern[i], NULL);
            else
                kern[i] = 0;

            glyphs[i] = glyph;
            clusters[i] = i;
        }
    }

    for (size_t i = 0; i < count; i++) {
        if (text[clusters[i]] == U'\t') {
            const struct fcft_glyph *space =
                fcft_rasterize_char_utf32(font, U' ', subpixel);

            if (space != NULL) {
                const size_t chars_to_next_tab_stop = tabs == 0
                    ? 0
                    : tabs - (clusters[i] % tabs);
                x += chars_to_next_tab_stop * space->advance.x;
            }

            continue;
        }

        bool is_match = false;
        for (size_t j = 0; j < match_count; j++) {
            const struct match_substring *match = &matches[j];
            assert(match->start >= 0);

            if (clusters[i] >= match->start &&
                clusters[i] < match->start + match->len)
            {
                is_match = true;
                break;
            }
        }

        if (x + (kern != NULL ? kern[i] : 0) + glyphs[i]->advance.x > max_x) {
            const struct fcft_glyph *ellipses =
                fcft_rasterize_char_utf32(font, U'…', subpixel);

            if (ellipses != NULL)
                render_glyph(pix, ellipses, x, y, &regular_color);

            break;
        }

        x += kern != NULL ? kern[i] : 0;
        render_glyph(pix, glyphs[i], x, y, is_match ? &match_color : &regular_color);
        x += glyphs[i]->advance.x + letter_spacing;
        y += glyphs[i]->advance.y;
    }

    if (*run == NULL) {
        free(kern);
        free(clusters);
        free(glyphs);
    }

    *_x = x;
}

#if defined(FUZZEL_ENABLE_SVG_LIBRSVG)
static void
render_svg_librsvg(const struct icon *icon, int x, int y, int size,
                   cairo_t *cairo)
{
    RsvgHandle *svg = icon->svg;

    cairo_save(cairo);
    cairo_set_operator(cairo, CAIRO_OPERATOR_ATOP);

 #if LIBRSVG_CHECK_VERSION(2, 46, 0)
    if (cairo_status(cairo) == CAIRO_STATUS_SUCCESS) {
        const RsvgRectangle viewport = {
            .x = x,
            .y = y,
            .width = size,
            .height = size,
        };

        cairo_rectangle(cairo, x, y, size, size);
        cairo_clip(cairo);

        rsvg_handle_render_document(svg, cairo, &viewport, NULL);
    }
 #else
    RsvgDimensionData dim;
    rsvg_handle_get_dimensions(svg, &dim);

    const double scale_x = size / dim.width;
    const double scale_y = size / dim.height;
    const double scale = scale_x < scale_y ? scale_x : scale_y;

    const double height = dim.height * scale;
    const double width = dim.width * scale;

    cairo_rectangle(cairo, x, y, height, width);
    cairo_clip(cairo);

    /* Translate + scale. Note: order matters! */
    cairo_translate(cairo, x, y);
    cairo_scale(cairo, scale, scale);

    if (cairo_status(cairo) == CAIRO_STATUS_SUCCESS)
        rsvg_handle_render_cairo(svg, cairo);
 #endif
    cairo_restore(cairo);
}
#endif /* FUZZEL_ENABLE_SVG_LIBRSVG */

#if defined(FUZZEL_ENABLE_SVG_NANOSVG)
static void
render_svg_nanosvg(struct icon *icon, int x, int y, int size,
                   pixman_image_t *pix, cairo_t *cairo, bool gamma_correct)
{
#if defined(FUZZEL_ENABLE_CAIRO)
    cairo_surface_flush(cairo_get_target(cairo));
#endif

    pixman_image_t *img = NULL;

    /* Look for a cached image, at the correct size */
    tll_foreach(icon->rasterized, it) {
        if (it->item.size == size) {
            img = it->item.pix;
            break;
        }
    }

    if (img == NULL) {
        NSVGimage *svg = icon->svg;
        struct NSVGrasterizer *rast = nsvgCreateRasterizer();

        if (rast == NULL)
            return;

        float scale = svg->width > svg->height ? size / svg->width : size / svg->height;

        const int width = roundf(svg->width * scale);
        const int height = roundf(svg->height * scale);

        uint8_t *data_8bit = xmalloc(width * height * 4);
        uint8_t *data_16bit = NULL;
        uint64_t *abgr16 = NULL;

        nsvgRasterize(rast, svg, 0, 0, scale, data_8bit, width, height, width * 4);

        if (gamma_correct) {
            /* For gamma-correct blending, create 16-bit buffer and image */
            data_16bit = xmalloc(width * height * 8);
            abgr16 = (uint64_t *)data_16bit;

            img = pixman_image_create_bits_no_clear(
                PIXMAN_a16b16g16r16, width, height, (uint32_t *)data_16bit,
                width * 8);
        } else {
            img = pixman_image_create_bits_no_clear(
                PIXMAN_a8b8g8r8, width, height, (uint32_t *)data_8bit, width * 4);
        }

        /* Nanosvg produces non-premultiplied ABGR, while pixman expects
         * premultiplied */

        for (uint32_t *abgr = (uint32_t *)data_8bit;
             abgr < (uint32_t *)(data_8bit + width * height * 4);
             abgr++)
        {
            uint16_t alpha = (*abgr >> 24) & 0xff;
            uint16_t blue = (*abgr >> 16) & 0xff;
            uint16_t green = (*abgr >> 8) & 0xff;
            uint16_t red = (*abgr >> 0) & 0xff;

            /*
             * TODO: decode sRGB -> linear here
             *
             * We should decode to 16-bit, meaning we have to prepare
             * a 16-bit buffer before looping the pixels.
             */

            if (gamma_correct) {
                if (alpha == 0x00)
                    blue = green = red = 0x00;
                else {
                    alpha |= alpha << 8;  /* Alpha already linear, expand to 16-bit */
                    blue = srgb_decode_8_to_16(blue);
                    green = srgb_decode_8_to_16(green);
                    red = srgb_decode_8_to_16(red);

                    blue = blue * alpha / 0xffff;
                    green = green * alpha / 0xffff;
                    red = red * alpha / 0xffff;
                }

                *abgr16 = (uint64_t)alpha << 48 | (uint64_t)blue << 32 | (uint64_t)green << 16 | (uint64_t)red;
                abgr16++;
            } else {
                if (alpha == 0x00)
                    blue = green = red = 0x00;
                else {
                    blue = (uint8_t)(blue * alpha / 0xff);
                    green = (uint8_t)(green * alpha / 0xff);
                    red = (uint8_t)(red * alpha / 0xff);
                }

                *abgr = (uint32_t)alpha << 24 | blue << 16 | green << 8 | red;
            }
        }

        if (gamma_correct) {
            /* Free the 8-bit buffer as we've converted everything to 16-bit */
            free(data_8bit);
        }

        nsvgDeleteRasterizer(rast);

        tll_push_back(icon->rasterized, ((struct rasterized){img, size}));
    }

    const int w = pixman_image_get_width(img);
    const int h = pixman_image_get_height(img);

    pixman_image_composite32(
        PIXMAN_OP_OVER, img, NULL, pix, 0, 0, 0, 0,
        x + (size - w) / 2,
        y + (size - h) / 2,
        w, h);

#if defined(FUZZEL_ENABLE_CAIRO)
    cairo_surface_mark_dirty(cairo_get_target(cairo));
#endif
}
#endif /* FUZZEL_ENABLE_SVG_NANOSVG */

#if defined(FUZZEL_ENABLE_SVG_RESVG)
static void
render_svg_resvg(struct icon *icon, int x, int y, int size,
                 pixman_image_t *pix, cairo_t *cairo, bool gamma_correct)
{
#if defined(FUZZEL_ENABLE_CAIRO)
    cairo_surface_flush(cairo_get_target(cairo));
#endif

    pixman_image_t *img = NULL;

    /* Look for a cached image, at the correct size */
    tll_foreach(icon->rasterized, it) {
        if (it->item.size == size) {
            img = it->item.pix;
            break;
        }
    }

    if (img == NULL) {
        resvg_render_tree *tree = icon->svg;
        resvg_size svg_size = resvg_get_image_size(tree);

        if (svg_size.width == 0 || svg_size.height == 0)
            return;

        float scale = svg_size.width > svg_size.height ? size / svg_size.width : size / svg_size.height;

        const int width = roundf(svg_size.width * scale);
        const int height = roundf(svg_size.height * scale);

        uint8_t *data_8bit = xmalloc(width * height * 4);
        memset(data_8bit, 0, width * height * 4);

        /* resvg renders to RGBA8888 premultiplied */
        resvg_transform transform = resvg_transform_identity();
        transform.a = scale;
        transform.d = scale;
        resvg_render(tree, transform, width, height, (char *)data_8bit);

        if (gamma_correct) {
            /* For gamma-correct blending, create 16-bit buffer and image */
            uint8_t *data_16bit = xmalloc(width * height * 8);
            uint64_t *abgr16 = (uint64_t *)data_16bit;

            /* resvg produces premultiplied RGBA, need to convert to ABGR16 for pixman */
            for (uint32_t *rgba = (uint32_t *)data_8bit;
                 rgba < (uint32_t *)(data_8bit + width * height * 4);
                 rgba++)
            {
                uint16_t red = (*rgba >> 0) & 0xff;
                uint16_t green = (*rgba >> 8) & 0xff;
                uint16_t blue = (*rgba >> 16) & 0xff;
                uint16_t alpha = (*rgba >> 24) & 0xff;

                /* Decode sRGB to linear for gamma-correct blending */
                if (alpha == 0x00) {
                    blue = green = red = 0x00;
                } else {
                    /* First, un-premultiply */
                    blue = (uint16_t)(blue * 0xff / alpha);
                    green = (uint16_t)(green * 0xff / alpha);
                    red = (uint16_t)(red * 0xff / alpha);

                    /* Convert to linear 16-bit */
                    alpha |= alpha << 8;  /* Alpha already linear, expand to 16-bit */
                    blue = srgb_decode_8_to_16(blue);
                    green = srgb_decode_8_to_16(green);
                    red = srgb_decode_8_to_16(red);

                    /* Re-premultiply in 16-bit linear space */
                    blue = blue * alpha / 0xffff;
                    green = green * alpha / 0xffff;
                    red = red * alpha / 0xffff;
                }

                *abgr16 = (uint64_t)alpha << 48 | (uint64_t)blue << 32 | (uint64_t)green << 16 | (uint64_t)red;
                abgr16++;
            }

            /* Free the 8-bit buffer as we've converted everything to 16-bit */
            free(data_8bit);

            /* Create 16-bit pixman image */
            img = pixman_image_create_bits_no_clear(
                PIXMAN_a16b16g16r16, width, height, (uint32_t *)data_16bit,
                width * 8);
        } else {
            /* Create 8-bit pixman image */
            img = pixman_image_create_bits_no_clear(
                PIXMAN_a8b8g8r8, width, height, (uint32_t *)data_8bit, width * 4);
        }

        tll_push_back(icon->rasterized, ((struct rasterized){img, size}));
    }

    const int w = pixman_image_get_width(img);
    const int h = pixman_image_get_height(img);

    pixman_image_composite32(
        PIXMAN_OP_OVER, img, NULL, pix, 0, 0, 0, 0,
        x + (size - w) / 2,
        y + (size - h) / 2,
        w, h);

#if defined(FUZZEL_ENABLE_CAIRO)
    cairo_surface_mark_dirty(cairo_get_target(cairo));
#endif
}
#endif /* FUZZEL_ENABLE_SVG_RESVG */

static void
render_svg(struct icon *icon, int x, int y, int size,
           pixman_image_t *pix, cairo_t *cairo, bool gamma_correct,
           bool print_timing_info)
{
    assert(icon->type == ICON_SVG);

    if (icon->svg == NULL) {
        struct timespec *start_load = time_begin();

        if (!icon_from_svg(icon, icon->path)) {
            free(start_load);
            return;
        }

        time_finish(start_load, NULL, "%s loaded", icon->path);
        LOG_DBG("%s", icon->path);
    }

    struct timespec *render_start = time_begin();

#if defined(FUZZEL_ENABLE_SVG_LIBRSVG)
    render_svg_librsvg(icon, x, y, size, cairo);
#elif defined(FUZZEL_ENABLE_SVG_NANOSVG)
    render_svg_nanosvg(icon, x, y, size, pix, cairo, gamma_correct);
#elif defined(FUZZEL_ENABLE_SVG_RESVG)
    render_svg_resvg(icon, x, y, size, pix, cairo, gamma_correct);
#endif

    time_finish(render_start, NULL, "%s rendered", icon->path);
}

#if defined(FUZZEL_ENABLE_PNG_LIBPNG)
static void
render_png_libpng(struct icon *icon, int x, int y, int size,
                  pixman_image_t *pix, cairo_t *cairo,
                  enum scaling_filter scaling_filter)
{
#if defined(FUZZEL_ENABLE_CAIRO)
    cairo_surface_flush(cairo_get_target(cairo));
#endif

    pixman_image_t *png = icon->png;
    pixman_format_code_t fmt = pixman_image_get_format(png);
    int height = pixman_image_get_height(png);
    int width = pixman_image_get_width(png);

    if (height > size || width > size) {
        double scale = (double)size / (height > width ? height : width);

        pixman_f_transform_t _scale_transform;
        pixman_f_transform_init_scale(&_scale_transform, 1. / scale, 1. / scale);

        pixman_transform_t scale_transform;
        pixman_transform_from_pixman_f_transform(
            &scale_transform, &_scale_transform);
        pixman_image_set_transform(png, &scale_transform);

        const int largest_side = max(width, height);

        if (largest_side >= 1024) {
            if (!icon->png_size_warned) {
                LOG_WARN(
                    "%s: PNG is too large (%dx%d); "
                    "downscaling using a less precise filter (fast)",
                    icon->path, width, height);
                icon->png_size_warned = true;
            }

            pixman_image_set_filter(png, PIXMAN_FILTER_FAST, NULL, 0);
        }

        else {

            bool slow_scaling_filter = false;

            switch (scaling_filter) {
            case SCALING_FILTER_NONE:
            case SCALING_FILTER_NEAREST:
            case SCALING_FILTER_BILINEAR:
            case SCALING_FILTER_BOX:
                slow_scaling_filter = false;
                break;

            case SCALING_FILTER_CUBIC:
            case SCALING_FILTER_LANCZOS3:
            case SCALING_FILTER_LINEAR:
            case SCALING_FILTER_LANCZOS2:
            case SCALING_FILTER_LANCZOS3_STRETCHED:
                slow_scaling_filter = true;
                break;
            }

            if (slow_scaling_filter && largest_side >= 256) {
                if (!icon->png_size_warned) {
                    LOG_WARN(
                        "%s: PNG is too large (%dx%d); "
                        "downscaling using a less precise filter (box)",
                        icon->path, width, height);
                    icon->png_size_warned = true;
                }

                scaling_filter = SCALING_FILTER_BOX;
            }

            switch (scaling_filter) {
            case SCALING_FILTER_NONE:
                break;

            /*
             * "simple" filters
             */

            case SCALING_FILTER_NEAREST:
                pixman_image_set_filter(png, PIXMAN_FILTER_NEAREST, NULL, 0);
                break;

            case SCALING_FILTER_BILINEAR:
                pixman_image_set_filter(png, PIXMAN_FILTER_BILINEAR, NULL, 0);
                break;

            /*
             * Separable convolution filters
             */
            case SCALING_FILTER_CUBIC:
            case SCALING_FILTER_LANCZOS3:
            case SCALING_FILTER_BOX:
            case SCALING_FILTER_LINEAR:
            case SCALING_FILTER_LANCZOS2:
            case SCALING_FILTER_LANCZOS3_STRETCHED: {

                pixman_kernel_t kernel;

                switch (scaling_filter) {
                case SCALING_FILTER_CUBIC: kernel = PIXMAN_KERNEL_CUBIC; break;
                case SCALING_FILTER_LANCZOS3: kernel = PIXMAN_KERNEL_LANCZOS3; break;
                case SCALING_FILTER_BOX: kernel = PIXMAN_KERNEL_BOX; break;
                case SCALING_FILTER_LINEAR: kernel = PIXMAN_KERNEL_LINEAR; break;
                case SCALING_FILTER_LANCZOS2: kernel = PIXMAN_KERNEL_LANCZOS2; break;
                case SCALING_FILTER_LANCZOS3_STRETCHED: kernel = PIXMAN_KERNEL_LANCZOS3_STRETCHED; break;
                default: assert(false); kernel = PIXMAN_KERNEL_CUBIC; break;
                }

                int param_count = 0;
                pixman_fixed_t *params = pixman_filter_create_separable_convolution(
                    &param_count,
                    pixman_double_to_fixed(1. / scale),
                    pixman_double_to_fixed(1. / scale),
                    kernel, kernel,
                    kernel, kernel,
                    pixman_int_to_fixed(1),
                    pixman_int_to_fixed(1));

                if (params != NULL || param_count == 0) {
                    pixman_image_set_filter(
                        png, PIXMAN_FILTER_SEPARABLE_CONVOLUTION,
                        params, param_count);
                }

                free(params);
                break;
            }
            }
        }

        width *= scale;
        height *= scale;

        int stride = stride_for_format_and_width(fmt, width);
        uint8_t *data = xmalloc(height * stride);
        pixman_image_t *scaled_png = pixman_image_create_bits_no_clear(
            fmt, width, height, (uint32_t *)data, stride);
        pixman_image_composite32(
            PIXMAN_OP_SRC, png, NULL, scaled_png, 0, 0, 0, 0, 0, 0, width, height);

        free(pixman_image_get_data(png));
        pixman_image_unref(png);

        png = scaled_png;
        icon->png = png;
    }

    pixman_image_composite32(
        PIXMAN_OP_OVER, png, NULL, pix, 0, 0, 0, 0,
        x + (size - width) / 2,
        y + (size - height) / 2,
        width, height);

#if defined(FUZZEL_ENABLE_CAIRO)
    cairo_surface_mark_dirty(cairo_get_target(cairo));
#endif
}
#endif /* FUZZEL_ENABLE_PNG_LIBPNG */

static void
render_png(struct icon *icon, int x, int y, int size, pixman_image_t *pix,
           cairo_t *cairo, bool gamma_correct, bool print_timing_info,
           enum scaling_filter scaling_filter)
{
    assert(icon->type == ICON_PNG);

    if (icon->png == NULL) {
        struct timespec *start_load = time_begin();

        if (!icon_from_png(icon, icon->path, gamma_correct)) {
            free(start_load);
            return;
        }

        time_finish(start_load, NULL, "%s loaded", icon->path);
        LOG_DBG("%s", icon->path);
    }

    struct timespec *start_render = time_begin();

#if defined(FUZZEL_ENABLE_PNG_LIBPNG)
    render_png_libpng(icon, x, y, size, pix, cairo, scaling_filter);
#endif

    time_finish(start_render, NULL, "%s rendered", icon->path);
}

static int
first_row_y(const struct render *render)
{
    return (render->border_size +
            render->y_margin +
            (render->conf->hide_prompt ? 0 : render->row_height) +
            (render->conf->hide_prompt ? 0 : render->inner_pad));
}

static void
render_match_entry_background(const struct render *render,
                              int idx, int row_count,
                              pixman_image_t *pix, int width)
{
    pixman_color_t bg = render->pix_background_color;

    const int sel_margin = render->x_margin / 3;

    const int x = render->border_size + render->x_margin - sel_margin;
    const int y = first_row_y(render) + idx * render->row_height;
    const int w = width - 2 * (render->border_size + render->x_margin - sel_margin);
    const int h = row_count * render->row_height;

    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, pix, &bg, 1, &(pixman_rectangle16_t){x, y, w, h});
}

static void
render_selected_match_entry_background(struct render *render,
                                       int idx, pixman_image_t *pix, int width)
{
    pixman_color_t bg = render->pix_selection_color;

    const int sel_margin = render->x_margin / 3;

    const int x = render->border_size + render->x_margin - sel_margin;
    const int y = first_row_y(render) + idx * render->row_height;
    const int w = width - 2 * (render->border_size + render->x_margin - sel_margin);
    const int h = 1 * render->row_height;

    // limit radius to half of height, any larger and it causes weird shapes
    // also limit it when horizontal padding is small, to prevent icon pop out
    const unsigned int radius = min(
        min(render->selection_border_radius, h / 2), render->x_margin);

    if (render->selection_corners == NULL) {
        render->selection_corners = pixman_image_create_bits(
            pixman_image_get_format(pix), w, h, NULL, w * 4);
        render_rounded_rectangle(render->selection_corners, &bg, &bg, radius, 0, 0, 0, w, h);
    }
    pixman_image_composite(
        PIXMAN_OP_OVER, render->selection_corners, NULL, pix,
        0, 0, 0, 0, x, y, w, h);
}

static void
render_one_match_entry(struct render *render, const struct matches *matches,
                       const struct match *match, bool render_icons,
                       int idx, bool is_selected, int width, int height,
                       pixman_image_t *pix, cairo_t *cairo)
{
    const enum fcft_subpixel subpixel =
        (render->conf->colors.background.a == 1. &&
         render->conf->colors.selection.a == 1.)
            ? render->subpixel : FCFT_SUBPIXEL_NONE;

    struct fcft_font *font =
        is_selected ? render->font_bold : render->font;
    const struct fcft_glyph *ellipses =
        fcft_rasterize_char_utf32(font, U'…', subpixel);

    const int first_row = first_row_y(render);
    double cur_x = render->border_size + render->x_margin;
    double max_x = width - render->border_size - render->x_margin;

    render_match_entry_background(render, idx, 1, pix, width);

    if (is_selected) {
        render_selected_match_entry_background(render, idx, pix, width);

        /* If currently selected item has a scalable icon, and if
         * there's "enough" free space, render a large representation
         * of the icon */

        const double ratio = render->conf->image_size_ratio;
        const double size = min(height * ratio, width * ratio);
        const double img_x = (width - size) / 2.;
        const double img_y_bottom = max(height - first_row, 0.);
        const double img_y = max(img_y_bottom - size, 0.);
        const size_t match_count = matches_get_count(matches);
        const double list_end = first_row + match_count * render->row_height;

        LOG_DBG("img_y=%f, list_end=%f", img_y, list_end);

        if (render_icons &&
            match->application->icon.type == ICON_SVG &&
            img_y > list_end + render->row_height)
        {
            render_svg(&match->application->icon, img_x, img_y, size, pix, cairo,
                       render->gamma_correct, render->conf->print_timing_info);
        }
    }

    if (render_icons) {
        struct icon *icon = &match->application->icon;
        const int size = render->icon_height;
        const int img_x = cur_x;
        const int img_y = first_row + idx * render->row_height + (render->row_height - size) / 2;

        switch (icon->type) {
        case ICON_NONE:
            break;

        case ICON_PNG:
            render_png(icon, img_x, img_y, size, pix, cairo,
                       render->gamma_correct, render->conf->print_timing_info,
                       render->conf->png_scaling_filter);
            break;

        case ICON_SVG:
            render_svg(icon, img_x, img_y, size, pix, cairo,
                       render->gamma_correct, render->conf->print_timing_info);
            break;
        }
    }

    const struct fcft_glyph *space =
        fcft_rasterize_char_utf32(render->font, U' ', subpixel);

    cur_x +=
        (render->conf->icons_enabled && matches_have_icons(matches)
            ? (render->row_height +
               (space != NULL ? space->advance.x : render->font->max_advance.x))
            : 0) +
        pt_or_px_as_pixels(render, &render->conf->letter_spacing);

    /* Replace newlines in title, with spaces (basic support for
     * multiline entries) */
    if (match->application->render_title == NULL) {
        char32_t *newline = c32chr(match->application->title, U'\n');

        if (newline != NULL) {
            char32_t *render_title = xc32dup(match->application->title);

            newline = render_title + (newline - match->application->title);
            *newline = U' ';

            while ((newline = c32chr(newline, U'\n')) != NULL)
                *newline = U' ';

            match->application->render_title = render_title;
        } else {
            /* No newlines, use title as-is */
            match->application->render_title = match->application->title;
        }
    }

    const int y = first_row + render_baseline(render) + idx * render->row_height;

    /* Application title */
    render_match_text(
        pix, &cur_x, y, max_x - (ellipses != NULL ? ellipses->width : 0),
        match->application->render_title, match->pos_count, match->pos,
        font, subpixel,
        pt_or_px_as_pixels(render, &render->conf->letter_spacing),
        render->conf->tabs,
        (is_selected
            ? render->pix_selection_text_color
            : render->pix_text_color),
        (is_selected
            ? render->pix_selection_match_color
            : render->pix_match_color),
        (is_selected
            ? &match->application->shaped_bold
            : &match->application->shaped));
}

void
render_match_list(struct render *render, struct buffer *buf,
                  const struct prompt *prompt, const struct matches *matches)
{
    const size_t match_count = matches_get_count(matches);
    const size_t selected = matches_get_match_index(matches);

    assert(match_count == 0 || selected < match_count);

    bool render_icons = mtx_trylock(render->icon_lock) == thrd_success;

    if (render->workers.count > 0) {
        mtx_lock(&render->workers.lock);
        render->workers.matches = matches;
        render->workers.buf = buf;
        render->workers.render_icons= render_icons;

        for (size_t i = 0; i < render->workers.count; i++)
            sem_post(&render->workers.start);

        assert(tll_length(render->workers.queue) == 0);
    }

    /* Erase background of the "empty" area, after the last match */
    const size_t effective_lines = matches_max_matches_per_page(matches);
    render_match_entry_background(
        render, match_count, effective_lines - match_count,
        buf->pix[0], buf->width);

    for (size_t i = 0; i < match_count; i++) {
        if (render->workers.count == 0) {
            const struct match *match = matches_get(matches, i);
            render_one_match_entry(
                render, matches, match, render_icons, i, i == selected,
                buf->width, buf->height, buf->pix[0],
#if defined(FUZZEL_ENABLE_CAIRO)
                buf->cairo[0]
#else
                NULL
#endif
                );
        } else {
            tll_push_back(render->workers.queue, i);
        }
    }

    if (render->workers.count > 0) {
        for (size_t i = 0; i < render->workers.count; i++)
            tll_push_back(render->workers.queue, -1);
        mtx_unlock(&render->workers.lock);

        for (size_t i = 0; i < render->workers.count; i++)
            sem_wait(&render->workers.done);

        render->workers.matches = NULL;
        render->workers.buf = NULL;
        render->workers.render_icons = false;
    }

    if (render_icons)
        mtx_unlock(render->icon_lock);
}

/* THREAD */
static int
render_worker_thread(void *_ctx)
{
    struct thread_context *ctx = _ctx;
    struct render *render = ctx->render;
    const int my_id = ctx->my_id;
    free(ctx);

    sigset_t mask;
    sigfillset(&mask);
    pthread_sigmask(SIG_SETMASK, &mask, NULL);

    char proc_title[16];
    xsnprintf(proc_title, sizeof(proc_title), "fuzzel:rend:%d", my_id);

    if (pthread_setname_np(pthread_self(), proc_title) < 0)
        LOG_ERRNO("render worker %d: failed to set process title", my_id);

    sem_t *start = &render->workers.start;
    sem_t *done = &render->workers.done;
    mtx_t *lock = &render->workers.lock;

    while (true) {
        sem_wait(start);

        const struct matches *matches = render->workers.matches;
        struct buffer *buf = render->workers.buf;
        const bool render_icons = render->workers.render_icons;
        const size_t selected = matches != NULL
            ? matches_get_match_index(matches)
            : 0;

        bool frame_done = false;

        while (!frame_done) {
            mtx_lock(lock);
            assert(tll_length(render->workers.queue) > 0);

            int row_no = tll_pop_front(render->workers.queue);
            mtx_unlock(lock);

            switch (row_no) {
            default: {
                assert(buf != NULL);

                const struct match *match = matches_get(matches, row_no);
                render_one_match_entry(
                    render, matches, match, render_icons,
                    row_no, row_no == selected, buf->width, buf->height,
                    buf->pix[my_id],
#if defined(FUZZEL_ENABLE_CAIRO)
                    buf->cairo[my_id]
#else
                    NULL
#endif
                    );

                break;
            }

            case -1:
                frame_done = true;
                sem_post(done);
                break;

            case -2:
                return 0;
            }
        }
    }

    return -1;
}

struct render *
render_init(const struct config *conf, mtx_t *icon_lock)
{
    struct render *render = xcalloc(1, sizeof(*render));
    *render = (struct render){
        .conf = conf,
        .icon_lock = icon_lock,
    };

    if (sem_init(&render->workers.start, 0, 0) < 0 ||
        sem_init(&render->workers.done, 0, 0) < 0)
    {
        LOG_ERRNO("failed to instantiate render worker semaphores");
        goto err_free_render;
    }

    int err;
    if ((err = mtx_init(&render->workers.lock, mtx_plain)) != thrd_success) {
        LOG_ERR("failed to instantiate render worker mutex: %d", err);
        goto err_free_semaphores;
    }

    const size_t num_workers = min(conf->render_worker_count, conf->lines);

    render->workers.threads = xcalloc(
        num_workers, sizeof(render->workers.threads[0]));

    for (size_t i = 0; i < num_workers; i++) {
        struct thread_context *ctx = xmalloc(sizeof(*ctx));
        *ctx = (struct thread_context){
            .render = render,
            .my_id = 1 + i,
        };

        int ret = thrd_create(
            &render->workers.threads[i], &render_worker_thread, ctx);

        if (ret != thrd_success) {
            LOG_ERR("failed to create render worker thread: %d", ret);
            render->workers.threads[i] = 0;
            goto err_free_semaphores_and_lock;
        }

        render->workers.count++;
    }

    LOG_INFO("using %hu render worker threads", render->workers.count);

    return render;

err_free_semaphores_and_lock:
    mtx_destroy(&render->workers.lock);
err_free_semaphores:
    sem_destroy(&render->workers.start);
    sem_destroy(&render->workers.done);
err_free_render:
    free(render);
    return NULL;
}

void
render_initialize_colors(struct render *render, const struct config *conf,
                         bool gamma_correct)
{
    render->gamma_correct = gamma_correct;
    render->pix_background_color = rgba2pixman(gamma_correct, conf->colors.background);
    render->pix_border_color = rgba2pixman(gamma_correct, conf->colors.border);
    render->pix_text_color = rgba2pixman(gamma_correct, conf->colors.text);
    render->pix_prompt_color = rgba2pixman(gamma_correct, conf->colors.prompt);
    render->pix_input_color = rgba2pixman(gamma_correct, conf->colors.input);
    render->pix_match_color = rgba2pixman(gamma_correct, conf->colors.match);
    render->pix_selection_color = rgba2pixman(gamma_correct, conf->colors.selection);
    render->pix_selection_text_color = rgba2pixman(gamma_correct, conf->colors.selection_text);
    render->pix_selection_match_color = rgba2pixman(gamma_correct, conf->colors.selection_match);
    render->pix_counter_color = rgba2pixman(gamma_correct, conf->colors.counter);
    render->pix_placeholder_color = rgba2pixman(gamma_correct, conf->colors.placeholder);
}

void
render_set_subpixel(struct render *render, enum fcft_subpixel subpixel)
{
    render->subpixel = subpixel;
}

void
render_resized(struct render *render, int *new_width, int *new_height)
{
    struct fcft_font *font = render->font;
    const float scale = render->scale;

    assert(font != NULL);
    assert(render->font_bold != NULL);

    const struct fcft_glyph *W = fcft_rasterize_char_utf32(
        font, U'o', render->subpixel);

    const unsigned x_margin = render->conf->pad.x * scale;
    const unsigned y_margin = render->conf->pad.y * scale;
    const unsigned inner_pad = render->conf->lines > 0
        ? render->conf->pad.inner * scale
        : 0;

    const unsigned border_size = render->conf->border.size * scale;
    const unsigned border_radius = render->conf->border.radius * scale;
    const unsigned selection_border_radius = render->conf->selection_border.radius * scale;

    const unsigned row_height = render->conf->line_height.px >= 0
        ? pt_or_px_as_pixels(render, &render->conf->line_height)
        : max(font->height, font->ascent + font->descent);

    const unsigned icon_height = max(0, row_height - font->descent);

    const unsigned height =
        border_size +                        /* Top border */
        y_margin +
        (render->conf->hide_prompt ? 0 : row_height) +          /* The prompt (hidden if hide_prompt) */
        (render->conf->hide_prompt ? 0 : inner_pad) +           /* Padding between prompt and matches (only if prompt shown) */
        render->conf->lines * row_height +   /* Matches */
        y_margin +
        border_size;                         /* Bottom border */

    const unsigned width =
        border_size +                        /* Top border */
        x_margin +
        (max((W->advance.x + pt_or_px_as_pixels(
                  render, &render->conf->letter_spacing)),
             0)
         * render->conf->chars) +
        x_margin +
        border_size;

    LOG_DBG("x-margin: %d, y-margin: %d, border-size: %d, border-radius: %d, "
            "row-height: %d, icon-height: %d, height: %d, width: %d, scale: %f",
            x_margin, y_margin, border_size, border_radius,
            row_height, icon_height, height, width, scale);

    render->y_margin = y_margin;
    render->x_margin = x_margin;
    render->inner_pad = inner_pad;
    render->border_size = border_size;
    render->border_radius = border_radius;
    render->selection_border_radius = selection_border_radius;
    render->row_height = row_height;
    render->icon_height = icon_height;

    if (new_width != NULL)
        *new_width = width;
    if (new_height != NULL)
        *new_height = height;

    /* invalidate cached corners since the size could have changed */
    if (render->selection_corners != NULL) {
        pixman_image_unref(render->selection_corners);
        render->selection_corners = NULL;
    }
}

bool
render_set_font_and_update_sizes(struct render *render, struct fcft_font *font,
                                 struct fcft_font *font_bold,
                                 float scale, float dpi, bool size_font_by_dpi,
                                 int *new_width, int *new_height)
{
    if (font != NULL) {
        fcft_destroy(render->font);
        render->font = font;
    } else {
        assert(render->font != NULL);
        font = render->font;
    }

    if (font_bold != NULL) {
        fcft_destroy(render->font_bold);
        render->font_bold = font_bold;
    } else {
        assert(render->font_bold != NULL);
        font_bold = render->font_bold;
    }

    render->scale = scale;
    render->dpi = dpi;
    render->size_font_by_dpi = size_font_by_dpi;

    render_resized(render, new_width, new_height);
    return true;
}

int
render_icon_size(const struct render *render)
{
    return render->icon_height;
}

void
render_destroy(struct render *render)
{
    if (render == NULL)
        return;

    mtx_lock(&render->workers.lock);
    {
        assert(tll_length(render->workers.queue) == 0);

        for (size_t i = 0; i < render->workers.count; i++) {
            assert(render->workers.threads[i] != 0);
            sem_post(&render->workers.start);
            tll_push_back(render->workers.queue, -2);
        }
    }
    mtx_unlock(&render->workers.lock);

    for (size_t i = 0; i < render->workers.count; i++) {
        assert(render->workers.threads[i] != 0);
        thrd_join(render->workers.threads[i], NULL);
    }

    free(render->workers.threads);
    mtx_destroy(&render->workers.lock);
    sem_destroy(&render->workers.start);
    sem_destroy(&render->workers.done);
    assert(tll_length(render->workers.queue) == 0);
    tll_free(render->workers.queue);

    fcft_text_run_destroy(render->prompt_text_run);
    fcft_text_run_destroy(render->placeholder_text_run);

    if (render->selection_corners != NULL)
        pixman_image_unref(render->selection_corners);

    fcft_destroy(render->font);
    fcft_destroy(render->font_bold);
    free(render);
}

ssize_t
render_get_row_num(const struct render *render, int width, int x, int y,
                   const struct matches *matches)
{
    const int y_margin = render->y_margin;
    const int inner_pad = render->inner_pad;
    const int border_size = render->border_size;
    const int row_height = render->row_height;

    const int min_x = render->border_size + render->x_margin - render->x_margin / 3;
    const int max_x = width - (min_x);

    const int first_row = 1 * border_size + y_margin +
        (render->conf->hide_prompt ? 0 : row_height) +
        (render->conf->hide_prompt ? 0 : inner_pad);

    const size_t match_count = matches_get_count(matches);
    const size_t last_row = first_row + match_count * row_height;

    ssize_t row = -1;

    if (y >= first_row && y < last_row && x >= min_x && x < max_x)
        row = (y - first_row) / row_height;

    return row;
}

void
render_flush_text_run_cache(struct render *render)
{
    fcft_text_run_destroy(render->prompt_text_run);
    fcft_text_run_destroy(render->placeholder_text_run);
    render->prompt_text_run = NULL;
    render->placeholder_text_run = NULL;
}
