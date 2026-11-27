#include "apic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    L_ID = 0x20, L_VER = 0x30, L_TPR = 0x80, L_APR = 0x90, L_PPR = 0xA0, L_EOI = 0xB0, L_LDR = 0xD0,
    L_DFR = 0xE0, L_SVR = 0xF0, L_ISR = 0x100, L_TMR = 0x180, L_IRR = 0x200, L_ESR = 0x280,
    L_ICR_LO = 0x300, L_ICR_HI = 0x310, L_LVT = 0x320, L_TIMER_INIT = 0x380, L_TIMER_CUR = 0x390,
    L_TIMER_DIV = 0x3E0,
};
enum { LVT_TIMER, LVT_THERMAL, LVT_PERF, LVT_LINT0, LVT_LINT1, LVT_ERROR };
#define LVT_MASKED   0x10000u
#define LVT_PERIODIC 0x20000u
#define SVR_ENABLED  0x100u
#define RED_MASKED   (1ull << 16)
#define RED_LEVEL    (1ull << 15)
#define RED_REMOTE   (1ull << 14)

static bool trace_apic(void)
{
    static int on = -1;
    if (on < 0) on = getenv("BEEMU_TRACE_APIC") != NULL;
    return on;
}

/* ---- vector bitmaps ---- */
static int highest(const uint32_t v[8])
{
    for (int i = 7; i >= 0; i--)
        if (v[i]) return i * 32 + 31 - __builtin_clz(v[i]);
    return -1;
}
static bool bit(const uint32_t v[8], int n) { return (v[n >> 5] >> (n & 31)) & 1; }
static void set_bit(uint32_t v[8], int n)   { v[n >> 5] |= 1u << (n & 31); }
static void clr_bit(uint32_t v[8], int n)   { v[n >> 5] &= ~(1u << (n & 31)); }

static bool globally_enabled(const lapic_t *l) { return (l->cpu->apic_base & 0x800u) != 0; }
static bool sw_enabled(const lapic_t *l)       { return globally_enabled(l) && (l->svr & SVR_ENABLED); }

static uint32_t ppr(const lapic_t *l)
{
    int isrv = highest(l->isr);
    uint32_t tpr = l->tpr & 0xFF;
    if (isrv < 0 || (tpr >> 4) >= ((uint32_t)isrv >> 4)) return tpr;
    return (uint32_t)isrv & 0xF0;
}

static bool lapic_pending(const lapic_t *l)
{
    if (!sw_enabled(l)) return false;
    int irrv = highest(l->irr);
    return irrv >= 0 && ((uint32_t)irrv & 0xF0) > (ppr(l) & 0xF0);
}

// The 8259 reaches CPU 0 when its APIC is globally disabled, or LINT0 is an unmasked ExtINT.
static bool pic_through(const lapic_t *l)
{
    if (l->index != 0) return false;
    if (!globally_enabled(l)) return true;
    uint32_t lint0 = l->lvt[LVT_LINT0];
    return !(lint0 & LVT_MASKED) && ((lint0 >> 8) & 7) == 7;
}

static void update_line(lapic_t *l)
{
    l->cpu->intr_line = lapic_pending(l) || (pic_through(l) && l->sys->pic_int);
}

static void accept(lapic_t *l, int vector, bool level)
{
    if (vector < 16) { l->esr_pending |= 0x40; return; }     // received illegal vector
    set_bit(l->irr, vector);
    if (level) set_bit(l->tmr, vector); else clr_bit(l->tmr, vector);
    update_line(l);
}

/* ---- destinations ---- */
static bool match_dest(const lapic_t *l, uint32_t dest, bool logical)
{
    if (!logical) return dest == 0xFF || dest == ((l->id >> 24) & 0xFF);
    uint32_t ld = (l->ldr >> 24) & 0xFF;
    if ((l->dfr >> 28) == 0xF) return (ld & dest) != 0;                    // flat model
    return (((ld >> 4) == (dest >> 4)) || (dest >> 4) == 0xF) && (ld & dest & 0xF);   // cluster model
}

// Lowest priority: the matching CPU with the lowest TPR (the first one on a tie).
static lapic_t *lowest_prio(apic_sys_t *s, const bool *hit)
{
    lapic_t *best = NULL;
    for (int i = 0; i < s->n; i++)
        if (hit[i] && sw_enabled(&s->lapic[i]) && (!best || (s->lapic[i].tpr & 0xFF) < (best->tpr & 0xFF)))
            best = &s->lapic[i];
    if (!best) for (int i = 0; i < s->n; i++) if (hit[i]) return &s->lapic[i];
    return best;
}

/* ---- I/O APIC ---- */
static void ioapic_deliver(apic_sys_t *s, int pin)
{
    uint64_t e = s->redir[pin];
    if (e & RED_MASKED) return;
    int vector = (int)(e & 0xFF), mode = (int)(e >> 8 & 7);
    bool logical = (e >> 11) & 1, level = (e & RED_LEVEL) != 0;
    uint32_t dest = (uint32_t)(e >> 56);
    bool hit[MAX_CPUS] = { false };
    int any = 0;
    for (int i = 0; i < s->n; i++) if ((hit[i] = match_dest(&s->lapic[i], dest, logical))) any++;
    if (level) s->redir[pin] |= RED_REMOTE;
    if (trace_apic())
        fprintf(stderr, "[ioapic] pin %d -> vector %02X mode %d %s dest %02X (%d cpus)\n", pin, vector, mode,
                logical ? "logical" : "physical", dest, any);
    if (!any) return;
    if (mode == 1) { lapic_t *l = lowest_prio(s, hit); if (l) accept(l, vector, level); return; }
    if (mode != 0) return;                                   // NMI/SMI/INIT/ExtINT pins: not used
    for (int i = 0; i < s->n; i++) if (hit[i]) accept(&s->lapic[i], vector, level);
}

static void ioapic_recheck(apic_sys_t *s, int pin)
{
    uint64_t e = s->redir[pin];
    if ((e & RED_LEVEL) && !(e & RED_REMOTE) && !(e & RED_MASKED) && ((s->pin_level >> pin) & 1))
        ioapic_deliver(s, pin);
}

void apic_set_irq(apic_sys_t *s, int pin, bool level)
{
    if (pin < 0 || pin >= IOAPIC_PINS) return;
    bool old = (s->pin_level >> pin) & 1;
    if (level) s->pin_level |= 1u << pin; else s->pin_level &= ~(1u << pin);
    if (s->redir[pin] & RED_LEVEL) { if (level) ioapic_recheck(s, pin); }
    else if (level && !old) ioapic_deliver(s, pin);
}

static void ioapic_eoi(apic_sys_t *s, int vector)
{
    for (int p = 0; p < IOAPIC_PINS; p++)
        if ((s->redir[p] & RED_LEVEL) && (s->redir[p] & RED_REMOTE) && (int)(s->redir[p] & 0xFF) == vector) {
            s->redir[p] &= ~RED_REMOTE;
            ioapic_recheck(s, p);
        }
}

uint32_t ioapic_read(apic_sys_t *s, uint32_t off)
{
    if (off == 0x00) return s->io_sel;
    if (off != 0x10) return 0;
    uint32_t r = s->io_sel & 0xFF;
    if (r == 0x00) return s->io_id << 24;
    if (r == 0x01) return 0x00170011u;                       // version 0x11, 24 entries
    if (r == 0x02) return s->io_id << 24;                    // arbitration ID
    if (r >= 0x10 && r < 0x10 + 2 * IOAPIC_PINS) {
        uint64_t e = s->redir[(r - 0x10) / 2];
        return (r & 1) ? (uint32_t)(e >> 32) : (uint32_t)e;
    }
    return 0;
}

void ioapic_write(apic_sys_t *s, uint32_t off, uint32_t v)
{
    if (off == 0x00) { s->io_sel = v & 0xFF; return; }
    if (off != 0x10) return;
    uint32_t r = s->io_sel & 0xFF;
    if (r == 0x00) { s->io_id = (v >> 24) & 0x0F; return; }
    if (r < 0x10 || r >= 0x10 + 2 * IOAPIC_PINS) return;
    int pin = (int)(r - 0x10) / 2;
    uint64_t e = s->redir[pin];
    const uint64_t ro = RED_REMOTE | (1ull << 12);           // remote IRR, delivery status: read-only
    if (r & 1) e = (e & 0xFFFFFFFFull) | (uint64_t)v << 32;
    else       e = (e & 0xFFFFFFFF00000000ull) | (e & ro) | ((uint64_t)v & ~ro);
    s->redir[pin] = e;
    if (!(e & RED_LEVEL)) s->redir[pin] &= ~RED_REMOTE;
    ioapic_recheck(s, pin);
}

/* ---- local APIC timer ---- */
static uint32_t timer_divisor(const lapic_t *l)
{
    uint32_t d = ((l->timer_div & 8) >> 1) | (l->timer_div & 3);
    return d == 7 ? 1 : 2u << d;
}

// Bus clock 1 GHz (like QEMU): one count every `divisor` ns.
static uint64_t timer_period_ns(const lapic_t *l) { return (uint64_t)l->timer_init * timer_divisor(l); }

static uint32_t timer_current(const lapic_t *l, uint64_t now)
{
    if (!l->timer_armed || !l->timer_init) return 0;
    uint64_t elapsed = (now - l->timer_start) / timer_divisor(l);
    if (l->lvt[LVT_TIMER] & LVT_PERIODIC) return l->timer_init - (uint32_t)(elapsed % l->timer_init);
    return elapsed >= l->timer_init ? 0 : l->timer_init - (uint32_t)elapsed;
}

uint64_t apic_update(apic_sys_t *s)
{
    uint64_t now = s->clock(s->clock_opaque), best = UINT64_MAX;
    for (int i = 0; i < s->n; i++) {
        lapic_t *l = &s->lapic[i];
        if (!l->timer_armed || !l->timer_init) continue;
        uint64_t period = timer_period_ns(l), due = l->timer_start + period;
        if (now >= due) {
            if (!(l->lvt[LVT_TIMER] & LVT_MASKED) && sw_enabled(l)) accept(l, (int)(l->lvt[LVT_TIMER] & 0xFF), false);
            if (l->lvt[LVT_TIMER] & LVT_PERIODIC) {
                l->timer_start += period * ((now - l->timer_start) / period);
                due = l->timer_start + period;
            } else {
                l->timer_armed = false;
                continue;
            }
        }
        if (due - now < best) best = due - now;
    }
    return best;
}

/* ---- local APIC registers ---- */
static void send_ipi(apic_sys_t *s, lapic_t *from)
{
    uint32_t lo = from->icr_lo, dest = from->icr_hi >> 24;
    int vector = (int)(lo & 0xFF), mode = (int)(lo >> 8 & 7), sh = (int)(lo >> 18 & 3);
    bool logical = (lo >> 11) & 1, assert_ = (lo >> 14) & 1, level_trig = (lo >> 15) & 1;
    bool hit[MAX_CPUS] = { false };
    for (int i = 0; i < s->n; i++) {
        lapic_t *t = &s->lapic[i];
        switch (sh) {
        case 1: hit[i] = t == from; break;
        case 2: hit[i] = true; break;
        case 3: hit[i] = t != from; break;
        default: hit[i] = match_dest(t, dest, logical); break;
        }
    }
    if (trace_apic())
        fprintf(stderr, "[apic] cpu %d IPI mode %d vector %02X dest %02X%s shorthand %d\n", from->index, mode, vector,
                dest, logical ? " logical" : "", sh);
    switch (mode) {
    case 0:                                                  // fixed
        for (int i = 0; i < s->n; i++) if (hit[i]) accept(&s->lapic[i], vector, false);
        break;
    case 1: { lapic_t *l = lowest_prio(s, hit); if (l) accept(l, vector, false); break; }
    case 5:                                                  // INIT (the level de-assert form does nothing)
        if (!assert_ && level_trig) break;
        for (int i = 1; i < s->n; i++) if (hit[i] && s->init_cpu) s->init_cpu(s->machine, i);   // never the BSP
        break;
    case 6:                                                  // start-up IPI
        for (int i = 1; i < s->n; i++) if (hit[i] && s->start_cpu) s->start_cpu(s->machine, i, (uint8_t)vector);
        break;
    default: break;                                          // SMI, NMI, ExtINT: not used
    }
}

static void set_lvt(lapic_t *l, int k, uint32_t v)
{
    static const uint32_t MASKS[6] = { 0x300FF, 0x107FF, 0x107FF, 0x1F7FF, 0x1F7FF, 0x100FF };
    v &= MASKS[k];
    if (!(l->svr & SVR_ENABLED)) v |= LVT_MASKED;            // software disabled: stays masked
    l->lvt[k] = v;
}

uint32_t lapic_read(apic_sys_t *s, int index, uint32_t off)
{
    lapic_t *l = &s->lapic[index];
    switch (off) {
    case L_ID:  return l->id;
    case L_VER: return 0x00040011u;                          // P6: version 0x11, 5 LVT entries
    case L_TPR: return l->tpr;
    case L_APR: return 0;
    case L_PPR: return ppr(l);
    case L_LDR: return l->ldr;
    case L_DFR: return l->dfr;
    case L_SVR: return l->svr;
    case L_ESR: return l->esr;
    case L_ICR_LO: return l->icr_lo & ~(1u << 12);           // delivery status: always idle
    case L_ICR_HI: return l->icr_hi;
    case L_TIMER_INIT: return l->timer_init;
    case L_TIMER_CUR: return timer_current(l, s->clock(s->clock_opaque));
    case L_TIMER_DIV: return l->timer_div;
    default:
        if (off >= L_ISR && off < L_ISR + 0x80) return l->isr[(off - L_ISR) >> 4];
        if (off >= L_TMR && off < L_TMR + 0x80) return l->tmr[(off - L_TMR) >> 4];
        if (off >= L_IRR && off < L_IRR + 0x80) return l->irr[(off - L_IRR) >> 4];
        if (off >= L_LVT && off < L_LVT + 0x60) return l->lvt[(off - L_LVT) >> 4];
        return 0;
    }
}

void lapic_write(apic_sys_t *s, int index, uint32_t off, uint32_t v)
{
    lapic_t *l = &s->lapic[index];
    if (trace_apic() && off != L_EOI && off != L_ICR_LO && off != L_ICR_HI && off != L_TPR)
        fprintf(stderr, "[apic] cpu %d write %03X = %08X\n", index, off, v);
    switch (off) {
    case L_ID:  l->id = v & 0xFF000000u; break;
    case L_TPR: l->tpr = v & 0xFF; update_line(l); break;
    case L_EOI: {
        int v_ = highest(l->isr);
        if (v_ >= 0) {
            clr_bit(l->isr, v_);
            if (bit(l->tmr, v_)) ioapic_eoi(s, v_);
        }
        update_line(l);
        break;
    }
    case L_LDR: l->ldr = v & 0xFF000000u; break;
    case L_DFR: l->dfr = v | 0x0FFFFFFFu; break;
    case L_SVR:
        l->svr = v & 0x3FF;
        if (!(l->svr & SVR_ENABLED)) for (int k = 0; k < 6; k++) l->lvt[k] |= LVT_MASKED;
        update_line(l);
        break;
    case L_ESR: l->esr = l->esr_pending; l->esr_pending = 0; break;
    case L_ICR_HI: l->icr_hi = v & 0xFF000000u; break;
    case L_ICR_LO: l->icr_lo = v & 0x000CDFFFu; send_ipi(s, l); break;
    case L_TIMER_INIT:
        l->timer_init = v;
        l->timer_start = s->clock(s->clock_opaque);
        l->timer_armed = v != 0;
        break;
    case L_TIMER_DIV: l->timer_div = v & 0xB; break;
    default:
        if (off >= L_LVT && off < L_LVT + 0x60 && !((off - L_LVT) & 0xF)) {
            set_lvt(l, (int)(off - L_LVT) >> 4, v);
            update_line(l);
        }
        break;
    }
}

/* ---- MMIO ---- */
static uint32_t lapic_mmio_rd(void *o, uint32_t off, int size)
{
    apic_sys_t *s = o;
    if (!s->cur) return 0;
    uint32_t v = lapic_read(s, s->cur->index, off & ~3u);
    return size == 4 ? v : (v >> (8 * (off & 3))) & ((1u << (8 * size)) - 1);
}

static void lapic_mmio_wr(void *o, uint32_t off, uint32_t v, int size)
{
    apic_sys_t *s = o;
    (void)size;
    if (s->cur) lapic_write(s, s->cur->index, off & ~3u, v);
}

static uint32_t ioapic_mmio_rd(void *o, uint32_t off, int size)
{
    uint32_t v = ioapic_read(o, off & ~3u);
    return size == 4 ? v : (v >> (8 * (off & 3))) & ((1u << (8 * size)) - 1);
}

static void ioapic_mmio_wr(void *o, uint32_t off, uint32_t v, int size)
{
    (void)size;
    ioapic_write(o, off & ~3u, v);
}

/* ---- the rest ---- */
int apic_ack(void *opaque)
{
    lapic_t *l = opaque;
    apic_sys_t *s = l->sys;
    if (lapic_pending(l)) {
        int v = highest(l->irr);
        clr_bit(l->irr, v);
        set_bit(l->isr, v);
        update_line(l);
        return v;
    }
    if (pic_through(l) && s->pic_int && s->pic_ack) return s->pic_ack(s->pic_opaque);
    return (int)(l->svr & 0xFF);                             // spurious
}

void apic_set_pic(apic_sys_t *s, bool level)
{
    s->pic_int = level;
    if (s->n) update_line(&s->lapic[0]);
}

void apic_set_current(apic_sys_t *s, int index) { s->cur = &s->lapic[index]; }

void apic_msr_base_written(apic_sys_t *s, int index) { update_line(&s->lapic[index]); }

void apic_reset_cpu(apic_sys_t *s, int index)
{
    lapic_t *l = &s->lapic[index];
    cpu_t *cpu = l->cpu;
    memset(l, 0, sizeof *l);
    l->sys = s;
    l->cpu = cpu;
    l->index = index;
    l->id = (uint32_t)index << 24;
    l->dfr = 0xFFFFFFFFu;
    l->svr = 0xFF;
    for (int k = 0; k < 6; k++) l->lvt[k] = LVT_MASKED;
    cpu->intr_ack = apic_ack;
    cpu->intr_opaque = l;
    update_line(l);
}

void apic_reset(apic_sys_t *s)
{
    for (int i = 0; i < s->n; i++) apic_reset_cpu(s, i);
    s->io_id = (uint32_t)s->n;                               // after the CPUs, as SeaBIOS's MP table says
    s->io_sel = 0;
    for (int p = 0; p < IOAPIC_PINS; p++) s->redir[p] = RED_MASKED;
    s->pin_level = 0;
    s->pic_int = false;
}

void apic_init(apic_sys_t *s, int n_cpus, cpu_t *cpus, mem_bus_t *bus,
               uint64_t (*clock)(void *), void *clock_opaque)
{
    memset(s, 0, sizeof *s);
    s->n = n_cpus;
    s->clock = clock;
    s->clock_opaque = clock_opaque;
    for (int i = 0; i < n_cpus; i++) { s->lapic[i].cpu = &cpus[i]; s->lapic[i].sys = s; }
    s->cur = &s->lapic[0];
    mem_map_mmio(bus, LAPIC_BASE, 0x1000, lapic_mmio_rd, lapic_mmio_wr, s);
    mem_map_mmio(bus, IOAPIC_BASE, 0x1000, ioapic_mmio_rd, ioapic_mmio_wr, s);
    apic_reset(s);
}
