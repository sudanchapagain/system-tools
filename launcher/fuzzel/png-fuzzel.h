#pragma once

#if defined(FUZZEL_ENABLE_PNG_LIBPNG)

#include <stdbool.h>
#include <pixman.h>

pixman_image_t *png_load(const char *path, bool gamma_correct);

#endif /* FUZZEL_ENABLE_PNG_LIBPNG */
