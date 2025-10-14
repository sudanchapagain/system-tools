#include "timing.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define LOG_MODULE "timing"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "xsnprintf.h"

static struct timespec boot_up = {0};
static bool enabled = false;

void
time_init(void)
{
    clock_gettime(CLOCK_MONOTONIC, &boot_up);
}

void
time_enable(void)
{
    enabled = true;
}

static void
timespec_sub(const struct timespec *a, const struct timespec *b,
             struct timespec *res)
{
    const long one_sec_in_ns = 1000000000;

    res->tv_sec = a->tv_sec - b->tv_sec;
    res->tv_nsec = a->tv_nsec - b->tv_nsec;

    /* tv_nsec may be negative */
    if (res->tv_nsec < 0) {
        res->tv_sec--;
        res->tv_nsec += one_sec_in_ns;
    }
}

void
time_since_boot(const char *fmt, ...)
{
    if (!enabled)
        return;

    struct timespec stop;
    clock_gettime(CLOCK_MONOTONIC, &stop);

    struct timespec diff;
    timespec_sub(&stop, &boot_up, &diff);

    va_list va1, va2;
    va_start(va1, fmt);
    va_copy(va2, va1);

    int len = vsnprintf(NULL, 0, fmt, va1);
    va_end(va1);

    char msg[len + 1];
    xvsnprintf(msg, len + 1, fmt, va2);
    va_end(va2);


    LOG_WARN("%s: %llds %lldµs since boot-up", msg,
             (unsigned long long)diff.tv_sec,
             (unsigned long long)diff.tv_nsec / 1000);
}

static struct timespec *
time_stamp(void)
{
    if (!enabled)
        return NULL;

    struct timespec *ret = malloc(sizeof(*ret));
    if (ret == NULL)
        return NULL;

    clock_gettime(CLOCK_MONOTONIC, ret);
    return ret;
}

struct timespec *
time_begin(void)
{
    return time_stamp();
}

struct timespec *
time_end(void)
{
    return time_stamp();
}

void
time_finish(struct timespec *start, struct timespec *stop, const char *fmt, ...)
{
    if (!enabled) {
        free(start);
        free(stop);
        return;
    }

    if (start == NULL)
        return;

    if (stop == NULL) {
        stop = time_stamp();

        if (stop == NULL) {
            free(start);
            return;
        }
    }

    struct timespec diff;
    timespec_sub(stop, start, &diff);

    va_list va1, va2;
    va_start(va1, fmt);
    va_copy(va2, va1);

    int len = vsnprintf(NULL, 0, fmt, va1);
    va_end(va1);

    char msg[len + 1];
    xvsnprintf(msg, len + 1, fmt, va2);
    va_end(va2);

    LOG_WARN("%s in %llds %lldµs", msg,
             (unsigned long long)diff.tv_sec,
             (unsigned long long)diff.tv_nsec / 1000);

    free(start);
    free(stop);
}
