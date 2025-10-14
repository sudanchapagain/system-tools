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
#include "xmalloc.h"

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
c32cmp(const char32_t *s1, const char32_t *s2)
{
    return wcscmp((const wchar_t *)s1, (const wchar_t *)s2);
}

int
c32casecmp(const char32_t *s1, const char32_t *s2)
{
    return wcscasecmp((const wchar_t *)s1, (const wchar_t *)s2);
}

char32_t *
c32cpy(char32_t *dest, const char32_t *src)
{
    return (char32_t *)wcscpy((wchar_t *)dest, (const wchar_t *)src);
}

char32_t *
c32memcpy(char32_t *dest, const char32_t *src, size_t n)
{
    return (char32_t *)wmemcpy((wchar_t *)dest, (const wchar_t *)src, n);
}

char32_t *
c32cat(char32_t *dest, const char32_t *src)
{
    return (char32_t *)wcscat((wchar_t *)dest, (const wchar_t *)src);
}

char32_t *
c32dup(const char32_t *s)
{
    return (char32_t *)wcsdup((const wchar_t *)s);
}

char32_t *
c32chr(const char32_t *s, char32_t c)
{
    return (char32_t *)wcschr((const wchar_t *)s, (wchar_t)c);
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

size_t
mbstoc32(char32_t *dst, const char *src, size_t len)
{
    return mbsntoc32(dst, src, strlen(src) + 1, len);
}

size_t
c32ntombs(char *dst, const char32_t *src, size_t nwc, size_t len)
{
    mbstate_t ps = {0};

    char *out = dst;
    const char32_t *in = src;

    size_t consumed = 0;
    size_t bytes = 0;
    size_t rc;

    char mb[MB_CUR_MAX];

    while ((out == NULL || bytes < len) &&
           consumed < nwc &&
           (rc = c32rtomb(mb, *in, &ps)) != 0)
    {
        switch (rc) {
        case 0:
            goto done;

        case (size_t)-1:
            goto err;
        }

        if (out != NULL) {
            for (size_t i = 0; i < rc; i++, out++)
                *out = mb[i];
        }

        if (*in == U'\0')
            break;

        in++;
        consumed++;
        bytes += rc;
    }

done:
    return bytes;

err:
    return (size_t)-1;
}

size_t
c32tombs(char *dst, const char32_t *src, size_t len)
{
    return c32ntombs(dst, src, c32len(src) + 1, len);
}

char32_t *
ambstoc32(const char *src)
{
    if (src == NULL)
        return NULL;

    const size_t src_len = strlen(src);

    char32_t *ret = xmalloc((src_len + 1) * sizeof(ret[0]));

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

    return xreallocarray(ret, chars + 1, sizeof(ret[0]));

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
    char *ret = xmalloc(allocated);

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
            ret = xrealloc(ret, allocated);
            out = &ret[bytes];
        }

        for (size_t i = 0; i < rc; i++, out++)
            *out = mb[i];

        bytes += rc;
    }

    assert(ret[bytes - 1] == '\0');
    return xrealloc(ret, bytes);

err:
    free(ret);
    return NULL;
}

char32_t
toc32lower(char32_t c)
{
    return (char32_t)towlower((wint_t)c);
}

char32_t
toc32upper(char32_t c)
{
    return (char32_t)towupper((wint_t)c);
}

bool
isc32space(char32_t c32)
{
    return iswspace((wint_t)c32);
}
