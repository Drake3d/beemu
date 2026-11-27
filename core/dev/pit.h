// Intel 8254 programmable interval timer (ports 0x40-0x43) and the PC
// "port B" at 0x61 (channel 2 gate, speaker, OUT2, refresh toggle).
// Counters run on host time at 1.193182 MHz. Channel 0 drives IRQ 0.
#ifndef PIT_H
#define PIT_H

#include <stdbool.h>
#include <stdint.h>
#include "clock.h"
#include "io.h"

#define PIT_HZ 1193182u

typedef struct {
    uint32_t count;        // reload value, 1..0x10000
    uint8_t  mode, rw, bcd;
    bool     write_hi, read_hi;     // lo/hi byte sequencing
    uint8_t  write_lo;
    bool     latched;      uint16_t latch;   bool latch_hi;
    bool     status_latched; uint8_t status;
    bool     gate;
    bool     armed;        // a count was loaded since the mode was set
    uint64_t load_ns;      // when counting started
    uint64_t irq_periods;  // channel 0: periods already signalled
    bool     out_last;
} pit_channel_t;

typedef struct {
    pit_channel_t ch[3];
    uint8_t  port61;       // bits 0 (gate 2) and 1 (speaker enable)
    clock_fn now;
    void    *now_opaque;
    void   (*set_irq)(void *opaque, int irq, int level);
    void    *irq_opaque;
} pit_t;

void pit_init(pit_t *p, clock_fn now, void *now_opaque,
              void (*set_irq)(void *, int, int), void *irq_opaque);
void pit_register(pit_t *p, io_bus_t *io);
// Raise IRQ 0 for elapsed periods. Returns nanoseconds until the next channel 0
// event (or a large value if none), so an idle CPU can sleep that long.
uint64_t pit_update(pit_t *p);

#endif
