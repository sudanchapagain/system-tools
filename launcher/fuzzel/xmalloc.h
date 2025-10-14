#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <wchar.h>
#include <uchar.h>

#include "char32.h"
#include "macros.h"

void *xmalloc(size_t size) XMALLOC;
void *xcalloc(size_t nmemb, size_t size) XMALLOC;
void *xrealloc(void *ptr, size_t size);
void *xreallocarray(void *ptr, size_t nmemb, size_t size) RETURNS_NONNULL WARN_UNUSED_RESULT;
char *xstrdup(const char *str) XSTRDUP;
char *xstrndup(const char *str, size_t n) XSTRDUP;
char32_t *xc32dup(const char32_t *str) XSTRDUP;
char *xasprintf(const char *format, ...) PRINTF(1) XMALLOC;

static inline void *
xmemdup(const void *ptr, size_t size)
{
    return memcpy(xmalloc(size), ptr, size);
}

static inline char *
xstrjoin(const char *s1, const char *s2)
{
    size_t n1 = strlen(s1);
    size_t n2 = strlen(s2);
    char *joined = xmalloc(n1 + n2 + 1);
    memcpy(joined, s1, n1);
    memcpy(joined + n1, s2, n2 + 1);
    return joined;
}

static inline char *
xstrjoin3(const char *s1, const char *s2, const char *s3)
{
    size_t n1 = strlen(s1);
    size_t n2 = strlen(s2);
    size_t n3 = strlen(s3);
    char *joined = xmalloc(n1 + n2 + n3 + 1);
    memcpy(joined, s1, n1);
    memcpy(joined + n1, s2, n2);
    memcpy(joined + n1 + n2, s3, n3 + 1);
    return joined;
}

static inline char32_t *
xc32join3(const char32_t *s1, const char32_t *s2, const char32_t *s3)
{
    size_t n1 = c32len(s1);
    size_t n2 = c32len(s2);
    size_t n3 = c32len(s3);
    char32_t *joined = xmalloc((n1 + n2 + n3 + 1) * sizeof(char32_t));
    c32memcpy(joined, s1, n1);
    c32memcpy(joined + n1, s2, n2);
    c32memcpy(joined + n1 + n2, s3, n3 + 1);
    return joined;
}
