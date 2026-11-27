#include "dma.h"
#include <string.h>

// Page register port for each channel.
static const uint8_t page_port[8] = { 0x87, 0x83, 0x81, 0x82, 0x8F, 0x8B, 0x89, 0x8A };

void dma_init(dma_t *d, mem_bus_t *bus)
{
    memset(d, 0, sizeof *d);
    d->bus = bus;
    dma_reset(d);
}

void dma_reset(dma_t *d)
{
    memset(d->ch, 0, sizeof d->ch);
    d->mask = 0xFF;
    d->status[0] = d->status[1] = 0;
    d->flipflop[0] = d->flipflop[1] = false;
    d->cmd[0] = d->cmd[1] = 0;
}

bool dma_masked(const dma_t *d, int ch) { return (d->mask >> ch) & 1; }

uint32_t dma_remaining(const dma_t *d, int ch)
{
    uint32_t units = (uint32_t)d->ch[ch].count + 1;
    return ch >= 4 ? units * 2 : units;
}

// Physical address of the channel's current unit.
static uint32_t cur_phys(const dma_t *d, int ch)
{
    const dma_chan_t *c = &d->ch[ch];
    if (ch >= 4) return (uint32_t)(c->page & 0xFE) << 16 | (uint32_t)c->addr << 1;   // words, 128 KB pages
    return (uint32_t)c->page << 16 | c->addr;
}

// Move one unit (byte or word) and advance; false at terminal count without auto-init.
static bool step(dma_t *d, int ch)
{
    dma_chan_t *c = &d->ch[ch];
    if (c->mode & 0x20) c->addr--; else c->addr++;     // bit 5: address decrement
    if (c->count-- == 0) {                              // terminal count
        d->status[ch >> 2] |= (uint8_t)(1u << (ch & 3));
        if (c->mode & 0x10) {                           // auto-initialize
            c->addr = c->base_addr;
            c->count = c->base_count;
            return true;
        }
        d->mask |= (uint8_t)(1u << ch);
        c->count = 0xFFFF;
        return false;
    }
    return true;
}

static int transfer(dma_t *d, int ch, uint8_t *buf, int len, bool to_mem)
{
    if (ch < 0 || ch > 7 || ch == 4 || dma_masked(d, ch)) return 0;
    int unit = ch >= 4 ? 2 : 1, done = 0;
    while (done + unit <= len) {
        uint32_t a = cur_phys(d, ch);
        if (to_mem) mem_write_block(d->bus, a, buf + done, (size_t)unit);
        else        mem_read_block(d->bus, a, buf + done, (size_t)unit);
        done += unit;
        if (!step(d, ch)) break;
    }
    return done;
}

int dma_read(dma_t *d, int ch, void *buf, int len)        { return transfer(d, ch, buf, len, false); }
int dma_write(dma_t *d, int ch, const void *buf, int len) { return transfer(d, ch, (uint8_t *)(uintptr_t)buf, len, true); }

/* ---- ports ---- */

// Controller k (0: ports 0x00-0x0F, byte registers; 1: 0xC0-0xDF, every other port), register r 0-15.
static uint8_t ctl_read(dma_t *d, int k, int r)
{
    if (r < 8) {
        dma_chan_t *c = &d->ch[k * 4 + (r >> 1)];
        uint16_t v = (r & 1) ? c->count : c->addr;
        bool hi = d->flipflop[k];
        d->flipflop[k] = !hi;
        return (uint8_t)(hi ? v >> 8 : v);
    }
    switch (r) {
    case 8: { uint8_t s = d->status[k]; d->status[k] = 0; return s; }   // reading clears TC bits
    case 13: return d->temp[k];
    case 15: return (uint8_t)((d->mask >> (4 * k)) & 0x0F);
    default: return 0xFF;
    }
}

static void ctl_write(dma_t *d, int k, int r, uint8_t v)
{
    if (r < 8) {
        dma_chan_t *c = &d->ch[k * 4 + (r >> 1)];
        uint16_t *base = (r & 1) ? &c->base_count : &c->base_addr;
        uint16_t *cur  = (r & 1) ? &c->count : &c->addr;
        bool hi = d->flipflop[k];
        d->flipflop[k] = !hi;
        *base = hi ? (uint16_t)((*base & 0x00FF) | v << 8) : (uint16_t)((*base & 0xFF00) | v);
        *cur = *base;
        return;
    }
    switch (r) {
    case 8:  d->cmd[k] = v; break;                                       // command
    case 9:  break;                                                      // request (memory-to-memory not supported)
    case 10: {                                                           // single mask bit
        int ch = k * 4 + (v & 3);
        if (v & 4) d->mask |= (uint8_t)(1u << ch); else d->mask &= (uint8_t)~(1u << ch);
        break;
    }
    case 11: d->ch[k * 4 + (v & 3)].mode = v & 0xFC; break;              // mode
    case 12: d->flipflop[k] = false; break;                              // clear byte pointer flip-flop
    case 13:                                                             // master clear
        d->flipflop[k] = false;
        d->status[k] = 0;
        d->cmd[k] = 0;
        d->mask |= (uint8_t)(0x0F << (4 * k));
        break;
    case 14: d->mask &= (uint8_t)~(0x0F << (4 * k)); break;              // clear all mask bits
    case 15: d->mask = (uint8_t)((d->mask & ~(0x0F << (4 * k))) | ((v & 0x0F) << (4 * k))); break;
    }
}

static uint32_t io_rd(void *o, uint16_t port, int size)
{
    dma_t *d = o;
    (void)size;
    if (port < 0x10) return ctl_read(d, 0, port);
    if (port >= 0xC0 && port < 0xE0) return (port & 1) ? 0xFF : ctl_read(d, 1, (port - 0xC0) >> 1);
    if (port >= 0x80 && port < 0x90) {
        for (int ch = 0; ch < 8; ch++) if (page_port[ch] == port) return d->ch[ch].page;
        return d->page_extra[port - 0x80];
    }
    return 0xFF;
}

static void io_wr(void *o, uint16_t port, uint32_t v, int size)
{
    dma_t *d = o;
    (void)size;
    if (port < 0x10) { ctl_write(d, 0, port, (uint8_t)v); return; }
    if (port >= 0xC0 && port < 0xE0) { if (!(port & 1)) ctl_write(d, 1, (port - 0xC0) >> 1, (uint8_t)v); return; }
    if (port >= 0x80 && port < 0x90) {
        for (int ch = 0; ch < 8; ch++) if (page_port[ch] == port) { d->ch[ch].page = (uint8_t)v; return; }
        d->page_extra[port - 0x80] = (uint8_t)v;
    }
}

void dma_register(dma_t *d, io_bus_t *io)
{
    io_register(io, 0x00, 0x10, io_rd, io_wr, d);
    io_register(io, 0x81, 0x03, io_rd, io_wr, d);   // 0x80 is the POST code port (machine.c)
    io_register(io, 0x87, 0x01, io_rd, io_wr, d);
    io_register(io, 0x89, 0x03, io_rd, io_wr, d);
    io_register(io, 0x8F, 0x01, io_rd, io_wr, d);
    io_register(io, 0xC0, 0x20, io_rd, io_wr, d);
}
