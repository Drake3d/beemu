// x87 FPU (Pentium III): all D8-DF instructions plus FWAIT, on Berkeley SoftFloat 3e
// with exact 80-bit extended precision.
//
// Model: cpu->st[] holds the eight physical registers as raw 80-bit values;
// TOP lives in fpu_sw bits 11-13 and fpu_tw keeps the full 2-bit tags. Every
// exception is emulated with its masked response; an unmasked exception sets
// ES/B, leaves the destination alone where the manual says so, and raises #MF
// at the next waiting FPU instruction (CR0.NE=1; with NE=0 nothing is signalled).
// Rounding control applies everywhere, precision control to add/sub/mul/div/sqrt.
// C1 reports "rounded up" for inexact results.
// Transcendentals are computed in quad precision (fpu_math.c) and rounded once;
// FSIN/FCOS/FPTAN/FSINCOS reduce their argument with Intel's 66-bit pi.
#ifndef FPU_H
#define FPU_H

#include <stdbool.h>
#include <stdint.h>
#include "cpu.h"
#include "decode.h"

// Execute one x87 instruction (in->op == OP_FPU). lin = linear address of the memory
// operand (if any). Returns 0, or JIT_EXIT_UNSUPPORTED (eip stays on the instruction).
uint32_t fpu_exec(cpu_t *c, const insn_t *in, uint32_t lin);
// FWAIT: #NM / #MF checks only.
void     fpu_wait(cpu_t *c);
void     fpu_init(cpu_t *c);            // FNINIT state
// Integer fast paths for loads/stores/int conversions with all exceptions masked (on by default;
// BEEMU_NO_FPU_FAST=1 turns them off). Tests switch them to compare against the general code.
void     fpu_set_fast_paths(bool on);
// Exact integer add/sub/mul/div inside the fast paths (on by default; BEEMU_NO_FPU_NATIVE=1 leaves
// them to SoftFloat). For tests.
void     fpu_set_native(bool on);
// Entry points for generated code (core/jit.c) for the common forms (fpu_jit_handles): each does
// the instruction when every exception is masked and the operands are ordinary, and returns 0;
// otherwise it changes nothing and returns 1, and the caller runs it through helper_exec. They
// never fault: memory goes through the fast TLB only. lin = linear address, off = its offset in
// the segment, op = the 11-bit x87 opcode (FOP), seg = the memory operand's segment register.
uint32_t fpu_jit_mem(cpu_t *c, uint32_t lin, uint32_t off, uint32_t eip, uint32_t op_seg /* op | seg << 16 */);
uint32_t fpu_jit_reg(cpu_t *c, uint32_t eip, uint32_t op);
bool     fpu_jit_handles(const insn_t *in);

#endif
