#include "char32.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <wctype.h>
#include <wchar.h>

#if defined __has_include
 #if __has_include (<stdc-predef.h>)
   #include <stdc-predef.h>
 #endif
#endif

#define LOG_MODULE "char32"
#define LOG_ENABLE_DBG 0
#include "log.h"

/*
 * For now, assume we can map directly to the corresponding wchar_t
 * functions. This is true if:
 *
 *  - both data types have the same size
 *  - both use the same encoding (though we require that encoding to be UTF-32)
 */

_Static_assert(
    sizeof(wchar_t) == sizeof(char32_t), "wchar_t vs. char32_t size mismatch");

#if !defined(__STDC_UTF_32__) || !__STDC_UTF_32__
 #error "char32_t does not use UTF-32"
#endif
#if (!defined(__STDC_ISO_10646__) || !__STDC_ISO_10646__) && !defined(__FreeBSD__)
 #error "wchar_t does not use UTF-32"
#endif

size_t
c32len(const char32_t *s)
{
    return wcslen((const wchar_t *)s);
}

int
c32ncasecmp(const char32_t *s1, const char32_t *s2, size_t n)
{
    return wcsncasecmp((const wchar_t *)s1, (const wchar_t *)s2, n);
}

char32_t *
c32dup(const char32_t *s)
{
    return (char32_t *)wcsdup((const wchar_t *)s);
}

size_t
mbsntoc32(char32_t *dst, const char *src, size_t nms, size_t len)
{
    mbstate_t ps = {0};

    char32_t *out = dst;
    const char *in = src;

    size_t consumed = 0;
    size_t chars = 0;
    size_t rc;

    while ((out == NULL || chars < len) &&
           consumed < nms &&
           (rc = mbrtoc32(out, in, nms - consumed, &ps)) != 0)
    {
        switch (rc) {
        case 0:
            goto done;

        case (size_t)-1:
        case (size_t)-2:
        case (size_t)-3:
            goto err;
        }

        in += rc;
        consumed += rc;
        chars++;

        if (out != NULL)
            out++;
    }

done:
    return chars;

err:
    return (size_t)-1;
}

char32_t *
ambstoc32(const char *src)
{
    if (src == NULL)
        return NULL;

    const size_t src_len = strlen(src);

    char32_t *ret = malloc((src_len + 1) * sizeof(ret[0]));
    if (ret == NULL)
        return NULL;

    mbstate_t ps = {0};
    char32_t *out = ret;
    const char *in = src;
    const char *const end = src + src_len + 1;

    size_t chars = 0;
    size_t rc;

    while ((rc = mbrtoc32(out, in, end - in, &ps)) != 0) {
        switch (rc) {
        case (size_t)-1:
        case (size_t)-2:
        case (size_t)-3:
            goto err;
        }

        in += rc;
        out++;
        chars++;
    }

    *out = U'\0';

    ret = realloc(ret, (chars + 1) * sizeof(ret[0]));
    return ret;

err:
    free(ret);
    return NULL;
}

char *
ac32tombs(const char32_t *src)
{
    if (src == NULL)
        return NULL;

    const size_t src_len = c32len(src);

    size_t allocated = src_len + 1;
    char *ret = malloc(allocated);
    if (ret == NULL)
        return NULL;

    mbstate_t ps = {0};
    char *out = ret;
    const char32_t *const end = src + src_len + 1;

    size_t bytes = 0;

    char mb[MB_CUR_MAX];

    for (const char32_t *in = src; in < end; in++) {
        size_t rc = c32rtomb(mb, *in, &ps);

        switch (rc) {
        case (size_t)-1:
            goto err;
        }

        if (bytes + rc > allocated) {
            allocated *= 2;
            ret = realloc(ret, allocated);
            out = &ret[bytes];
        }

        for (size_t i = 0; i < rc; i++, out++)
            *out = mb[i];

        bytes += rc;
    }

    assert(ret[bytes - 1] == '\0');
    ret = realloc(ret, bytes);
    return ret;

err:
    free(ret);
    return NULL;
}

bool
isc32space(char32_t c32)
{
    return iswspace((wint_t)c32);
}
