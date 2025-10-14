#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include "xmalloc.h"
#include "debug.h"
#include "xsnprintf.h"

static void *
check_alloc(void *alloc)
{
    if (unlikely(alloc == NULL)) {
        FATAL_ERROR(__func__, ENOMEM);
    }
    return alloc;
}

void *
xmalloc(size_t size)
{
    xassert(size != 0);
    return check_alloc(malloc(size));
}

void *
xcalloc(size_t nmemb, size_t size)
{
    xassert(size != 0);
    return check_alloc(calloc(likely(nmemb) ? nmemb : 1, size));
}

void *
xrealloc(void *ptr, size_t size)
{
    xassert(size != 0);
    return check_alloc(realloc(ptr, size));
}

void *
xreallocarray(void *ptr, size_t nmemb, size_t size)
{
    xassert(nmemb != 0 && size != 0);
    return check_alloc(reallocarray(ptr, nmemb, size));
}

char *
xstrdup(const char *str)
{
    return check_alloc(strdup(str));
}

char *
xstrndup(const char *str, size_t n)
{
    return check_alloc(strndup(str, n));
}

char32_t *
xc32dup(const char32_t *str)
{
    return check_alloc(c32dup(str));
}

static VPRINTF(1) XMALLOC char *
xvasprintf(const char *format, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, format, ap2);
    va_end(ap2);

    if (unlikely(n < 0 || n == INT_MAX)) {
        FATAL_ERROR(__func__, n < 0 ? errno : EOVERFLOW);
    }

    char *str = xmalloc(n + 1);
    size_t m = xvsnprintf(str, n + 1, format, ap);
    xassert(m == n);
    return str;
}

char *
xasprintf(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    char *str = xvasprintf(format, ap);
    va_end(ap);
    return str;
}
