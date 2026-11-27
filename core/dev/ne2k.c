#include "ne2k.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool trace_nic(void)
{
    static int t = -1;
    if (t < 0) { const char *e = getenv("BEEMU_TRACE_NIC"); t = e && *e == '1'; }
    return t;
}

enum {   // CR
    CR_STP = 0x01, CR_STA = 0x02, CR_TXP = 0x04,
    CR_RD_READ = 0x08, CR_RD_WRITE = 0x10, CR_RD_ABORT = 0x20,
};
enum {   // ISR
    ISR_PRX = 0x01, ISR_PTX = 0x02, ISR_RXE = 0x04, ISR_TXE = 0x08,
    ISR_OVW = 0x10, ISR_CNT = 0x20, ISR_RDC = 0x40, ISR_RST = 0x80,
};
enum { RCR_AB = 0x04, RCR_AM = 0x08, RCR_PRO = 0x10, RCR_MON = 0x20 };
enum { RSR_PRX = 0x01, RSR_PHY = 0x20 };
enum { TSR_PTX = 0x01 };
enum { DCR_WTS = 0x01 };

#define MAX_FRAME 1514

/* ------------------------------------------------------------------ */

static void update_irq(ne2k_t *n)
{
    if (n->set_intx) n->set_intx(n->intx_opaque, (n->isr & n->imr & 0x7F) != 0);
}

static void soft_reset(ne2k_t *n)
{
    n->cr = CR_STP | CR_RD_ABORT;
    n->isr = ISR_RST;
    n->imr = 0;
    n->tsr = n->rsr = 0;
    n->rcr = n->tcr = n->dcr = 0;
    n->rsar = n->rbcr = 0;
    // The PROM: MAC, then 0x57 0x57 (word-wide card); each byte appears twice.
    uint8_t prom[16];
    memset(prom, 0, sizeof prom);
    memcpy(prom, n->mac, 6);
    prom[14] = prom[15] = 0x57;
    for (int i = 0; i < 16; i++) n->mem[2 * i] = n->mem[2 * i + 1] = prom[i];
    update_irq(n);
}

/* ---- buffer memory ---- */

static bool mem_ok(uint32_t a) { return a < 32 || (a >= NE2K_PMEM_START && a < NE2K_MEM_SIZE); }

static uint8_t mem_rd8(const ne2k_t *n, uint32_t a) { return mem_ok(a) ? n->mem[a] : 0xFF; }
static void mem_wr8(ne2k_t *n, uint32_t a, uint8_t v) { if (a >= NE2K_PMEM_START && a < NE2K_MEM_SIZE) n->mem[a] = v; }

// Advance the remote DMA after `len` bytes: wraps at PSTOP (so a driver can read a
// packet that wraps around the ring end), RDC when the count runs out.
static void dma_step(ne2k_t *n, unsigned len)
{
    n->rsar = (uint16_t)(n->rsar + len);
    if (n->rsar == n->pstop && n->pstop > n->pstart) n->rsar = n->pstart;
    if (n->rbcr <= len) {
        n->rbcr = 0;
        n->isr |= ISR_RDC;
        update_irq(n);
    } else {
        n->rbcr = (uint16_t)(n->rbcr - len);
    }
}

static uint32_t data_read(ne2k_t *n, int size)
{
    if (n->rbcr == 0) return size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFFu;
    if (size == 4) {   // two words
        uint32_t a = n->rsar & ~1u, v = 0;
        for (int i = 0; i < 4; i++) v |= (uint32_t)mem_rd8(n, (a + (uint32_t)i) & 0xFFFF) << (8 * i);
        dma_step(n, 4);
        return v;
    }
    if (n->dcr & DCR_WTS) {
        uint32_t a = n->rsar & ~1u;
        uint32_t v = mem_rd8(n, a) | (uint32_t)mem_rd8(n, a + 1) << 8;
        dma_step(n, 2);
        return v;
    }
    uint32_t v = mem_rd8(n, n->rsar);
    dma_step(n, 1);
    return v;
}

static void data_write(ne2k_t *n, uint32_t v, int size)
{
    if (n->rbcr == 0) return;
    if (size == 4) {
        uint32_t a = n->rsar & ~1u;
        for (int i = 0; i < 4; i++) mem_wr8(n, (a + (uint32_t)i) & 0xFFFF, (uint8_t)(v >> (8 * i)));
        dma_step(n, 4);
    } else if (n->dcr & DCR_WTS) {
        uint32_t a = n->rsar & ~1u;
        mem_wr8(n, a, (uint8_t)v);
        mem_wr8(n, a + 1, (uint8_t)(v >> 8));
        dma_step(n, 2);
    } else {
        mem_wr8(n, n->rsar, (uint8_t)v);
        dma_step(n, 1);
    }
}

/* ---- transmit ---- */

static void transmit(ne2k_t *n)
{
    uint32_t start = (uint32_t)n->tpsr << 8, len = n->tbcr;
    if (start >= NE2K_MEM_SIZE) start -= 0x8000;     // QEMU's NetWare 3.11 workaround
    if (start >= NE2K_PMEM_START && start + len <= NE2K_MEM_SIZE && len) {
        n->tx_frames++;
        if (n->send) n->send(n->send_opaque, n->mem + start, len);
    }
    n->tsr = TSR_PTX;
    n->isr |= ISR_PTX;
    n->cr &= (uint8_t)~CR_TXP;
    update_irq(n);
}

/* ---- receive ---- */

// The DP8390 multicast hash: top 6 bits of the big-endian CRC-32 of the destination.
static unsigned mcast_index(const uint8_t *mac)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (int i = 0; i < 6; i++) {
        uint8_t b = mac[i];
        for (int k = 0; k < 8; k++, b >>= 1) {
            uint32_t carry = ((crc >> 31) ^ b) & 1;
            crc <<= 1;
            if (carry) crc ^= 0x04C11DB7u;
        }
    }
    return crc >> 26;
}

static bool accept(const ne2k_t *n, const uint8_t *f)
{
    static const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    if (n->rcr & RCR_PRO) return true;
    if (!memcmp(f, BCAST, 6)) return (n->rcr & RCR_AB) != 0;
    if (f[0] & 1) {
        if (!(n->rcr & RCR_AM)) return false;
        unsigned h = mcast_index(f);
        return (n->mar[h >> 3] >> (h & 7)) & 1;
    }
    return !memcmp(f, n->par, 6);
}

static bool ring_valid(const ne2k_t *n)
{
    return n->pstart >= NE2K_PMEM_START && n->pstop <= NE2K_MEM_SIZE && n->pstop > n->pstart;
}

// 256-byte pages a frame of `len` bytes takes: header + frame + room for a CRC.
static uint32_t pages_for(size_t len)
{
    if (len < 60) len = 60;
    return (uint32_t)((len + 4 + 4 + 255) >> 8);
}

// Free pages between CURR and BNRY. CURR may never catch up with BNRY, so a frame fits
// only if it needs fewer pages than this.
static uint32_t free_pages(const ne2k_t *n)
{
    uint32_t ring = (uint32_t)(n->pstop - n->pstart) >> 8;
    uint32_t cur = n->curr, bnd = n->bnry;
    uint32_t first = n->pstart >> 8;
    if (cur < first || cur >= first + ring) cur = first;
    if (bnd < first || bnd >= first + ring) bnd = first;
    if (cur < bnd) return bnd - cur;
    return ring - (cur - bnd);
}

bool ne2k_can_receive(const ne2k_t *n, size_t len)
{
    if (n->cr & CR_STP) return true;                 // stopped: frames are dropped
    if (!ring_valid(n)) return false;
    return pages_for(len) < free_pages(n);
}

bool ne2k_receive(ne2k_t *n, const uint8_t *frame, size_t len)
{
    if ((n->cr & CR_STP) || len < 14 || len > MAX_FRAME + 4) { n->rx_dropped++; return true; }
    uint8_t pkt[MAX_FRAME + 4];
    memcpy(pkt, frame, len);
    if (len < 60) { memset(pkt + len, 0, 60 - len); len = 60; }   // pad runts like a real MAC
    if (!accept(n, pkt)) return true;
    if (n->rcr & RCR_MON) return true;               // monitor mode: checked, not stored
    if (!ring_valid(n) || pages_for(len) >= free_pages(n)) {
        n->isr |= ISR_OVW;
        n->rx_dropped++;
        update_irq(n);
        return false;
    }
    uint32_t index = (uint32_t)n->curr << 8;
    if (index < n->pstart || index >= n->pstop) index = n->pstart;
    uint32_t total = (uint32_t)len + 4;
    uint32_t next = index + (pages_for(len) << 8);
    if (next >= n->pstop) next -= n->pstop - n->pstart;
    n->rsr = RSR_PRX | ((pkt[0] & 1) ? RSR_PHY : 0);
    uint8_t *p = n->mem + index;
    p[0] = n->rsr;
    p[1] = (uint8_t)(next >> 8);
    p[2] = (uint8_t)total;
    p[3] = (uint8_t)(total >> 8);
    index += 4;
    const uint8_t *src = pkt;
    size_t left = len;
    while (left) {                                   // the frame, wrapping at PSTOP
        if (index >= n->pstop) index = n->pstart;
        size_t chunk = n->pstop - index;
        if (chunk > left) chunk = left;
        memcpy(n->mem + index, src, chunk);
        src += chunk; index += (uint32_t)chunk; left -= chunk;
    }
    n->curr = (uint8_t)(next >> 8);
    n->isr |= ISR_PRX;
    n->rx_frames++;
    update_irq(n);
    return true;
}

/* ---- registers ---- */

static uint8_t reg_read8(ne2k_t *n, unsigned off)
{
    if (off == 0) return n->cr;
    switch ((n->cr >> 6) << 4 | off) {
    case 0x03: return n->bnry;
    case 0x04: return n->tsr;
    case 0x07: return n->isr;
    case 0x08: return (uint8_t)n->rsar;              // CRDA: current remote DMA address
    case 0x09: return (uint8_t)(n->rsar >> 8);
    case 0x0A: return 0x50;                          // RTL8029 ID: 'P'
    case 0x0B: return 0x43;                          //             'C'
    case 0x0C: return n->rsr;
    case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: return n->par[off - 1];
    case 0x17: return n->curr;
    case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F: return n->mar[off - 8];
    case 0x21: return (uint8_t)(n->pstart >> 8);
    case 0x22: return (uint8_t)(n->pstop >> 8);
    case 0x24: return n->tpsr;
    case 0x2C: return n->rcr;
    case 0x2D: return n->tcr;
    case 0x2E: return n->dcr;
    case 0x2F: return n->imr;
    case 0x35: return 0x40;                          // RTL8029 CONFIG2: 10BASE-T
    case 0x36: return 0x40;                          // CONFIG3: full duplex
    default:   return 0;
    }
}

static void reg_write8(ne2k_t *n, unsigned off, uint8_t v)
{
    if (off == 0) {
        n->cr = v;
        if (v & CR_STP) { n->isr |= ISR_RST; return; }
        n->isr &= (uint8_t)~ISR_RST;
        if ((v & (CR_RD_READ | CR_RD_WRITE)) && n->rbcr == 0) {   // zero-length remote DMA
            n->isr |= ISR_RDC;
            update_irq(n);
        }
        if (v & CR_TXP) transmit(n);
        return;
    }
    switch ((n->cr >> 6) << 4 | off) {
    case 0x01: if ((uint32_t)v << 8 <= NE2K_MEM_SIZE) n->pstart = (uint16_t)(v << 8); break;
    case 0x02: if ((uint32_t)v << 8 <= NE2K_MEM_SIZE) n->pstop = (uint16_t)(v << 8); break;
    case 0x03: n->bnry = v; break;
    case 0x04: n->tpsr = v; break;
    case 0x05: n->tbcr = (uint16_t)((n->tbcr & 0xFF00) | v); break;
    case 0x06: n->tbcr = (uint16_t)((n->tbcr & 0x00FF) | v << 8); break;
    case 0x07: n->isr &= (uint8_t)~(v & 0x7F); update_irq(n); break;   // write 1 to clear (not RST)
    case 0x08: n->rsar = (uint16_t)((n->rsar & 0xFF00) | v); break;
    case 0x09: n->rsar = (uint16_t)((n->rsar & 0x00FF) | v << 8); break;
    case 0x0A: n->rbcr = (uint16_t)((n->rbcr & 0xFF00) | v); break;
    case 0x0B: n->rbcr = (uint16_t)((n->rbcr & 0x00FF) | v << 8); break;
    case 0x0C: n->rcr = v; break;
    case 0x0D: n->tcr = v; break;
    case 0x0E: n->dcr = v; break;
    case 0x0F: n->imr = v; update_irq(n); break;
    case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: n->par[off - 1] = v; break;
    case 0x17: n->curr = v; break;
    case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F: n->mar[off - 8] = v; break;
    default: break;
    }
}

static uint32_t io_rd(void *o, uint16_t port, int size)
{
    ne2k_t *n = o;
    unsigned off = (unsigned)(port - n->io_base) & 0x1F;
    uint32_t v = 0;
    if (off >= 0x18) { soft_reset(n); v = 0; }       // reset port
    else if (off >= 0x10) v = data_read(n, size);
    else for (int i = 0; i < size; i++) v |= (uint32_t)reg_read8(n, (off + (unsigned)i) & 0x0F) << (8 * i);
    if (trace_nic() && off != 0x07 && (off < 0x10 || off >= 0x18))
        fprintf(stderr, "[ne2k] rd%d %02X = %0*X (page %d)\n", size * 8, off, size * 2, v, n->cr >> 6);
    return v;
}

static void io_wr(void *o, uint16_t port, uint32_t v, int size)
{
    ne2k_t *n = o;
    unsigned off = (unsigned)(port - n->io_base) & 0x1F;
    if (trace_nic() && off < 0x10)
        fprintf(stderr, "[ne2k] wr%d %02X = %0*X (page %d)\n", size * 8, off, size * 2, v, n->cr >> 6);
    if (off >= 0x18) return;                         // writing the reset port ends the reset pulse
    if (off >= 0x10) { data_write(n, v, size); return; }
    for (int i = 0; i < size; i++) reg_write8(n, (off + (unsigned)i) & 0x0F, (uint8_t)(v >> (8 * i)));
}

static void pci_map(pci_dev_t *d, int bar, uint32_t addr, bool on)
{
    ne2k_t *n = d->opaque;
    if (bar != 0) return;
    if (on) { n->io_base = (uint16_t)addr; io_register(n->io, (uint16_t)addr, 32, io_rd, io_wr, n); }
    else    io_unregister(n->io, (uint16_t)addr, 32);
}

void ne2k_init(ne2k_t *n, io_bus_t *io, const uint8_t mac[6])
{
    memset(n, 0, sizeof *n);
    n->io = io;
    memcpy(n->mac, mac, 6);
    pci_dev_t *p = &n->pci;
    pci_set_ids(p, 0x10EC, 0x8029, 0x00, 0x020000, 0x0000, 0x0000);
    pci_set_bar(p, 0, 32, true, false);
    p->cfg[0x3D] = 1;                                // INTA#
    p->map = pci_map;
    p->opaque = n;
    soft_reset(n);
}

void ne2k_reset(ne2k_t *n)
{
    if (n->pci.bar_on[0]) { pci_map(&n->pci, 0, n->pci.bar_addr[0], false); n->pci.bar_on[0] = false; }
    pci_cfg_write(&n->pci, 0x04, 0, 2);
    pci_cfg_write(&n->pci, 0x10, 0, 4);
    pci_cfg_write(&n->pci, 0x3C, 0, 1);
    memset(n->par, 0, sizeof n->par);
    memset(n->mar, 0, sizeof n->mar);
    n->pstart = n->pstop = 0;
    n->bnry = n->curr = n->tpsr = 0;
    n->tbcr = 0;
    soft_reset(n);
}
