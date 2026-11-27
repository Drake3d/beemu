// Minimal PNG writer (uncompressed deflate), for screenshots. No dependencies.
#ifndef PNG_H
#define PNG_H

#include <stdbool.h>
#include <stdint.h>

bool png_write_xrgb(const char *path, const uint32_t *xrgb, int width, int height);

#endif
