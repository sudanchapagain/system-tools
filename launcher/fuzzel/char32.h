#pragma once

#include <stdbool.h>
#include <uchar.h>
#include <stddef.h>
#include <stdarg.h>
#include "macros.h"

size_t c32len(const char32_t *s) PURE;
int c32cmp(const char32_t *s1, const char32_t *s2);
int c32casecmp(const char32_t *s1, const char32_t *s2);
char32_t *c32cpy(char32_t *dest, const char32_t *src);
char32_t *c32memcpy(char32_t *dest, const char32_t *src, size_t n);
char32_t *c32cat(char32_t *dest, const char32_t *src);
char32_t *c32dup(const char32_t *s);
char32_t *c32chr(const char32_t *s, char32_t c);

size_t mbsntoc32(char32_t *dst, const char *src, size_t nms, size_t len);
size_t mbstoc32(char32_t *dst, const char *src, size_t len);
size_t c32ntombs(char *dst, const char32_t *src, size_t nwc, size_t len);
size_t c32tombs(char *dst, const char32_t *src, size_t len);
char32_t *ambstoc32(const char *src);
char *ac32tombs(const char32_t *src);

char32_t toc32lower(char32_t c) CONST_FN;
char32_t toc32upper(char32_t c) CONST_FN;

bool isc32space(char32_t c32) CONST_FN;
