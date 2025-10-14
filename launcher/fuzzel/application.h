#pragma once

#include <stdbool.h>
#include <pixman.h>
#include <threads.h>

#if defined(FUZZEL_ENABLE_SVG_LIBRSVG)
 #include <librsvg/rsvg.h>
#endif

#if defined(FUZZEL_ENABLE_SVG_NANOSVG)
 #include <nanosvg/nanosvg.h>
#endif

#if defined(FUZZEL_ENABLE_SVG_RESVG)
 #include <resvg.h>
#endif

#include <fcft/fcft.h>
#include <tllist.h>

#include "prompt.h"

enum icon_type { ICON_NONE, ICON_PNG, ICON_SVG };

struct rasterized {
    pixman_image_t *pix;
    int size;
};
typedef tll(struct rasterized) rasterized_list_t;

struct icon {
    char *name;
    char *path;
    enum icon_type type;
    union {
#if defined(FUZZEL_ENABLE_PNG_LIBPNG)
        pixman_image_t *png;
#else
        void *png;
#endif
#if defined(FUZZEL_ENABLE_SVG_LIBRSVG)
        RsvgHandle *svg;
#elif defined(FUZZEL_ENABLE_SVG_NANOSVG)
        NSVGimage *svg;
#elif defined(FUZZEL_ENABLE_SVG_RESVG)
        resvg_render_tree *svg;
#else
        void *svg;
#endif
    };

    /* Icon is "too large" and we've already warned about this */
    bool png_size_warned;

    /* List of cached rasterizations (used with SVGs) */
    rasterized_list_t rasterized;
};

typedef tll(char32_t *) char32_list_t;
typedef tll(char *) char_list_t;

struct application {
    char *id; /* Desktop File ID, as defined in the Desktop Entry specicication
                 https://specifications.freedesktop.org/desktop-entry-spec/desktop-entry-spec-latest.html */
    char *path;
    char *exec;
    char *app_id;
    char32_t *title;
    char32_t *render_title;
    size_t index;

    /*
     * To get good search performance, we cache both the lower-case
     * versions of metadata, and their (string) lengths. This way, we
     * can avoid having to call c32len() and toc32lower() over and
     * over and over again when searching...
     */
    char32_t *title_lowercase; /* Lower cased! */
    char32_t *basename;        /* Lower cased! */
    char32_t *wexec;           /* Lower cased! Same as ‘exec’, but for matching purposes */
    char32_t *generic_name;    /* Lower cased! */
    char32_t *comment;         /* Lower cased! */
    char32_list_t keywords;    /* Lower cased! */
    char32_list_t categories;  /* Lower cased! */

    char32_t *dmenu_input;     /* Full dmenu input, may contain multiple columns */
    char32_t *dmenu_match_nth; /* What to match against, with --match-nth= */

    size_t title_len;
    size_t basename_len;
    size_t wexec_len;
    size_t generic_name_len;
    size_t comment_len;
    size_t dmenu_match_nth_len;
    /* keywords and categories lengths not cached */

    struct icon icon;
    bool visible;
    bool startup_notify;
    unsigned count;
    struct fcft_text_run *shaped;
    struct fcft_text_run *shaped_bold;
};

bool application_execute(
    const struct application *app, const struct prompt *prompt,
    const char *launch_prefix, const char *xdg_activation_token);

struct application_list {
    struct application **v;
    size_t count;
    size_t visible_count;
    mtx_t lock;
};

struct application_list *applications_init(void);
void applications_destroy(struct application_list *apps);
void applications_flush_text_run_cache(struct application_list *apps);
