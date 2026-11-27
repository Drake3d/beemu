// Matrox Millennium G400 MAX (PCI 102B:0525), for BeOS R5's stock g400 driver and accelerant.
//
// What the R5 driver uses (found by disassembling it; see CLAUDE.md):
//   - BAR 0 framebuffer (it maps 16 MB; the card has 32), BAR 1 the 16 KB register aperture
//   - config OPTION/OPTION2/OPTION3 (0x40/0x50/0x54) for memory and clock setup
//   - no BIOS: without a "PInS" block in the ROM it falls back to built-in clock limits
//   - VGA + CRTC extension registers through the aperture (0x1FC0-0x1FDF), the DAC at 0x3C00
//     (palette, indexed X registers: pixel format, PLLs, cursor, DDC pins), cursor position
//   - the 2D engine for RectangleFill/SpanFill/RectangleInvert (TRAP) and
//     ScreenToScreen(Trans)Blit (BITBLT), started by writing a drawing register + 0x100
//   - the vertical-line interrupt (IEN/ICLEAR bit 5) as its vertical-blank signal
// This model runs drawing operations to completion when they are started (no FIFO
// stalls, engine always idle), keeps the existing VGA card as the boot display, and is
// shown by the frontend once the driver switches the CRTC into MGA mode (CRTCEXT3 bit 7).
#ifndef G400_H
#define G400_H

#include <stdbool.h>
#include <stdint.h>
#include "io.h"
#include "mem.h"
#include "pci.h"
#include "fbrows.h"

#define G400_VRAM_SIZE  (32u << 20)
#define G400_ROM_SIZE   0x10000u

typedef struct g400 {
    pci_dev_t pci;
    mem_bus_t *bus;
    uint8_t  *vram;
    uint8_t  *rom;                  // all zero: no BIOS, so the driver uses its defaults

    // drawing engine (0x1C00-0x1CFF, one dword each)
    uint32_t dwg[64];
    // host interface
    uint32_t status, ien, opmode, rst;
    uint32_t raw[0x4000 / 4];       // registers kept as written (by dword), for everything else

    // VGA core through the aperture
    uint8_t  misc, seq_idx, seq[8], crtc_idx, crtc[0x40], crtcext_idx, crtcext[8];
    uint8_t  gctl_idx, gctl[16];

    // DAC
    uint8_t  pal[256][3];
    uint8_t  pal_w, pal_r, pal_sub_w, pal_sub_r, pixrdmsk;
    uint8_t  x[256];                // indexed X registers (PALWTADD selects, X_DATAREG accesses)
    uint8_t  curpos[4];             // CURPOSXL/XH/YL/YH

    // timing (60 Hz frames for the vertical-line interrupt and retrace status)
    uint64_t (*clock)(void *opaque);
    void     *clock_opaque;
    uint64_t  frame_base_ns;
    uint64_t  frames;

    // output
    uint32_t *fb;                   // XRGB render target
    int       fb_w, fb_h;
    int       dirty_y0, dirty_y1;   // rows the last g400_render() changed: [y0, y1)
    fbrows_t  rows;                 // raw rows already converted
    uint64_t  cur_key;              // hardware cursor state drawn into fb
    int       cur_y0, cur_y1;       // its rows

    void    (*set_intx)(void *opaque, int level);
    void     *intx_opaque;
    uint64_t  ops, unsupported_ops;
} g400_t;

bool g400_init(g400_t *g, mem_bus_t *bus, uint64_t (*clock)(void *), void *clock_opaque);
void g400_free(g400_t *g);
void g400_reset(g400_t *g);
void g400_update(g400_t *g);                    // vertical-line interrupt
bool g400_active(const g400_t *g);             // the driver has the CRTC in MGA mode
const uint32_t *g400_render(g400_t *g, int *w, int *h);   // sets dirty_y0/dirty_y1

// Register aperture access (exposed for tests).
uint32_t g400_reg_read(g400_t *g, uint32_t off, int size);
void     g400_reg_write(g400_t *g, uint32_t off, uint32_t v, int size);

#endif
