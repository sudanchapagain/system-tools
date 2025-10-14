#pragma once

#include <stdbool.h>
#include <uchar.h>
#include <stddef.h>
#include <stdarg.h>

size_t c32len(const char32_t *s);
char32_t *c32dup(const char32_t *s);
int c32ncasecmp(const char32_t *s1, const char32_t *s2, size_t n);

size_t mbsntoc32(char32_t *dst, const char *src, size_t nms, size_t len);
char32_t *ambstoc32(const char *src);
char *ac32tombs(const char32_t *src);

bool isc32space(char32_t c32);
