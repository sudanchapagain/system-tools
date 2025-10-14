#include "config.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <pwd.h>
#include <fcntl.h>

#include <fontconfig/fontconfig.h>
#include <tllist.h>

#define LOG_MODULE "config"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "tokenize.h"

struct config_file {
    char *path;
    int fd;
};

enum section {
    SECTION_MAIN,
    SECTION_LOW,
    SECTION_NORMAL,
    SECTION_CRITICAL,
    SECTION_COUNT,
};

static const char *const section_names[] = {
    [SECTION_MAIN] = "main",
    [SECTION_LOW] = "low",
    [SECTION_NORMAL] = "normal",
    [SECTION_CRITICAL] = "critical",
};


static const char *
get_user_home_dir(void)
{
    const struct passwd *passwd = getpwuid(getuid());
    if (passwd == NULL)
        return NULL;
    return passwd->pw_dir;
}

static struct config_file
open_config(void)
{
     char *path = NULL;
    struct config_file ret = {.path = NULL, .fd = -1};

    const char *xdg_config_home = getenv("XDG_CONFIG_HOME");
    const char *xdg_config_dirs = getenv("XDG_CONFIG_DIRS");
    const char *home_dir = get_user_home_dir();
    char *xdg_config_dirs_copy = NULL;

    /* First, check XDG_CONFIG_HOME (or .config, if unset) */
    if (xdg_config_home != NULL && xdg_config_home[0] != '\0') {
        if (asprintf(&path, "%s/fnott/fnott.ini", xdg_config_home) < 0) {
            LOG_ERRNO("failed to build fnott.ini path");
            goto done;
        }
    } else if (home_dir != NULL) {
        if (asprintf(&path, "%s/.config/fnott/fnott.ini", home_dir) < 0) {
            LOG_ERRNO("failed to build fnott.ini path");
            goto done;
        }
    }

    if (path != NULL) {
        LOG_DBG("checking for %s", path);
        int fd = open(path, O_RDONLY | O_CLOEXEC);

        if (fd >= 0) {
            ret = (struct config_file) {.path = path, .fd = fd};
            path = NULL;
            goto done;
        }
    }

    xdg_config_dirs_copy = xdg_config_dirs != NULL && xdg_config_dirs[0] != '\0'
        ? strdup(xdg_config_dirs)
        : strdup("/etc/xdg");

    if (xdg_config_dirs_copy == NULL || xdg_config_dirs_copy[0] == '\0')
        goto done;

    for (const char *conf_dir = strtok(xdg_config_dirs_copy, ":");
         conf_dir != NULL;
         conf_dir = strtok(NULL, ":"))
    {
        free(path);
        path = NULL;

        if (asprintf(&path, "%s/fnott/fnott.ini", conf_dir) < 0) {
            LOG_ERRNO("failed to build fnott.ini path");
            goto done;
        }

        LOG_DBG("checking for %s", path);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            ret = (struct config_file){.path = path, .fd = fd};
            path = NULL;
            goto done;
        }
    }

done:
    free(xdg_config_dirs_copy);
    free(path);
    return ret;
}

static bool
str_to_bool(const char *s, bool *res)
{
    static const char *const yes[] = {"on", "true", "yes", "1"};
    static const char *const  no[] = {"off", "false", "no", "0"};

    for (size_t i = 0; i < sizeof(yes) / sizeof(yes[0]); i++) {
        if (strcasecmp(s, yes[i]) == 0) {
            *res = true;
            return true;
        }
    }

    for (size_t i = 0; i < sizeof(no) / sizeof(no[0]); i++) {
        if (strcasecmp(s, no[i]) == 0) {
            *res = false;
            return true;
        }
    }

    return false;
}

static bool
str_to_ulong(const char *s, int base, unsigned long *res)
{
    if (s == NULL)
        return false;

    errno = 0;
    char *end = NULL;

    *res = strtoul(s, &end, base);
    return errno == 0 && *end == '\0';
}

static inline pixman_color_t
color_hex_to_pixman_with_alpha(uint32_t color, uint16_t alpha)
{
    return (pixman_color_t){
        .red =   ((color >> 16 & 0xff) | (color >> 8 & 0xff00)) * alpha / 0xffff,
        .green = ((color >>  8 & 0xff) | (color >> 0 & 0xff00)) * alpha / 0xffff,
        .blue =  ((color >>  0 & 0xff) | (color << 8 & 0xff00)) * alpha / 0xffff,
        .alpha = alpha,
    };
}

static bool
str_to_color(const char *s, pixman_color_t *color, const char *path, int lineno)
{
    if (strlen(s) != 8) {
        LOG_ERR("%s:%d: %s: invalid RGBA color (not 8 digits)",
                path, lineno, s);
        return false;
    }

    unsigned long value;
    if (!str_to_ulong(s, 16, &value)) {
        LOG_ERRNO("%s:%d: invalid color: %s", path, lineno, s);
        return false;
    }

    uint32_t rgb = value >> 8;
    uint16_t alpha = value & 0xff; alpha |= alpha << 8;

    *color = color_hex_to_pixman_with_alpha(rgb, alpha);
    return true;
}

static bool
str_to_spawn_template(struct config *conf,
                      const char *s, struct config_spawn_template *template,
                      const char *path, int lineno)
{
    free(template->raw_cmd);
    free(template->argv);

    template->raw_cmd = NULL;
    template->argv = NULL;

    if (strlen(s) == 0)
        return true;

    char *raw_cmd = strdup(s);
    char **argv = NULL;

    if (!tokenize_cmdline(raw_cmd, &argv)) {
        LOG_ERR("%s:%d: syntax error in command line", path, lineno);
        return false;
    }

    template->raw_cmd = raw_cmd;
    template->argv = argv;
    return true;
}

static bool
config_font_parse(const char *pattern, struct config_font *font)
{
    FcPattern *pat = FcNameParse((const FcChar8 *)pattern);
    if (pat == NULL)
        return false;

    /*
     * First look for user specified {pixel}size option
     * e.g. “font-name:size=12”
     */

    double pt_size = -1.0;
    FcResult have_pt_size = FcPatternGetDouble(pat, FC_SIZE, 0, &pt_size);

    int px_size = -1;
    FcResult have_px_size = FcPatternGetInteger(pat, FC_PIXEL_SIZE, 0, &px_size);

    if (have_pt_size != FcResultMatch && have_px_size != FcResultMatch) {
        /*
         * Apply fontconfig config. Can’t do that until we’ve first
         * checked for a user provided size, since we may end up with
         * both “size” and “pixelsize” being set, and we don’t know
         * which one takes priority.
         */
        FcPattern *pat_copy = FcPatternDuplicate(pat);
        if (pat_copy == NULL ||
            !FcConfigSubstitute(NULL, pat_copy, FcMatchPattern))
        {
            LOG_WARN("%s: failed to do config substitution", pattern);
        } else {
            have_pt_size = FcPatternGetDouble(pat_copy, FC_SIZE, 0, &pt_size);
            have_px_size = FcPatternGetInteger(pat_copy, FC_PIXEL_SIZE, 0, &px_size);
        }

        FcPatternDestroy(pat_copy);

        if (have_pt_size != FcResultMatch && have_px_size != FcResultMatch)
            pt_size = 8.0;
    }

    FcPatternRemove(pat, FC_SIZE, 0);
    FcPatternRemove(pat, FC_PIXEL_SIZE, 0);

    char *stripped_pattern = (char *)FcNameUnparse(pat);
    FcPatternDestroy(pat);

    LOG_DBG("%s: pt-size=%.2f, px-size=%d", stripped_pattern, pt_size, px_size);

    *font = (struct config_font){
        .pattern = stripped_pattern,
        .pt_size = pt_size,
        .px_size = px_size
    };
    return true;
}

static bool
parse_section_urgency(const char *key, const char *value,
                      struct urgency_config *conf,
                      const char *path, unsigned lineno)
{
    if (strcmp(key, "layer") == 0) {
        enum zwlr_layer_shell_v1_layer layer = 0;

        if (strcasecmp(value, "background") == 0)
            layer = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
        else if (strcasecmp(value, "top") == 0)
            layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
        else if (strcasecmp(value, "bottom") == 0)
            layer = ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM;
        else if (strcasecmp(value, "overlay") == 0)
            layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
        else if (strcmp(key, "progress-style") == 0) {
            if (strcasecmp(value, "bar") == 0)
                conf->progress.style = PROGRESS_STYLE_BAR;
            else if (strcasecmp(value, "background") == 0)
                conf->progress.style = PROGRESS_STYLE_BACKGROUND;
            else {
                LOG_ERR("%s:%d: invalid progress style: %s", path, lineno, value);
                return false;
            }
        }
        else {
            LOG_ERR(
                "%s:%u: %s: invalid layer value, must be one of "
                "\"background\", "
                "\"bottom\", "
                "\"top\" or "
                "\"overlay\"",
                path, lineno, value);
            return false;
        }

        conf->layer = layer;
    }

    else if (strcmp(key, "background") == 0) {
        pixman_color_t bg;
        if (!str_to_color(value, &bg, path, lineno))
            return false;

        conf->bg = bg;
    }

    else if (strcmp(key, "border-color") == 0) {
        pixman_color_t color;
        if (!str_to_color(value, &color, path, lineno))
            return false;

        conf->border.color = color;
    }

    else if (strcmp(key, "border-radius") == 0) {
        unsigned long rad;
        if (!str_to_ulong(value, 10, &rad)) {
            LOG_ERR("%s:%u: invalid border-radius (expected an integer): %s",
                    path, lineno, value);
            return false;
        }
        conf->border.radius = rad;
    }

    else if (strcmp(key, "border-size") == 0) {
        unsigned long sz;
        if (!str_to_ulong(value, 10, &sz)) {
            LOG_ERR("%s:%u: invalid border-size (expected an integer): %s",
                    path, lineno, value);
            return false;
        }

        conf->border.size = sz;
    }

    else if (strcmp(key, "padding-vertical") == 0) {
        unsigned long p;
        if (!str_to_ulong(value, 10, &p)) {
            LOG_ERR("%s:%u: invalid padding-vertical (expected an integer): %s",
                    path, lineno, value);
            return false;
        }

        conf->padding.vertical = p;
    }

    else if (strcmp(key, "padding-horizontal") == 0) {
        unsigned long p;
        if (!str_to_ulong(value, 10, &p)) {
            LOG_ERR("%s:%u: invalid padding-horizontal (expected an integer): %s",
                    path, lineno, value);
            return false;
        }

        conf->padding.horizontal = p;
    }

    else if (strcmp(key, "title-font") == 0 ||
             strcmp(key, "summary-font") == 0 ||
             strcmp(key, "body-font") == 0 ||
             strcmp(key, "action-font") == 0)
    {
        struct config_font *font =
            strcmp(key, "title-font") == 0 ? &conf->app.font :
            strcmp(key, "summary-font") == 0 ? &conf->summary.font :
            strcmp(key, "body-font") == 0 ? &conf->body.font :
            strcmp(key, "action-font") == 0 ? &conf->action.font : NULL;

        assert(font != NULL);
        free(font->pattern);
        config_font_parse(value, font);
    }

    else if (strcmp(key, "title-color") == 0 ||
             strcmp(key, "summary-color") == 0 ||
             strcmp(key, "body-color") == 0 ||
             strcmp(key, "action-color") == 0)
    {
        pixman_color_t color;
        if (!str_to_color(value, &color, path, lineno))
            return false;

        pixman_color_t *c =
            strcmp(key, "title-color") == 0 ? &conf->app.color :
            strcmp(key, "summary-color") == 0 ? &conf->summary.color :
            strcmp(key, "body-color") == 0 ? &conf->body.color :
            strcmp(key, "action-color") == 0 ? &conf->action.color : NULL;

        assert(c != NULL);
        *c = color;
    }

    else if (strcmp(key, "title-format") == 0) {
        free(conf->app.format);
        conf->app.format = ambstoc32(value);
    }

    else if (strcmp(key, "summary-format") == 0) {
        free(conf->summary.format);
        conf->summary.format = ambstoc32(value);
    }

    else if (strcmp(key, "body-format") == 0) {
        free(conf-> body.format);
        conf->body.format = ambstoc32(value);
    }

    else if (strcmp(key, "progress-color") == 0 || strcmp(key, "progress-bar-color") == 0) {
        if (strcmp(key, "progress-bar-color") == 0) {
            LOG_WARN("%s:%d: 'progress-bar-color' is deprecated, use 'progress-color' instead",
                    path, lineno);
        }

        pixman_color_t color;
        if (!str_to_color(value, &color, path, lineno))
            return false;

        conf->progress.color = color;
    }

    else if (strcmp(key, "progress-bar-height") == 0) {
        unsigned long height;
        if (!str_to_ulong(value, 10, &height)) {
            LOG_ERR(
                "%s:%d: invalid progress-bar-height (expected an integer): %s",
                path, lineno, value);
            return false;
        }

        conf->progress.height = height;
    }

    else if (strcmp(key, "max-timeout") == 0) {
        unsigned long max_timeout_secs;
        if (!str_to_ulong(value, 10, &max_timeout_secs)) {
            LOG_ERR("%s:%d: invalid max-timeout (expected an integer): %s",
                    path, lineno, value);
            return false;
        }

        conf->max_timeout_secs = max_timeout_secs;
    }

    else if (strcmp(key, "default-timeout") == 0) {
        unsigned long default_timeout_secs;
        if (!str_to_ulong(value, 10, &default_timeout_secs)) {
            LOG_ERR("%s:%d: invalid default-timeout (expected an integer): %s", path,
                    lineno, value);
            return false;
        }
        conf->default_timeout_secs = default_timeout_secs;
    }

    else if (strcmp(key, "idle-timeout") == 0) {
        unsigned long idle_timeout_secs;
        if (!str_to_ulong(value, 10, &idle_timeout_secs)) {
            LOG_ERR("%s:%d: invalid idle-timeout (expected an integer): %s", path,
                    lineno, value);
            return false;
        }
        conf->idle_timeout_secs = idle_timeout_secs;
    }

    else if (strcmp(key, "sound-file") == 0) {
        free(conf->sound_file);
        conf->sound_file = strlen(value) > 0 ? strdup(value) : NULL;
    }

    else if (strcmp(key, "icon") == 0) {
        free(conf->icon);
        conf->icon = strlen(value) > 0 ? strdup(value) : NULL;
    }

    else {
        LOG_ERR("%s:%u: invalid key: %s", path, lineno, key);
        return false;
    }

    return true;
}

static bool
parse_section_low(const char *key, const char *value, struct config *conf,
                  const char *path, unsigned lineno)
{
    return parse_section_urgency(
        key, value, &conf->by_urgency[0], path, lineno);
}

static bool
parse_section_normal(const char *key, const char *value, struct config *conf,
                     const char *path, unsigned lineno)
{
    return parse_section_urgency(
        key, value, &conf->by_urgency[1], path, lineno);
}

static bool
parse_section_critical(const char *key, const char *value, struct config *conf,
                       const char *path, unsigned lineno)
{
    return parse_section_urgency(
        key, value, &conf->by_urgency[2], path, lineno);
}

static bool
parse_section_main(const char *key, const char *value, struct config *conf,
                   const char *path, unsigned lineno)
{
    if (strcmp(key, "output") == 0) {
        free(conf->output);
        conf->output = strdup(value);
    }

    else if (strcmp(key, "max-width") == 0) {
        unsigned long w;
        if (!str_to_ulong(value, 10, &w)) {
            LOG_ERR("%s:%u: invalid max-width (expected an integer): %s",
                    path, lineno, value);
            return false;
        }

        conf->max_width = w;
    }

    else if (strcmp(key, "min-width") == 0) {
        unsigned long w;
        if (!str_to_ulong(value, 10, &w)) {
            LOG_ERR("%s:%u: invalid min-width (expected an integer): %s",
                    path, lineno, value);
            return false;
        }

        conf->min_width = w;
    }

    else if (strcmp(key, "max-height") == 0) {
        unsigned long h;
        if (!str_to_ulong(value, 10, &h)) {
            LOG_ERR("%s:%u: invalid max-height (expected an integer): %s",
                    path, lineno, value);
            return false;
        }

        conf->max_height = h;
    }

    else if (strcmp(key, "dpi-aware") == 0) {
        bool enabled;
        if (!str_to_bool(value, &enabled)) {
            LOG_ERR("%s:%d: %s: invalid boolean value", path, lineno, value);
            return false;
        }

        conf->dpi_aware = enabled;
    }

    else if (strcmp(key, "icon-theme") == 0) {
        free(conf->icon_theme_name);
        conf->icon_theme_name = strdup(value);
    }

    else if (strcmp(key, "max-icon-size") == 0) {
        unsigned long sz;
        if (!str_to_ulong(value, 10, &sz)) {
            LOG_ERR("%s:%u: invalid max-height (expected an integer): %s",
                    path, lineno, value);
            return false;
        }

        conf->max_icon_size = sz;
    }

    else if (strcmp(key, "stacking-order") == 0) {
        if (strcasecmp(value, "bottom-up") == 0)
            conf->stacking_order = STACK_BOTTOM_UP;
        else if (strcasecmp(value, "top-down") == 0)
            conf->stacking_order = STACK_TOP_DOWN;
        else {
            LOG_ERR("%s:%u: %s: invalid stacking-order value, must be one of "
                    "\"bottom-up\", "
                    "\"top-down\"",
                    path, lineno, value);
            return false;
        }
    }


    else if (strcmp(key, "layer") == 0) {
        enum zwlr_layer_shell_v1_layer layer = 0;

        if (strcasecmp(value, "background") == 0)
            layer = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
        else if (strcasecmp(value, "top") == 0)
            layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
        else if (strcasecmp(value, "bottom") == 0)
            layer = ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM;
        else if (strcasecmp(value, "overlay") == 0)
            layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
        else {
            LOG_ERR(
                "%s:%u: %s: invalid layer value, must be one of "
                "\"background\", "
                "\"bottom\", "
                "\"top\" or "
                "\"overlay\"",
                path, lineno, value);
            return false;
        }

        for (int i = 0; i < 3; i++)
            conf->by_urgency[i].layer = layer;
    }

    else if (strcmp(key, "anchor") == 0) {
        if (strcasecmp(value, "top-left") == 0)
            conf->anchor = ANCHOR_TOP_LEFT;
        else if (strcasecmp(value, "top-right") == 0)
            conf->anchor = ANCHOR_TOP_RIGHT;
        else if (strcasecmp(value, "bottom-left") == 0)
            conf->anchor = ANCHOR_BOTTOM_LEFT;
        else if (strcasecmp(value, "bottom-right") == 0)
            conf->anchor = ANCHOR_BOTTOM_RIGHT;
        else if (strcasecmp(value, "center") == 0)
            conf->anchor = ANCHOR_CENTER;
        else {
            LOG_ERR(
                "%s:%u: %s: invalid anchor value, must be one of "
                "\"top-left\", "
                "\"top-right\", "
                "\"bottom-left\" or "
                "\"bottom-right\"",
                path, lineno, value);
            return false;
        }
    }

    else if (strcmp(key, "edge-margin-vertical") == 0) {
        unsigned long m;
        if (!str_to_ulong(value, 10, &m)) {
            LOG_ERR(
                "%s:%u: invalid edge-margin-vertical (expected an integer): %s",
                path, lineno, value);
            return false;
        }

        conf->margins.vertical = m;
    }

    else if (strcmp(key, "edge-margin-horizontal") == 0) {
        unsigned long m;
        if (!str_to_ulong(value, 10, &m)) {
            LOG_ERR(
                "%s:%u: invalid edge-margin-horizontal (expected an integer): %s",
                path, lineno, value);
            return false;
        }

        conf->margins.horizontal = m;
    }

    else if (strcmp(key, "notification-margin") == 0) {
        unsigned long m;
        if (!str_to_ulong(value, 10, &m)) {
            LOG_ERR(
                "%s:%u: invalid nofication-margin (expected an integer): %s",
                path, lineno, value);
            return false;
        }

        conf->margins.between = m;
    }

    else if (strcmp(key, "selection-helper") == 0) {
        free(conf->selection_helper);
        conf->selection_helper = strdup(value);
    }

    else if (strcmp(key, "selection-helper-uses-null-separator") == 0) {
        bool enabled;
        if (!str_to_bool(value, &enabled)) {
            LOG_ERR("%s:%d: %s: invalid boolean value", path, lineno, value);
            return false;
        }

        conf->selection_helper_uses_null_separator = enabled;
    }

    else if (strcmp(key, "play-sound") == 0) {
        if (!str_to_spawn_template(conf, value, &conf->play_sound, path, lineno))
            return false;
    }

    else if (strcmp(key, "scaling-filter") == 0) {
        enum scaling_filter filter;

        if (strcasecmp(value, "none") == 0)
            filter = SCALING_FILTER_NONE;
        else if (strcasecmp(value, "nearest") == 0)
            filter = SCALING_FILTER_NEAREST;
        else if (strcasecmp(value, "bilinear") == 0)
            filter = SCALING_FILTER_BILINEAR;
        else if (strcasecmp(value, "cubic") == 0)
            filter = SCALING_FILTER_CUBIC;
        else if (strcasecmp(value, "lanczos3") == 0)
            filter = SCALING_FILTER_LANCZOS3;
        else {
            LOG_ERR(
                "%s:%u: %s: invalid scaling-filter value, must be one of "
                "\"none\", "
                "\"nearest\", "
                "\"bilinear\", "
                "\"cubic\" or "
                "\"lanczos3\"",
                path, lineno, value);
            return false;
        }

        conf->scaling_filter = filter;
    }

    else if (strcmp(key, "background") == 0) {
        pixman_color_t bg;
        if (!str_to_color(value, &bg, path, lineno))
            return false;

        for (int i = 0; i < 3; i++)
            conf->by_urgency[i].bg = bg;
    }

    else if (strcmp(key, "border-color") == 0) {
        pixman_color_t color;
        if (!str_to_color(value, &color, path, lineno))
            return false;

        for (int i = 0; i < 3; i++)
            conf->by_urgency[i].border.color = color;
    }

    else if (strcmp(key, "border-radius") == 0) {
        unsigned long rad;
        if (!str_to_ulong(value, 10, &rad)) {
            LOG_ERR("%s:%u: invalid border-radius (expected an integer): %s",
                    path, lineno, value);
            return false;
        }

        for (int i = 0; i < 3; i++)
            conf->by_urgency[i].border.radius = rad;
    }

    else if (strcmp(key, "border-size") == 0) {
        unsigned long sz;
        if (!str_to_ulong(value, 10, &sz)) {
            LOG_ERR("%s:%u: invalid border-size (expected an integer): %s",
                    path, lineno, value);
            return false;
        }

        for (int i = 0; i < 3; i++)
            conf->by_urgency[i].border.size = sz;
    }

    else if (strcmp(key, "padding-vertical") == 0) {
        unsigned long p;
        if (!str_to_ulong(value, 10, &p)) {
            LOG_ERR("%s:%u: invalid padding-vertical (expected an integer): %s",
                    path, lineno, value);
            return false;
        }

        for (int i = 0; i < 3; i++)
            conf->by_urgency[i].padding.vertical = p;
    }

    else if (strcmp(key, "padding-horizontal") == 0) {
        unsigned long p;
        if (!str_to_ulong(value, 10, &p)) {
            LOG_ERR("%s:%u: invalid padding-horizontal (expected an integer): %s",
                    path, lineno, value);
            return false;
        }

        for (int i = 0; i < 3; i++)
            conf->by_urgency[i].padding.horizontal = p;
    }

    else if (strcmp(key, "title-font") == 0 ||
             strcmp(key, "summary-font") == 0 ||
             strcmp(key, "body-font") == 0 ||
             strcmp(key, "action-font") == 0)
    {
        for (int i = 0; i < 3; i++) {
            struct urgency_config *urgency = &conf->by_urgency[i];
            struct config_font *font =
                strcmp(key, "title-font") == 0 ? &urgency->app.font :
                strcmp(key, "summary-font") == 0 ? &urgency->summary.font :
                strcmp(key, "body-font") == 0 ? &urgency->body.font :
                strcmp(key, "action-font") == 0 ? &urgency->action.font : NULL;

            assert(font != NULL);
            free(font->pattern);
            config_font_parse(value, font);
        }
    }

    else if (strcmp(key, "title-color") == 0 ||
             strcmp(key, "summary-color") == 0 ||
             strcmp(key, "body-color") == 0 ||
             strcmp(key, "action-color") == 0)
    {
        pixman_color_t color;
        if (!str_to_color(value, &color, path, lineno))
            return false;

        for (int i = 0; i < 3; i++) {
            struct urgency_config *urgency = &conf->by_urgency[i];
            pixman_color_t *c =
                strcmp(key, "title-color") == 0 ? &urgency->app.color :
                strcmp(key, "summary-color") == 0 ? &urgency->summary.color :
                strcmp(key, "body-color") == 0 ? &urgency->body.color :
                strcmp(key, "action-color") == 0 ? &urgency->action.color : NULL;

            assert(c != NULL);
            *c = color;
        }
    }

    else if (strcmp(key, "title-format") == 0) {
        for (int i = 0; i < 3; i++) {
            struct urgency_config *urgency = &conf->by_urgency[i];

            free(urgency->app.format);
            urgency->app.format = ambstoc32(value);
        }
    }

    else if (strcmp(key, "summary-format") == 0) {
        for (int i = 0; i < 3; i++) {
            struct urgency_config *urgency = &conf->by_urgency[i];

            free(urgency->summary.format);
            urgency->summary.format = ambstoc32(value);
        }
    }

    else if (strcmp(key, "body-format") == 0) {
        for (int i = 0; i < 3; i++) {
            struct urgency_config *urgency = &conf->by_urgency[i];

            free(urgency->body.format);
            urgency->body.format = ambstoc32(value);
        }
    }

    else if (strcmp(key, "progress-color") == 0 || strcmp(key, "progress-bar-color") == 0) {
        if (strcmp(key, "progress-bar-color") == 0) {
            LOG_WARN("%s:%d: 'progress-bar-color' is deprecated, use 'progress-color' instead",
                    path, lineno);
        }

        pixman_color_t color;
        if (!str_to_color(value, &color, path, lineno))
            return false;

        for (int i = 0; i < 3; i++) {
            struct urgency_config *urgency = &conf->by_urgency[i];
            urgency->progress.color = color;
        }
    }

    else if (strcmp(key, "progress-bar-height") == 0) {
        unsigned long height;
        if (!str_to_ulong(value, 10, &height)) {
            LOG_ERR(
                "%s:%d: invalid progress-bar-height (expected an integer): %s",
                path, lineno, value);
            return false;
        }

        for (int i = 0; i < 3; i++) {
            struct urgency_config *urgency = &conf->by_urgency[i];
            urgency->progress.height = height;
        }
    }

    else if (strcmp(key, "progress-style") == 0) {
        enum progress_style style;
        if (strcasecmp(value, "bar") == 0)
            style = PROGRESS_STYLE_BAR;
        else if (strcasecmp(value, "background") == 0)
            style = PROGRESS_STYLE_BACKGROUND;
        else {
            LOG_ERR("%s:%d: invalid progress style: %s", path, lineno, value);
            return false;
        }
        
        for (int i = 0; i < 3; i++)
            conf->by_urgency[i].progress.style = style;
    }

    else if (strcmp(key, "max-timeout") == 0) {
        unsigned long max_timeout_secs;
        if (!str_to_ulong(value, 10, &max_timeout_secs)) {
            LOG_ERR("%s:%d: invalid max-timeout (expected an integer): %s",
                    path, lineno, value);
            return false;
        }

        for (int i = 0; i < 3; i++) {
            struct urgency_config *urgency = &conf->by_urgency[i];
            urgency->max_timeout_secs = max_timeout_secs;
        }
    }

    else if (strcmp(key, "default-timeout") == 0) {
        unsigned long default_timeout_secs;
        if (!str_to_ulong(value, 10, &default_timeout_secs)) {
             LOG_ERR("%s:%d: invalid default-timeout (expected an integer): %s", path,
                     lineno, value);
             return false;
        }

        for (int i = 0; i < 3; i++) {
            struct urgency_config *urgency = &conf->by_urgency[i];
            urgency->default_timeout_secs = default_timeout_secs;
        }
    }

    else if (strcmp(key, "idle-timeout") == 0) {
        unsigned long idle_timeout_secs;
        if (!str_to_ulong(value, 10, &idle_timeout_secs)) {
            LOG_ERR("%s:%d: invalid idle-timeout (expected an integer): %s", path,
                    lineno, value);
            return false;
        }
        for (int i = 0; i < 3; i++) {
            struct urgency_config *urgency = &conf->by_urgency[i];
            urgency->idle_timeout_secs = idle_timeout_secs;
        }
    }

    else if (strcmp(key, "sound-file") == 0) {
        for (int i = 0; i < 3; i++) {
            struct urgency_config *urgency = &conf->by_urgency[i];
            free(urgency->sound_file);
            urgency->sound_file = strlen(value) > 0 ? strdup(value) : NULL;
        }
    }

    else if (strcmp(key, "icon") == 0) {
        for (int i = 0; i < 3; i++) {
            struct urgency_config *urgency = &conf->by_urgency[i];
            free(urgency->icon);
            urgency->icon = strlen(value) > 0 ? strdup(value) : NULL;
        }
    }

    else {
        LOG_ERR("%s:%u: invalid key: %s", path, lineno, key);
        return false;
    }

    return true;
}

static bool
parse_config_file_section(FILE *f, struct config *conf, const char *path, enum section target_sec) {

    /* Function pointer, called for each key/value line */
    typedef bool (*parser_fun_t)(
        const char *key, const char *value, struct config *conf,
        const char *path, unsigned lineno);

    /* Maps sections to line parser functions */
    static const parser_fun_t section_parser_map[] = {
        [SECTION_MAIN] = &parse_section_main,
        [SECTION_LOW] = &parse_section_low,
        [SECTION_NORMAL] = &parse_section_normal,
        [SECTION_CRITICAL] = &parse_section_critical,
    };

    unsigned lineno = 0;
    char *_line = NULL;
    size_t count = 0;
    /* If target_sec is main we can parse as if we are inside the main section */
    bool inside_target_sec = target_sec == SECTION_MAIN;

    parser_fun_t section_parser = section_parser_map[target_sec];
    assert(section_parser != NULL);

    while (true) {
        errno = 0;
        lineno++;

        ssize_t ret = getline(&_line, &count, f);

        if (ret < 0) {
            if (errno != 0) {
                LOG_ERRNO("failed to read from configuration");
                goto err;
            }
            break;
        }

        /* Strip whitespace */
        char *line = _line;
        {
            while (isspace(*line))
                line++;
            if (line[0] != '\0') {
                char *end = line + strlen(line) - 1;
                while (isspace(*end))
                    end--;
                *(end + 1) = '\0';
            }
        }

        /* Empty line, or comment */
        if (line[0] == '\0' || line[0] == '#')
            continue;

        /* Split up into key/value pair + trailing comment */
        char *key_value = strtok(line, "#");
        char *comment __attribute__((unused)) = strtok(NULL, "\n");

        /* Check for new section */
        if (key_value[0] == '[') {
            char *end = strchr(key_value, ']');
            if (end == NULL) {
                LOG_ERR("%s:%d: syntax error: %s", path, lineno, key_value);
                goto err;
            }

            *end = '\0';

            bool invalid_sec = true;
            for (enum section s = SECTION_MAIN; s < SECTION_COUNT; ++s) {
                if (strcmp(&key_value[1], section_names[s]) == 0) {
                    invalid_sec = false;
                    if (s == target_sec) {
                        inside_target_sec = true;
                    }else {
                        inside_target_sec = false;
                    }
                };
            }

            if (invalid_sec) {
                LOG_ERR("%s:%d: invalid section name: %s", path, lineno, &key_value[1]);
                goto err;
            }

            continue;
        }
        if (!inside_target_sec) {
            continue;
        }

        char *key = strtok(key_value, "=");
        if (key == NULL) {
            LOG_ERR("%s:%d: syntax error: no key specified", path, lineno);
            goto err;
        }

        char *value = strtok(NULL, "\n");
        if (value == NULL) {
            /* Empty value, i.e. "key=" */
            value = key + strlen(key);
        }

        /* Strip trailing whitespace from key (leading stripped earlier) */
        {
            assert(!isspace(*key));

            char *end = key + strlen(key) - 1;
            while (isspace(*end))
                end--;
            *(end + 1) = '\0';
        }

        /* Strip leading whitespace from value (trailing stripped earlier) */
        {
            while (isspace(*value))
                value++;

            if (value[0] != '\0') {
                char *end = value + strlen(value) - 1;
                while (isspace(*end))
                    end--;
                *(end + 1) = '\0';
            }
        }

        LOG_DBG("section=%s, key='%s', value='%s'",
                section_names[section], key, value);

        if (!section_parser(key, value, conf, path, lineno))
            goto err;
    }

    free(_line);
    return true;

err:
    free(_line);
    return false;
}

static bool
parse_config_file(FILE *f, struct config *conf, const char *path)
{
    for (enum section sec=SECTION_MAIN; sec < SECTION_COUNT; sec++) {
        rewind(f);
        if (!parse_config_file_section(f, conf, path, sec)){
            return false;
        }
    }
    return true;
}

bool
config_load(struct config *conf, const char *path)
{
    const char *const default_font_name = "sans serif";

    *conf = (struct config){
        .output = NULL,
        .min_width = 0,
        .max_width = 0,
        .max_height = 0,
        .dpi_aware = false,
        .icon_theme_name = strdup("default"),
        .max_icon_size = 48,
        .stacking_order = STACK_BOTTOM_UP,
        .anchor = ANCHOR_TOP_RIGHT,
        .margins = {
            .vertical = 10,
            .horizontal = 10,
            .between = 10,
        },
        .by_urgency = {
            /* urgency == low */
            {
                .layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP,
                .bg = {0x2b2b, 0x2b2b, 0x2b2b, 0xffff},
                .border = {
                    .color = {0x9090, 0x9090, 0x9090, 0xffff},
                    .size = 1,
                },
                .padding = {
                    .vertical = 20,
                    .horizontal = 20,
                },
                .app = {
                    .color = {0x8888, 0x8888, 0x8888, 0xffff},
                    .format = c32dup(U"<i>%a%A</i>"),
                },
                .summary = {
                    .color = {0x8888, 0x8888, 0x8888, 0xffff},
                    .format = c32dup(U"<b>%s</b>\\n"),
                },
                .body = {
                    .color = {0x8888, 0x8888, 0x8888, 0xffff},
                    .format = c32dup(U"%b"),
                },
                .action = {
                    .color = {0x8888, 0x8888, 0x8888, 0xffff},
                },
                .progress = {
                    .height = 20,
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                    .style = PROGRESS_STYLE_BAR,
                },
                .max_timeout_secs = 0,
                .default_timeout_secs = 0,
                .idle_timeout_secs = 0,
                .icon = NULL,
            },
            {
                .layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP,
                .bg = {0x3f3f, 0x5f5f, 0x3f3f, 0xffff},
                .border = {
                    .color = {0x9090, 0x9090, 0x9090, 0xffff},
                    .size = 1,
                },
                .padding = {
                    .vertical = 20,
                    .horizontal = 20,
                },
                .app = {
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                    .format = c32dup(U"<i>%a%A</i>"),
                },
                .summary = {
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                    .format = c32dup(U"<b>%s</b>\\n"),
                },
                .body = {
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                    .format = c32dup(U"%b"),
                },
                .action = {
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
                .progress = {
                    .height = 20,
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
                .max_timeout_secs = 0,
                .default_timeout_secs = 0,
                .idle_timeout_secs = 0,
                .icon = NULL,
            },
            {
                .layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP,
                .bg = {0x6c6c, 0x3333, 0x3333, 0xffff},
                .border = {
                    .color = {0x9090, 0x9090, 0x9090, 0xffff},
                    .size = 1,
                },
                .padding = {
                    .vertical = 20,
                    .horizontal = 20,
                },
                .app = {
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                    .format = c32dup(U"<i>%a%A</i>"),
                },
                .summary = {
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                    .format = c32dup(U"<b>%s</b>\\n"),
                },
                .body = {
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                    .format = c32dup(U"%b"),
                },
                .action = {
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
                .progress = {
                    .height = 20,
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
                .max_timeout_secs = 0,
                .default_timeout_secs = 0,
                .idle_timeout_secs = 0,
                .icon = NULL,
            },
        },
        .selection_helper = strdup("dmenu"),
        .scaling_filter = SCALING_FILTER_LANCZOS3,
    };

    for (size_t i = 0; i < sizeof(conf->by_urgency) / sizeof(conf->by_urgency[0]); i++) {
        config_font_parse(default_font_name, &conf->by_urgency[i].app.font);
        config_font_parse(default_font_name, &conf->by_urgency[i].summary.font);
        config_font_parse(default_font_name, &conf->by_urgency[i].body.font);
        config_font_parse(default_font_name, &conf->by_urgency[i].action.font);
    }

    conf->play_sound.raw_cmd = strdup("aplay ${filename}");
    tokenize_cmdline(conf->play_sound.raw_cmd, &conf->play_sound.argv);

    bool ret = false;

    struct config_file conf_file = {.path = NULL, .fd = -1};
    if (path != NULL) {
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            LOG_ERRNO("%s: failed to open", path);
            goto out;
        }

        conf_file.path = strdup(path);
        conf_file.fd = fd;
    } else {
        conf_file = open_config();
        if (conf_file.fd < 0) {
            /* Default conf */
            LOG_WARN("no configuration found, using defaults");
            ret = true;
            goto out;
        }
    }

    assert(conf_file.path != NULL);
    assert(conf_file.fd >= 0);

    LOG_INFO("loading configuration from %s", conf_file.path);

    FILE *f = fdopen(conf_file.fd, "r");
    if (f == NULL) {
        LOG_ERR("%s: failed to open", conf_file.path);
        goto out;
    }

    ret = parse_config_file(f, conf, conf_file.path);
    fclose(f);

out:
    free(conf_file.path);
    return ret;
}

static void
free_spawn_template(struct config_spawn_template *template)
{
    free(template->raw_cmd);
    free(template->argv);
}

void
config_destroy(struct config conf)
{
    free(conf.output);
    free(conf.icon_theme_name);
    free(conf.selection_helper);
    free_spawn_template(&conf.play_sound);

    for (int i = 0; i < 3; i++) {
        struct urgency_config *uconf = &conf.by_urgency[i];
        free(uconf->app.font.pattern);
        free(uconf->app.format);
        free(uconf->summary.font.pattern);
        free(uconf->summary.format);
        free(uconf->body.font.pattern);
        free(uconf->body.format);
        free(uconf->action.font.pattern);
        free(uconf->sound_file);
        free(uconf->icon);
    }
}
