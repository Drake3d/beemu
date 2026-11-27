#include "cpu.h"
#include <string.h>

void cpu_init(cpu_t *c, int index)
{
    memset(c, 0, sizeof(*c));
    c->cpu_index = index;
    c->apic_id = (uint8_t)index;
    cpu_reset(c);
}

void cpu_reset(cpu_t *c)
{
    int idx = c->cpu_index;
    uint8_t apic = c->apic_id;
    struct mem_bus *mem = c->mem;
    struct io_bus *io = c->io;
    int (*ack)(void *) = c->intr_ack;
    void *ack_opaque = c->intr_opaque;
    bool has_apic = c->has_apic;
    memset(c, 0, sizeof(*c));
    c->has_apic = has_apic;
    c->cpu_index = idx;
    c->apic_id = apic;
    c->mem = mem;
    c->io = io;
    c->ftlb_base = c->ftlb;
    c->jc_base = c->jc;
    c->intr_ack = ack;
    c->intr_opaque = ack_opaque;

    // Reset state: real mode, CS base 0xFFFF0000, EIP 0xFFF0
    // (SeaBIOS is mapped at the top of the 4 GB space)
    c->cr0 = 0x60000010;          // CD | NW | ET
    c->eflags = 0x2;
    c->eip = 0xFFF0;
    c->seg[S_CS].selector = 0xF000;
    c->seg[S_CS].base  = 0xFFFF0000;
    c->seg[S_CS].limit = 0xFFFF;
    c->seg[S_CS].flags = SEG_P | SEG_S | (0xBu << 8);   // present, code, readable, accessed
    for (int i = 0; i < 6; i++)
        if (i != S_CS) {
            c->seg[i].limit = 0xFFFF;
            c->seg[i].flags = SEG_P | SEG_S | (0x3u << 8);  // present, data, writable, accessed
        }
    c->idtr.limit = 0x3FF;           // real-mode IVT at 0
    c->apic_base = 0xFEE00800u | (idx == 0 ? 0x100u : 0);   // enabled, BSP flag on CPU 0
    c->fpu_cw = 0x40;                // power-on: FNINIT gives 0x37F
    c->fpu_tw = 0x5555;
    c->mxcsr  = 0x1F80;
    c->cc_op  = CC_OP_EFLAGS;
}
