#pragma once

#include <stdarg.h>
#include <stddef.h>
#include "macros.h"

size_t xsnprintf(char *restrict buf, size_t n, const char *restrict format, ...) PRINTF(3) NONNULL_ARGS;
size_t xvsnprintf(char *restrict buf, size_t n, const char *restrict format, va_list ap) VPRINTF(3) NONNULL_ARGS;
