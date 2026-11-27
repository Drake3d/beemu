// Local APICs (one per CPU, xAPIC at 0xFEE00000) and the I/O APIC (82093AA-style, 24 pins at
// 0xFEC00000), for multiprocessor machines. A Pentium III's local APIC: version 0x11, 4 LVTs
// (+ error), timer with divider and periodic mode, IPIs (fixed, lowest priority, INIT, SIPI;
// physical, flat and cluster logical destinations, shorthands). The 8259 reaches CPU 0 through
// LINT0 ("virtual wire": ExtINT, or the whole APIC globally disabled).
//
// Every CPU sees its own local APIC at the same address, so MMIO goes to the CPU that the run
// loop marked as current (apic_set_current). Interrupt state is pushed into cpu->intr_line;
// cpu->intr_ack / intr_opaque point here (apic_ack). INIT and SIPI call back into the machine.
#ifndef APIC_H
#define APIC_H

#include <stdbool.h>
#include <stdint.h>
#include "cpu.h"
#include "mem.h"

#define LAPIC_BASE  0xFEE00000u
#define IOAPIC_BASE 0xFEC00000u
#define IOAPIC_PINS 24

typedef struct apic_sys apic_sys_t;

typedef struct {
    apic_sys_t *sys;
    cpu_t   *cpu;
    int      index;
    uint32_t id, tpr, ldr, dfr, svr, esr, esr_pending;
    uint32_t lvt[6];                // timer, thermal (unused), perf, LINT0, LINT1, error
    uint32_t icr_lo, icr_hi;
    uint32_t timer_init, timer_div; // divide configuration register
    uint64_t timer_start;           // host ns when the count was (re)loaded
    bool     timer_armed;
    uint32_t irr[8], isr[8], tmr[8];
} lapic_t;

struct apic_sys {
    int       n;
    lapic_t   lapic[MAX_CPUS];
    lapic_t  *cur;                  // CPU the run loop is executing (for local APIC MMIO)

    // I/O APIC
    uint32_t  io_id, io_sel;
    uint64_t  redir[IOAPIC_PINS];
    uint32_t  pin_level;            // asserted inputs (bit per pin)

    // The 8259's INT output (into CPU 0's LINT0) and its acknowledge.
    bool      pic_int;
    int     (*pic_ack)(void *opaque);
    void     *pic_opaque;

    uint64_t (*clock)(void *opaque);
    void     *clock_opaque;
    // INIT (reset the CPU and wait for SIPI) and SIPI (start it at vector << 12).
    void    (*init_cpu)(void *opaque, int index);
    void    (*start_cpu)(void *opaque, int index, uint8_t vector);
    void     *machine;
};

void apic_init(apic_sys_t *s, int n_cpus, cpu_t *cpus, mem_bus_t *bus,
               uint64_t (*clock)(void *), void *clock_opaque);
void apic_reset(apic_sys_t *s);
void apic_reset_cpu(apic_sys_t *s, int index);      // INIT: local APIC back to power-on state (ID kept)
void apic_set_current(apic_sys_t *s, int index);
void apic_set_pic(apic_sys_t *s, bool level);       // 8259 INT output changed
void apic_set_irq(apic_sys_t *s, int pin, bool level);   // I/O APIC input
int  apic_ack(void *lapic);                         // INTA for the CPU owning this local APIC
void apic_msr_base_written(apic_sys_t *s, int index);    // IA32_APIC_BASE changed (global enable)
// Advance timers; returns ns until the next timer interrupt (UINT64_MAX if none).
uint64_t apic_update(apic_sys_t *s);

// Register access (exposed for tests): local APIC register `off` of CPU `index`, I/O APIC.
uint32_t lapic_read(apic_sys_t *s, int index, uint32_t off);
void     lapic_write(apic_sys_t *s, int index, uint32_t off, uint32_t v);
uint32_t ioapic_read(apic_sys_t *s, uint32_t off);
void     ioapic_write(apic_sys_t *s, uint32_t off, uint32_t v);

#endif
