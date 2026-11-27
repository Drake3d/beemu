#include "vga.h"
#include <stdlib.h>
#include <string.h>

enum {
    DISPI_ID = 0, DISPI_XRES, DISPI_YRES, DISPI_BPP, DISPI_ENABLE, DISPI_BANK,
    DISPI_VIRT_W, DISPI_VIRT_H, DISPI_X_OFF, DISPI_Y_OFF, DISPI_VMEM_64K, DISPI_COUNT
};
#define DISPI_ENABLED     0x01
#define DISPI_GETCAPS     0x02
#define DISPI_8BIT_DAC    0x20
#define DISPI_LFB         0x40
#define DISPI_NOCLEARMEM  0x80
#define DISPI_MAX_X 2560
#define DISPI_MAX_Y 1600
#define DISPI_MAX_BPP 32
#define NO_BANK 0xFFFFFFFFu

/* ---- legacy window ---- */

static void map_window(vga_t *v)
{
    bool banked = v->dispi[DISPI_ENABLE] & DISPI_ENABLED;
    uint32_t want = banked ? v->dispi[DISPI_BANK] : NO_BANK;
    if (want == v->bank_mapped) return;
    if (banked) {
        uint32_t off = (want * 0x10000u) % VGA_VRAM_SIZE;
        mem_map_ram(v->bus, 0xA0000, 0x10000, v->vram + off);
        mem_unmap(v->bus, 0xB0000, 0x10000);
    } else {
        mem_map_ram(v->bus, 0xA0000, 0x20000, v->vram);
    }
    v->bank_mapped = want;
    if (v->remapped) v->remapped(v->remap_opaque);
}

/* ---- PCI ---- */

static void pci_map(pci_dev_t *d, int bar, uint32_t addr, bool on)
{
    vga_t *v = d->opaque;
    if (bar == 0) {
        if (on) mem_map_ram(v->bus, addr, VGA_VRAM_SIZE, v->vram);
        else    mem_unmap(v->bus, addr, VGA_VRAM_SIZE);
    } else if (bar == PCI_ROM_BAR && v->rom) {
        if (on) mem_map_rom(v->bus, addr, v->rom_size, v->rom);
        else    mem_unmap(v->bus, addr, v->rom_size);
    } else {
        return;                       // BAR 2 (MMIO registers) is sized but not decoded
    }
    if (v->remapped) v->remapped(v->remap_opaque);
}

/* ---- registers ---- */

static uint32_t dispi_get(vga_t *v, uint16_t i)
{
    if (i >= DISPI_COUNT) return 0;
    if (v->dispi[DISPI_ENABLE] & DISPI_GETCAPS) {
        if (i == DISPI_XRES) return DISPI_MAX_X;
        if (i == DISPI_YRES) return DISPI_MAX_Y;
        if (i == DISPI_BPP)  return DISPI_MAX_BPP;
    }
    if (i == DISPI_VMEM_64K) return VGA_VRAM_SIZE / 0x10000;
    return v->dispi[i];
}

static void dispi_set(vga_t *v, uint16_t i, uint16_t val)
{
    switch (i) {
    case DISPI_ID:
        if (val >= 0xB0C0 && val <= 0xB0C5) v->dispi[DISPI_ID] = val;
        break;
    case DISPI_BPP:
        v->dispi[DISPI_BPP] = val == 0 ? 8 : val;
        break;
    case DISPI_ENABLE: {
        bool was = v->dispi[DISPI_ENABLE] & DISPI_ENABLED;
        v->dispi[DISPI_ENABLE] = val;
        if ((val & DISPI_ENABLED) && !was) {
            uint32_t bpp = v->dispi[DISPI_BPP] ? v->dispi[DISPI_BPP] : 8;
            v->dispi[DISPI_VIRT_W] = v->dispi[DISPI_XRES];
            uint32_t line = (v->dispi[DISPI_XRES] * ((bpp + 7) / 8));
            v->dispi[DISPI_VIRT_H] = (uint16_t)(line ? VGA_VRAM_SIZE / line > 0xFFFF ? 0xFFFF : VGA_VRAM_SIZE / line : 0);
            v->dispi[DISPI_X_OFF] = v->dispi[DISPI_Y_OFF] = 0;
            if (!(val & DISPI_NOCLEARMEM)) {
                uint64_t n = (uint64_t)line * v->dispi[DISPI_YRES];
                memset(v->vram, 0, n < VGA_VRAM_SIZE ? n : VGA_VRAM_SIZE);
            }
        }
        map_window(v);
        break;
    }
    case DISPI_BANK:
        v->dispi[DISPI_BANK] = val;
        map_window(v);
        break;
    case DISPI_VMEM_64K:
        break;
    default:
        if (i < DISPI_COUNT) v->dispi[i] = val;
        break;
    }
}

static uint8_t status1(vga_t *v)
{
    uint64_t t = v->now(v->now_opaque);
    bool vr = (t % 16666667u) < 1000000u;          // 60 Hz frames, ~1 ms of vertical retrace
    bool hr = (t % 31778u) < 6000u;                // ~31.5 kHz lines
    return (uint8_t)((vr ? 0x08 : 0) | ((vr || hr) ? 0x01 : 0));
}

static uint8_t reg_read(vga_t *v, uint16_t port)
{
    switch (port) {
    case 0x3C0: return v->attr_idx;
    case 0x3C1: return v->attr[v->attr_idx & 0x1F];
    case 0x3C2: return 0x10;                       // input status 0
    case 0x3C3: return 1;
    case 0x3C4: return v->seq_idx;
    case 0x3C5: return v->seq[v->seq_idx & 7];
    case 0x3C6: return v->dac_mask;
    case 0x3C7: return v->dac_reading ? 3 : 0;
    case 0x3C8: return v->dac_write;
    case 0x3C9: {
        uint8_t c = v->dac[v->dac_read][v->dac_rsub];
        if (++v->dac_rsub == 3) { v->dac_rsub = 0; v->dac_read++; }
        return c;
    }
    case 0x3CA: return v->feature;
    case 0x3CC: return v->misc;
    case 0x3CE: return v->gc_idx;
    case 0x3CF: return v->gc[v->gc_idx & 15];
    case 0x3B4: case 0x3D4: return v->crtc_idx;
    case 0x3B5: case 0x3D5: return v->crtc[v->crtc_idx & 31];
    case 0x3BA: case 0x3DA: v->attr_flip = false; return status1(v);
    default: return 0xFF;
    }
}

static void reg_write(vga_t *v, uint16_t port, uint8_t val)
{
    switch (port) {
    case 0x3C0:
        if (!v->attr_flip) v->attr_idx = val & 0x3F;
        else               v->attr[v->attr_idx & 0x1F] = val;
        v->attr_flip = !v->attr_flip;
        break;
    case 0x3C2: v->misc = val; break;
    case 0x3C4: v->seq_idx = val; break;
    case 0x3C5: v->seq[v->seq_idx & 7] = val; break;
    case 0x3C6: v->dac_mask = val; break;
    case 0x3C7: v->dac_read = val; v->dac_rsub = 0; v->dac_reading = true; break;
    case 0x3C8: v->dac_write = val; v->dac_wsub = 0; v->dac_reading = false; break;
    case 0x3C9:
        v->dac_latch[v->dac_wsub++] = val;
        if (v->dac_wsub == 3) {
            memcpy(v->dac[v->dac_write++], v->dac_latch, 3);
            v->dac_wsub = 0;
        }
        break;
    case 0x3CE: v->gc_idx = val; break;
    case 0x3CF: v->gc[v->gc_idx & 15] = val; break;
    case 0x3B4: case 0x3D4: v->crtc_idx = val; break;
    case 0x3B5: case 0x3D5:
        // CR0-7 are write-protected when CR11 bit 7 is set (except CR7 bit 4).
        if ((v->crtc_idx & 31) <= 7 && (v->crtc[0x11] & 0x80)) {
            if ((v->crtc_idx & 31) == 7) v->crtc[7] = (uint8_t)((v->crtc[7] & ~0x10) | (val & 0x10));
            break;
        }
        v->crtc[v->crtc_idx & 31] = val;
        break;
    case 0x3BA: case 0x3DA: v->feature = val; break;
    default: break;
    }
}

// Multi-byte accesses to VGA ports are consecutive byte accesses (outw 0x3C4 = index + data).
static uint32_t vga_read(void *opaque, uint16_t port, int size)
{
    uint32_t r = 0;
    for (int i = 0; i < size; i++) r |= (uint32_t)reg_read(opaque, (uint16_t)(port + i)) << (8 * i);
    return r;
}

static void vga_write(void *opaque, uint16_t port, uint32_t val, int size)
{
    for (int i = 0; i < size; i++) reg_write(opaque, (uint16_t)(port + i), (uint8_t)(val >> (8 * i)));
}

static uint32_t dispi_read(void *opaque, uint16_t port, int size)
{
    (void)size;
    vga_t *v = opaque;
    return port == 0x1CE ? v->dispi_idx : dispi_get(v, v->dispi_idx);
}

static void dispi_write(void *opaque, uint16_t port, uint32_t val, int size)
{
    (void)size;
    vga_t *v = opaque;
    if (port == 0x1CE) v->dispi_idx = (uint16_t)val;
    else               dispi_set(v, v->dispi_idx, (uint16_t)val);
}

/* ---- setup ---- */

bool vga_init(vga_t *v, mem_bus_t *bus, const uint8_t *rom, uint32_t rom_len,
              clock_fn now, void *now_opaque, void (*remapped)(void *), void *remap_opaque)
{
    memset(v, 0, sizeof *v);
    v->bus = bus;
    v->now = now; v->now_opaque = now_opaque;
    v->remapped = remapped; v->remap_opaque = remap_opaque;
    v->vram = mem_alloc_host(VGA_VRAM_SIZE);
    if (!v->vram) return false;
    if (rom && rom_len) {
        uint32_t size = 0x10000;
        while (size < rom_len) size <<= 1;
        v->rom = mem_alloc_host(size);
        if (!v->rom) return false;
        memcpy(v->rom, rom, rom_len);
        v->rom_size = size;
    }
    v->dispi[DISPI_ID] = 0xB0C5;
    v->dispi[DISPI_BPP] = 8;
    v->dac_mask = 0xFF;
    v->bank_mapped = 0;                                   // force the first map
    map_window(v);

    pci_dev_t *d = &v->pci;
    pci_set_ids(d, 0x1234, 0x1111, 0x02, 0x030000, 0x1AF4, 0x1100);
    pci_set_bar(d, 0, VGA_VRAM_SIZE, false, true);
    pci_set_bar(d, 2, 0x1000, false, false);
    if (v->rom) pci_set_bar(d, PCI_ROM_BAR, v->rom_size, false, false);
    d->map = pci_map;
    d->opaque = v;
    return true;
}

void vga_free(vga_t *v)
{
    mem_free_host(v->vram);
    mem_free_host(v->rom);
    free(v->fb);
    fbrows_free(&v->rows);
    memset(v, 0, sizeof *v);
}

void vga_register(vga_t *v, io_bus_t *io)
{
    io_register(io, 0x3B0, 0x30, vga_read, vga_write, v);
    io_register(io, 0x1CE, 3, dispi_read, dispi_write, v);
}

/* ---- rendering ---- */

static bool ensure_fb(vga_t *v, int w, int h)
{
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return false;
    if (w != v->fb_w || h != v->fb_h || !v->fb) {
        uint32_t *nfb = realloc(v->fb, (size_t)w * (size_t)h * 4);
        if (!nfb) return false;
        v->fb = nfb; v->fb_w = w; v->fb_h = h;
    }
    return true;
}

static uint32_t dac_rgb(const vga_t *v, uint8_t i)
{
    const uint8_t *c = v->dac[i];
    if (v->dispi[DISPI_ENABLE] & DISPI_8BIT_DAC) return (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2];
    uint32_t r = (uint32_t)(c[0] & 63) << 2 | (c[0] & 63) >> 4;
    uint32_t g = (uint32_t)(c[1] & 63) << 2 | (c[1] & 63) >> 4;
    uint32_t b = (uint32_t)(c[2] & 63) << 2 | (c[2] & 63) >> 4;
    return r << 16 | g << 8 | b;
}

// 4-bit attribute color -> RGB through the attribute palette and the DAC.
static uint32_t attr_rgb(const vga_t *v, uint8_t color)
{
    uint8_t pal = v->attr[color & 15];
    uint8_t sel = v->attr[0x14];
    uint8_t idx = (v->attr[0x10] & 0x80) ? (uint8_t)(((sel & 0xF) << 4) | (pal & 0xF))
                                          : (uint8_t)(((sel & 0xC) << 4) | (pal & 0x3F));
    return dac_rgb(v, idx);
}

static uint32_t text_base(const vga_t *v)
{
    static const uint32_t base[4] = { 0, 0, 0x10000, 0x18000 };
    return base[(v->gc[6] >> 2) & 3];
}

static void text_geometry(const vga_t *v, int *cols, int *rows, int *ch)
{
    *cols = v->crtc[1] + 1;
    *ch = (v->crtc[9] & 0x1F) + 1;
    int vde = v->crtc[0x12] | ((v->crtc[7] & 0x02) << 7) | ((v->crtc[7] & 0x40) << 3);
    *rows = (vde + 1) / *ch;
    if (*cols < 20 || *cols > 132 || *rows < 10 || *rows > 60 || *ch < 8) { *cols = 80; *rows = 25; *ch = 16; }
}

static void render_text(vga_t *v)
{
    int cols, rows, ch;
    text_geometry(v, &cols, &rows, &ch);
    if (!ensure_fb(v, cols * 8, rows * ch)) return;
    uint32_t base = text_base(v);
    uint32_t start = (uint32_t)(v->crtc[0x0C] << 8 | v->crtc[0x0D]);
    uint32_t stride = v->crtc[0x13] ? v->crtc[0x13] * 2u : (uint32_t)cols;
    uint32_t cursor = (uint32_t)(v->crtc[0x0E] << 8 | v->crtc[0x0F]);
    int cur_start = v->crtc[0x0A] & 0x1F, cur_end = v->crtc[0x0B] & 0x1F;
    bool cur_on = !(v->crtc[0x0A] & 0x20) && ((v->now(v->now_opaque) / 266666667u) & 1);
    bool blink = v->attr[0x10] & 0x08;
    static const uint32_t fontmap[8] = { 0, 0x4000, 0x8000, 0xC000, 0x2000, 0x6000, 0xA000, 0xE000 };
    uint8_t s3 = v->seq[3];
    uint32_t font_a = fontmap[((s3 >> 3) & 4) | (s3 & 3)];        // attribute bit 3 clear
    uint32_t font_b = fontmap[((s3 >> 4) & 4) | ((s3 >> 2) & 3)]; // attribute bit 3 set

    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            uint32_t cell = start + (uint32_t)r * stride + (uint32_t)c;
            uint32_t off = (base + cell * 2) & (VGA_VRAM_SIZE - 1);
            uint8_t chr = v->vram[off], at = v->vram[off + 1];
            uint32_t fg = attr_rgb(v, at & 0x0F);
            uint32_t bg = attr_rgb(v, blink ? (at >> 4) & 7 : at >> 4);
            uint32_t font = (at & 0x08) ? font_b : font_a;
            for (int y = 0; y < ch; y++) {
                uint8_t bits = v->vram[font + chr * 32u + (uint32_t)y];
                if (cur_on && cell == cursor && y >= cur_start && y <= cur_end) bits = 0xFF;
                uint32_t *px = v->fb + (size_t)(r * ch + y) * (size_t)v->fb_w + (size_t)c * 8;
                for (int x = 0; x < 8; x++) px[x] = (bits & (0x80 >> x)) ? fg : bg;
            }
        }
}

// 15/16 bpp -> XRGB lookup tables (the same rounding as before: c * 255 / max).
static uint32_t lut15[1 << 15], lut16[1 << 16];
static void build_luts(void)
{
    static bool done;
    if (done) return;
    for (uint32_t q = 0; q < (1u << 16); q++) {
        lut16[q] = ((q >> 11 & 31) * 255 / 31) << 16 | ((q >> 5 & 63) * 255 / 63) << 8 | (q & 31) * 255 / 31;
        if (q < (1u << 15))
            lut15[q] = ((q >> 10 & 31) * 255 / 31) << 16 | ((q >> 5 & 31) * 255 / 31) << 8 | (q & 31) * 255 / 31;
    }
    done = true;
}

static void convert_row(uint32_t *px, const uint8_t *p, int w, int bpp, const uint32_t *pal)
{
    switch (bpp) {
    case 8:  for (int x = 0; x < w; x++) px[x] = pal[p[x]]; break;
    case 15: for (int x = 0; x < w; x++) px[x] = lut15[(p[2 * x] | p[2 * x + 1] << 8) & 0x7FFF]; break;
    case 16: for (int x = 0; x < w; x++) px[x] = lut16[p[2 * x] | p[2 * x + 1] << 8]; break;
    case 24: for (int x = 0; x < w; x++) px[x] = (uint32_t)p[3 * x + 2] << 16 | (uint32_t)p[3 * x + 1] << 8 | p[3 * x]; break;
    default: for (int x = 0; x < w; x++) px[x] = (uint32_t)p[4 * x + 2] << 16 | (uint32_t)p[4 * x + 1] << 8 | p[4 * x]; break;
    }
}

// Rows whose bytes match the previous frame are not converted again (see fbrows.h).
static void render_vbe(vga_t *v)
{
    int w = v->dispi[DISPI_XRES], h = v->dispi[DISPI_YRES], bpp = v->dispi[DISPI_BPP];
    if (!ensure_fb(v, w, h)) return;
    build_luts();
    int bytes = (bpp + 7) / 8;
    uint32_t pitch = (uint32_t)(v->dispi[DISPI_VIRT_W] ? v->dispi[DISPI_VIRT_W] : w) * (uint32_t)bytes;
    uint32_t origin = v->dispi[DISPI_Y_OFF] * pitch + v->dispi[DISPI_X_OFF] * (uint32_t)bytes;
    uint32_t pal[256];
    for (int i = 0; i < 256; i++) pal[i] = dac_rgb(v, (uint8_t)i);
    uint32_t geo[5] = { (uint32_t)w, (uint32_t)h, (uint32_t)bpp, pitch, origin };
    uint64_t key = fbrows_hash(0, geo, sizeof geo);
    if (bpp == 8) key = fbrows_hash(key, pal, sizeof pal);
    size_t row_bytes = (size_t)w * (size_t)bytes;
    fbrows_begin(&v->rows, key, row_bytes, h);
    uint8_t *tmp = NULL;
    for (int y = 0; y < h; y++) {
        uint32_t o = (origin + (uint32_t)y * pitch) & (VGA_VRAM_SIZE - 1);
        const uint8_t *src = v->vram + o;
        if (o + row_bytes > VGA_VRAM_SIZE) {                     // wraps around VRAM: gather it
            if (!tmp && !(tmp = malloc(row_bytes))) continue;
            for (size_t i = 0; i < row_bytes; i++) tmp[i] = v->vram[(o + i) & (VGA_VRAM_SIZE - 1)];
            src = tmp;
        }
        if (fbrows_same(&v->rows, y, src)) continue;
        convert_row(v->fb + (size_t)y * (size_t)w, src, w, bpp, pal);
    }
    free(tmp);
    fbrows_end(&v->rows);
    v->dirty_y0 = v->rows.y0;
    v->dirty_y1 = v->rows.y1;
}

static void render_mode13(vga_t *v)
{
    if (!ensure_fb(v, 320, 200)) return;
    for (int i = 0; i < 320 * 200; i++) v->fb[i] = dac_rgb(v, v->vram[i]);
}

const uint32_t *vga_render(vga_t *v, int *w, int *h)
{
    if (v->dispi[DISPI_ENABLE] & DISPI_ENABLED) {
        render_vbe(v);
    } else {
        // Small legacy modes: redrawn whole (cheap); the VBE cache starts over afterwards.
        fbrows_invalidate(&v->rows);
        if (!(v->attr[0x10] & 1)) render_text(v);
        else if (v->seq[4] & 8) render_mode13(v);
        else if (ensure_fb(v, 640, 480)) {                // planar modes: not emulated yet
            for (int i = 0; i < 640 * 480; i++) v->fb[i] = 0x202020;
        }
        v->dirty_y0 = 0;
        v->dirty_y1 = v->fb_h;
    }
    *w = v->fb_w; *h = v->fb_h;
    return v->fb;
}

int vga_text(vga_t *v, char *out, int out_len)
{
    int cols, rows, ch, n = 0;
    text_geometry(v, &cols, &rows, &ch);
    uint32_t base = text_base(v);
    uint32_t start = (uint32_t)(v->crtc[0x0C] << 8 | v->crtc[0x0D]);
    uint32_t stride = v->crtc[0x13] ? v->crtc[0x13] * 2u : (uint32_t)cols;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols && n < out_len - 2; c++) {
            uint8_t chr = v->vram[(base + (start + (uint32_t)r * stride + (uint32_t)c) * 2) & (VGA_VRAM_SIZE - 1)];
            out[n++] = (chr >= 32 && chr < 127) ? (char)chr : ' ';
        }
        if (n < out_len - 1) out[n++] = '\n';
    }
    out[n] = 0;
    return n;
}
