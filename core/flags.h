// Lazy EFLAGS evaluation. See the cc_op_t comment in cpu.h for the state layout.
#ifndef FLAGS_H
#define FLAGS_H

#include <stdbool.h>
#include <stdint.h>
#include "cpu.h"

#define EFL_ARITH (EFL_CF | EFL_PF | EFL_AF | EFL_ZF | EFL_SF | EFL_OF)

// eflags with the six arithmetic flags recomputed from the lazy state.
uint32_t cpu_calc_flags(const cpu_t *c);

// Evaluate x86 condition code 0..15 (O NO B AE E NE BE A S NS P NP L GE LE G).
bool cpu_eval_cond(const cpu_t *c, int cond);

// Write the computed flags into eflags and mark them materialized.
void cpu_flush_flags(cpu_t *c);

// Current carry flag (0 or 1); used by INC/DEC, ADC/SBB and rotates.
uint32_t cpu_get_cf(const cpu_t *c);

#endif
