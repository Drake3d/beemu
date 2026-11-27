#include "g400.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Drawing registers, as dword indexes into dwg[] (aperture offset 0x1C00 + 4 * index). */
enum {
    D_DWGCTL = 0x00, D_MACCESS = 0x01, D_MCTLWTST = 0x02, D_PLNWT = 0x07, D_BCOL = 0x08, D_FCOL = 0x09,
    D_SGN = 0x16, D_LEN = 0x17, D_AR0 = 0x18, D_AR3 = 0x1B, D_AR5 = 0x1D,
    D_CXBNDRY = 0x20, D_FXBNDRY = 0x21, D_YDSTLEN = 0x22, D_PITCH = 0x23, D_YDST = 0x24, D_YDSTORG = 0x25,
    D_YTOP = 0x26, D_YBOT = 0x27, D_CXLEFT = 0x28, D_CXRIGHT = 0x29, D_FXLEFT = 0x2A, D_FXRIGHT = 0x2B,
};

/* DWGCTL fields */
#define OPCOD(c)   ((c) & 0xF)
#define ATYPE(c)   (((c) >> 4) & 7)
#define BOP(c)     (((c) >> 16) & 0xF)
#define TRANSC     (1u << 30)
#define CLIPDIS    (1u << 31)
enum { OP_TRAP = 0x4, OP_TEXTURE_TRAP = 0x6, OP_BITBLT = 0x8, OP_FBITBLT = 0xC };

/* Host interface */
enum { R_FIFOSTATUS = 0x1E10, R_STATUS = 0x1E14, R_ICLEAR = 0x1E18, R_IEN = 0x1E1C, R_VCOUNT = 0x1E20,
       R_RST = 0x1E40, R_OPMODE = 0x1E54 };
#define ST_PICKPEN   0x01u
#define ST_VSYNCSTS  0x08u
#define ST_VSYNCPEN  0x10u
#define ST_VLINEPEN  0x20u

/* DAC */
enum { DAC_PALWTADD = 0x3C00, DAC_PALDATA = 0x3C01, DAC_PIXRDMSK = 0x3C02, DAC_PALRDADD = 0x3C03,
       DAC_XDATA = 0x3C0A, DAC_CURPOS = 0x3C0C };
enum { X_CURADDL = 0x04, X_CURADDH = 0x05, X_CURCTRL = 0x06, X_CURCOL0 = 0x08, X_MULCTRL = 0x19,
       X_MISCCTRL = 0x1E, X_GENIOCTRL = 0x2A, X_GENIODATA = 0x2B, X_SYSPLLSTAT = 0x2F, X_PIXPLLSTAT = 0x4F };

#define FRAME_NS 16666667ull

static bool trace_g400(void)
{
    static int t = -1;
    if (t < 0) { const char *e = getenv("BEEMU_TRACE_G400"); t = e && *e == '1'; }
    return t;
}

static uint64_t now_ns(const g400_t *g) { return g->clock ? g->clock(g->clock_opaque) : 0; }

static void update_irq(g400_t *g)
{
    if (g->set_intx) g->set_intx(g->intx_opaque, (g->status & g->ien & (ST_PICKPEN | ST_VLINEPEN)) != 0);
}

// Position within the current 60 Hz frame, 0..1; the last 5% is vertical retrace.
static double frame_phase(const g400_t *g)
{
    uint64_t t = now_ns(g) - g->frame_base_ns;
    return (double)(t % FRAME_NS) / (double)FRAME_NS;
}

static bool in_retrace(const g400_t *g) { return frame_phase(g) >= 0.95; }

/* ------------------------------------------------------------------ */
/* Pixels                                                              */
/* ------------------------------------------------------------------ */

static int bpp_bytes(const g400_t *g)
{
    static const int t[4] = { 1, 2, 4, 3 };     // MACCESS.pwidth: 8, 16, 32, 24 bpp
    return t[g->dwg[D_MACCESS] & 3];
}

static uint32_t pix_mask(int b) { return b == 4 ? 0xFFFFFFFFu : (1u << (8 * b)) - 1; }

static uint32_t rd_px(const g400_t *g, int64_t a, int b)
{
    uint64_t off = (uint64_t)a * (uint64_t)b;
    if (a < 0 || off + (uint64_t)b > G400_VRAM_SIZE) return 0;
    uint32_t v = 0;
    memcpy(&v, g->vram + off, (size_t)b);
    return v;
}

static void wr_px(g400_t *g, int64_t a, int b, uint32_t v)
{
    uint64_t off = (uint64_t)a * (uint64_t)b;
    if (a < 0 || off + (uint64_t)b > G400_VRAM_SIZE) return;
    memcpy(g->vram + off, &v, (size_t)b);
}

// Matrox boolean ops: bit (S * 2 + D) of bop gives the result bit.
static uint32_t rop(uint32_t bop, uint32_t s, uint32_t d)
{
    uint32_t r = 0;
    if (bop & 8) r |= s & d;
    if (bop & 4) r |= s & ~d;
    if (bop & 2) r |= ~s & d;
    if (bop & 1) r |= ~s & ~d;
    return r;
}

static int32_t sext(uint32_t v, int bits) { return (int32_t)(v << (32 - bits)) >> (32 - bits); }

typedef struct {
    int b;                       // bytes per pixel
    int32_t pitch, org;          // pixels
    int32_t cxl, cxr;            // clip, inclusive
    int64_t ytop, ybot;          // clip, line start addresses
    bool clip;
    bool ylin;                   // PITCH.ylin: YDST is a line address in units of 32 pixels
    uint32_t mask, plnwt, bop;
    bool replace;                // result does not depend on the destination
} ctx_t;

static void setup(const g400_t *g, ctx_t *c)
{
    uint32_t ctl = g->dwg[D_DWGCTL];
    c->b = bpp_bytes(g);
    c->pitch = (int32_t)(g->dwg[D_PITCH] & 0xFFF);
    c->org = (int32_t)(g->dwg[D_YDSTORG] & 0xFFFFFF);
    c->cxl = (int32_t)(g->dwg[D_CXBNDRY] & 0xFFF);
    c->cxr = (int32_t)((g->dwg[D_CXBNDRY] >> 16) & 0xFFF);
    c->ytop = g->dwg[D_YTOP] & 0xFFFFFF;
    c->ybot = g->dwg[D_YBOT] & 0xFFFFFF;
    c->clip = !(ctl & CLIPDIS);
    c->ylin = (g->dwg[D_PITCH] >> 15) & 1;
    c->mask = pix_mask(c->b);
    c->plnwt = g->dwg[D_PLNWT] & c->mask;
    c->bop = BOP(ctl);
    c->replace = c->bop == 0xC || c->bop == 0x3 || c->bop == 0x0 || c->bop == 0xF;
}

// YDST as a register value <-> the engine's y (a row, or with ylin a line address / 32).
static int32_t ydst_get(const g400_t *g, const ctx_t *c)
{
    return c->ylin ? (int32_t)(g->dwg[D_YDST] & 0x7FFFFF) : sext(g->dwg[D_YDST], 22);
}

// One line down (dir 1) or up (dir -1).
static int32_t ydst_step(const ctx_t *c, int32_t y, int dir)
{
    return y + (c->ylin ? dir * (c->pitch >> 5) : dir);
}

// Line start address of y, or -1 when y is clipped away.
static int64_t line_addr(const ctx_t *c, int32_t y)
{
    int64_t la = c->ylin ? ((int64_t)y << 5) + c->org : (int64_t)y * c->pitch + c->org;
    if (c->clip && (la < c->ytop || la > c->ybot)) return -1;
    return la;
}

static void put(g400_t *g, const ctx_t *c, int64_t la, int32_t x, uint32_t s)
{
    if (c->clip && (x < c->cxl || x > c->cxr)) return;
    int64_t a = la + x;
    uint32_t v;
    if (c->replace && c->plnwt == c->mask) {
        v = rop(c->bop, s, 0) & c->mask;
    } else {
        uint32_t d = rd_px(g, a, c->b);
        v = rop(c->bop, s, d);
        v = (v & c->plnwt) | (d & ~c->plnwt);
    }
    wr_px(g, a, c->b, v & c->mask);
}

/* ------------------------------------------------------------------ */
/* Drawing engine                                                      */
/* ------------------------------------------------------------------ */

// Fast paths: the engine runs on the guest's time, so the common cases (solid fill, plain copy,
// all planes) use memset/memmove on whole spans instead of going pixel by pixel.

// Clip the inclusive span [*xa, *xb] to CXBNDRY; false when nothing is left.
static bool clip_span(const ctx_t *c, int32_t *xa, int32_t *xb)
{
    if (c->clip) {
        if (*xa < c->cxl) *xa = c->cxl;
        if (*xb > c->cxr) *xb = c->cxr;
    }
    return *xa <= *xb;
}

// Host pointer for pixels [a, a + n) of VRAM, or NULL if any of them is outside it.
static uint8_t *span_ptr(g400_t *g, int64_t a, int32_t n, int b)
{
    if (a < 0 || n <= 0) return NULL;
    uint64_t off = (uint64_t)a * (uint64_t)b, len = (uint64_t)n * (uint64_t)b;
    if (off + len > G400_VRAM_SIZE) return NULL;
    return g->vram + off;
}

static bool fill_span(g400_t *g, int64_t a, int32_t n, int b, uint32_t v)
{
    uint8_t *p = span_ptr(g, a, n, b);
    if (!p) return false;
    switch (b) {
    case 1: memset(p, (int)(v & 0xFF), (size_t)n); return true;
    case 2: { uint16_t w = (uint16_t)v; for (int32_t i = 0; i < n; i++) memcpy(p + 2 * i, &w, 2); return true; }
    case 4: for (int32_t i = 0; i < n; i++) memcpy(p + 4 * i, &v, 4); return true;
    default: return false;
    }
}

// Rectangle (TRAP with zero slopes): rows YDST .. YDST+LEN-1, columns [fxleft, fxright).
static void do_trap(g400_t *g)
{
    ctx_t c;
    setup(g, &c);
    int32_t x0 = sext(g->dwg[D_FXBNDRY] & 0xFFFF, 16), x1 = sext(g->dwg[D_FXBNDRY] >> 16, 16);
    int32_t y = ydst_get(g, &c);
    int32_t n = (int32_t)(g->dwg[D_LEN] & 0xFFFF);
    uint32_t s = g->dwg[D_FCOL] & c.mask;
    bool fast = c.replace && c.plnwt == c.mask && c.b != 3;
    uint32_t v = rop(c.bop, s, 0) & c.mask;
    for (int32_t i = 0; i < n; i++, y = ydst_step(&c, y, 1)) {
        int64_t la = line_addr(&c, y);
        if (la < 0) continue;
        int32_t xa = x0, xb = x1 - 1;
        if (fast && (!clip_span(&c, &xa, &xb) || fill_span(g, la + xa, xb - xa + 1, c.b, v))) continue;
        for (int32_t x = x0; x < x1; x++) put(g, &c, la, x, s);
    }
    g->dwg[D_YDST] = (uint32_t)y & (c.ylin ? 0x7FFFFF : 0x3FFFFF);
    g->dwg[D_LEN] = 0;
}

// Screen to screen copy. Source pixels run from AR3 towards AR0 (right to left when
// SGN.scanleft), one line per LEN, both addresses moving by AR5 after each line;
// destination columns fxleft..fxright inclusive, lines from YDST down (or up, SGN.sdy).
static void do_bitblt(g400_t *g)
{
    ctx_t c;
    setup(g, &c);
    uint32_t ctl = g->dwg[D_DWGCTL], sgn = g->dwg[D_SGN];
    bool left = sgn & 1, up = sgn & 4;
    int32_t x0 = sext(g->dwg[D_FXBNDRY] & 0xFFFF, 16), x1 = sext(g->dwg[D_FXBNDRY] >> 16, 16);
    int32_t y = ydst_get(g, &c);
    int32_t n = (int32_t)(g->dwg[D_LEN] & 0xFFFF);
    int64_t ar3 = sext(g->dwg[D_AR3], 24), ar0 = sext(g->dwg[D_AR0], 24), ar5 = sext(g->dwg[D_AR5], 18);
    int32_t w = x1 - x0 + 1;
    bool trans = ctl & TRANSC;
    uint32_t key = g->dwg[D_FCOL] & c.mask, keymask = g->dwg[D_BCOL] & c.mask;
    bool fast = !trans && c.bop == 0xC && c.plnwt == c.mask;
    for (int32_t i = 0; i < n; i++) {
        int64_t la = line_addr(&c, y);
        int32_t xa = x0, xb = x1;
        uint8_t *dp, *sp;
        if (la >= 0 && fast && w > 0) {
            if (!clip_span(&c, &xa, &xb)) goto next;
            // Source pixel for column x: AR3 + (x - x0), or AR3 - (x1 - x) scanning left.
            int64_t sa = (left ? ar3 - (x1 - xa) : ar3 + (xa - x0));
            int32_t cnt = xb - xa + 1;
            if ((dp = span_ptr(g, la + xa, cnt, c.b)) && (sp = span_ptr(g, sa, cnt, c.b))) {
                memmove(dp, sp, (size_t)cnt * (size_t)c.b);
                goto next;
            }
        }
        if (la >= 0) {
            for (int32_t k = 0; k < w; k++) {
                int64_t src = left ? ar3 - k : ar3 + k;
                int32_t x = left ? x1 - k : x0 + k;
                uint32_t s = rd_px(g, src, c.b);
                if (trans && ((s ^ key) & keymask) == 0) continue;     // color key
                put(g, &c, la, x, s);
            }
        }
    next:
        ar3 += ar5;
        ar0 += ar5;
        y = ydst_step(&c, y, up ? -1 : 1);
    }
    g->dwg[D_AR3] = (uint32_t)ar3 & 0xFFFFFF;
    g->dwg[D_AR0] = (uint32_t)ar0 & 0xFFFFFF;
    g->dwg[D_YDST] = (uint32_t)y & (c.ylin ? 0x7FFFFF : 0x3FFFFF);
    g->dwg[D_LEN] = 0;
}

static void execute(g400_t *g)
{
    uint32_t ctl = g->dwg[D_DWGCTL];
    g->ops++;
    switch (OPCOD(ctl)) {
    case OP_TRAP: case OP_TEXTURE_TRAP: do_trap(g); break;
    case OP_BITBLT: case OP_FBITBLT: do_bitblt(g); break;
    default:
        if (g->unsupported_ops++ < 16 || trace_g400())
            fprintf(stderr, "beemu: g400: drawing opcode %X not emulated (DWGCTL %08X)\n", OPCOD(ctl), ctl);
        break;
    }
}

// A drawing register write. Some registers are views of others.
static void dwg_write(g400_t *g, int i, uint32_t v)
{
    switch (i) {
    case D_YDSTLEN:
        g->dwg[D_YDST] = (v >> 16) & 0xFFFF;
        g->dwg[D_LEN] = v & 0xFFFF;
        break;
    case D_CXLEFT:  g->dwg[D_CXBNDRY] = (g->dwg[D_CXBNDRY] & 0xFFFF0000u) | (v & 0xFFF); break;
    case D_CXRIGHT: g->dwg[D_CXBNDRY] = (g->dwg[D_CXBNDRY] & 0xFFFFu) | ((v & 0xFFF) << 16); break;
    case D_FXLEFT:  g->dwg[D_FXBNDRY] = (g->dwg[D_FXBNDRY] & 0xFFFF0000u) | (v & 0xFFFF); break;
    case D_FXRIGHT: g->dwg[D_FXBNDRY] = (g->dwg[D_FXBNDRY] & 0xFFFFu) | ((v & 0xFFFF) << 16); break;
    default: break;
    }
    g->dwg[i] = v;
}

/* ------------------------------------------------------------------ */
/* VGA core and DAC (byte registers)                                   */
/* ------------------------------------------------------------------ */

static uint8_t pins(const g400_t *g)
{
    // DDC / I2C pins are open drain: a pin reads low only while we drive it low.
    // Nothing is attached, so undriven pins read high (no monitor, no Maven).
    uint8_t ctl = g->x[X_GENIOCTRL], out = g->x[X_GENIODATA];
    return (uint8_t)((~ctl | out) & 0xFF);
}

static uint8_t xreg_read(const g400_t *g, uint8_t i)
{
    switch (i) {
    case X_SYSPLLSTAT: case X_PIXPLLSTAT: return 0x40;      // PLL locked
    case X_GENIODATA: return pins(g);
    default: return g->x[i];
    }
}

static uint8_t byte_read(g400_t *g, uint32_t off)
{
    switch (off) {
    case 0x1FC4: return g->seq_idx;
    case 0x1FC5: return g->seq[g->seq_idx & 7];
    case 0x1FCC: return g->misc;
    case 0x1FCE: return g->gctl_idx;
    case 0x1FCF: return g->gctl[g->gctl_idx & 15];
    case 0x1FD4: return g->crtc_idx;
    case 0x1FD5: return g->crtc[g->crtc_idx & 0x3F];
    case 0x1FDA: {                                           // INSTS1
        bool vr = in_retrace(g);
        return (uint8_t)((vr ? 0x08 : 0) | ((vr || ((now_ns(g) >> 12) & 1)) ? 0x01 : 0));
    }
    case 0x1FDE: return g->crtcext_idx;
    case 0x1FDF: return g->crtcext[g->crtcext_idx & 7];
    case DAC_PALWTADD: return g->pal_w;
    case DAC_PALDATA: {
        uint8_t v = g->pal[g->pal_r][g->pal_sub_r];
        if (++g->pal_sub_r == 3) { g->pal_sub_r = 0; g->pal_r++; }
        return v;
    }
    case DAC_PIXRDMSK: return g->pixrdmsk;
    case DAC_PALRDADD: return g->pal_r;
    case DAC_XDATA: return xreg_read(g, g->pal_w);
    case DAC_CURPOS: case DAC_CURPOS + 1: case DAC_CURPOS + 2: case DAC_CURPOS + 3:
        return g->curpos[off - DAC_CURPOS];
    default:
        return (uint8_t)(g->raw[off / 4] >> (8 * (off & 3)));
    }
}

static void byte_write(g400_t *g, uint32_t off, uint8_t v)
{
    switch (off) {
    case 0x1FC2: g->misc = v; return;
    case 0x1FC4: g->seq_idx = v; return;
    case 0x1FC5: g->seq[g->seq_idx & 7] = v; return;
    case 0x1FCE: g->gctl_idx = v; return;
    case 0x1FCF: g->gctl[g->gctl_idx & 15] = v; return;
    case 0x1FD4: g->crtc_idx = v; return;
    case 0x1FD5: g->crtc[g->crtc_idx & 0x3F] = v; return;
    case 0x1FDE: g->crtcext_idx = v; return;
    case 0x1FDF: g->crtcext[g->crtcext_idx & 7] = v; return;
    case DAC_PALWTADD: g->pal_w = v; g->pal_sub_w = 0; return;
    case DAC_PALDATA:
        g->pal[g->pal_w][g->pal_sub_w] = v;
        if (++g->pal_sub_w == 3) { g->pal_sub_w = 0; g->pal_w++; }
        return;
    case DAC_PIXRDMSK: g->pixrdmsk = v; return;
    case DAC_PALRDADD: g->pal_r = v; g->pal_sub_r = 0; return;
    case DAC_XDATA: g->x[g->pal_w] = v; return;
    case DAC_CURPOS: case DAC_CURPOS + 1: case DAC_CURPOS + 2: case DAC_CURPOS + 3:
        g->curpos[off - DAC_CURPOS] = v;
        return;
    default: {
        uint32_t *r = &g->raw[off / 4];
        unsigned sh = 8 * (off & 3);
        *r = (*r & ~(0xFFu << sh)) | ((uint32_t)v << sh);
        return;
    }
    }
}

/* ------------------------------------------------------------------ */
/* Register aperture                                                   */
/* ------------------------------------------------------------------ */

static bool is_dword_reg(uint32_t off) { return off >= 0x1C00 && off < 0x1F00; }

uint32_t g400_reg_read(g400_t *g, uint32_t off, int size)
{
    off &= 0x3FFF;
    uint32_t v;
    if (is_dword_reg(off & ~3u)) {
        uint32_t base = off & ~3u, d;
        if (base >= 0x1C00 && base < 0x1E00) d = g->dwg[((base - 0x1C00) & 0xFF) / 4];
        else switch (base) {
            case R_FIFOSTATUS: d = 64 | (1u << 9); break;          // 64 free slots, FIFO empty
            case R_STATUS: d = g->status | (1u << 17) | (in_retrace(g) ? ST_VSYNCSTS : 0); break;   // ENDPRDMASTS: DMA idle; DWGENGSTS clear: engine idle
            case R_IEN: d = g->ien; break;
            case R_VCOUNT: d = (uint32_t)(frame_phase(g) * 1066); break;
            case R_RST: d = g->rst; break;
            case R_OPMODE: d = g->opmode; break;
            default: d = g->raw[base / 4]; break;
        }
        v = d >> (8 * (off & 3));
        if (size < 4) v &= (1u << (8 * size)) - 1;
    } else {
        v = 0;
        for (int i = 0; i < size; i++) v |= (uint32_t)byte_read(g, (off + (uint32_t)i) & 0x3FFF) << (8 * i);
    }
    if (trace_g400() && off != R_FIFOSTATUS && off != R_STATUS && off != 0x1FDA)
        fprintf(stderr, "[g400] rd%d %04X = %0*X\n", size * 8, off, size * 2, v);
    return v;
}

void g400_reg_write(g400_t *g, uint32_t off, uint32_t v, int size)
{
    off &= 0x3FFF;
    if (trace_g400()) fprintf(stderr, "[g400] wr%d %04X = %0*X\n", size * 8, off, size * 2, v);
    if (is_dword_reg(off & ~3u)) {
        uint32_t base = off & ~3u;
        if (size < 4) {                                     // merge into the dword
            uint32_t cur = g400_reg_read(g, base, 4), sh = 8 * (off & 3), m = ((size == 1) ? 0xFFu : 0xFFFFu) << sh;
            v = (cur & ~m) | ((v << sh) & m);
        }
        if (base >= 0x1C00 && base < 0x1E00) {
            int i = (int)((base - 0x1C00) & 0xFF) / 4;
            dwg_write(g, i, v);
            if (base >= 0x1D00) execute(g);                 // "go" alias
            return;
        }
        switch (base) {
        case R_ICLEAR: g->status &= ~(v & (ST_PICKPEN | ST_VSYNCPEN | ST_VLINEPEN)); update_irq(g); return;
        case R_IEN: g->ien = v; update_irq(g); return;
        case R_RST: g->rst = v; return;
        case R_OPMODE: g->opmode = v; return;
        default: g->raw[base / 4] = v; return;
        }
    }
    for (int i = 0; i < size; i++) byte_write(g, (off + (uint32_t)i) & 0x3FFF, (uint8_t)(v >> (8 * i)));
}

static uint32_t ctl_read(void *o, uint32_t off, int size) { return g400_reg_read(o, off, size); }
static void ctl_write(void *o, uint32_t off, uint32_t v, int size) { g400_reg_write(o, off, v, size); }
static uint32_t iload_read(void *o, uint32_t off, int size) { (void)o; (void)off; (void)size; return 0; }
static void iload_write(void *o, uint32_t off, uint32_t v, int size) { (void)o; (void)off; (void)v; (void)size; }

/* ------------------------------------------------------------------ */
/* Display                                                             */
/* ------------------------------------------------------------------ */

bool g400_active(const g400_t *g) { return (g->crtcext[3] & 0x80) != 0; }

static uint32_t pal_rgb(const g400_t *g, uint8_t i)
{
    const uint8_t *p = g->pal[i];
    if (g->x[X_MISCCTRL] & 0x08) return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];   // 8-bit DAC
    return (uint32_t)(p[0] & 63) << 18 | (uint32_t)(p[1] & 63) << 10 | (uint32_t)(p[2] & 63) << 2;
}

// Hardware cursor geometry and a key for everything that affects how it looks.
// Returns false when the cursor is off (or its image lies outside VRAM).
static bool cursor_state(const g400_t *g, int *px, int *py, uint64_t *key)
{
    int mode = g->x[X_CURCTRL] & 3;
    uint32_t addr = (uint32_t)(g->x[X_CURADDH] << 8 | g->x[X_CURADDL]) << 10;
    if (!mode || addr + 1024 > G400_VRAM_SIZE) { *key = 0; return false; }
    *px = (g->curpos[0] | (g->curpos[1] & 0x0F) << 8) - 64;
    *py = (g->curpos[2] | (g->curpos[3] & 0x0F) << 8) - 64;
    uint32_t k[4] = { (uint32_t)mode, addr, (uint32_t)*px, (uint32_t)*py };
    uint64_t h = fbrows_hash(0, k, sizeof k);
    h = fbrows_hash(h, &g->x[X_CURCOL0], 12);
    h = fbrows_hash(h, g->vram + addr, 1024);
    *key = h | 1;
    return true;
}

static void draw_cursor(g400_t *g, int w, int h)
{
    int mode = g->x[X_CURCTRL] & 3;
    if (!mode) return;
    uint32_t addr = (uint32_t)(g->x[X_CURADDH] << 8 | g->x[X_CURADDL]) << 10;
    if (addr + 1024 > G400_VRAM_SIZE) return;
    int px = (g->curpos[0] | (g->curpos[1] & 0x0F) << 8) - 64;
    int py = (g->curpos[2] | (g->curpos[3] & 0x0F) << 8) - 64;
    uint32_t col[3];
    for (int i = 0; i < 3; i++) {
        const uint8_t *c = &g->x[X_CURCOL0 + 4 * i];
        col[i] = (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2];
    }
    const uint8_t *img = g->vram + addr;
    for (int row = 0; row < 64; row++) {
        int y = py + row;
        if (y < 0 || y >= h) continue;
        const uint8_t *line = img + row * 16;
        for (int k = 0; k < 64; k++) {
            int x = px + k;
            if (x < 0 || x >= w) continue;
            // Each plane is a little-endian 64-bit word per line, leftmost pixel in bit 63.
            int b0 = (line[7 - (k >> 3)] >> (7 - (k & 7))) & 1;    // plane 0: bytes 0-7
            int b1 = (line[15 - (k >> 3)] >> (7 - (k & 7))) & 1;   // plane 1: bytes 8-15
            int code = b1 << 1 | b0;
            uint32_t *p = &g->fb[y * w + x];
            switch (mode) {
            case 1: if (code) *p = col[code - 1]; break;                       // three color
            case 2: if (code < 2) *p = col[code]; else if (code == 3) *p ^= 0xFFFFFF; break;   // XGA
            case 3: if (code >= 2) *p = col[code - 2]; break;                  // X-Windows
            }
        }
    }
}

// 15/16 bpp -> XRGB by bit replication.
static uint32_t lut15[1 << 15], lut16[1 << 16];
static void build_luts(void)
{
    static bool done;
    if (done) return;
    for (uint32_t p = 0; p < (1u << 16); p++) {
        uint32_t r = p >> 11, gg = (p >> 5) & 63, b = p & 31;
        lut16[p] = ((r << 19 | r << 14) & 0xFF0000) | ((gg << 10 | gg << 4) & 0xFF00) | (b << 3 | b >> 2);
        if (p < (1u << 15)) {
            r = (p >> 10) & 31; gg = (p >> 5) & 31;
            lut15[p] = ((r << 19 | r << 14) & 0xFF0000) | ((gg << 11 | gg << 6) & 0xFF00) | (b << 3 | b >> 2);
        }
    }
    done = true;
}

static int depth_bytes(int depth) { return depth == 0 ? 1 : depth <= 2 ? 2 : depth == 3 ? 3 : 4; }

static void convert_row(uint32_t *out, const uint8_t *s, int w, int depth, const uint32_t *pal)
{
    switch (depth) {
    case 0: for (int x = 0; x < w; x++) out[x] = pal[s[x]]; break;
    case 1: for (int x = 0; x < w; x++) out[x] = lut15[(s[2 * x] | s[2 * x + 1] << 8) & 0x7FFF]; break;
    case 2: for (int x = 0; x < w; x++) out[x] = lut16[s[2 * x] | s[2 * x + 1] << 8]; break;
    case 3: for (int x = 0; x < w; x++) out[x] = (uint32_t)s[3 * x] | (uint32_t)s[3 * x + 1] << 8 | (uint32_t)s[3 * x + 2] << 16; break;
    default: for (int x = 0; x < w; x++) { uint32_t v; memcpy(&v, s + 4 * x, 4); out[x] = v & 0xFFFFFF; } break;
    }
}

// Only rows whose raw bytes changed are converted (fbrows.h). The cursor is drawn into fb:
// when it moves or changes, or a row under it is redrawn, all its rows are converted again
// and it is drawn once more (XGA mode inverts, so it must never be drawn twice on a row).
const uint32_t *g400_render(g400_t *g, int *pw, int *ph)
{
    if (!g400_active(g)) return NULL;
    int w = (g->crtc[1] + 1) * 8;
    int vd = g->crtc[0x12] | (g->crtc[7] & 0x02) << 7 | (g->crtc[7] & 0x40) << 3 | (g->crtcext[2] & 0x04) << 8;
    int h = vd + 1;
    if (w < 64 || h < 64 || w > 2048 || h > 1536) return NULL;
    uint32_t offset = g->crtc[0x13] | (uint32_t)(g->crtcext[0] & 0x30) << 4;
    uint32_t pitch = offset * 16;
    uint32_t start = (g->crtc[0x0D] | (uint32_t)g->crtc[0x0C] << 8 | (uint32_t)(g->crtcext[0] & 0x0F) << 16 |
                      (uint32_t)(g->crtcext[0] & 0x40) << 14) * 8;
    int depth = g->x[X_MULCTRL] & 7;
    if (w != g->fb_w || h != g->fb_h) {
        free(g->fb);
        g->fb = malloc((size_t)w * (size_t)h * 4);
        if (!g->fb) { g->fb_w = g->fb_h = 0; return NULL; }
        g->fb_w = w; g->fb_h = h;
        fbrows_invalidate(&g->rows);
    }
    build_luts();
    uint32_t pal[256];
    for (int i = 0; i < 256; i++) pal[i] = pal_rgb(g, (uint8_t)i);
    size_t row_bytes = (size_t)w * (size_t)depth_bytes(depth);
    uint32_t geo[5] = { (uint32_t)w, (uint32_t)h, (uint32_t)depth, pitch, start };
    uint64_t key = fbrows_hash(0, geo, sizeof geo);
    if (depth == 0) key = fbrows_hash(key, pal, sizeof pal);
    fbrows_begin(&g->rows, key, row_bytes, h);

    int cx = 0, cy = 0;
    uint64_t ckey;
    bool cur_on = cursor_state(g, &cx, &cy, &ckey);
    bool cur_changed = ckey != g->cur_key || !g->rows.valid;
    if (cur_changed) fbrows_touch(&g->rows, g->cur_y0, g->cur_y1);   // take the old cursor away

    const uint8_t *row_src[2048];
    for (int y = 0; y < h; y++) {
        uint64_t row = start + (uint64_t)y * pitch;
        uint32_t *out = g->fb + (size_t)y * (size_t)w;
        if (row + row_bytes > G400_VRAM_SIZE) {
            row_src[y] = NULL;
            if (!fbrows_same(&g->rows, y, NULL)) memset(out, 0, (size_t)w * 4);
            continue;
        }
        const uint8_t *s = g->vram + row;
        row_src[y] = s;
        if (fbrows_same(&g->rows, y, s)) continue;
        convert_row(out, s, w, depth, pal);
    }

    int cy0 = cur_on ? (cy < 0 ? 0 : cy) : 0, cy1 = cur_on ? (cy + 64 > h ? h : cy + 64) : 0;
    bool under = cy0 < cy1 && g->rows.y0 < cy1 && g->rows.y1 > cy0;   // something under it was redrawn
    if (cur_on && cy0 < cy1 && (cur_changed || under)) {
        for (int y = cy0; y < cy1; y++) {
            uint32_t *out = g->fb + (size_t)y * (size_t)w;
            if (row_src[y]) convert_row(out, row_src[y], w, depth, pal); else memset(out, 0, (size_t)w * 4);
        }
        draw_cursor(g, w, h);
        fbrows_mark(&g->rows, cy0, cy1);
    }
    fbrows_end(&g->rows);
    g->cur_key = ckey;
    g->cur_y0 = cy0;
    g->cur_y1 = cy1;
    g->dirty_y0 = g->rows.y0;
    g->dirty_y1 = g->rows.y1;
    *pw = w;
    *ph = h;
    return g->fb;
}

/* ------------------------------------------------------------------ */
/* Timing, PCI, setup                                                  */
/* ------------------------------------------------------------------ */

void g400_update(g400_t *g)
{
    uint64_t f = (now_ns(g) - g->frame_base_ns) / FRAME_NS;
    if (f != g->frames) {
        g->frames = f;
        g->status |= ST_VSYNCPEN | ST_VLINEPEN;
        update_irq(g);
    }
}

static void pci_map(pci_dev_t *d, int bar, uint32_t addr, bool on)
{
    g400_t *g = d->opaque;
    switch (bar) {
    case 0: if (on) mem_map_ram(g->bus, addr, G400_VRAM_SIZE, g->vram); else mem_unmap(g->bus, addr, G400_VRAM_SIZE); break;
    case 1: if (on) mem_map_mmio(g->bus, addr, 0x4000, ctl_read, ctl_write, g); else mem_unmap(g->bus, addr, 0x4000); break;
    case 2: if (on) mem_map_mmio(g->bus, addr, 0x800000, iload_read, iload_write, g); else mem_unmap(g->bus, addr, 0x800000); break;
    case PCI_ROM_BAR: if (on) mem_map_rom(g->bus, addr, G400_ROM_SIZE, g->rom); else mem_unmap(g->bus, addr, G400_ROM_SIZE); break;
    default: break;
    }
}

bool g400_init(g400_t *g, mem_bus_t *bus, uint64_t (*clock)(void *), void *clock_opaque)
{
    memset(g, 0, sizeof *g);
    g->bus = bus;
    g->clock = clock;
    g->clock_opaque = clock_opaque;
    g->vram = mem_alloc_host(G400_VRAM_SIZE);
    g->rom = mem_alloc_host(G400_ROM_SIZE);
    if (!g->vram || !g->rom) { g400_free(g); return false; }
    pci_dev_t *p = &g->pci;
    pci_set_ids(p, 0x102B, 0x0525, 0x04, 0x030000, 0x102B, 0x2179);   // Millennium G400 MAX
    pci_set_bar(p, 0, G400_VRAM_SIZE, false, true);
    pci_set_bar(p, 1, 0x4000, false, false);
    pci_set_bar(p, 2, 0x800000, false, false);
    pci_set_bar(p, PCI_ROM_BAR, G400_ROM_SIZE, false, false);
    p->cfg[0x3D] = 1;                                      // INTA#
    pci_set_wmask(p, 0x40, 0x20, 0xFF);                    // OPTION, MGA_INDEX/DATA, OPTION2, OPTION3
    p->map = pci_map;
    p->opaque = g;
    g400_reset(g);
    return true;
}

void g400_reset(g400_t *g)
{
    for (int bar = 0; bar < 7; bar++)
        if (g->pci.bar_on[bar]) { pci_map(&g->pci, bar, g->pci.bar_addr[bar], false); g->pci.bar_on[bar] = false; }
    pci_cfg_write(&g->pci, 0x04, 0, 2);
    for (int off = 0x10; off <= 0x18; off += 4) pci_cfg_write(&g->pci, off, 0, 4);
    pci_cfg_write(&g->pci, 0x30, 0, 4);
    memset(g->dwg, 0, sizeof g->dwg);
    memset(g->raw, 0, sizeof g->raw);
    memset(g->seq, 0, sizeof g->seq);
    memset(g->crtc, 0, sizeof g->crtc);
    memset(g->crtcext, 0, sizeof g->crtcext);
    memset(g->x, 0, sizeof g->x);
    g->status = g->ien = g->opmode = g->rst = 0;
    g->pixrdmsk = 0xFF;
    g->frame_base_ns = now_ns(g);
    g->frames = 0;
    update_irq(g);
}

void g400_free(g400_t *g)
{
    mem_free_host(g->vram);
    mem_free_host(g->rom);
    free(g->fb);
    fbrows_free(&g->rows);
    g->vram = g->rom = NULL;
    g->fb = NULL;
}
