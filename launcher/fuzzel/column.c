#include "column.h"

#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <uchar.h>
#include <unistd.h>

#define LOG_MODULE "column"
#define LOG_ENABLE_DBG 1
#include "log.h"
#include "debug.h"
#include "xmalloc.h"

char32_t *
nth_column(const char32_t *string, char32_t delim, const char *nth_format)
{
    const char32_t *string_end = string + c32len(string);
    xassert(*string_end == U'\0');

    /* Count number of columns */
    size_t col_count = 1;
    for (const char32_t *next = c32chr(string, delim); next != NULL; next = c32chr(next + 1, delim))
        col_count++;

    /* Find the beginning, and length, of each column */
    const char32_t *col_starts[col_count];
    size_t col_lens[col_count];

    col_starts[0] = string;
    for (size_t i = 1; i < col_count; i++) {
        const char32_t *next = c32chr(col_starts[i - 1], delim);

        xassert(next != NULL);
        col_starts[i] = next + 1;
        col_lens[i - 1] = next - col_starts[i - 1];
    }

    col_lens[col_count - 1] = string_end - col_starts[col_count - 1];

    for (size_t i = 0; i < col_count; i++) {
        LOG_DBG("col #%zu: \"%.*ls\" (%zu chars)", i,
                (int)col_lens[i], (const wchar_t *)col_starts[i], col_lens[i]);
    }

    /* TODO: allocate dynamically */
    char32_t *result = NULL;
    size_t res_chars = 0;
    size_t res_idx = 0;

    /* Expand the 'nth' template, replacing '{1}' with column[0] etc */
    const char *copy_start = nth_format;
    const char *p = nth_format;
    while (true) {
        const char *arg_start = strchr(p, U'{');
        if (arg_start == NULL) {
            const size_t copy_len = mbstoc32(NULL, copy_start, 0);
            if (copy_len == (size_t)-1) {
                result[res_idx] = U'\0';
                break;
            }

            const size_t req_res_len = res_idx + copy_len;

            if (req_res_len > res_chars) {
                res_chars = req_res_len + 1;
                result = xrealloc(result, res_chars * sizeof(result[0]));
            }

            mbstoc32(&result[res_idx], copy_start, res_chars - res_idx);
            res_idx += copy_len;

            xassert(res_idx <= res_chars);

            result[res_idx] = U'\0';
            break;
        }

        const char *arg_end = strchr(arg_start, U'}');
        if (arg_end == NULL) {
            /* Unclosed '{' */
            p++;
            continue;
        }

        unsigned int idx_start = 0;
        unsigned int idx_end = 0;
        int dots = 0;

        for (const char *d = arg_start + 1; d != arg_end; d++) {
            if (*d == '.') {
                if (idx_start == 0 || dots >= 2) {
                    idx_start = 0;
                    break;
                }

                dots++;
                xassert(dots >= 1 && dots <= 3);
            } else if (*d >= '0' && *d <= '9') {
                if (dots == 0) {
                    idx_start *= 10;
                    idx_start += *d - '0';
                } else if (dots == 2) {
                    idx_end *= 10;
                    idx_end += *d - '0';
                } else {
                    idx_start = 0;
                    break;
                }
            } else {
                /* Invalid index */
                idx_start = 0;
                break;
            }
        }

        if (idx_end == 0) {
            if (dots == 0)
                idx_end = idx_start;
            else if (dots == 2) {
                /* Open ended - append all remaining columns */
                idx_end = col_count;
            } else
                idx_start = 0;
        }

        if (idx_start == 0 || idx_start > col_count ||
            idx_end > col_count || idx_end < idx_start)
        {
            /* Invalid index */
            p++;
            continue;
        }

        xassert(idx_end >= idx_start);

        /* Non-template prefix length */
        const size_t copy_len = arg_start - copy_start;
        const size_t copy_len_wide_chars = mbsntoc32(NULL, copy_start, copy_len, 0);

        if (copy_len_wide_chars == (size_t)-1) {
            result[res_idx] = U'\0';
            break;
        }

        /* Column(s) length */
        size_t total_col_len = 0;
        for (unsigned int i = idx_start; i <= idx_end; i++)
            total_col_len += col_lens[i - 1];

        if (idx_end > idx_start) {
            /* Space-separated columns */
            total_col_len += idx_end - idx_start;
        }

        const size_t req_res_len =
            res_idx + copy_len_wide_chars + total_col_len + 1;

        if (req_res_len > res_chars) {
            res_chars = req_res_len * 2;
            result = xrealloc(result, res_chars * sizeof(result[0]));
        }

        mbsntoc32(&result[res_idx], copy_start, copy_len, res_chars - res_idx);
        res_idx += copy_len_wide_chars;
        xassert(res_idx <= res_chars);

        for (unsigned int i = idx_start; i <= idx_end; i++) {
            c32memcpy(&result[res_idx], col_starts[i - 1], col_lens[i - 1]);
            res_idx += col_lens[i - 1];
            xassert(res_idx <= res_chars);

            if (i != idx_end) {
                result[res_idx++] = U' ';
                xassert(res_idx <= res_chars);
            }
        }

        result[res_idx] = U'\0';
        copy_start = p = arg_end + 1;
    }

    return result;
}
