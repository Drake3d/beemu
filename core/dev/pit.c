#include "pit.h"
#include <string.h>

static uint64_t ticks_since(const pit_t *p, const pit_channel_t *c)
{
    uint64_t now = p->now(p->now_opaque);
    if (now <= c->load_ns) return 0;
    return (uint64_t)(((unsigned __int128)(now - c->load_ns) * PIT_HZ) / 1000000000u);
}

static uint64_t ns_for_ticks(uint64_t t)
{
    return (uint64_t)(((unsigned __int128)t * 1000000000u + PIT_HZ - 1) / PIT_HZ);
}

static uint32_t current_count(const pit_t *p, const pit_channel_t *c)
{
    if (!c->armed) return c->count & 0xFFFF;
    uint64_t d = ticks_since(p, c);
    uint32_t n = c->count;
    switch (c->mode) {
    case 2:  return (uint32_t)(n - d % n) & 0xFFFF;
    case 3:  return (uint32_t)(n - (2 * d) % n) & 0xFFFF;
    default: return (uint32_t)(n - d) & 0xFFFF;          // modes 0, 1, 4, 5 count down and wrap
    }
}

static bool output(const pit_t *p, const pit_channel_t *c)
{
    if (!c->armed) return c->mode != 0;                   // mode 0 starts low, others high
    uint64_t d = ticks_since(p, c);
    uint32_t n = c->count;
    switch (c->mode) {
    case 0: case 1: return d >= n;
    case 2:         return !(d != 0 && d % n == 0);       // a one-tick low pulse each period
    case 3:         return (d % n) < ((n + 1) / 2);
    default:        return d != n;                        // 4, 5: strobe
    }
}

static uint8_t to_bcd(uint32_t v) { return (uint8_t)(((v / 10) % 10) << 4 | (v % 10)); }

static uint32_t from_count_reg(const pit_channel_t *c, uint32_t v)
{
    if (c->bcd) v = (v >> 12 & 0xF) * 1000 + (v >> 8 & 0xF) * 100 + (v >> 4 & 0xF) * 10 + (v & 0xF);
    return v ? v : (c->bcd ? 10000u : 0x10000u);
}

static void load(pit_t *p, pit_channel_t *c, uint32_t raw)
{
    c->count = from_count_reg(c, raw);
    c->load_ns = p->now(p->now_opaque);
    c->armed = true;
    c->irq_periods = 0;
    // One-shot modes: OUT drops when a count is loaded. Lower IRQ 0 now, so the next
    // terminal count is a fresh edge even if it comes before the next pit_update.
    if (c == &p->ch[0] && (c->mode == 0 || c->mode == 4) && c->out_last) {
        c->out_last = false;
        p->set_irq(p->irq_opaque, 0, 0);
    }
}

static uint32_t read_byte(pit_t *p, pit_channel_t *c)
{
    if (c->status_latched) { c->status_latched = false; return c->status; }
    uint32_t v;
    bool hi;
    if (c->latched) {
        v = c->latch;
        if (c->rw == 3) { hi = c->latch_hi; c->latch_hi = !c->latch_hi; if (!hi) return v & 0xFF; c->latched = false; return v >> 8; }
        c->latched = false;
        return c->rw == 2 ? v >> 8 : v & 0xFF;
    }
    v = current_count(p, c);
    if (c->bcd) v = (uint32_t)to_bcd(v / 100) << 8 | to_bcd(v % 100);
    switch (c->rw) {
    case 1: return v & 0xFF;
    case 2: return (v >> 8) & 0xFF;
    default:
        hi = c->read_hi;
        c->read_hi = !c->read_hi;
        return hi ? (v >> 8) & 0xFF : v & 0xFF;
    }
}

static void latch_count(pit_t *p, pit_channel_t *c)
{
    if (c->latched) return;
    uint32_t v = current_count(p, c);
    c->latch = (uint16_t)v;
    c->latched = true;
    c->latch_hi = false;
}

static uint32_t pit_read(void *opaque, uint16_t port, int size)
{
    (void)size;
    pit_t *p = opaque;
    if (port == 0x43) return 0xFF;
    return read_byte(p, &p->ch[port - 0x40]);
}

static void pit_write(void *opaque, uint16_t port, uint32_t v, int size)
{
    (void)size;
    pit_t *p = opaque;
    v &= 0xFF;
    if (port == 0x43) {
        int sc = (int)(v >> 6);
        if (sc == 3) {                                   // read-back
            for (int i = 0; i < 3; i++) {
                if (!(v & (2u << i))) continue;
                pit_channel_t *c = &p->ch[i];
                if (!(v & 0x20)) latch_count(p, c);
                if (!(v & 0x10) && !c->status_latched) {
                    c->status = (uint8_t)((output(p, c) ? 0x80 : 0) | (c->armed ? 0 : 0x40) |
                                          (c->rw << 4) | (c->mode << 1) | c->bcd);
                    c->status_latched = true;
                }
            }
            return;
        }
        pit_channel_t *c = &p->ch[sc];
        uint8_t rw = (uint8_t)((v >> 4) & 3);
        if (rw == 0) { latch_count(p, c); return; }      // counter latch command
        c->rw = rw;
        c->mode = (uint8_t)((v >> 1) & 7);
        if (c->mode > 5) c->mode -= 4;                   // 6, 7 alias 2, 3
        c->bcd = (uint8_t)(v & 1);
        c->write_hi = c->read_hi = false;
        c->latched = false;
        c->armed = false;
        return;
    }
    pit_channel_t *c = &p->ch[port - 0x40];
    switch (c->rw) {
    case 1: load(p, c, v); break;
    case 2: load(p, c, v << 8); break;
    default:
        if (!c->write_hi) { c->write_lo = (uint8_t)v; c->write_hi = true; }
        else { c->write_hi = false; load(p, c, c->write_lo | (v << 8)); }
        break;
    }
}

static uint32_t port61_read(void *opaque, uint16_t port, int size)
{
    (void)port; (void)size;
    pit_t *p = opaque;
    uint64_t now = p->now(p->now_opaque);
    uint32_t refresh = (uint32_t)((now / 15085) & 1);    // toggles every ~15 us
    pit_channel_t *c2 = &p->ch[2];
    bool out2 = output(p, c2);                           // gate low is not modeled as pausing
    return (p->port61 & 3u) | (refresh << 4) | (out2 ? 0x20u : 0);
}

static void port61_write(void *opaque, uint16_t port, uint32_t v, int size)
{
    (void)port; (void)size;
    pit_t *p = opaque;
    pit_channel_t *c2 = &p->ch[2];
    bool gate = v & 1;
    // A rising gate restarts modes 1, 2, 3 and 5 (and the hardware one-shot).
    if (gate && !c2->gate && c2->armed && c2->mode != 0 && c2->mode != 4)
        c2->load_ns = p->now(p->now_opaque);
    c2->gate = gate;
    p->port61 = (uint8_t)(v & 3);
}

void pit_init(pit_t *p, clock_fn now, void *now_opaque,
              void (*set_irq)(void *, int, int), void *irq_opaque)
{
    memset(p, 0, sizeof *p);
    p->now = now; p->now_opaque = now_opaque;
    p->set_irq = set_irq; p->irq_opaque = irq_opaque;
    for (int i = 0; i < 3; i++) { p->ch[i].count = 0x10000; p->ch[i].rw = 3; p->ch[i].gate = i != 2; }
}

void pit_register(pit_t *p, io_bus_t *io)
{
    io_register(io, 0x40, 4, pit_read, pit_write, p);
    io_register(io, 0x61, 1, port61_read, port61_write, p);
}

uint64_t pit_update(pit_t *p)
{
    pit_channel_t *c = &p->ch[0];
    if (!c->armed) return 10000000;                      // nothing scheduled: check again in 10 ms
    uint64_t d = ticks_since(p, c);
    if (c->mode == 2 || c->mode == 3) {
        uint64_t periods = d / c->count;
        if (periods > c->irq_periods) {                  // missed periods are coalesced into one
            c->irq_periods = periods;
            p->set_irq(p->irq_opaque, 0, 1);
            p->set_irq(p->irq_opaque, 0, 0);
        }
        uint64_t next = (periods + 1) * c->count;
        return ns_for_ticks(next - d);
    }
    bool out = output(p, c);                             // one-shot modes: IRQ follows OUT
    if (out != c->out_last) { p->set_irq(p->irq_opaque, 0, out); c->out_last = out; }
    return d < c->count ? ns_for_ticks(c->count - d) : 10000000;
}
