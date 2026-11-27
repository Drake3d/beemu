#include "es1370.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    R_CTRL = 0x00, R_STATUS = 0x04, R_UART_DATA = 0x08, R_UART_STATUS = 0x09, R_MEMPAGE = 0x0C,
    R_CODEC = 0x10, R_SCTRL = 0x20, R_DAC1_SCOUNT = 0x24, R_DAC2_SCOUNT = 0x28, R_ADC_SCOUNT = 0x2C,
    R_PAGE = 0x30,
};
// CTRL
#define CTRL_ADC_EN    0x00000010u
#define CTRL_DAC2_EN   0x00000020u
#define CTRL_DAC1_EN   0x00000040u
// STATUS
#define STAT_ADC       0x00000001u
#define STAT_DAC2      0x00000002u
#define STAT_DAC1      0x00000004u
#define STAT_INTR      0x80000000u
// SCTRL
#define SCTRL_P1_INTEN 0x00000100u
#define SCTRL_P2_INTEN 0x00000200u
#define SCTRL_R1_INTEN 0x00000400u
#define SCTRL_P1_PAUSE 0x00000800u
#define SCTRL_P2_PAUSE 0x00001000u

enum { DAC1, DAC2, ADC };
static const uint32_t ch_en[3]    = { CTRL_DAC1_EN, CTRL_DAC2_EN, CTRL_ADC_EN };
static const uint32_t ch_stat[3]  = { STAT_DAC1, STAT_DAC2, STAT_ADC };
static const uint32_t ch_inten[3] = { SCTRL_P1_INTEN, SCTRL_P2_INTEN, SCTRL_R1_INTEN };

static bool trace_sound(void)
{
    static int on = -1;
    if (on < 0) on = getenv("BEEMU_TRACE_SOUND") != NULL;
    return on;
}

static void update_irq(es1370_t *e)
{
    bool on = (e->status & (STAT_DAC1 | STAT_DAC2 | STAT_ADC)) != 0;
    if (on) e->status |= STAT_INTR; else e->status &= ~STAT_INTR;
    if (e->set_intx) e->set_intx(e->intx_opaque, on);
}

static double ch_rate(const es1370_t *e, int c)
{
    static const double wtsr[4] = { 5512.5, 11025, 22050, 44100 };
    if (c == DAC1) return wtsr[(e->ctl >> 12) & 3];
    return 1411200.0 / (double)(((e->ctl >> 16) & 0x1FFF) + 2);     // DAC2 and ADC share the clock
}

static bool ch_stereo(const es1370_t *e, int c) { return (e->sctl >> (2 * c)) & 1; }
static bool ch_16bit(const es1370_t *e, int c)  { return (e->sctl >> (2 * c + 1)) & 1; }
static bool ch_running(const es1370_t *e, int c)
{
    if (!(e->ctl & ch_en[c])) return false;
    if (c == DAC1 && (e->sctl & SCTRL_P1_PAUSE)) return false;
    if (c == DAC2 && (e->sctl & SCTRL_P2_PAUSE)) return false;
    return true;
}

/* ---- registers ---- */

static uint32_t page_reg(es1370_t *e, uint32_t off, bool *known, uint32_t **p)
{
    *known = true;
    unsigned page = e->mempage & 0xF, slot = (off - R_PAGE) / 4;
    if (page == 0xC) {
        es1370_chan_t *c = &e->ch[slot < 2 ? DAC1 : DAC2];
        *p = slot & 1 ? &c->frame_cnt : &c->frame_addr;
        return **p;
    }
    if (page == 0xD && slot < 2) {
        *p = slot & 1 ? &e->ch[ADC].frame_cnt : &e->ch[ADC].frame_addr;
        return **p;
    }
    *known = false;          // UART FIFO pages and the rest: read as zero, writes ignored
    *p = NULL;
    return 0;
}

static uint32_t reg_get(es1370_t *e, uint32_t off)
{
    bool known; uint32_t *p;
    switch (off) {
    case R_CTRL:    return e->ctl;
    case R_STATUS:  return e->status;              // codec never busy (CSTAT/CBUSY/CWRIP 0)
    case R_UART_DATA: return 0x02u << 8;           // byte 0x09, UART status: TX ready, no RX
    case R_MEMPAGE: return e->mempage;
    case R_CODEC:   return 0;
    case R_SCTRL:   return e->sctl;
    case R_DAC1_SCOUNT: return e->ch[DAC1].scount;
    case R_DAC2_SCOUNT: return e->ch[DAC2].scount;
    case R_ADC_SCOUNT:  return e->ch[ADC].scount;
    default:
        if (off >= R_PAGE && off < 0x40) return page_reg(e, off, &known, &p);
        return 0;
    }
}

static void reg_set(es1370_t *e, uint32_t off, uint32_t v)
{
    bool known; uint32_t *p;
    switch (off) {
    case R_CTRL: {
        uint32_t newly = v & ~e->ctl;
        e->ctl = v;
        for (int c = 0; c < 3; c++)
            if (newly & ch_en[c]) {                // channel starts: count from the top
                es1370_chan_t *ch = &e->ch[c];
                ch->scount = (ch->scount & 0xFFFF) * 0x10001u;
                ch->sub = 0;
                ch->phase = 0;
                memset(ch->prev, 0, sizeof ch->prev);
                memset(ch->cur, 0, sizeof ch->cur);
            }
        break;
    }
    case R_STATUS: break;
    case R_UART_DATA: e->uart_ctl = (uint8_t)(v >> 8); break;      // byte 0x09 written: UART control
    case R_MEMPAGE: e->mempage = v & 0xF; break;
    case R_CODEC: {
        unsigned reg = (v >> 8) & 0xFF;
        if (reg < sizeof e->codec) { e->codec[reg] = (uint8_t)v; e->gain_dirty = true; }
        break;
    }
    case R_SCTRL: {
        // Turning a channel's interrupt enable off acknowledges its interrupt.
        for (int c = 0; c < 3; c++)
            if (!(v & ch_inten[c])) e->status &= ~ch_stat[c];
        e->sctl = v;
        update_irq(e);
        break;
    }
    case R_DAC1_SCOUNT: e->ch[DAC1].scount = (v & 0xFFFF) * 0x10001u; break;
    case R_DAC2_SCOUNT: e->ch[DAC2].scount = (v & 0xFFFF) * 0x10001u; break;
    case R_ADC_SCOUNT:  e->ch[ADC].scount  = (v & 0xFFFF) * 0x10001u; break;
    default:
        if (off >= R_PAGE && off < 0x40) {
            page_reg(e, off, &known, &p);
            if (known) {
                *p = v;
                for (int c = 0; c < 3; c++) if (p == &e->ch[c].frame_cnt) e->ch[c].sub = 0;
            }
        }
        break;
    }
}

uint32_t es1370_read(es1370_t *e, uint32_t off, int size)
{
    es1370_update(e);
    uint32_t d = reg_get(e, off & 0x3C);
    d >>= 8 * (off & 3);
    return size == 4 ? d : d & ((1u << (8 * size)) - 1);
}

void es1370_write(es1370_t *e, uint32_t off, uint32_t v, int size)
{
    es1370_update(e);
    if (trace_sound())
        fprintf(stderr, "[es1370 %.3f] wr%d %02X = %08X (page %X)\n", (double)(e->clock(e->clock_opaque) - e->t0) / 1e9,
                size * 8, off, v, e->mempage);
    uint32_t base = off & 0x3C, sh = 8 * (off & 3);
    uint32_t mask = size == 4 ? 0xFFFFFFFFu : ((1u << (8 * size)) - 1) << sh;
    uint32_t cur = base == R_CODEC ? 0 : reg_get(e, base);
    reg_set(e, base, (cur & ~mask) | ((v << sh) & mask));
}

static uint32_t io_rd(void *o, uint16_t port, int size) { es1370_t *e = o; return es1370_read(e, (uint32_t)(port - e->io_base), size); }
static void io_wr(void *o, uint16_t port, uint32_t v, int size) { es1370_t *e = o; es1370_write(e, (uint32_t)(port - e->io_base), v, size); }

/* ---- sample transport ---- */

// Next sample frame of channel c from guest memory (or, for the ADC, silence into it).
static void fetch(es1370_t *e, int c)
{
    es1370_chan_t *ch = &e->ch[c];
    bool st = ch_stereo(e, c), w16 = ch_16bit(e, c);
    uint32_t bytes = (st ? 2u : 1u) * (w16 ? 2u : 1u);
    uint32_t cnt = ch->frame_cnt >> 16, size = ch->frame_cnt & 0xFFFF;
    uint32_t addr = ch->frame_addr + cnt * 4 + ch->sub;
    uint8_t b[4] = { 0 };
    if (c == ADC) {
        uint8_t z[4] = { 0 };
        if (!w16) memset(z, 0x80, sizeof z);
        mem_write_block(e->bus, addr, z, bytes);
    } else {
        mem_read_block(e->bus, addr, b, bytes);
        memcpy(ch->prev, ch->cur, sizeof ch->prev);
        int16_t l, r;
        if (w16) { l = (int16_t)(b[0] | b[1] << 8); r = st ? (int16_t)(b[2] | b[3] << 8) : l; }
        else     { l = (int16_t)((b[0] - 128) * 256); r = st ? (int16_t)((b[1] - 128) * 256) : l; }
        ch->cur[0] = l; ch->cur[1] = r;
    }
    ch->sub += bytes;
    while (ch->sub >= 4) {
        ch->sub -= 4;
        if (++cnt > size) cnt = 0;                 // the buffer loops
    }
    ch->frame_cnt = (ch->frame_cnt & 0xFFFF) | cnt << 16;
    // Sample counter: an interrupt every (low half + 1) samples.
    uint32_t csc = ch->scount >> 16;
    if (csc == 0) {
        if (e->sctl & ch_inten[c]) {
            if (trace_sound() && !(e->status & ch_stat[c]))
                fprintf(stderr, "[es1370 %.3f] irq ch%d at dword %u (frame_cnt %08X scount %08X sub %u)\n",
                        (double)e->produced / SOUND_RATE, c, cnt, ch->frame_cnt, ch->scount, ch->sub);
            else if (trace_sound()) fprintf(stderr, "[es1370 %.3f] irq ch%d at dword %u while previous still pending\n",
                                            (double)e->produced / SOUND_RATE, c, cnt);
            e->status |= ch_stat[c];
        }
        csc = ch->scount & 0xFFFF;
    } else {
        csc--;
    }
    ch->scount = (ch->scount & 0xFFFF) | csc << 16;
}

// AK4531 gains as linear factors.
static double db(double x) { return pow(10.0, x / 20.0); }
static double input_gain(const es1370_t *e, int reg)      // voice / FM: +12 dB .. -50 dB, bit 7 mute
{
    uint8_t v = e->codec[reg];
    return (v & 0x80) ? 0.0 : db(12.0 - 2.0 * (v & 0x1F));
}
static double master_gain(const es1370_t *e, int reg)     // 0 dB .. -62 dB, bit 7 mute
{
    uint8_t v = e->codec[reg];
    return (v & 0x80) ? 0.0 : db(-2.0 * (v & 0x1F));
}

uint64_t es1370_update(es1370_t *e)
{
    uint64_t now = e->clock(e->clock_opaque);
    if (trace_sound()) {
        static uint64_t last, hist[6];
        if (last && now - last > 5000000) {
            uint64_t ms = (now - last) / 1000000;
            hist[ms < 10 ? 0 : ms < 20 ? 1 : ms < 40 ? 2 : ms < 80 ? 3 : ms < 160 ? 4 : 5]++;
            static uint64_t n;
            if (++n % 50 == 0)
                fprintf(stderr, "[es1370] update gaps: 5-10 ms %llu, 10-20 %llu, 20-40 %llu, 40-80 %llu, 80-160 %llu, >160 %llu\n",
                        (unsigned long long)hist[0], (unsigned long long)hist[1], (unsigned long long)hist[2],
                        (unsigned long long)hist[3], (unsigned long long)hist[4], (unsigned long long)hist[5]);
        }
        last = now;
    }
    uint64_t target = (now - e->t0) / 1000u * SOUND_RATE / 1000000u;
    if (target <= e->produced) goto next_event;
    if (target - e->produced > SOUND_RATE / 5) e->produced = target - SOUND_RATE / 50;   // stalled: drop the backlog

    bool any = false, audible = false;
    for (int c = 0; c < 3; c++) {
        any |= ch_running(e, c);
        if (c != ADC) audible |= ch_running(e, c);
    }
    if (!any) { e->produced = target; goto next_event; }

    if (e->gain_dirty) {
        double ml = master_gain(e, 0x00), mr = master_gain(e, 0x01);
        e->gain[0][0] = input_gain(e, 0x04) * ml; e->gain[0][1] = input_gain(e, 0x05) * mr;   // FM = DAC1
        e->gain[1][0] = input_gain(e, 0x02) * ml; e->gain[1][1] = input_gain(e, 0x03) * mr;   // voice = DAC2
        e->gain_dirty = false;
    }
    double (*g)[2] = e->gain;
    double step[3];
    for (int c = 0; c < 3; c++) step[c] = ch_rate(e, c) / SOUND_RATE;

    int16_t buf[512 * 2];
    while (e->produced < target) {
        int n = target - e->produced > 512 ? 512 : (int)(target - e->produced);
        for (int i = 0; i < n; i++) {
            double l = 0, r = 0;
            for (int c = 0; c < 3; c++) {
                if (!ch_running(e, c)) continue;
                es1370_chan_t *ch = &e->ch[c];
                ch->phase += step[c];
                while (ch->phase >= 1.0) { ch->phase -= 1.0; fetch(e, c); }
                if (c == ADC) continue;
                double f = ch->phase;              // linear interpolation between the last two samples
                l += (ch->prev[0] + (ch->cur[0] - ch->prev[0]) * f) * g[c][0];
                r += (ch->prev[1] + (ch->cur[1] - ch->prev[1]) * f) * g[c][1];
            }
            buf[2 * i]     = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
            buf[2 * i + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
        }
        e->produced += (uint64_t)n;
        if (audible && e->out) e->out(e->out_opaque, buf, n);
    }
    update_irq(e);
    if (trace_sound() && ch_running(e, DAC2)) {
        // Note when the guest rewrites a half of the DAC2 buffer (and whether it wrote silence).
        static uint8_t snap[2][65536];
        static uint8_t now_b[65536];
        uint32_t half = ((e->ch[DAC2].frame_cnt & 0xFFFF) + 1) * 2;
        if (half <= sizeof now_b) {
            for (int h = 0; h < 2; h++) {
                mem_read_block(e->bus, e->ch[DAC2].frame_addr + (uint32_t)h * half, now_b, half);
                if (memcmp(now_b, snap[h], half)) {
                    bool zero = true;
                    for (uint32_t i = 0; i < half && zero; i++) zero = now_b[i] == 0;
                    fprintf(stderr, "[es1370 %.3f] half %d rewritten%s (playing dword %u)\n", (double)e->produced / SOUND_RATE,
                            h, zero ? " with SILENCE" : "", e->ch[DAC2].frame_cnt >> 16);
                    memcpy(snap[h], now_b, half);
                }
            }
        }
    }

next_event:;
    uint64_t best = UINT64_MAX;
    for (int c = 0; c < 3; c++) {
        if (!ch_running(e, c) || !(e->sctl & ch_inten[c])) continue;
        double samples = (double)(e->ch[c].scount >> 16) + 1.0 - e->ch[c].phase;
        uint64_t ns = (uint64_t)(samples / ch_rate(e, c) * 1e9) + 1000;
        if (ns < best) best = ns;
    }
    return best;
}

/* ---- PCI ---- */

static void pci_map(pci_dev_t *d, int bar, uint32_t addr, bool on)
{
    es1370_t *e = d->opaque;
    if (bar != 0) return;
    if (on) { e->io_base = (uint16_t)addr; e->io_on = io_register(e->io, (uint16_t)addr, 64, io_rd, io_wr, e); }
    else if (e->io_on) { io_unregister(e->io, (uint16_t)addr, 64); e->io_on = false; }
}

void es1370_init(es1370_t *e, mem_bus_t *bus, io_bus_t *io, uint64_t (*clock)(void *), void *clock_opaque)
{
    memset(e, 0, sizeof *e);
    e->bus = bus;
    e->io = io;
    e->clock = clock;
    e->clock_opaque = clock_opaque;
    pci_dev_t *p = &e->pci;
    pci_set_ids(p, 0x1274, 0x5000, 0x00, 0x040100, 0x1274, 0x5000);   // Ensoniq AudioPCI ES1370
    pci_set_bar(p, 0, 64, true, false);
    p->cfg[0x3D] = 1;                                                // INTA#
    p->cfg[0x3E] = 0x0C;                                             // MIN_GNT
    p->cfg[0x3F] = 0x80;                                             // MAX_LAT
    p->map = pci_map;
    p->opaque = e;
    es1370_reset(e);
}

void es1370_reset(es1370_t *e)
{
    e->ctl = e->status = e->sctl = e->mempage = 0;
    e->uart_ctl = 0;
    memset(e->ch, 0, sizeof e->ch);
    memset(e->codec, 0, sizeof e->codec);
    e->codec[0x02] = e->codec[0x03] = 0x06;      // voice 0 dB
    e->codec[0x04] = e->codec[0x05] = 0x06;      // FM 0 dB
    e->gain_dirty = true;
    e->t0 = e->clock(e->clock_opaque);
    e->produced = 0;
    update_irq(e);
}
