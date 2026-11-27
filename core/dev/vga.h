// VGA with Bochs VBE ("dispi") extensions on PCI (1234:1111, like QEMU's -vga std),
// which SeaVGABIOS's stdvga build and BeOS's stock VESA driver both understand.
//
// Memory model (simplified, documented in CLAUDE.md): the legacy window
// 0xA0000-0xBFFFF maps the first 128 KB of VRAM linearly, not through the four
// VGA planes. That is exact for VBE modes, mode 13h and text mode as SeaVGABIOS
// uses them (it loads the font through 0xA0000 with plane 2 selected, so glyphs
// land at VRAM offset char*32). Planar 16-color modes are not rendered yet.
#ifndef VGA_H
#define VGA_H

#include <stdbool.h>
#include <stdint.h>
#include "clock.h"
#include "io.h"
#include "mem.h"
#include "pci.h"
#include "fbrows.h"

#define VGA_VRAM_SIZE (16u << 20)

typedef struct {
    uint8_t  *vram;
    uint8_t  *rom;            // option ROM image, padded to rom_size
    uint32_t  rom_size;
    mem_bus_t *bus;
    void    (*remapped)(void *opaque);   // memory map changed (the JIT must flush)
    void     *remap_opaque;
    clock_fn  now;
    void     *now_opaque;

    uint8_t  misc, feature;
    uint8_t  seq_idx, seq[8];
    uint8_t  gc_idx, gc[16];
    uint8_t  crtc_idx, crtc[32];
    uint8_t  attr_idx, attr[32];
    bool     attr_flip;
    uint8_t  dac_mask, dac_read, dac_write, dac_rsub, dac_wsub, dac_latch[3];
    bool     dac_reading;
    uint8_t  dac[256][3];

    uint16_t dispi_idx;
    uint16_t dispi[16];
    uint32_t bank_mapped;     // bank currently mapped at 0xA0000, or ~0 for the 128 KB linear map

    pci_dev_t pci;

    uint32_t *fb;             // rendered frame
    int       fb_w, fb_h;
    int       dirty_y0, dirty_y1; // rows of fb that the last vga_render() changed: [y0, y1)
    fbrows_t  rows;           // VBE modes: raw rows already converted
} vga_t;

// rom may be NULL. Returns false on allocation failure.
bool vga_init(vga_t *v, mem_bus_t *bus, const uint8_t *rom, uint32_t rom_len,
              clock_fn now, void *now_opaque, void (*remapped)(void *), void *remap_opaque);
void vga_free(vga_t *v);
void vga_register(vga_t *v, io_bus_t *io);
// Render the current screen; the buffer stays valid until the next call. Afterwards
// dirty_y0..dirty_y1 are the rows that differ from the previous call's result.
const uint32_t *vga_render(vga_t *v, int *w, int *h);
// The text-mode screen as plain characters (rows separated by '\n'), for tests and logs.
int vga_text(vga_t *v, char *out, int out_len);

#endif
