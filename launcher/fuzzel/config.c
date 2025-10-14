#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

#define LOG_MODULE "config"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "key-binding.h"
#include "macros.h"
#include "xmalloc.h"
#include "xsnprintf.h"

const struct anchors_map anchors_map[] = {
    {"top-left", ANCHOR_TOP_LEFT},
    {"top", ANCHOR_TOP},
    {"top-right", ANCHOR_TOP_RIGHT},
    {"left", ANCHOR_LEFT},
    {"center", ANCHOR_CENTER},
    {"right", ANCHOR_RIGHT},
    {"bottom-left", ANCHOR_BOTTOM_LEFT},
    {"bottom", ANCHOR_BOTTOM},
    {"bottom-right", ANCHOR_BOTTOM_RIGHT},
    {NULL, 0},
};

static const char *const binding_action_map[] = {
    [BIND_ACTION_NONE] = NULL,
    [BIND_ACTION_CANCEL] = "cancel",
    [BIND_ACTION_CURSOR_HOME] = "cursor-home",
    [BIND_ACTION_CURSOR_END] = "cursor-end",
    [BIND_ACTION_CURSOR_LEFT] = "cursor-left",
    [BIND_ACTION_CURSOR_LEFT_WORD] = "cursor-left-word",
    [BIND_ACTION_CURSOR_RIGHT] = "cursor-right",
    [BIND_ACTION_CURSOR_RIGHT_WORD] = "cursor-right-word",
    [BIND_ACTION_DELETE_LINE] = "delete-line",
    [BIND_ACTION_DELETE_PREV] = "delete-prev",
    [BIND_ACTION_DELETE_PREV_WORD] = "delete-prev-word",
    [BIND_ACTION_DELETE_LINE_BACKWARD] = "delete-line-backward",
    [BIND_ACTION_DELETE_NEXT] = "delete-next",
    [BIND_ACTION_DELETE_NEXT_WORD] = "delete-next-word",
    [BIND_ACTION_DELETE_LINE_FORWARD] = "delete-line-forward",
    [BIND_ACTION_INSERT_SELECTED] = "insert-selected",
    [BIND_ACTION_EXPUNGE] = "expunge",
    [BIND_ACTION_CLIPBOARD_PASTE] = "clipboard-paste",
    [BIND_ACTION_PRIMARY_PASTE] = "primary-paste",
    [BIND_ACTION_MATCHES_EXECUTE] = "execute",
    [BIND_ACTION_MATCHES_EXECUTE_OR_NEXT] = "execute-or-next",
    [BIND_ACTION_MATCHES_EXECUTE_INPUT] = "execute-input",
    [BIND_ACTION_MATCHES_PREV] = "prev",
    [BIND_ACTION_MATCHES_PREV_WITH_WRAP] = "prev-with-wrap",
    [BIND_ACTION_MATCHES_PREV_PAGE] = "prev-page",
    [BIND_ACTION_MATCHES_NEXT] = "next",
    [BIND_ACTION_MATCHES_NEXT_WITH_WRAP] = "next-with-wrap",
    [BIND_ACTION_MATCHES_NEXT_PAGE] = "next-page",
    [BIND_ACTION_MATCHES_FIRST] = "first",
    [BIND_ACTION_MATCHES_LAST] = "last",

    [BIND_ACTION_CUSTOM_1] = "custom-1",
    [BIND_ACTION_CUSTOM_2] = "custom-2",
    [BIND_ACTION_CUSTOM_3] = "custom-3",
    [BIND_ACTION_CUSTOM_4] = "custom-4",
    [BIND_ACTION_CUSTOM_5] = "custom-5",
    [BIND_ACTION_CUSTOM_6] = "custom-6",
    [BIND_ACTION_CUSTOM_7] = "custom-7",
    [BIND_ACTION_CUSTOM_8] = "custom-8",
    [BIND_ACTION_CUSTOM_9] = "custom-9",
    [BIND_ACTION_CUSTOM_10] = "custom-10",
    [BIND_ACTION_CUSTOM_11] = "custom-11",
    [BIND_ACTION_CUSTOM_12] = "custom-12",
    [BIND_ACTION_CUSTOM_13] = "custom-13",
    [BIND_ACTION_CUSTOM_14] = "custom-14",
    [BIND_ACTION_CUSTOM_15] = "custom-15",
    [BIND_ACTION_CUSTOM_16] = "custom-16",
    [BIND_ACTION_CUSTOM_17] = "custom-17",
    [BIND_ACTION_CUSTOM_18] = "custom-18",
    [BIND_ACTION_CUSTOM_19] = "custom-19",

};

struct context {
    struct config *conf;
    const char *section;
    const char *key;
    const char *value;

    const char *path;
    unsigned lineno;

    bool errors_are_fatal;
};

#define LOG_CONTEXTUAL_ERR(...)                                         \
    log_contextual(ctx, LOG_CLASS_ERROR, __FILE__, __LINE__, __VA_ARGS__)

#define LOG_CONTEXTUAL_WARN(...) \
    log_contextual(ctx, LOG_CLASS_WARNING, __FILE__, __LINE__, __VA_ARGS__)

#define LOG_CONTEXTUAL_ERRNO(...) \
    log_contextual_errno(ctx, __FILE__, __LINE__, __VA_ARGS__)


static void PRINTF(5)
log_contextual(struct context *ctx, enum log_class log_class,
               const char *file, int lineno, const char *fmt, ...)
{
    char *formatted_msg = NULL;
    va_list va;
    va_start(va, fmt);
    int ret = vasprintf(&formatted_msg, fmt, va);
    va_end(va);

    if (unlikely(ret < 0))
        return;

    bool print_dot = ctx->key != NULL;
    bool print_colon = ctx->value != NULL;

    if (!print_dot)
        ctx->key = "";

    if (!print_colon)
        ctx->value = "";

    log_msg(
        log_class, LOG_MODULE, file, lineno, "%s:%d: [%s]%s%s%s%s: %s",
        ctx->path, ctx->lineno, ctx->section, print_dot ? "." : "",
        ctx->key, print_colon ? ": " : "", ctx->value, formatted_msg);
    free(formatted_msg);
}

static void PRINTF(4)
log_contextual_errno(struct context *ctx, const char *file, int lineno,
                     const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    char *formatted_msg;
    int ret = vasprintf(&formatted_msg, fmt, va);
    va_end(va);

    if (unlikely(ret < 0))
        return;

    bool print_dot = ctx->key != NULL;
    bool print_colon = ctx->value != NULL;

    if (!print_dot)
        ctx->key = "";

    if (!print_colon)
        ctx->value = "";

    log_errno(
        LOG_CLASS_ERROR, LOG_MODULE, file, lineno, "%s:%d: [%s]%s%s%s%s: %s",
        ctx->path, ctx->lineno, ctx->section, print_dot ? "." : "",
        ctx->key, print_colon ? ": " : "", ctx->value, formatted_msg);

    free(formatted_msg);
}

struct config_file {
    char *path;       /* Full, absolute, path */
    int fd;           /* FD of file, O_RDONLY */
};

static struct config_file
open_config(void)
{
    char *path = NULL;
    struct config_file ret = {.path = NULL, .fd = -1};

    const char *xdg_config_home = getenv("XDG_CONFIG_HOME");
    const char *xdg_config_dirs = getenv("XDG_CONFIG_DIRS");
    const char *home_dir = getenv("HOME");
    char *xdg_config_dirs_copy = NULL;

    /* First, check XDG_CONFIG_HOME (or .config, if unset) */
    if (xdg_config_home != NULL && xdg_config_home[0] == '/') {
        path = xstrjoin(xdg_config_home, "/fuzzel/fuzzel.ini");
    } else if (home_dir != NULL) {
        path = xstrjoin(home_dir, "/.config/fuzzel/fuzzel.ini");
    }

    if (path != NULL) {
        int fd = open(path, O_RDONLY | O_CLOEXEC);

        if (fd >= 0) {
            ret = (struct config_file) {.path = path, .fd = fd};
            path = NULL;
            goto done;
        }
    }

    xdg_config_dirs_copy = xdg_config_dirs != NULL && xdg_config_dirs[0] != '\0'
        ? xstrdup(xdg_config_dirs)
        : xstrdup("/etc/xdg");

    if (xdg_config_dirs_copy == NULL || xdg_config_dirs_copy[0] == '\0')
        goto done;

    for (const char *conf_dir = strtok(xdg_config_dirs_copy, ":");
         conf_dir != NULL;
         conf_dir = strtok(NULL, ":"))
    {
        free(path);
        path = xstrjoin(conf_dir, "/fuzzel/fuzzel.ini");
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

static void
free_key_binding_list(struct config_key_binding_list *bindings)
{
    free(bindings->arr);
    bindings->arr = NULL;
    bindings->count = 0;
}

static bool
parse_modifiers(struct context *ctx, const char *text, size_t len,
                struct config_key_modifiers *modifiers)
{
    bool ret = false;

    *modifiers = (struct config_key_modifiers){0};

    /* Handle "none" separately because e.g. none+shift is nonsense */
    if (strncmp(text, "none", len) == 0)
        return true;

    char *copy = xstrndup(text, len);

    for (char *tok_ctx = NULL, *key = strtok_r(copy, "+", &tok_ctx);
         key != NULL;
         key = strtok_r(NULL, "+", &tok_ctx))
    {
        if (strcmp(key, XKB_MOD_NAME_SHIFT) == 0)
            modifiers->shift = true;
        else if (strcmp(key, XKB_MOD_NAME_CTRL) == 0)
            modifiers->ctrl = true;
        else if (strcmp(key, XKB_MOD_NAME_ALT) == 0)
            modifiers->alt = true;
        else if (strcmp(key, XKB_MOD_NAME_LOGO) == 0)
            modifiers->super = true;
        else {
            LOG_CONTEXTUAL_ERR("not a valid modifier name: %s", key);
            goto out;
        }
    }

    ret = true;

out:
    free(copy);
    return ret;
}

static void
remove_from_key_bindings_list(struct config_key_binding_list *bindings,
                              int action)
{
    size_t remove_first_idx = 0;
    size_t remove_count = 0;

    for (size_t i = 0; i < bindings->count; i++) {
        struct config_key_binding *binding = &bindings->arr[i];

        if (binding->action != action)
            continue;

        if (remove_count++ == 0)
            remove_first_idx = i;

        assert(remove_first_idx + remove_count - 1 == i);
        //free_key_binding(binding);
    }

    if (remove_count == 0)
        return;

    size_t move_count = bindings->count - (remove_first_idx + remove_count);

    memmove(
        &bindings->arr[remove_first_idx],
        &bindings->arr[remove_first_idx + remove_count],
        move_count * sizeof(bindings->arr[0]));
    bindings->count -= remove_count;
}

static bool
value_to_key_combos(struct context *ctx, int action,
                    struct config_key_binding_list *bindings)
{
    if (strcasecmp(ctx->value, "none") == 0) {
        remove_from_key_bindings_list(bindings, action);
        return true;
    }

    /* Count number of combinations */
    size_t combo_count = 1;
    for (const char *p = strchr(ctx->value, ' ');
         p != NULL;
         p = strchr(p + 1, ' '))
    {
        combo_count++;
    }

    struct config_key_binding new_combos[combo_count];

    char *copy = xstrdup(ctx->value);
    size_t idx = 0;

    for (char *tok_ctx = NULL, *combo = strtok_r(copy, " ", &tok_ctx);
         combo != NULL;
         combo = strtok_r(NULL, " ", &tok_ctx),
             idx++)
    {
        struct config_key_binding *new_combo = &new_combos[idx];
        new_combo->action = action;
        new_combo->path = ctx->path;
        new_combo->lineno = ctx->lineno;

        char *key = strrchr(combo, '+');

        if (key == NULL) {
            /* No modifiers */
            key = combo;
            new_combo->modifiers = (struct config_key_modifiers){0};
        } else {
            *key = '\0';
            if (!parse_modifiers(ctx, combo, key - combo, &new_combo->modifiers))
                goto err;
            key++;  /* Skip past the '+' */
        }

        /* Translate key name to symbol */
        new_combo->k.sym = xkb_keysym_from_name(key, 0);
        if (new_combo->k.sym == XKB_KEY_NoSymbol) {
            LOG_CONTEXTUAL_ERR("not a valid XKB key name: %s", key);
            goto err;
        }
    }

    if (idx == 0) {
        LOG_CONTEXTUAL_ERR(
            "empty binding not allowed (set to 'none' to unmap)");
        goto err;
    }

    remove_from_key_bindings_list(bindings, action);

    bindings->arr = xreallocarray(
        bindings->arr,
        bindings->count + combo_count, sizeof(bindings->arr[0]));

    memcpy(&bindings->arr[bindings->count],
           new_combos,
           combo_count * sizeof(bindings->arr[0]));
    bindings->count += combo_count;

    free(copy);
    return true;

err:
    free(copy);
    return false;
}

static bool
modifiers_equal(const struct config_key_modifiers *mods1,
                const struct config_key_modifiers *mods2)
{
    bool shift = mods1->shift == mods2->shift;
    bool alt = mods1->alt == mods2->alt;
    bool ctrl = mods1->ctrl == mods2->ctrl;
    bool super = mods1->super == mods2->super;
    return shift && alt && ctrl && super;
}

static char *
modifiers_to_str(const struct config_key_modifiers *mods)
{
    return xasprintf(
        "%s%s%s%s",
        mods->ctrl ? XKB_MOD_NAME_CTRL "+" : "",
        mods->alt ? XKB_MOD_NAME_ALT "+": "",
        mods->super ? XKB_MOD_NAME_LOGO "+": "",
        mods->shift ? XKB_MOD_NAME_SHIFT "+": ""
    );
}

static bool
resolve_key_binding_collisions(struct config *conf, const char *section_name,
                               const char *const action_map[],
                               struct config_key_binding_list *bindings)
{
    bool ret = true;

    for (size_t i = 1; i < bindings->count; i++) {
        enum {
            COLLISION_NONE,
            COLLISION_BINDING,
        } collision_type = COLLISION_NONE;

        const struct config_key_binding *collision_binding = NULL;

        struct config_key_binding *binding1 = &bindings->arr[i];
        assert(binding1->action != BIND_ACTION_NONE);

        const struct config_key_modifiers *mods1 = &binding1->modifiers;

        /* Does our binding collide with another binding? */
        for (ssize_t j = i - 1;
             collision_type == COLLISION_NONE && j >= 0;
             j--)
        {
            const struct config_key_binding *binding2 = &bindings->arr[j];
            assert(binding2->action != BIND_ACTION_NONE);

            if (binding2->action == binding1->action)
                continue;

            const struct config_key_modifiers *mods2 = &binding2->modifiers;

            bool mods_equal = modifiers_equal(mods1, mods2);
            bool sym_equal;

            sym_equal = binding1->k.sym == binding2->k.sym;
            if (!mods_equal || !sym_equal)
                continue;

            collision_binding = binding2;
            collision_type = COLLISION_BINDING;
            break;
        }

        if (collision_type != COLLISION_NONE) {
            char *modifier_names = modifiers_to_str(mods1);
            char sym_name[64];
            xkb_keysym_get_name(binding1->k.sym, sym_name, sizeof(sym_name));

            switch (collision_type) {
            case COLLISION_NONE:
                break;

            case COLLISION_BINDING: {
                LOG_ERR(
                    "%s:%d: [%s].%s: %s%s already mapped to '%s'",
                    binding1->path, binding1->lineno, section_name,
                    action_map[binding1->action],
                    modifier_names, sym_name,
                    action_map[collision_binding->action]);
                ret = false;
                break;
            }
            }

            free(modifier_names);
            //free_key_binding(binding1);

            /* Remove the most recent binding */
            size_t move_count = bindings->count - (i + 1);
            memmove(&bindings->arr[i], &bindings->arr[i + 1],
                    move_count * sizeof(bindings->arr[0]));
            bindings->count--;

            i--;
        }
    }

    return ret;
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

static bool
str_to_uint32(const char *s, int base, uint32_t *res)
{
    unsigned long v;
    bool ret = str_to_ulong(s, base, &v);
    if (!ret)
        return false;
    if (v > UINT32_MAX)
        return false;
    *res = v;
    return true;
}

static bool
str_to_uint16(const char *s, int base, uint16_t *res)
{
    unsigned long v;
    bool ret = str_to_ulong(s, base, &v);
    if (!ret)
        return false;
    if (v > UINT16_MAX)
        return false;
    *res = v;
    return true;
}

static bool
value_to_str(struct context *ctx, char **res)
{
    free(*res);
    *res = xstrdup(ctx->value);
    return true;
}

static bool
value_to_wchars(struct context *ctx, char32_t **res)
{
    char32_t *s = ambstoc32(ctx->value);
    if (s == NULL) {
        LOG_CONTEXTUAL_ERR("not a valid string value");
        return false;
    }

    free(*res);
    *res = s;
    return true;
}

static bool
value_to_bool(struct context *ctx, bool *res)
{
    static const char *const yes[] = {"on", "true", "yes", "1"};
    static const char *const  no[] = {"off", "false", "no", "0"};

    for (size_t i = 0; i < ALEN(yes); i++) {
        if (strcasecmp(ctx->value, yes[i]) == 0) {
            *res = true;
            return true;
        }
    }

    for (size_t i = 0; i < ALEN(no); i++) {
        if (strcasecmp(ctx->value, no[i]) == 0) {
            *res = false;
            return true;
        }
    }

    LOG_CONTEXTUAL_ERR("invalid boolean value");
    return false;
}

static bool
value_to_uint32(struct context *ctx, int base, uint32_t *res)
{
    if (!str_to_uint32(ctx->value, base, res)){
        LOG_CONTEXTUAL_ERR(
            "invalid integer value, or outside range 0-%u", UINT32_MAX);
        return false;
    }
    return true;
}

static bool
value_to_uint16(struct context *ctx, int base, uint16_t *res)
{
    if (!str_to_uint16(ctx->value, base, res)){
        LOG_CONTEXTUAL_ERR(
            "invalid integer value, or outside range 0-%u", UINT32_MAX);
        return false;
    }
    return true;
}

static bool
value_to_color(struct context *ctx, bool allow_alpha, struct rgba *color)
{
    const char *clr_start = ctx->value;
    if (clr_start[0] == '#')
        clr_start++;

    if (strlen(clr_start) != 8) {
        LOG_CONTEXTUAL_ERR("not a valid color value");
        return false;
    }

    uint32_t v;
    if (!str_to_uint32(clr_start, 16, &v)) {
        LOG_CONTEXTUAL_ERR("not a valid color value");
        return false;
    }

    if (!allow_alpha && (v & 0x000000ff) != 0) {
        LOG_CONTEXTUAL_ERR("color value must not have an alpha component");
        return false;
    }

    *color = conf_hex_to_rgba(v);
    return true;
}

static bool
value_to_double(struct context *ctx, float *res)
{
    const char *s = ctx->value;

    if (s == NULL)
        return false;

    errno = 0;
    char *end = NULL;

    *res = strtof(s, &end);
    if (!(errno == 0 && *end == '\0')) {
        LOG_CONTEXTUAL_ERR("invalid decimal value");
        return false;
    }

    return true;
}

static bool
value_to_pt_or_px(struct context *ctx, struct pt_or_px *res)
{
    const char *s = ctx->value;

    size_t len = s != NULL ? strlen(s) : 0;
    if (len >= 2 && s[len - 2] == 'p' && s[len - 1] == 'x') {
        errno = 0;
        char *end = NULL;

        long value = strtol(s, &end, 10);
        if (!(errno == 0 && end == s + len - 2)) {
            LOG_CONTEXTUAL_ERR("invalid px value (must be on the form 12px)");
            return false;
        }
        res->pt = 0;
        res->px = value;
    } else {
        float value;
        if (!value_to_double(ctx, &value))
            return false;
        res->pt = value;
        res->px = 0;
    }

    return true;
}

static bool
value_to_enum(struct context *ctx, const char **value_map, int *res)
{
    size_t str_len = 0;
    size_t count = 0;

    for (; value_map[count] != NULL; count++) {
        if (strcasecmp(value_map[count], ctx->value) == 0) {
            *res = count;
            return true;
        }
        str_len += strlen(value_map[count]);
    }

    const size_t size = str_len + count * 4 + 1;
    char valid_values[512];
    size_t idx = 0;
    assert(size < sizeof(valid_values));

    for (size_t i = 0; i < count; i++)
        idx += xsnprintf(&valid_values[idx], size - idx, "'%s', ", value_map[i]);

    if (count > 0)
        valid_values[idx - 2] = '\0';

    LOG_CONTEXTUAL_ERR("not one of %s", valid_values);
    *res = -1;
    return false;
}


static bool parse_config_file(
    FILE *f, struct config *conf, const char *path, bool errors_are_fatal);

static bool
parse_section_main(struct context *ctx)
{
    struct config *conf = ctx->conf;
    const char *key = ctx->key;
    const char *value = ctx->value;
    bool errors_are_fatal = ctx->errors_are_fatal;

    if (strcmp(key, "include") == 0) {
        char *_include_path = NULL;
        const char *include_path = NULL;

        if (value[0] == '~' && value[1] == '/') {
            const char *home_dir = getenv("HOME");

            if (home_dir == NULL) {
                LOG_CONTEXTUAL_ERRNO("failed to expand '~'");
                return false;
            }

            _include_path = xstrjoin3(home_dir, "/", value + 2);
            include_path = _include_path;
        } else
            include_path = value;

        if (include_path[0] != '/') {
            LOG_CONTEXTUAL_ERR("not an absolute path");
            free(_include_path);
            return false;
        }

        FILE *include = fopen(include_path, "re");

        if (include == NULL) {
            LOG_CONTEXTUAL_ERRNO("failed to open");
            free(_include_path);
            return false;
        }

        bool ret = parse_config_file(
            include, conf, include_path, errors_are_fatal);
        fclose(include);

        LOG_INFO("imported sub-configuration from %s", include_path);
        free(_include_path);
        return ret;
    }

    else if (strcmp(key, "namespace") == 0)
        return value_to_str(ctx, &conf->namespace);

    else if (strcmp(key, "output") == 0)
        return value_to_str(ctx, &conf->output);

    else if (strcmp(key, "font") == 0)
        return value_to_str(ctx, &conf->font);

    else if (strcmp(key, "use-bold") == 0)
        return value_to_bool(ctx, &conf->use_bold);

    else if (strcmp(key, "dpi-aware") == 0) {
        if (strcmp(value, "auto") == 0)
            conf->dpi_aware = DPI_AWARE_AUTO;
        else {
            bool value;
            if (!value_to_bool(ctx, &value))
                return false;
            conf->dpi_aware = value ? DPI_AWARE_YES : DPI_AWARE_NO;
        }
        return true;
    }

    else if (strcmp(key, "gamma-correct-blending") == 0)
        return value_to_bool(ctx, &conf->gamma_correct);

    else if (strcmp(key, "render-workers") == 0)
        return value_to_uint16(ctx, 10, &conf->render_worker_count);

    else if (strcmp(key, "match-workers") == 0)
        return value_to_uint16(ctx, 10, &conf->match_worker_count);

    else if (strcmp(key, "prompt") == 0)
        return value_to_wchars(ctx, &conf->prompt);

    else if (strcmp(key, "placeholder") == 0)
        return value_to_wchars(ctx, &conf->placeholder);

    else if (strcmp(key, "icon-theme") == 0)
        return value_to_str(ctx, &conf->icon_theme);

    else if (strcmp(key, "icons-enabled") == 0)
        return value_to_bool(ctx, &conf->icons_enabled);

    else if (strcmp(key, "hide-before-typing") == 0)
        return value_to_bool(ctx, &conf->hide_when_prompt_empty);

    else if (strcmp(key, "list-executables-in-path") == 0)
        return value_to_bool(ctx, &conf->list_executables_in_path);

    else if (strcmp(key, "fields") == 0) {
        _Static_assert(sizeof(conf->match_fields) == sizeof(int),
            "enum is not 32-bit");

        enum match_fields match_fields = 0;

        char *copy = xstrdup(value);
        for (const char *field = strtok(copy, ",");
             field != NULL;
             field = strtok(NULL, ","))
        {
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

            enum match_fields field_value = 0;

            for (size_t i = 0; i < ALEN(map); i++) {
                if (strcmp(field, map[i].name) == 0) {
                    field_value = map[i].value;
                    break;
                }
            }

            if (field_value == 0) {
                LOG_CONTEXTUAL_ERR(
                    "invalid field name \"%s\", "
                    "must be one of: "
                    "\"filename\", \"name\", \"generic\", \"exec\", "
                    "\"categories\", \"keywords\", \"comment\"",
                    field);
                free(copy);
                return false;
            }

            match_fields |= field_value;
        }

        conf->match_fields = match_fields;
        free(copy);
        return true;
    }

    else if (strcmp(key, "password-character") == 0) {
        char32_t *password_chars = ambstoc32(value);
        if (password_chars == NULL) {
            LOG_CONTEXTUAL_ERR("invalid password character");
            return false;
        }

        if (c32len(password_chars) > 1) {
            LOG_CONTEXTUAL_ERR(
                "password character must be a single character, or empty");
            free(password_chars);
            return false;
        }

        conf->password_mode.character = password_chars[0];
        conf->password_mode.character_set = true;
        free(password_chars);
        return true;
    }

    else if (strcmp(key, "filter-desktop") == 0)
        return value_to_bool(ctx, &conf->filter_desktop);

    else if (strcmp(key, "match-mode") == 0) {
        _Static_assert(sizeof(conf->match_mode) == sizeof(int),
                       "enum is not 32-bit");

        return value_to_enum(
            ctx,
            (const char *[]){"exact", "fzf", "fuzzy", NULL},
            (int *)&conf->match_mode);
    }

    else if (strcmp(key, "sort-result") == 0)
        return value_to_bool(ctx, &conf->sort_result);

    else if (strcmp(key, "match-counter") == 0)
        return value_to_bool(ctx, &conf->match_counter);

    else if (strcmp(key, "delayed-filter-ms") == 0)
        return value_to_uint32(ctx, 10, &conf->delayed_filter_ms);

    else if (strcmp(key, "delayed-filter-limit") == 0)
        return value_to_uint32(ctx, 10, &conf->delayed_filter_limit);

    else if (strcmp(key, "show-actions") == 0)
        return value_to_bool(ctx, &conf->actions_enabled);

    else if (strcmp(key, "terminal") == 0)
        return value_to_str(ctx, &conf->terminal);

    else if (strcmp(key, "launch-prefix") == 0)
        return value_to_str(ctx, &conf->launch_prefix);

    else if (strcmp(key, "anchor") == 0) {
        enum anchors anchor;
        bool valid_anchor = false;

        for (size_t i = 0; anchors_map[i].name != NULL; i++) {
            if (strcmp(value, anchors_map[i].name) == 0) {
                anchor = anchors_map[i].value;
                valid_anchor = true;
                break;
            }
        }

        if (!valid_anchor) {
            LOG_CONTEXTUAL_ERR(
                "invalid anchor \"%s\", "
                "must be one of: "
                "\"center\", \"top-left\", \"top\", \"top-right\", "
                "\"right\", \"bottom-right\", \"bottom\""
                "\"bottom-left\", \"left\"",
                value);
            return false;
        }

        conf->anchor = anchor;
        return true;
    }
    else if (strcmp(key, "x-margin") == 0)
      return value_to_uint32(ctx, 10, &conf->margin.x);

    else if (strcmp(key, "y-margin") == 0)
      return value_to_uint32(ctx, 10, &conf->margin.y);

    else if (strcmp(key, "lines") == 0)
        return value_to_uint32(ctx, 10, &conf->lines);

    else if (strcmp(key, "minimal-lines") == 0)
        return value_to_bool(ctx, &conf->minimal_lines);

    else if (strcmp(key, "hide-prompt") == 0)
        return value_to_bool(ctx, &conf->hide_prompt);

    else if (strcmp(key, "width") == 0)
        return value_to_uint32(ctx, 10, &conf->chars);

    else if (strcmp(key, "tabs") == 0)
        return value_to_uint32(ctx, 10, &conf->tabs);

    else if (strcmp(key, "horizontal-pad") == 0)
        return value_to_uint32(ctx, 10, &conf->pad.x);

    else if (strcmp(key, "vertical-pad") == 0)
        return value_to_uint32(ctx, 10, &conf->pad.y);

    else if (strcmp(key, "inner-pad") == 0)
        return value_to_uint32(ctx, 10, &conf->pad.inner);

    else if (strcmp(key, "line-height") == 0)
        return value_to_pt_or_px(ctx, &conf->line_height);

    else if (strcmp(key, "letter-spacing") == 0)
        return value_to_pt_or_px(ctx, &conf->letter_spacing);

    else if (strcmp(key, "image-size-ratio") == 0) {
        float ratio;
        if (!value_to_double(ctx, &ratio))
            return false;

        if (ratio < 0. || ratio > 1.) {
            LOG_CONTEXTUAL_ERR("not in range 0.0 - 1.0");
            return false;
        }

        conf->image_size_ratio = ratio;
        return true;
    }

    else if (strcmp(key, "scaling-filter") == 0) {
        _Static_assert(sizeof(conf->png_scaling_filter) == sizeof(int),
            "enum is not 32-bit");

        return value_to_enum(
            ctx,
            (const char *[]){"none", "nearest", "bilinear", "box", "linear",
                             "cubic", "lanczos2", "lanczos3",
                             "lanczos3-stretched", NULL},
            (int *)&conf->png_scaling_filter);
    }

    else if (strcmp(key, "layer") == 0) {
        if (strcasecmp(value, "top") == 0) {
            conf->layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
            return true;
        } else if (strcasecmp(value, "overlay") == 0) {
            conf->layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
            return true;
        }

        LOG_CONTEXTUAL_ERR("not one of 'top', 'overlay'");
        return false;
    }

    else if (strcmp(key, "keyboard-focus") == 0) {
        if (strcasecmp(value, "exclusive") == 0) {
            conf->keyboard_focus = ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;
            return true;
        } else if (strcasecmp(value, "on-demand") == 0) {
            conf->keyboard_focus = ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND;
            return true;
        }
        LOG_CONTEXTUAL_ERR("not one of 'exclusive', 'on-demand'");
        return false;
    }

    else if (strcmp(key, "exit-on-keyboard-focus-loss") == 0)
        return value_to_bool(ctx, &conf->exit_on_kb_focus_loss);

    else if (strcmp(key, "cache") == 0)
        return value_to_str(ctx, &conf->cache_path);

    else if (strcmp(key, "auto-select") == 0)
        return value_to_bool(ctx, &conf->auto_select);

    else if (strcmp(key, "enable-mouse") == 0)
        return value_to_bool(ctx, &conf->enable_mouse);

    else
        LOG_CONTEXTUAL_ERR("not a valid option: %s", key);

    return false;
}

static bool
parse_section_colors(struct context *ctx)
{
    struct config *conf = ctx->conf;
    const char *key = ctx->key;

    if (strcmp(key, "background") == 0)
        return value_to_color(ctx, true, &conf->colors.background);

    else if (strcmp(key, "text") == 0)
        return value_to_color(ctx, true, &conf->colors.text);

    else if (strcmp(key, "prompt") == 0)
        return value_to_color(ctx, true, &conf->colors.prompt);

    else if (strcmp(key, "placeholder") == 0)
        return value_to_color(ctx, true, &conf->colors.placeholder);

    else if (strcmp(key, "input") == 0)
        return value_to_color(ctx, true, &conf->colors.input);

    else if (strcmp(key, "match") == 0)
        return value_to_color(ctx, true, &conf->colors.match);

    else if (strcmp(key, "selection") == 0)
        return value_to_color(ctx, true, &conf->colors.selection);

    else if (strcmp(key, "selection-text") == 0)
        return value_to_color(ctx, true, &conf->colors.selection_text);

    else if (strcmp(key, "selection-match") == 0)
        return value_to_color(ctx, true, &conf->colors.selection_match);

    else if (strcmp(key, "counter") == 0)
        return value_to_color(ctx, true, &conf->colors.counter);

    else if (strcmp(key, "border") == 0)
        return value_to_color(ctx, true, &conf->colors.border);

    else
        LOG_CONTEXTUAL_ERR("not a valid option: %s", key);

    return false;
}

static bool
parse_section_border(struct context *ctx)
{
    struct config *conf = ctx->conf;
    const char *key = ctx->key;

    if (strcmp(key, "width") == 0)
        return value_to_uint32(ctx, 10, &conf->border.size);

    else if (strcmp(key, "radius") == 0)
        return value_to_uint32(ctx, 10, &conf->border.radius);

    else if (strcmp(key, "selection-radius") == 0)
        return value_to_uint32(ctx, 10, &conf->selection_border.radius);

    else
        LOG_CONTEXTUAL_ERR("not a valid option: %s", key);

    return false;
}

static bool
parse_section_dmenu(struct context *ctx)
{
    struct config *conf = ctx->conf;
    const char *key = ctx->key;

    if (strcmp(key, "mode") == 0) {
        _Static_assert(sizeof(conf->dmenu.mode) == sizeof(int),
            "enum is not 32-bit");

        return value_to_enum(
            ctx,
            (const char *[]){"text", "index", NULL},
            (int *)&conf->dmenu.mode);
    }

    else if (strcmp(key, "exit-immediately-if-empty") == 0)
        return value_to_bool(ctx, &conf->dmenu.exit_immediately_if_empty);

    else
        LOG_CONTEXTUAL_ERR("not a valid option: %s", key);

    return false;
}

static bool
parse_section_key_bindings(struct context *ctx)
{
    for (int action = 0; action < ALEN(binding_action_map); action++) {
        if (binding_action_map[action] == NULL)
            continue;

        if (strcmp(ctx->key, binding_action_map[action]) != 0)
            continue;

        if (!value_to_key_combos(ctx, action, &ctx->conf->key_bindings))
            return false;

        return true;
    }

    LOG_CONTEXTUAL_ERR("not a valid action: %s", ctx->key);
    return false;
}

enum section {
    SECTION_MAIN,
    SECTION_COLORS,
    SECTION_BORDER,
    SECTION_DMENU,
    SECTION_KEY_BINDINGS,
    SECTION_COUNT,
};

/* Function pointer, called for each key/value line */
typedef bool (*parser_fun_t)(struct context *ctx);

static const struct {
    parser_fun_t fun;
    const char *name;
} section_info[] = {
    [SECTION_MAIN] =         {&parse_section_main, "main"},
    [SECTION_COLORS] =       {&parse_section_colors, "colors"},
    [SECTION_BORDER] =       {&parse_section_border, "border"},
    [SECTION_DMENU] =        {&parse_section_dmenu, "dmenu"},
    [SECTION_KEY_BINDINGS] = {&parse_section_key_bindings, "key-bindings"},
};

static enum section
str_to_section(const char *str)
{
    for (enum section section = SECTION_MAIN; section < SECTION_COUNT; ++section) {
        if (strcmp(str, section_info[section].name) == 0)
            return section;
    }
    return SECTION_COUNT;
}

static bool
parse_key_value(char *kv, const char **section, const char **key, const char **value)
{
    bool section_is_needed = section != NULL;

    /* Strip leading whitespace */
    while (isspace(kv[0]))
        ++kv;

    if (section_is_needed)
        *section = "main";

    if (kv[0] == '=')
        return false;

    *key = kv;
    *value = NULL;

    size_t kvlen = strlen(kv);

    /* Strip trailing whitespace */
    while (isspace(kv[kvlen - 1]))
        kvlen--;
    kv[kvlen] = '\0';

    for (size_t i = 0; i < kvlen; ++i) {
        if (kv[i] == '.' && section_is_needed) {
            section_is_needed = false;
            *section = kv;
            kv[i] = '\0';
            if (i == kvlen - 1 || kv[i + 1] == '=') {
                *key = NULL;
                return false;
            }
            *key = &kv[i + 1];
        } else if (kv[i] == '=') {
            kv[i] = '\0';
            if (i != kvlen - 1)
                *value = &kv[i + 1];
            break;
        }
    }

    if (*value == NULL)
        return false;

    /* Strip trailing whitespace from key (leading stripped earlier) */
    {
        assert(!isspace(*key[0]));

        char *end = (char *)*key + strlen(*key) - 1;
        while (isspace(end[0]))
            end--;
        end[1] = '\0';
    }

    /* Strip leading whitespace from value (trailing stripped earlier) */
    while (isspace(*value[0]))
        ++*value;

    /* Un-quote
     *
     * Note: this is very simple; we only support the *entire* value
     * being quoted. That is, no mid-value quotes. Both double and
     * single quotes are supported.
     *
     *  - key="value"              OK
     *  - key=abc "quote" def  NOT OK
     *  - key=’value’              OK
     *
     * Finally, we support escaping the quote character, and the
     * escape character itself:
     *
     *  - key="value \"quotes\""
     *  - key="backslash: \\"
     *
     * ONLY the "current" quote character can be escaped:
     *
     *  key="value \'"   NOt OK (both backslash and single quote is kept)
     */
    {
        char *end = (char *)*value + strlen(*value) - 1;
        if ((*value[0] == '"' && *end == '"') ||
            (*value[0] == '\'' && *end == '\''))
        {
            const char quote = (*value)[0];
            (*value)++;
            *end = '\0';

            /* Un-escape */
            for (char *p = (char *)*value; *p != '\0'; p++) {
                if (p[0] == '\\' && (p[1] == '\\' || p[1] == quote)) {
                    memmove(p, p + 1, end - p);
                }
            }
        }
    }

    return true;
}

static bool
parse_config_file(FILE *f, struct config *conf, const char *path,
                  bool errors_are_fatal)
{
    enum section section = SECTION_MAIN;

    char *_line = NULL;
    size_t count = 0;
    bool ret = true;

#define error_or_continue()                     \
    {                                           \
        if (errors_are_fatal) {                 \
            ret = false;                        \
            goto done;                          \
        } else                                  \
            continue;                           \
    }

    char *section_name = xstrdup("main");

    struct context context = {
        .conf = conf,
        .section = section_name,
        .path = path,
        .lineno = 0,
        .errors_are_fatal = errors_are_fatal,
    };
    struct context *ctx = &context;  /* For LOG_AND_*() */

    errno = 0;
    ssize_t len;

    while ((len = getline(&_line, &count, f)) != -1) {
        context.key = NULL;
        context.value = NULL;
        context.lineno++;

        char *line = _line;

        /* Strip leading whitespace */
        while (isspace(line[0])) {
            line++;
            len--;
        }

        /* Empty line, or comment */
        if (line[0] == '\0' || line[0] == '#')
            continue;

        /* Strip the trailing newline - may be absent on the last line */
        if (line[len - 1] == '\n')
            line[--len] = '\0';

        /* Split up into key/value pair + trailing comment separated by blank */
        char *key_value = line;
        char *kv_trailing = &line[len - 1];
        char *comment = &line[1];
        while (comment[1] != '\0') {
            if (isblank(comment[0]) && comment[1] == '#') {
                comment[1] = '\0'; /* Terminate key/value pair */
                kv_trailing = comment++;
                break;
            }
            comment++;
        }
        comment++;

        /* Strip trailing whitespace */
        while (isspace(kv_trailing[0]))
            kv_trailing--;
        kv_trailing[1] = '\0';

        /* Check for new section */
        if (key_value[0] == '[') {
            key_value++;

            if (key_value[0] == ']') {
                LOG_CONTEXTUAL_ERR("empty section name");
                section = SECTION_COUNT;
                error_or_continue();
            }

            char *end = strchr(key_value, ']');

            if (end == NULL) {
                context.section = key_value;
                LOG_CONTEXTUAL_ERR("syntax error: no closing ']'");
                context.section = section_name;
                section = SECTION_COUNT;
                error_or_continue();
            }

            end[0] = '\0';

            if (end[1] != '\0') {
                context.section = key_value;
                LOG_CONTEXTUAL_ERR("section declaration contains trailing "
                                   "characters");
                context.section = section_name;
                section = SECTION_COUNT;
                error_or_continue();
            }

            section = str_to_section(key_value);
            if (section == SECTION_COUNT) {
                context.section = key_value;
                LOG_CONTEXTUAL_ERR("invalid section name: %s", key_value);
                context.section = section_name;
                error_or_continue();
            }

            free(section_name);
            section_name = xstrdup(key_value);
            context.section = section_name;

            /* Process next line */
            continue;
        }

        if (section >= SECTION_COUNT) {
            /* Last section name was invalid; ignore all keys in it */
            continue;
        }

        if (!parse_key_value(key_value, NULL, &context.key, &context.value)) {
            LOG_CONTEXTUAL_ERR("syntax error: key/value pair has no %s",
                               context.key == NULL ? "key" : "value");
            error_or_continue();
        }

        LOG_DBG("section=%s, key='%s', value='%s', comment='%s'",
                section_info[section].name, context.key, context.value, comment);

        assert(section >= 0 && section < SECTION_COUNT);

        parser_fun_t section_parser = section_info[section].fun;
        assert(section_parser != NULL);

        if (!section_parser(ctx))
            error_or_continue();
    }

    if (errno != 0) {
        LOG_ERRNO("failed to read from configuration");
        if (errors_are_fatal)
            ret = false;
    }

done:
    free(section_name);
    free(_line);
    return ret;
}

static bool
overrides_apply(struct config *conf, const config_override_t *overrides,
                bool errors_are_fatal)
{
    if (overrides == NULL)
        goto resolve_key_bindings;

    struct context context = {
        .conf = conf,
        .path = "override",
        .lineno = 0,
        .errors_are_fatal = errors_are_fatal,
    };
    struct context *ctx = &context;

    tll_foreach(*overrides, it) {
        context.lineno++;

        if (!parse_key_value(
                it->item, &context.section, &context.key, &context.value))
        {
            LOG_CONTEXTUAL_ERR("syntax error: key/value pair has no %s",
                               context.key == NULL ? "key" : "value");
            if (errors_are_fatal)
                return false;
            continue;
        }

        if (context.section[0] == '\0') {
            LOG_CONTEXTUAL_ERR("empty section name");
            if (errors_are_fatal)
                return false;
            continue;
        }

        enum section section = str_to_section(context.section);
        if (section == SECTION_COUNT) {
            LOG_CONTEXTUAL_ERR("invalid section name: %s", context.section);
            if (errors_are_fatal)
                return false;
            continue;
        }
        parser_fun_t section_parser = section_info[section].fun;
        assert(section_parser != NULL);

        if (!section_parser(ctx)) {
            if (errors_are_fatal)
                return false;
            continue;
        }
    }

resolve_key_bindings:
    return resolve_key_binding_collisions(
        conf, section_info[SECTION_KEY_BINDINGS].name, binding_action_map,
        &conf->key_bindings);
}

#define m_none       {0}
#define m_alt        {.alt = true}
#define m_ctrl       {.ctrl = true}
#define m_shift      {.shift = true}
#define m_ctrl_shift {.ctrl = true, .shift = true}

static void
add_default_key_bindings(struct config *conf)
{
    static const struct config_key_binding bindings[] = {
        {BIND_ACTION_CANCEL, m_none, {{XKB_KEY_Escape}}},
        {BIND_ACTION_CANCEL, m_ctrl, {{XKB_KEY_g}}},
        {BIND_ACTION_CANCEL, m_ctrl, {{XKB_KEY_c}}},
        {BIND_ACTION_CANCEL, m_ctrl, {{XKB_KEY_bracketleft}}},

        {BIND_ACTION_CURSOR_HOME, m_none, {{XKB_KEY_Home}}},
        {BIND_ACTION_CURSOR_HOME, m_ctrl, {{XKB_KEY_a}}},

        {BIND_ACTION_CURSOR_END, m_none, {{XKB_KEY_End}}},
        {BIND_ACTION_CURSOR_END, m_ctrl, {{XKB_KEY_e}}},

        {BIND_ACTION_CURSOR_LEFT, m_ctrl, {{XKB_KEY_b}}},
        {BIND_ACTION_CURSOR_LEFT, m_none, {{XKB_KEY_Left}}},

        {BIND_ACTION_CURSOR_LEFT_WORD, m_alt, {{XKB_KEY_b}}},
        {BIND_ACTION_CURSOR_LEFT_WORD, m_ctrl, {{XKB_KEY_Left}}},

        {BIND_ACTION_CURSOR_RIGHT, m_ctrl, {{XKB_KEY_f}}},
        {BIND_ACTION_CURSOR_RIGHT, m_none, {{XKB_KEY_Right}}},

        {BIND_ACTION_CURSOR_RIGHT_WORD, m_alt, {{XKB_KEY_f}}},
        {BIND_ACTION_CURSOR_RIGHT_WORD, m_ctrl, {{XKB_KEY_Right}}},

        {BIND_ACTION_DELETE_LINE, m_ctrl_shift, {{XKB_KEY_BackSpace}}},

        {BIND_ACTION_DELETE_PREV, m_none, {{XKB_KEY_BackSpace}}},
        {BIND_ACTION_DELETE_PREV, m_ctrl, {{XKB_KEY_h}}},

        {BIND_ACTION_DELETE_PREV_WORD, m_ctrl, {{XKB_KEY_BackSpace}}},
        {BIND_ACTION_DELETE_PREV_WORD, m_ctrl, {{XKB_KEY_w}}},
        {BIND_ACTION_DELETE_PREV_WORD, m_alt, {{XKB_KEY_BackSpace}}},

        {BIND_ACTION_DELETE_NEXT, m_none, {{XKB_KEY_Delete}}},
        {BIND_ACTION_DELETE_NEXT, m_none, {{XKB_KEY_KP_Delete}}},
        {BIND_ACTION_DELETE_NEXT, m_ctrl, {{XKB_KEY_d}}},

        {BIND_ACTION_DELETE_NEXT_WORD, m_alt, {{XKB_KEY_d}}},
        {BIND_ACTION_DELETE_NEXT_WORD, m_ctrl, {{XKB_KEY_Delete}}},
        {BIND_ACTION_DELETE_NEXT_WORD, m_ctrl, {{XKB_KEY_KP_Delete}}},

        {BIND_ACTION_DELETE_LINE_BACKWARD, m_ctrl, {{XKB_KEY_u}}},
        {BIND_ACTION_DELETE_LINE_FORWARD, m_ctrl, {{XKB_KEY_k}}},

        {BIND_ACTION_INSERT_SELECTED, m_ctrl, {{XKB_KEY_Tab}}},

        {BIND_ACTION_EXPUNGE, m_shift, {{XKB_KEY_Delete}}},
        {BIND_ACTION_EXPUNGE, m_shift, {{XKB_KEY_KP_Delete}}},

        {BIND_ACTION_CLIPBOARD_PASTE, m_ctrl, {{XKB_KEY_v}}},
        {BIND_ACTION_CLIPBOARD_PASTE, m_none, {{XKB_KEY_XF86Paste}}},
        {BIND_ACTION_PRIMARY_PASTE, m_shift, {{XKB_KEY_Insert}}},
        {BIND_ACTION_PRIMARY_PASTE, m_shift, {{XKB_KEY_KP_Insert}}},

        {BIND_ACTION_MATCHES_EXECUTE, m_none, {{XKB_KEY_Return}}},
        {BIND_ACTION_MATCHES_EXECUTE, m_none, {{XKB_KEY_KP_Enter}}},
        {BIND_ACTION_MATCHES_EXECUTE, m_ctrl, {{XKB_KEY_y}}},

        {BIND_ACTION_MATCHES_EXECUTE_OR_NEXT, m_none, {{XKB_KEY_Tab}}},

        {BIND_ACTION_MATCHES_EXECUTE_INPUT, m_shift, {{XKB_KEY_Return}}},
        {BIND_ACTION_MATCHES_EXECUTE_INPUT, m_shift, {{XKB_KEY_KP_Enter}}},

        {BIND_ACTION_MATCHES_PREV, m_none, {{XKB_KEY_Up}}},
        {BIND_ACTION_MATCHES_PREV, m_ctrl, {{XKB_KEY_p}}},
        {BIND_ACTION_MATCHES_PREV_WITH_WRAP, m_none, {{XKB_KEY_ISO_Left_Tab}}},

        {BIND_ACTION_MATCHES_PREV_PAGE, m_none, {{XKB_KEY_Page_Up}}},
        {BIND_ACTION_MATCHES_PREV_PAGE, m_none, {{XKB_KEY_KP_Page_Up}}},

        {BIND_ACTION_MATCHES_NEXT, m_none, {{XKB_KEY_Down}}},
        {BIND_ACTION_MATCHES_NEXT, m_ctrl, {{XKB_KEY_n}}},

        {BIND_ACTION_MATCHES_NEXT_PAGE, m_none, {{XKB_KEY_Page_Down}}},
        {BIND_ACTION_MATCHES_NEXT_PAGE, m_none, {{XKB_KEY_KP_Page_Down}}},

        {BIND_ACTION_MATCHES_FIRST, m_ctrl, {{XKB_KEY_Home}}},
        {BIND_ACTION_MATCHES_LAST, m_ctrl, {{XKB_KEY_End}}},

        {BIND_ACTION_CUSTOM_1, m_alt, {{XKB_KEY_1}}},
        {BIND_ACTION_CUSTOM_2, m_alt, {{XKB_KEY_2}}},
        {BIND_ACTION_CUSTOM_3, m_alt, {{XKB_KEY_3}}},
        {BIND_ACTION_CUSTOM_4, m_alt, {{XKB_KEY_4}}},
        {BIND_ACTION_CUSTOM_5, m_alt, {{XKB_KEY_5}}},
        {BIND_ACTION_CUSTOM_6, m_alt, {{XKB_KEY_6}}},
        {BIND_ACTION_CUSTOM_7, m_alt, {{XKB_KEY_7}}},
        {BIND_ACTION_CUSTOM_8, m_alt, {{XKB_KEY_8}}},
        {BIND_ACTION_CUSTOM_9, m_alt, {{XKB_KEY_9}}},
        {BIND_ACTION_CUSTOM_10, m_alt, {{XKB_KEY_0}}},
        {BIND_ACTION_CUSTOM_11, m_alt, {{XKB_KEY_exclam}}},
        {BIND_ACTION_CUSTOM_12, m_alt, {{XKB_KEY_at}}},
        {BIND_ACTION_CUSTOM_13, m_alt, {{XKB_KEY_numbersign}}},
        {BIND_ACTION_CUSTOM_14, m_alt, {{XKB_KEY_dollar}}},
        {BIND_ACTION_CUSTOM_15, m_alt, {{XKB_KEY_percent}}},
        {BIND_ACTION_CUSTOM_16, m_alt, {{XKB_KEY_dead_circumflex}}},
        {BIND_ACTION_CUSTOM_17, m_alt, {{XKB_KEY_ampersand}}},
        {BIND_ACTION_CUSTOM_18, m_alt, {{XKB_KEY_asterisk}}},
        {BIND_ACTION_CUSTOM_19, m_alt, {{XKB_KEY_parenleft}}},
    };

    conf->key_bindings.count = ALEN(bindings);
    conf->key_bindings.arr = xmemdup(bindings, sizeof(bindings));
}

bool
config_load(struct config *conf, const char *conf_path,
            const config_override_t *overrides, bool errors_are_fatal)
{
    bool ret = !errors_are_fatal;
    *conf = (struct config) {
        .output = NULL,
        .namespace = xstrdup("launcher"),
        .prompt = xc32dup(U"> "),
        .placeholder = xc32dup(U""),
        .search_text = xc32dup(U""),
        .match_fields = MATCH_FILENAME | MATCH_NAME | MATCH_GENERIC,
        .password_mode = {
            .character = U'\0',
            .enabled = false,
        },
        .terminal = NULL,
        .launch_prefix = NULL,
        .font = xstrdup("monospace"),
        .use_bold = false,
        .dpi_aware = DPI_AWARE_AUTO,
        .gamma_correct = false,
        .render_worker_count = sysconf(_SC_NPROCESSORS_ONLN),
        .match_worker_count = sysconf(_SC_NPROCESSORS_ONLN),
        .filter_desktop = false,
        .icons_enabled = true,
        .icon_theme = xstrdup("default"),
        .hide_when_prompt_empty = false,
        .actions_enabled = false,
        .match_mode = MATCH_MODE_FZF,
        .sort_result = true,
        .match_counter = false,
        .delayed_filter_ms = 300,
        .delayed_filter_limit = 20000,
        .fuzzy = {
            .min_length = 3,
            .max_length_discrepancy = 2,
            .max_distance = 1,
        },
        .dmenu = {
            .enabled = false,
            .mode = DMENU_MODE_TEXT,
            .exit_immediately_if_empty = false,
            .delim = '\n',
            .nth_delim = '\t',
            .with_nth_format = NULL,
            .accept_nth_format = NULL,
        },
        .anchor = ANCHOR_CENTER,
        .margin = {
            .x = 0,
            .y = 0,
        },
        .lines = 15,
        .chars = 30,
        .tabs = 8,
        .pad = {
            .x = 40,
            .y = 8,
            .inner = 0,
        },
        .colors = {
            .background = conf_hex_to_rgba(0xfdf6e3ff),
            .border = conf_hex_to_rgba(0x002b36ff),
            .text = conf_hex_to_rgba(0x657b83ff),
            .prompt = conf_hex_to_rgba(0x586e75ff),
            .input = conf_hex_to_rgba(0x657b83ff),
            .match = conf_hex_to_rgba(0xcb4b16ff),
            .selection = conf_hex_to_rgba(0xeee8d5ff),
            .selection_text = conf_hex_to_rgba(0x586e75ff),
            .selection_match = conf_hex_to_rgba(0xcb4b16ff),
            .counter = conf_hex_to_rgba(0x93a1a1ff),
            .placeholder = conf_hex_to_rgba(0x93a1a1ff),
        },
        .border = {
            .size = 1u,
            .radius = 10u,
        },
        .selection_border = {
            .size = 0u,
            .radius = 0u,
        },
        .image_size_ratio = 0.5,
        .png_scaling_filter = SCALING_FILTER_BOX,
        .line_height = {-1, 0.0},
        .letter_spacing = {0},
        .layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
        .keyboard_focus = ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE,
        .exit_on_kb_focus_loss = true,
        .list_executables_in_path = false,
        .cache_path = NULL,
        .auto_select = false,
        .print_timing_info = false,
        .enable_mouse = true,
    };

    add_default_key_bindings(conf);

    struct config_file conf_file = {.path = NULL, .fd = -1};

    const char *terminal = getenv("TERMINAL");
    if (terminal != NULL) {
        conf->terminal = xstrjoin(terminal, " -e");
    }

    if (conf_path != NULL) {
        int fd = open(conf_path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            LOG_ERRNO("%s: failed to open", conf_path);
            goto out;
        }

        conf_file.path = xstrdup(conf_path);
        conf_file.fd = fd;
    } else {
        conf_file = open_config();
        if (conf_file.fd < 0) {
            LOG_WARN("no configuration found, using defaults");
            ret = true;
            goto out;
        }
    }

    LOG_INFO("loading configuration from %s", conf_file.path);

    FILE *f = fdopen(conf_file.fd, "r");
    if (f == NULL) {
        LOG_ERRNO("%s: failed to open", conf_file.path);
        goto out;
    }

    if (!parse_config_file(f, conf, conf_file.path, errors_are_fatal) ||
        !overrides_apply(conf, overrides, errors_are_fatal))
    {
        ret = !errors_are_fatal;
    } else
        ret = true;

    fclose(f);
    conf_file.fd = -1;

out:

    free(conf_file.path);
    if (conf_file.fd >= 0)
        close(conf_file.fd);

    return ret;
}

void
config_free(struct config *conf)
{
    free(conf->namespace);
    free(conf->output);
    free(conf->prompt);
    free(conf->placeholder);
    free(conf->search_text);
    free(conf->terminal);
    free(conf->launch_prefix);
    free(conf->font);
    free(conf->icon_theme);
    free(conf->cache_path);
    free(conf->dmenu.with_nth_format);
    free(conf->dmenu.accept_nth_format);
    free(conf->dmenu.match_nth_format);
    free_key_binding_list(&conf->key_bindings);
}

struct rgba
conf_hex_to_rgba(uint32_t color)
{
    return (struct rgba){
        .r = (double)((color >> 24) & 0xff) / 255.0,
        .g = (double)((color >> 16) & 0xff) / 255.0,
        .b = (double)((color >>  8) & 0xff) / 255.0,
        .a = (double)((color >>  0) & 0xff) / 255.0,
    };
}
