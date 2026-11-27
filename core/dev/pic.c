#include "pic.h"
#include <string.h>

static void reset_chip(pic8259_t *s)
{
    uint8_t elcr = s->elcr, mask = s->elcr_mask;
    memset(s, 0, sizeof *s);
    s->elcr = elcr;
    s->elcr_mask = mask;
}

void pic_init(pic_t *p, cpu_t *cpu)
{
    memset(p, 0, sizeof *p);
    p->cpu = cpu;
    p->c[0].elcr_mask = 0xF8;      // IRQ 0, 1, 2 are always edge
    p->c[1].elcr_mask = 0xDE;      // IRQ 8 and 13 are always edge
}

// Priority 0..7 of the highest-priority bit in mask (relative to prio_add), 8 if none.
static int get_priority(const pic8259_t *s, uint8_t mask)
{
    if (!mask) return 8;
    int p = 0;
    while (!(mask & (1u << ((p + s->prio_add) & 7)))) p++;
    return p;
}

// Highest pending IRQ that beats everything in service, or -1.
static int get_irq(const pic8259_t *s)
{
    int pr = get_priority(s, s->irr & ~s->imr);
    if (pr == 8) return -1;
    uint8_t in_service = s->isr;
    if (s->special_mask) in_service &= ~s->imr;
    if (pr < get_priority(s, in_service)) return (pr + s->prio_add) & 7;
    return -1;
}

static void chip_set(pic8259_t *s, int irq, int level)
{
    uint8_t bit = (uint8_t)(1u << irq);
    if (s->elcr & bit) {                       // level triggered
        if (level) s->irr |= bit; else s->irr &= ~bit;
    } else if (level) {                        // edge triggered: rising edge latches
        if (!(s->lines & bit)) s->irr |= bit;
    }
    if (level) s->lines |= bit; else s->lines &= ~bit;
}

static void update(pic_t *p)
{
    // The cascade input follows the slave's INT output, so a slave request that
    // goes away before INTA doesn't leave a stale (spurious) request on IRQ 2.
    if (get_irq(&p->c[1]) >= 0) p->c[0].irr |= 0x04;
    else                        p->c[0].irr &= (uint8_t)~0x04u;
    if (p->out) p->out(p->out_opaque, get_irq(&p->c[0]) >= 0);
    else if (p->cpu) p->cpu->intr_line = get_irq(&p->c[0]) >= 0;
}

void pic_set_irq(pic_t *p, int irq, int level)
{
    chip_set(&p->c[irq >> 3], irq & 7, level);
    update(p);
}

static void ack_chip(pic8259_t *s, int irq)
{
    uint8_t bit = (uint8_t)(1u << irq);
    if (s->auto_eoi) {
        if (s->rotate_aeoi) s->prio_add = (uint8_t)((irq + 1) & 7);
    } else {
        s->isr |= bit;
    }
    if (!(s->elcr & bit)) s->irr &= ~bit;      // edge: the request is consumed
}

int pic_ack(void *opaque)
{
    pic_t *p = opaque;
    int vec, irq = get_irq(&p->c[0]);
    if (irq < 0) {
        vec = p->c[0].base + 7;                // spurious
    } else {
        ack_chip(&p->c[0], irq);
        if (irq == 2) {
            int irq2 = get_irq(&p->c[1]);
            if (irq2 >= 0) { ack_chip(&p->c[1], irq2); vec = p->c[1].base + irq2; }
            else vec = p->c[1].base + 7;       // spurious on the slave
        } else {
            vec = p->c[0].base + irq;
        }
    }
    update(p);
    return vec;
}

static void eoi(pic8259_t *s, int irq, bool rotate)
{
    s->isr &= (uint8_t)~(1u << irq);
    if (rotate) s->prio_add = (uint8_t)((irq + 1) & 7);
}

static void write_cmd(pic_t *p, pic8259_t *s, uint8_t v)
{
    if (v & 0x10) {                            // ICW1
        reset_chip(s);
        s->need_icw4 = v & 1;
        s->single = (v & 2) != 0;
        s->icw_step = 2;
    } else if (v & 0x08) {                     // OCW3
        if (v & 0x04) s->poll = true;
        if (v & 0x02) s->read_isr = v & 1;
        if (v & 0x40) s->special_mask = (v >> 5) & 1;
    } else {                                   // OCW2
        int cmd = v >> 5;
        switch (cmd) {
        case 0: case 4: s->rotate_aeoi = cmd == 4; break;
        case 1: case 5: {                      // non-specific EOI (with rotate)
            int pr = get_priority(s, s->isr);
            if (pr != 8) eoi(s, (pr + s->prio_add) & 7, cmd == 5);
            break;
        }
        case 3: eoi(s, v & 7, false); break;   // specific EOI
        case 6: s->prio_add = (uint8_t)((v + 1) & 7); break;
        case 7: eoi(s, v & 7, true); break;
        default: break;
        }
    }
    update(p);
}

static void write_data(pic_t *p, pic8259_t *s, uint8_t v)
{
    switch (s->icw_step) {
    case 2:
        s->base = v & 0xF8;
        s->icw_step = s->single ? (s->need_icw4 ? 4 : 0) : 3;
        break;
    case 3:
        s->icw_step = s->need_icw4 ? 4 : 0;
        break;
    case 4:
        s->auto_eoi = (v & 2) != 0;
        s->icw_step = 0;
        break;
    default:
        s->imr = v;                            // OCW1
        break;
    }
    update(p);
}

static uint32_t pic_read(void *opaque, uint16_t port, int size)
{
    (void)size;
    pic_t *p = opaque;
    pic8259_t *s = &p->c[port >= 0xA0 ? 1 : 0];
    if (!(port & 1)) {
        if (s->poll) {
            s->poll = false;
            int irq = get_irq(s);
            if (irq < 0) return 0;
            ack_chip(s, irq);
            update(p);
            return 0x80u | (uint32_t)irq;
        }
        return s->read_isr ? s->isr : s->irr;
    }
    return s->imr;
}

static void pic_write(void *opaque, uint16_t port, uint32_t v, int size)
{
    (void)size;
    pic_t *p = opaque;
    pic8259_t *s = &p->c[port >= 0xA0 ? 1 : 0];
    if (port & 1) write_data(p, s, (uint8_t)v);
    else          write_cmd(p, s, (uint8_t)v);
}

static uint32_t elcr_read(void *opaque, uint16_t port, int size)
{
    (void)size;
    pic_t *p = opaque;
    return p->c[port & 1].elcr;
}

static void elcr_write(void *opaque, uint16_t port, uint32_t v, int size)
{
    pic_t *p = opaque;
    for (int i = 0; i < size && (port & 1) + i < 2; i++) {
        pic8259_t *s = &p->c[(port & 1) + i];
        s->elcr = (uint8_t)(v >> (8 * i)) & s->elcr_mask;
    }
    update(p);
}

void pic_register(pic_t *p, io_bus_t *io)
{
    io_register(io, 0x20, 2, pic_read, pic_write, p);
    io_register(io, 0xA0, 2, pic_read, pic_write, p);
    io_register(io, 0x4D0, 2, elcr_read, elcr_write, p);
}
