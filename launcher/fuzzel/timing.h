#pragma once

#include <time.h>

void time_init(void);
void time_enable(void);

void time_since_boot(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

struct timespec *time_begin(void);
struct timespec *time_end(void);
void time_finish(struct timespec *start, struct timespec *stop, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
