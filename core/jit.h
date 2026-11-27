// Baseline x86-32 -> ARM64 JIT.
//
// Generated inline, 8/16/32-bit, register/memory/immediate operands:
//   mov (incl. mov r/m,sreg), add adc sub sbb cmp and or xor test, inc dec not neg,
//   xchg, movzx movsx, lea, push pop, near call ret jmp, leave, jcc, loop/loope/
//   loopne/jcxz, setcc, cmovcc, cli sti cld std clc stc cmc, nop.
// Most other integer and system instructions call helper_exec() (helpers.h).
// What remains (x87/MMX/SSE, BCD, ...) ends the block with JIT_EXIT_UNSUPPORTED
// and leaves cpu->eip pointing at the offending instruction (none of its code runs).
//
// Generated blocks have the C signature   uint32_t block(cpu_t *cpu)
// and return a jit_exit_t. The prologue keeps cpu_t* in x19 and saves x19..x24
// and x30, so generated code can call C helpers (memory access, lazy flags).
// Guest registers live in cpu_t and are loaded and stored around each
// instruction (no host register mapping yet). Memory operands go through the
// bus via cpu->mem, which must be set before running.
//
// Blocks are cached by linear address plus the code and stack segment sizes
// they were translated for.
#ifndef JIT_H
#define JIT_H

#include <stddef.h>
#include <stdint.h>
#include "cpu.h"
#include "mem.h"

typedef enum {
    JIT_EXIT_CONTINUE = 0,  // internal: a helper lets the block go on (never returned by jit_run)
    JIT_EXIT_OK,            // block finished normally, keep going
    JIT_EXIT_HALT,          // HLT executed
    JIT_EXIT_UNSUPPORTED,   // valid x86 not emulated yet (eip = that insn)
    JIT_EXIT_INVALID,       // illegal opcode (eip = that insn); #UD delivery comes later
    JIT_EXIT_LIMIT,         // jit_run hit its block budget
    JIT_EXIT_NO_HOST,       // built for a non-ARM64 host: translation only, no execution
    JIT_EXIT_SHUTDOWN,      // triple fault: the CPU shut down
    JIT_EXIT_REQUEST,       // a device set cpu->exit_request (reset, ...)
} jit_exit_t;

typedef struct {
    const uint32_t *code;   // generated ARM64 words
    uint32_t nwords;
    uint32_t guest_insns;   // guest instructions translated into this block
    uint32_t guest_bytes;
    uint32_t end_eip;       // guest eip when the block exits (if !dyn_exit)
    bool     dyn_exit;      // ends in ret / indirect jmp / indirect call: eip computed at run time
    jit_exit_t exit;        // how the block ends
} jit_block_info;

typedef struct jit jit_t;

jit_t *jit_create(mem_bus_t *bus, size_t code_bytes);
void   jit_destroy(jit_t *j);

// Translate (or fetch from cache) the block at cpu's current CS:EIP. Blocks are
// cached by physical address and never cross a 4 KB page.
// Works on any host, so translation can be unit-tested off-device.
// May raise #PF (through cpu_fault) when the code page is not mapped: only call it
// inside jit_run, or with paging off.
bool jit_get_block(jit_t *j, cpu_t *cpu, jit_block_info *out);

// Run guest code until halt, an unsupported instruction, or max_blocks blocks.
// Sets cpu->mem to the JIT's bus if it is not set yet.
jit_exit_t jit_run(jit_t *j, cpu_t *cpu, uint64_t max_blocks);

// Debugging: translate one guest instruction per block (so traces and the
// recent-block ring see every instruction), and print CPU state to stderr
// before each block whose linear address is in [lo, hi].
void jit_set_single_step(jit_t *j, bool on);
void jit_set_trace(jit_t *j, uint32_t lo, uint32_t hi);

// Drop all translated blocks (also called automatically on writes to code pages).
void jit_flush(jit_t *j);

// Stats
uint64_t jit_blocks_translated(const jit_t *j);
uint64_t jit_chain_links(const jit_t *j);      // block-chaining slots patched so far
uint64_t jit_dispatches(const jit_t *j);       // blocks entered from the dispatcher (not chained / cached)

// The linear addresses of the most recently executed blocks, oldest first
// (for diagnostics). Returns how many were written to out.
#define JIT_TRACE_LEN 256
int jit_recent_blocks(const jit_t *j, uint32_t *out, int max);

// Several CPUs share this JIT (and the host thread): PAUSE then ends its block.
void jit_set_smp(jit_t *j, bool on);
// x87 data moves as ARM64 code (on by default; BEEMU_NO_X87_INLINE=1). Flushes the cache. For tests.
void jit_set_x87_inline(jit_t *j, bool on);
void jit_set_regcache(jit_t *j, bool on);          // guest registers in host registers within a block

#endif
