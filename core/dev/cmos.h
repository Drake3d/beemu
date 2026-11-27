// MC146818 real-time clock and 128 bytes of CMOS RAM (ports 0x70/0x71).
// Time follows the host clock plus an offset the guest can change. Periodic
// and update-ended interrupts drive IRQ 8.
#ifndef CMOS_H
#define CMOS_H

#include <stdbool.h>
#include <stdint.h>
#include "clock.h"
#include "io.h"

typedef struct {
    uint8_t  ram[128];
    uint8_t  index;
    int64_t  offset_s;          // guest time = host wall time + offset_s
    uint64_t next_periodic_ns, next_update_ns;
    clock_fn now;
    void    *now_opaque;
    void   (*set_irq)(void *opaque, int irq, int level);
    void    *irq_opaque;
} cmos_t;

void cmos_init(cmos_t *c, clock_fn now, void *now_opaque,
               void (*set_irq)(void *, int, int), void *irq_opaque);
void cmos_register(cmos_t *c, io_bus_t *io);
void cmos_set(cmos_t *c, int index, uint8_t value);   // for machine setup (memory size, CPUs...)
uint64_t cmos_update(cmos_t *c);                       // returns ns until the next RTC event

#endif
