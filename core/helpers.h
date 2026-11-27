// C implementations of the instructions the JIT does not generate inline:
// shifts/rotates, multiply/divide, bit ops, string ops, I/O, flag transfers,
// segment loads, far transfers, interrupts, and system instructions.
//
// The translator copies the decoded instruction into a jit_hrec_t (kept in a
// per-JIT arena that lives as long as the generated code) and emits a call to
// helper_exec(cpu, rec).
#ifndef HELPERS_H
#define HELPERS_H

#include <stdbool.h>
#include <stdint.h>
#include "cpu.h"
#include "decode.h"

typedef struct {
    insn_t   in;
    uint32_t eip;        // address of the instruction (CS offset)
    uint32_t next_eip;   // address of the next instruction
} jit_hrec_t;

// Execute one instruction. Returns 0 to let the block continue, or a jit_exit_t
// to end it, with cpu->eip already set (branch target, next instruction, or the
// instruction itself for JIT_EXIT_UNSUPPORTED / an unfinished REP).
// Faults longjmp out through cpu_fault() (see system.h).
uint32_t helper_exec(cpu_t *cpu, const jit_hrec_t *rec);

// Can helper_exec run this instruction? *ends is set when the block must end
// after it (control transfers, and anything that changes CS/SS size, CR0 or IF).
bool helper_supports(const insn_t *in, bool *ends);

#endif
