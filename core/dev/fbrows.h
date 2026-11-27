// Row cache for framebuffer renderers: keeps a copy of the guest's raw pixel rows so a
// renderer converts only the rows that changed since the last frame, and reports which
// rows those were (the frontend then uploads only them).
//
// Usage per frame: fbrows_begin() with a key that covers everything besides the raw bytes
// that affects the output (mode, geometry, start address, palette); then for each row
// fbrows_same() (true: the converted row from last time is still right); then fbrows_end().
#ifndef FBROWS_H
#define FBROWS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *shadow;         // last converted raw rows, row_bytes each
    size_t   cap;
    size_t   row_bytes;
    int      rows;
    uint64_t key;
    bool     valid;          // shadow matches the converted output
    int      y0, y1;         // rows converted this frame: [y0, y1)
} fbrows_t;

static inline uint64_t fbrows_hash(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    if (!h) h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

// Start a frame. Anything that changes the key, the size, or a call to fbrows_invalidate()
// makes every row count as changed.
static inline void fbrows_begin(fbrows_t *c, uint64_t key, size_t row_bytes, int rows)
{
    size_t need = row_bytes * (size_t)rows;
    if (!c->valid || key != c->key || row_bytes != c->row_bytes || rows != c->rows) {
        c->valid = false;
        if (need > c->cap) {
            uint8_t *n = realloc(c->shadow, need);
            if (n) { c->shadow = n; c->cap = need; }
        }
        c->key = key;
        c->row_bytes = row_bytes;
        c->rows = rows;
    }
    c->y0 = rows;
    c->y1 = 0;
}

// True if row y's raw bytes are unchanged (skip converting it). Otherwise records them and
// marks the row changed. src == NULL: the row can't be cached (wraps, out of range): changed.
static inline bool fbrows_same(fbrows_t *c, int y, const uint8_t *src)
{
    bool cacheable = src && c->shadow && c->cap >= c->row_bytes * (size_t)c->rows;
    if (cacheable) {
        uint8_t *s = c->shadow + (size_t)y * c->row_bytes;
        if (c->valid && memcmp(s, src, c->row_bytes) == 0) return true;
        memcpy(s, src, c->row_bytes);
    }
    if (y < c->y0) c->y0 = y;
    if (y + 1 > c->y1) c->y1 = y + 1;
    return false;
}

// Force a row to count as changed next frame's check (its output was drawn over, e.g. a cursor).
static inline void fbrows_touch(fbrows_t *c, int y0, int y1)
{
    if (!c->shadow || !c->valid) return;
    if (y0 < 0) y0 = 0;
    if (y1 > c->rows) y1 = c->rows;
    for (int y = y0; y < y1; y++) c->shadow[(size_t)y * c->row_bytes] ^= 0xFF;   // never matches
}

static inline void fbrows_mark(fbrows_t *c, int y0, int y1)
{
    if (y0 < 0) y0 = 0;
    if (y1 > c->rows) y1 = c->rows;
    if (y0 >= y1) return;
    if (y0 < c->y0) c->y0 = y0;
    if (y1 > c->y1) c->y1 = y1;
}

static inline void fbrows_end(fbrows_t *c)
{
    c->valid = c->shadow && c->cap >= c->row_bytes * (size_t)c->rows;
}

static inline void fbrows_invalidate(fbrows_t *c) { c->valid = false; }
static inline void fbrows_free(fbrows_t *c) { free(c->shadow); memset(c, 0, sizeof *c); }

#endif
