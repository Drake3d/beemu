#include "pci.h"
#include <string.h>

void pci_bus_init(pci_bus_t *b) { memset(b, 0, sizeof *b); }

void pci_add(pci_bus_t *b, int slot, int fn, pci_dev_t *d) { b->dev[slot & 31][fn & 7] = d; }

void pci_set_wmask(pci_dev_t *d, int off, int len, uint8_t mask)
{
    for (int i = 0; i < len && off + i < 256; i++) d->wmask[off + i] = mask;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

void pci_set_ids(pci_dev_t *d, uint16_t vendor, uint16_t device, uint8_t rev,
                 uint32_t class_code, uint16_t sub_vendor, uint16_t sub_device)
{
    put16(&d->cfg[0x00], vendor);
    put16(&d->cfg[0x02], device);
    d->cfg[0x08] = rev;
    d->cfg[0x09] = (uint8_t)class_code;
    d->cfg[0x0A] = (uint8_t)(class_code >> 8);
    d->cfg[0x0B] = (uint8_t)(class_code >> 16);
    put16(&d->cfg[0x2C], sub_vendor);
    put16(&d->cfg[0x2E], sub_device);
    pci_set_wmask(d, 0x04, 1, 0x07);      // command: I/O, memory, bus master
    pci_set_wmask(d, 0x05, 1, 0x04);      //          interrupt disable
    pci_set_wmask(d, 0x0C, 1, 0xFF);      // cache line size
    pci_set_wmask(d, 0x0D, 1, 0xFF);      // latency timer
    pci_set_wmask(d, 0x3C, 1, 0xFF);      // interrupt line
}

static int bar_off(int bar) { return bar == PCI_ROM_BAR ? 0x30 : 0x10 + 4 * bar; }

void pci_set_bar(pci_dev_t *d, int bar, uint32_t size, bool io, bool prefetch)
{
    d->bar_size[bar] = size;
    d->bar_io[bar] = io;
    int off = bar_off(bar);
    uint32_t mask;
    if (bar == PCI_ROM_BAR) { mask = ~(size - 1) | 1u; put32(&d->cfg[off], 0); }
    else if (io)            { mask = ~(size - 1) & ~3u; put32(&d->cfg[off], 1); }
    else                    { mask = ~(size - 1) & ~0xFu; put32(&d->cfg[off], prefetch ? 8u : 0u); }
    for (int i = 0; i < 4; i++) d->wmask[off + i] = (uint8_t)(mask >> (8 * i));
}

// Recompute where each BAR is decoded and tell the device about changes.
static void update_bars(pci_dev_t *d)
{
    uint16_t cmd = (uint16_t)(d->cfg[4] | d->cfg[5] << 8);
    for (int bar = 0; bar < 7; bar++) {
        if (!d->bar_size[bar]) continue;
        uint32_t v = get32(&d->cfg[bar_off(bar)]);
        uint32_t addr;
        bool on;
        if (bar == PCI_ROM_BAR) { addr = v & ~0x7FFu; on = (v & 1) && (cmd & 2); }
        else if (d->bar_io[bar]) { addr = v & ~3u; on = cmd & 1; }
        else                     { addr = v & ~0xFu; on = cmd & 2; }
        addr &= ~(d->bar_size[bar] - 1);
        if (addr == 0 || addr == ~(d->bar_size[bar] - 1)) on = false;   // unassigned or being sized
        if (on != d->bar_on[bar] || (on && addr != d->bar_addr[bar])) {
            if (d->bar_on[bar] && d->map) {
                uint32_t old = d->bar_addr[bar], end = old + (d->bar_size[bar] - 1);
                d->map(d, bar, old, false);
                // Unmapping took the whole range off the bus, including any other BAR it was
                // overlapping (drivers enable the ROM BAR on top of the framebuffer to read
                // the BIOS): map those again.
                for (int o = 0; o < 7; o++) {
                    if (o == bar || !d->bar_on[o] || d->bar_io[o] != d->bar_io[bar]) continue;
                    uint32_t oa = d->bar_addr[o], oe = oa + (d->bar_size[o] - 1);
                    if (oa <= end && old <= oe) d->map(d, o, oa, true);
                }
            }
            d->bar_on[bar] = on;
            d->bar_addr[bar] = addr;
            if (on && d->map) d->map(d, bar, addr, true);
        }
    }
}

uint32_t pci_cfg_read(pci_dev_t *d, int off, int size)
{
    uint32_t v = 0;
    for (int i = 0; i < size && off + i < 256; i++) v |= (uint32_t)d->cfg[off + i] << (8 * i);
    return v;
}

void pci_cfg_write(pci_dev_t *d, int off, uint32_t v, int size)
{
    for (int i = 0; i < size && off + i < 256; i++) {
        uint8_t m = d->wmask[off + i];
        uint8_t b = (uint8_t)(v >> (8 * i));
        d->cfg[off + i] = (uint8_t)((d->cfg[off + i] & ~m) | (b & m));
    }
    if (off < 0x34 && off + size > 0x04) update_bars(d);
    if (d->written) d->written(d, off, size);
}

static pci_dev_t *addressed(pci_bus_t *b, int *reg)
{
    uint32_t a = b->addr;
    if (!(a & 0x80000000u) || ((a >> 16) & 0xFF) != 0) return NULL;   // bus 0 only
    *reg = (int)(a & 0xFC);
    return b->dev[(a >> 11) & 31][(a >> 8) & 7];
}

static uint32_t cfg_read(void *opaque, uint16_t port, int size)
{
    pci_bus_t *b = opaque;
    if (port == 0xCF8 && size == 4) return b->addr;
    if (port == 0xCF9 && size == 1) return b->rcr;
    if (port < 0xCFC) return 0xFFFFFFFFu;
    int reg;
    pci_dev_t *d = addressed(b, &reg);
    if (!d) return 0xFFFFFFFFu;
    return pci_cfg_read(d, reg + (port - 0xCFC), size);
}

static void cfg_write(void *opaque, uint16_t port, uint32_t v, int size)
{
    pci_bus_t *b = opaque;
    if (port == 0xCF8 && size == 4) { b->addr = v; return; }
    if (port == 0xCF9 && size == 1) {                         // reset control register
        if ((v & 4) && !(b->rcr & 4) && b->reset) b->reset(b->reset_opaque);
        b->rcr = (uint8_t)(v & 0x0A);
        return;
    }
    if (port < 0xCFC) return;
    int reg;
    pci_dev_t *d = addressed(b, &reg);
    if (d) pci_cfg_write(d, reg + (port - 0xCFC), v, size);
}

void pci_bus_register(pci_bus_t *b, io_bus_t *io)
{
    io_register(io, 0xCF8, 8, cfg_read, cfg_write, b);
}
