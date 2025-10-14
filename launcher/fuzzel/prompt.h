#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <uchar.h>

struct prompt;
struct wayland;
struct prompt *prompt_init(
    const char32_t *prompt_text, const char32_t *placeholder,
    const char32_t *text);
void prompt_destroy(struct prompt *prompt);

void prompt_insert_chars(struct prompt *prompt, const char *text, size_t len);

const char32_t *prompt_prompt(const struct prompt *prompt);
const char32_t *prompt_placeholder(const struct prompt *prompt);
const char32_t *prompt_text(const struct prompt *prompt);
size_t prompt_cursor(const struct prompt *prompt);

bool prompt_cursor_home(struct prompt *prompt);
bool prompt_cursor_end(struct prompt *prompt);
bool prompt_cursor_next_char(struct prompt *prompt);
bool prompt_cursor_prev_char(struct prompt *prompt);
bool prompt_cursor_prev_word(struct prompt *prompt);
bool prompt_cursor_next_word(struct prompt *prompt);

bool prompt_erase_all(struct prompt *prompt);
bool prompt_erase_next_char(struct prompt *prompt);
bool prompt_erase_prev_char(struct prompt *prompt);
bool prompt_erase_next_word(struct prompt *prompt);
bool prompt_erase_prev_word(struct prompt *prompt);
bool prompt_erase_after_cursor(struct prompt *prompt);
bool prompt_erase_before_cursor(struct prompt *prompt);
