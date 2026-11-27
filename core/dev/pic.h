// Two cascaded 8259A programmable interrupt controllers (ports 0x20/0x21,
// 0xA0/0xA1) plus the PIIX edge/level control registers (ELCR, 0x4D0/0x4D1).
// The master's INT output drives cpu->intr_line; the CPU acknowledges with pic_ack().
#ifndef PIC_H
#define PIC_H

#include <stdbool.h>
#include <stdint.h>
#include "cpu.h"
#include "io.h"

typedef struct {
    uint8_t irr, isr, imr;
    uint8_t base;          // ICW2 vector base
    uint8_t icw_step;      // 0 ready, 2/3/4 waiting for that ICW
    bool    need_icw4, single, auto_eoi, read_isr, poll, special_mask, rotate_aeoi;
    uint8_t elcr, elcr_mask;
    uint8_t lines;         // current input levels (edge detection)
    uint8_t prio_add;      // IRQ with the highest priority
} pic8259_t;

typedef struct {
    pic8259_t c[2];        // 0 = master, 1 = slave (on master IRQ 2)
    cpu_t    *cpu;
    // With local APICs the INT output goes to them instead (CPU 0's LINT0): set after pic_init.
    void    (*out)(void *opaque, bool level);
    void     *out_opaque;
} pic_t;

void pic_init(pic_t *p, cpu_t *cpu);
void pic_register(pic_t *p, io_bus_t *io);
void pic_set_irq(pic_t *p, int irq, int level);    // irq 0..15
int  pic_ack(void *pic);                           // INTA: returns the vector

#endif
