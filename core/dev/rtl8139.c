#include "rtl8139.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool trace_nic(void)
{
    static int t = -1;
    if (t < 0) { const char *e = getenv("BEEMU_TRACE_NIC"); t = e && *e == '1'; }
    return t;
}

/* Register offsets (RTL8139C datasheet). */
enum {
    R_IDR = 0x00, R_MAR = 0x08, R_TSD = 0x10, R_TSAD = 0x20, R_RBSTART = 0x30,
    R_ERBCR = 0x34, R_ERSR = 0x36, R_CR = 0x37, R_CAPR = 0x38, R_CBR = 0x3A,
    R_IMR = 0x3C, R_ISR = 0x3E, R_TCR = 0x40, R_RCR = 0x44, R_TCTR = 0x48,
    R_MPC = 0x4C, R_9346 = 0x50, R_CONFIG0 = 0x51, R_CONFIG1 = 0x52,
    R_TIMERINT = 0x54, R_MSR = 0x58, R_CONFIG3 = 0x59, R_CONFIG4 = 0x5A,
    R_MULINT = 0x5C, R_RERID = 0x5E, R_TSAD_ALL = 0x60, R_BMCR = 0x62,
    R_BMSR = 0x64, R_ANAR = 0x66, R_ANLPAR = 0x68, R_ANER = 0x6A,
    R_CONFIG5 = 0xD8,
};

enum { CR_BUFE = 0x01, CR_TE = 0x04, CR_RE = 0x08, CR_RST = 0x10 };

enum {
    ISR_ROK = 0x0001, ISR_RER = 0x0002, ISR_TOK = 0x0004, ISR_TER = 0x0008,
    ISR_RXOVW = 0x0010, ISR_LINKCHG = 0x0020, ISR_FOVW = 0x0040,
    ISR_TIMEOUT = 0x4000, ISR_SERR = 0x8000,
};

enum { TSD_SIZE = 0x1FFF, TSD_OWN = 0x2000, TSD_TUN = 0x4000, TSD_TOK = 0x8000 };

enum { RCR_AAP = 0x01, RCR_APM = 0x02, RCR_AM = 0x04, RCR_AB = 0x08, RCR_WRAP = 0x80 };

enum { RX_ROK = 0x0001, RX_BAR = 0x2000, RX_PAM = 0x4000, RX_MAR = 0x8000 };

#define TCR_HWVER_MASK 0x7CC00000u
#define TCR_HWVER_8139C 0x74000000u
#define TCR_LOOPBACK   0x00060000u     // LBK1:0 = 11

/* ------------------------------------------------------------------ */

static void update_irq(rtl8139_t *n)
{
    if (n->set_intx) n->set_intx(n->intx_opaque, (n->isr & n->imr) != 0);
}

static uint32_t rx_size(const rtl8139_t *n) { return 8192u << ((n->rcr >> 11) & 3); }

// Bytes the driver has not consumed yet are not free. Empty ring = whole size.
static uint32_t rx_free(const rtl8139_t *n)
{
    uint32_t size = rx_size(n);
    uint32_t avail = (n->rx_read + size - n->rx_write) % size;
    return avail ? avail : size;
}

static bool rx_empty(const rtl8139_t *n) { return n->rx_read % rx_size(n) == n->rx_write; }

static uint32_t crc32_ether(const uint8_t *p, size_t len)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static uint64_t now_ns(const rtl8139_t *n) { return n->clock ? n->clock(n->clock_opaque) : 0; }

static uint32_t tctr(const rtl8139_t *n)
{
    return (uint32_t)((now_ns(n) - n->tctr_base_ns) * 33 / 1000);   // 33 MHz PCI clock
}

/* ---- 93C46 EEPROM (64 x 16 bits), read-only here ---- */

static void ee_clock(rtl8139_t *n, bool di)
{
    switch (n->ee_state) {
    case 0:                                           // waiting for the start bit
        if (di) { n->ee_state = 1; n->ee_bits = 0; n->ee_count = 0; }
        break;
    case 1:                                           // 2 opcode bits + 6 address bits
        n->ee_bits = (n->ee_bits << 1) | (di ? 1u : 0u);
        if (++n->ee_count == 8) {
            n->ee_addr = (uint8_t)(n->ee_bits & 63);
            if ((n->ee_bits >> 6) == 2) {             // READ: a dummy 0, then 16 bits MSB first
                n->ee_state = 2;
                n->ee_out = n->eeprom[n->ee_addr];
                n->ee_count = 0;
                n->ee_do = false;
            } else {
                n->ee_state = 3;                      // EWEN/EWDS/WRITE/ERASE: not kept
            }
        }
        break;
    case 2:
        n->ee_do = (n->ee_out & 0x8000) != 0;
        n->ee_out = (uint16_t)(n->ee_out << 1);
        if (++n->ee_count == 16) {                    // sequential read continues
            n->ee_addr = (n->ee_addr + 1) & 63;
            n->ee_out = n->eeprom[n->ee_addr];
            n->ee_count = 0;
        }
        break;
    default:
        break;
    }
}

static void write_9346(rtl8139_t *n, uint8_t v)
{
    int eem = v >> 6;
    n->cfg9346 = v & 0xCE;
    if (eem == 1) {                                   // auto-load: MAC from the EEPROM, back to normal
        for (int i = 0; i < 3; i++) {
            n->mac[2 * i] = (uint8_t)n->eeprom[7 + i];
            n->mac[2 * i + 1] = (uint8_t)(n->eeprom[7 + i] >> 8);
        }
        n->cfg9346 &= 0x3F;
        n->ee_state = 0;
        return;
    }
    if (eem != 2) { n->ee_state = 0; n->ee_sk = false; return; }   // EEPROM pins only in programming mode
    bool cs = v & 0x08, sk = v & 0x04, di = v & 0x02;
    if (!cs) { n->ee_state = 0; n->ee_do = false; n->ee_sk = sk; return; }
    if (sk && !n->ee_sk) ee_clock(n, di);             // rising clock edge
    n->ee_sk = sk;
}

/* ---- reset ---- */

static void soft_reset(rtl8139_t *n)
{
    for (int i = 0; i < 3; i++) {                     // the chip reloads its MAC from the EEPROM
        n->mac[2 * i] = (uint8_t)n->eeprom[7 + i];
        n->mac[2 * i + 1] = (uint8_t)(n->eeprom[7 + i] >> 8);
    }
    for (int i = 0; i < 4; i++) n->tsd[i] = TSD_OWN;
    n->cr = 0;
    n->imr = n->isr = 0;
    n->tcr = 0;
    n->rcr = 0;
    n->rx_read = n->rx_write = 0;
    n->mpc = 0;
    n->cfg9346 = 0;
    n->ee_state = 0;
    n->timer_int = 0;
    n->timer_fired = false;
    n->tctr_base_ns = now_ns(n);
    n->config[0] = 0x00;          // no boot ROM
    n->config[1] = 0x0C;          // I/O and memory mapped registers
    n->config[3] = 0x01;          // fast back-to-back
    n->config[4] = 0x00;
    n->config[5] = 0x00;
    n->bmcr = 0x3100;             // auto-negotiation enabled, 100 Mb/s, full duplex
    n->anar = 0x05E1;
    n->mulint = 0;
    update_irq(n);
}

/* ---- transmit ---- */

static bool receive(rtl8139_t *n, const uint8_t *frame, size_t len, bool loopback);

static void transmit(rtl8139_t *n, int i)
{
    uint32_t v = n->tsd[i];
    if ((v & TSD_OWN) || !(n->cr & CR_TE)) return;
    uint32_t len = v & TSD_SIZE;
    if (len > 1792) len = 1792;                       // the chip's limit
    uint8_t buf[1792];
    mem_read_block(n->bus, n->tsad[i], buf, len);
    n->tsd[i] = (v & ~(uint32_t)TSD_TUN) | TSD_OWN | TSD_TOK;
    n->isr |= ISR_TOK;
    n->tx_frames++;
    if ((n->tcr & TCR_LOOPBACK) == TCR_LOOPBACK) receive(n, buf, len, true);
    else if (n->send && len) n->send(n->send_opaque, buf, len);
    update_irq(n);
}

/* ---- receive ---- */

static void rx_dma(rtl8139_t *n, uint32_t off, const void *data, uint32_t len)
{
    if (len) mem_write_block(n->bus, n->rbstart + off, data, len);
}

// Copy into the ring at CBR. Without RCR.WRAP the record wraps around the end;
// with it (rings below 64 KB) it runs on past the end into the driver's slack.
static void rx_put(rtl8139_t *n, const void *buf, uint32_t len)
{
    uint32_t size = rx_size(n);
    if (n->rx_write + len > size) {
        uint32_t wrapped = (n->rx_write + len) % size;
        if (wrapped && !(size < 65536 && (n->rcr & RCR_WRAP))) {
            rx_dma(n, n->rx_write, buf, len - wrapped);
            rx_dma(n, 0, (const uint8_t *)buf + (len - wrapped), wrapped);
            n->rx_write = wrapped;
            return;
        }
    }
    rx_dma(n, n->rx_write, buf, len);
    n->rx_write += len;
}

static uint16_t accept(const rtl8139_t *n, const uint8_t *frame)
{
    static const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    bool bcast = !memcmp(frame, BCAST, 6), mcast = !bcast && (frame[0] & 1);
    bool mine = !memcmp(frame, n->mac, 6);
    uint16_t st = RX_ROK | (bcast ? RX_BAR : 0) | (mcast ? RX_MAR : 0) | (mine ? RX_PAM : 0);
    if (n->rcr & RCR_AAP) return st;
    if (bcast) return (n->rcr & RCR_AB) ? st : 0;
    if (mcast) return (n->rcr & RCR_AM) ? st : 0;
    return (mine && (n->rcr & RCR_APM)) ? st : 0;
}

static uint32_t record_size(size_t len)
{
    if (len < 60) len = 60;
    return (uint32_t)((len + 4 + 4 + 3) & ~(size_t)3);     // header + frame + CRC, dword aligned
}

bool rtl8139_can_receive(const rtl8139_t *n, size_t len)
{
    if (!(n->cr & CR_RE)) return true;                // receiver off: frames are dropped
    return record_size(len) < rx_free(n);
}

static bool receive(rtl8139_t *n, const uint8_t *frame, size_t len, bool loopback)
{
    (void)loopback;
    if (!(n->cr & CR_RE) || len < 14 || len > 1514 + 4) { n->rx_dropped++; return true; }
    uint8_t pkt[1600];
    memcpy(pkt, frame, len);
    if (len < 60) { memset(pkt + len, 0, 60 - len); len = 60; }   // pad runts like a real MAC
    uint16_t st = accept(n, pkt);
    if (!st) return true;                             // filtered out
    if (record_size(len) >= rx_free(n)) {             // ring full: missed packet
        n->isr |= ISR_RXOVW;
        n->mpc = (n->mpc + 1) & 0xFFFFFF;
        n->rx_dropped++;
        update_irq(n);
        return false;
    }
    uint32_t crc = crc32_ether(pkt, len);
    memcpy(pkt + len, &crc, 4);                       // little-endian, as on the wire
    uint8_t hdr[4] = { (uint8_t)st, (uint8_t)(st >> 8), (uint8_t)(len + 4), (uint8_t)((len + 4) >> 8) };
    rx_put(n, hdr, 4);
    rx_put(n, pkt, (uint32_t)len + 4);
    n->rx_write = ((n->rx_write + 3) & ~3u) % rx_size(n);
    n->isr |= ISR_ROK;
    n->rx_frames++;
    update_irq(n);
    return true;
}

bool rtl8139_receive(rtl8139_t *n, const uint8_t *frame, size_t len) { return receive(n, frame, len, false); }

void rtl8139_update(rtl8139_t *n)
{
    if (n->timer_int && !n->timer_fired && tctr(n) >= n->timer_int) {
        n->timer_fired = true;
        n->isr |= ISR_TIMEOUT;
        update_irq(n);
    }
}

/* ---- register access (byte granular; wide accesses are composed) ---- */

static uint8_t byte_of(uint32_t v, unsigned b) { return (uint8_t)(v >> (8 * b)); }
static void set_byte(uint32_t *v, unsigned b, uint8_t x) { *v = (*v & ~(0xFFu << (8 * b))) | ((uint32_t)x << (8 * b)); }

static uint8_t reg_read8(rtl8139_t *n, uint32_t off)
{
    unsigned b = off & 3;
    if (off < 6) return n->mac[off];
    if (off >= R_MAR && off < R_MAR + 8) return n->mar[off - R_MAR];
    if (off >= R_TSD && off < R_TSD + 16) return byte_of(n->tsd[(off - R_TSD) / 4], b);
    if (off >= R_TSAD && off < R_TSAD + 16) return byte_of(n->tsad[(off - R_TSAD) / 4], b);
    if (off >= R_RBSTART && off < R_RBSTART + 4) return byte_of(n->rbstart, b);
    switch (off) {
    case R_ERBCR: case R_ERBCR + 1: case R_ERSR: return 0;
    case R_CR:   return (uint8_t)((n->cr & (CR_RE | CR_TE)) | (rx_empty(n) ? CR_BUFE : 0));
    case R_CAPR: case R_CAPR + 1: return byte_of((n->rx_read - 16) & 0xFFFF, off - R_CAPR);
    case R_CBR:  case R_CBR + 1:  return byte_of(n->rx_write, off - R_CBR);
    case R_IMR:  case R_IMR + 1:  return byte_of(n->imr, off - R_IMR);
    case R_ISR:  case R_ISR + 1:  return byte_of(n->isr, off - R_ISR);
    case R_9346: return (uint8_t)(n->cfg9346 | (n->ee_do ? 1 : 0));
    case R_CONFIG0: return n->config[0];
    case R_CONFIG1: return n->config[1];
    case R_CONFIG3: return n->config[3];
    case R_CONFIG4: return n->config[4];
    case R_CONFIG5: return n->config[5];
    case R_MSR:  return 0x00;                         // 100 Mb/s, link up
    case R_MULINT: case R_MULINT + 1: return byte_of(n->mulint, off - R_MULINT);
    case R_RERID: return 0x10;
    case R_RERID + 1: return 0;
    case R_TSAD_ALL: case R_TSAD_ALL + 1: {           // TOK3-0, TUN3-0, TABT3-0, OWN3-0
        uint32_t s = 0;
        for (int i = 0; i < 4; i++) {
            if (n->tsd[i] & TSD_TOK) s |= 1u << (12 + i);
            if (n->tsd[i] & TSD_TUN) s |= 1u << (8 + i);
            if (n->tsd[i] & TSD_OWN) s |= 1u << i;
        }
        return byte_of(s, off - R_TSAD_ALL);
    }
    case R_BMCR: case R_BMCR + 1: return byte_of(n->bmcr, off - R_BMCR);
    case R_BMSR: case R_BMSR + 1: return byte_of(0x782D, off - R_BMSR);   // capable, link up, autoneg done
    case R_ANAR: case R_ANAR + 1: return byte_of(n->anar, off - R_ANAR);
    case R_ANLPAR: case R_ANLPAR + 1: return byte_of(0x05E1, off - R_ANLPAR);
    case R_ANER: case R_ANER + 1: return byte_of(0x0001, off - R_ANER);
    default: break;
    }
    if (off >= R_TCR && off < R_TCR + 4) return byte_of((n->tcr & ~TCR_HWVER_MASK) | TCR_HWVER_8139C, b);
    if (off >= R_RCR && off < R_RCR + 4) return byte_of(n->rcr, b);
    if (off >= R_TCTR && off < R_TCTR + 4) return byte_of(tctr(n), b);
    if (off >= R_MPC && off < R_MPC + 4) return byte_of(n->mpc, b);
    if (off >= R_TIMERINT && off < R_TIMERINT + 4) return byte_of(n->timer_int, b);
    return byte_of(n->raw[off / 4], b);
}

static void reg_write8(rtl8139_t *n, uint32_t off, uint8_t v)
{
    unsigned b = off & 3;
    if (off < 6) { n->mac[off] = v; return; }
    if (off >= R_MAR && off < R_MAR + 8) { n->mar[off - R_MAR] = v; return; }
    if (off >= R_TSD && off < R_TSD + 16) {
        int i = (int)(off - R_TSD) / 4;
        set_byte(&n->tsd[i], b, v);
        if (b == 1) transmit(n, i);                   // the byte holding OWN starts the send
        return;
    }
    if (off >= R_TSAD && off < R_TSAD + 16) { set_byte(&n->tsad[(off - R_TSAD) / 4], b, v); return; }
    if (off >= R_RBSTART && off < R_RBSTART + 4) { set_byte(&n->rbstart, b, v); return; }
    switch (off) {
    case R_CR:
        if (v & CR_RST) { soft_reset(n); return; }
        n->cr = v & (CR_RE | CR_TE);
        return;
    case R_CAPR: case R_CAPR + 1: {
        uint32_t capr = (n->rx_read - 16) & 0xFFFF;
        set_byte(&capr, off - R_CAPR, v);
        n->rx_read = (capr + 16) % rx_size(n);
        return;
    }
    case R_IMR: case R_IMR + 1: {
        uint32_t x = n->imr;
        set_byte(&x, off - R_IMR, v);
        n->imr = (uint16_t)x;
        update_irq(n);
        return;
    }
    case R_ISR: case R_ISR + 1:                       // write 1 to clear
        n->isr &= (uint16_t)~((uint32_t)v << (8 * (off - R_ISR)));
        update_irq(n);
        return;
    case R_9346: write_9346(n, v); return;
    case R_CONFIG0: n->config[0] = v; return;
    case R_CONFIG1: n->config[1] = v; return;
    case R_CONFIG3: n->config[3] = v; return;
    case R_CONFIG4: n->config[4] = v; return;
    case R_CONFIG5: n->config[5] = v; return;
    case R_MULINT: case R_MULINT + 1: { uint32_t x = n->mulint; set_byte(&x, off - R_MULINT, v); n->mulint = (uint16_t)x; return; }
    case R_BMCR: case R_BMCR + 1: {
        uint32_t x = n->bmcr;
        set_byte(&x, off - R_BMCR, v);
        n->bmcr = (uint16_t)(x & 0x7FFF);             // PHY reset self-clears
        return;
    }
    case R_ANAR: case R_ANAR + 1: { uint32_t x = n->anar; set_byte(&x, off - R_ANAR, v); n->anar = (uint16_t)x; return; }
    default: break;
    }
    if (off >= R_TCR && off < R_TCR + 4) { set_byte(&n->tcr, b, v); n->tcr &= ~TCR_HWVER_MASK; return; }
    if (off >= R_RCR && off < R_RCR + 4) { set_byte(&n->rcr, b, v); n->rx_write %= rx_size(n); return; }
    if (off >= R_TCTR && off < R_TCTR + 4) { n->tctr_base_ns = now_ns(n); n->timer_fired = false; return; }
    if (off >= R_MPC && off < R_MPC + 4) { n->mpc = 0; return; }
    if (off >= R_TIMERINT && off < R_TIMERINT + 4) { set_byte(&n->timer_int, b, v); n->timer_fired = false; return; }
    set_byte(&n->raw[off / 4], b, v);
}

static uint32_t reg_read(rtl8139_t *n, uint32_t off, int size)
{
    uint32_t v = 0;
    for (int i = 0; i < size; i++) v |= (uint32_t)reg_read8(n, (off + (uint32_t)i) & 0xFF) << (8 * i);
    if (trace_nic() && off != R_9346 && off != R_ISR) fprintf(stderr, "[nic] rd%d %02X = %0*X\n", size * 8, off, size * 2, v);
    return v;
}

static void reg_write(rtl8139_t *n, uint32_t off, uint32_t v, int size)
{
    if (trace_nic() && off != R_9346) fprintf(stderr, "[nic] wr%d %02X = %0*X\n", size * 8, off, size * 2, v);
    if (size == 4 && off >= R_TSD && off < R_TSD + 16 && !(off & 3)) {   // the usual TSD write
        int i = (int)(off - R_TSD) / 4;
        n->tsd[i] = v;
        transmit(n, i);
        return;
    }
    for (int i = 0; i < size; i++) reg_write8(n, (off + (uint32_t)i) & 0xFF, (uint8_t)(v >> (8 * i)));
}

static uint32_t io_rd(void *o, uint16_t port, int size)
{
    rtl8139_t *n = o;
    return reg_read(n, (uint32_t)(port - n->io_base) & 0xFF, size);
}
static void io_wr(void *o, uint16_t port, uint32_t v, int size)
{
    rtl8139_t *n = o;
    reg_write(n, (uint32_t)(port - n->io_base) & 0xFF, v, size);
}
static uint32_t mmio_rd(void *o, uint32_t off, int size) { return reg_read(o, off & 0xFF, size); }
static void mmio_wr(void *o, uint32_t off, uint32_t v, int size) { reg_write(o, off & 0xFF, v, size); }

static void pci_map(pci_dev_t *d, int bar, uint32_t addr, bool on)
{
    rtl8139_t *n = d->opaque;
    if (bar == 0) {
        if (on) { n->io_base = (uint16_t)addr; io_register(n->io, (uint16_t)addr, 256, io_rd, io_wr, n); }
        else    io_unregister(n->io, (uint16_t)addr, 256);
    } else if (bar == 1) {
        if (on) mem_map_mmio(n->bus, addr, 4096, mmio_rd, mmio_wr, n);
        else    mem_unmap(n->bus, addr, 4096);
    }
}

void rtl8139_init(rtl8139_t *n, mem_bus_t *bus, io_bus_t *io, const uint8_t mac[6],
                  uint64_t (*clock)(void *), void *clock_opaque)
{
    memset(n, 0, sizeof *n);
    n->bus = bus;
    n->io = io;
    n->clock = clock;
    n->clock_opaque = clock_opaque;
    n->eeprom[0] = 0x8129;                            // what the EEPROM of a real board starts with
    n->eeprom[1] = 0x10EC;
    n->eeprom[2] = 0x8139;
    for (int i = 0; i < 3; i++) n->eeprom[7 + i] = (uint16_t)(mac[2 * i] | (mac[2 * i + 1] << 8));
    pci_dev_t *p = &n->pci;
    pci_set_ids(p, 0x10EC, 0x8139, 0x10, 0x020000, 0x10EC, 0x8139);
    pci_set_bar(p, 0, 256, true, false);
    pci_set_bar(p, 1, 4096, false, false);            // 256 bytes of registers, page-sized for the bus
    p->cfg[0x3D] = 1;                                 // INTA#
    p->cfg[0x3E] = 0x20;                              // min grant
    p->cfg[0x3F] = 0x40;                              // max latency
    p->map = pci_map;
    p->opaque = n;
    soft_reset(n);
}

void rtl8139_reset(rtl8139_t *n)
{
    for (int bar = 0; bar < 2; bar++)
        if (n->pci.bar_on[bar]) { pci_map(&n->pci, bar, n->pci.bar_addr[bar], false); n->pci.bar_on[bar] = false; }
    pci_cfg_write(&n->pci, 0x04, 0, 2);
    pci_cfg_write(&n->pci, 0x10, 0, 4);
    pci_cfg_write(&n->pci, 0x14, 0, 4);
    pci_cfg_write(&n->pci, 0x3C, 0, 1);
    soft_reset(n);
}
