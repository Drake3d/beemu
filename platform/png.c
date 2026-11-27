#include "png.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];

static uint32_t crc(uint32_t c, const uint8_t *p, size_t n)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    c = ~c;
    while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static bool chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t hdr[8];
    be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    uint32_t c = crc(0, hdr + 4, 4);
    c = crc(c, data, len);
    uint8_t tail[4];
    be32(tail, c);
    return fwrite(hdr, 1, 8, f) == 8 && (len == 0 || fwrite(data, 1, len, f) == len) && fwrite(tail, 1, 4, f) == 4;
}

bool png_write_xrgb(const char *path, const uint32_t *xrgb, int w, int h)
{
    if (w <= 0 || h <= 0) return false;
    size_t row = (size_t)w * 3 + 1, raw_len = row * (size_t)h;
    uint8_t *raw = malloc(raw_len);
    if (!raw) return false;
    for (int y = 0; y < h; y++) {
        uint8_t *r = raw + (size_t)y * row;
        r[0] = 0;                                         // filter: none
        for (int x = 0; x < w; x++) {
            uint32_t p = xrgb[(size_t)y * (size_t)w + (size_t)x];
            r[1 + 3 * x] = (uint8_t)(p >> 16); r[2 + 3 * x] = (uint8_t)(p >> 8); r[3 + 3 * x] = (uint8_t)p;
        }
    }
    // zlib stream of stored (uncompressed) deflate blocks
    size_t nblocks = (raw_len + 65534) / 65535;
    size_t z_len = 2 + raw_len + 5 * nblocks + 4;
    uint8_t *z = malloc(z_len), *q = z;
    if (!z) { free(raw); return false; }
    *q++ = 0x78; *q++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t off = 0; off < raw_len; ) {
        size_t n = raw_len - off > 65535 ? 65535 : raw_len - off;
        *q++ = off + n == raw_len ? 1 : 0;
        *q++ = (uint8_t)n; *q++ = (uint8_t)(n >> 8);
        *q++ = (uint8_t)~n; *q++ = (uint8_t)(~n >> 8);
        memcpy(q, raw + off, n);
        for (size_t i = 0; i < n; i++) { a = (a + raw[off + i]) % 65521; b = (b + a) % 65521; }
        q += n; off += n;
    }
    be32(q, (b << 16) | a);
    q += 4;
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)w); be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;   // 8-bit RGB
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite("\x89PNG\r\n\x1a\n", 1, 8, f) == 8 && chunk(f, "IHDR", ihdr, 13) &&
              chunk(f, "IDAT", z, (uint32_t)(q - z)) && chunk(f, "IEND", NULL, 0);
    if (f) ok = (fclose(f) == 0) && ok;
    free(raw); free(z);
    return ok;
}
