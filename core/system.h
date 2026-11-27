// x86 system architecture: guest linear memory, segmentation, interrupt and
// exception delivery, far control transfers, control registers, MSRs, CPUID.
//
// Faults. Code running inside jit_run (C helpers called from generated code)
// raises an exception with cpu_fault(): it delivers the exception right away
// (pushing the frame, loading CS:EIP from the IVT/IDT) and then longjmps back
// to the dispatcher, abandoning the current block. cpu->eip must hold the
// address of the faulting instruction when cpu_fault is called (the JIT's
// helper entry sets it). A fault while delivering becomes a double fault, and
// a fault while delivering that shuts the CPU down (JIT_EXIT_SHUTDOWN).
#ifndef SYSTEM_H
#define SYSTEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cpu.h"

// Exception vectors
enum {
    EXC_DE = 0, EXC_DB = 1, EXC_BP = 3, EXC_OF = 4, EXC_BR = 5, EXC_UD = 6, EXC_NM = 7,
    EXC_DF = 8, EXC_TS = 10, EXC_NP = 11, EXC_SS = 12, EXC_GP = 13, EXC_PF = 14,
    EXC_MF = 16, EXC_AC = 17,
};

// The fast TLB lookup (also used inline by generated code): host address of a guest access that
// stays within one page and that the fast TLB allows, or NULL. Only RAM is ever in it; write tags
// never cover ROM or pages holding translated code.
static inline uint8_t *cpu_fast_host(const cpu_t *c, uint32_t lin, int size, int write)
{
    if ((lin & 0xFFFu) + (uint32_t)size > 0x1000u) return NULL;
    uint32_t vpn = lin >> 12;
    const ftlb_entry_t *f = &c->ftlb[vpn & (FTLB_SIZE - 1)];
    if (f->tag[((c->cpl == 3 && !c->sys_access) ? 2 : 0) + write] != vpn + 1) return NULL;
    return (uint8_t *)(uintptr_t)(f->addend + lin);
}

/* ---- guest linear memory, through the MMU when CR0.PG is set ---- */
// Accesses check U/S and R/W against the current privilege (CPL 3 = user unless
// c->sys_access), set the accessed/dirty bits and raise #PF (CR2 = address).
// Writes that cross a page check both pages before writing anything.
uint32_t cpu_read_lin (cpu_t *c, uint32_t lin, int size);
void     cpu_write_lin(cpu_t *c, uint32_t lin, uint32_t value, int size);
// Fault now (#PF) unless [lin, lin+len) is writable: for instructions that write
// several places and must not leave partial results behind.
void     cpu_probe_write(cpu_t *c, uint32_t lin, uint32_t len);
// Instruction fetch: copies up to len bytes, stopping at the first byte that cannot
// be fetched (or faulting there if fault is true). Returns the number of bytes.
size_t   cpu_fetch(cpu_t *c, uint32_t lin, uint8_t *buf, size_t len, bool fault);
// Physical address of a code byte (fetch permission); false if not mapped (no fault).
bool     cpu_fetch_phys(cpu_t *c, uint32_t lin, bool fault, uint32_t *phys);
// Translation for diagnostics: no fault, no accessed/dirty updates, supervisor view.
bool     cpu_peek_phys(cpu_t *c, uint32_t lin, uint32_t *phys);
void     cpu_tlb_flush(cpu_t *c, bool keep_global);
void     cpu_tlb_invlpg(cpu_t *c, uint32_t lin);
// Fast TLB (cpu.h; flushing it also clears the jump cache): fill after a successful access through the slow path, flush on
// mapping changes, and drop write permission for a physical page that now holds code.
void     cpu_ftlb_fill(cpu_t *c, uint32_t lin, bool write);
void     cpu_ftlb_flush(cpu_t *c);
void     cpu_ftlb_drop_writes(cpu_t *c, uint32_t phys_page);

/* ---- stack, using the current SS (16- or 32-bit) ---- */
void     cpu_push(cpu_t *c, uint32_t value, int size);
uint32_t cpu_pop (cpu_t *c, int size);

/* ---- faults ---- */
_Noreturn void cpu_fault(cpu_t *c, int vec, uint32_t err);
// Stop jit_run with the given jit_exit_t (for guest behavior not emulated yet).
_Noreturn void cpu_stop(cpu_t *c, uint32_t exit_code);

// Deliver interrupt `vec` with return address ret_eip. sw = INT n/INT3/INTO
// (gate DPL is checked); err is pushed if has_err (protected mode only).
void cpu_interrupt(cpu_t *c, int vec, uint32_t ret_eip, bool sw, bool has_err, uint32_t err);

/* ---- segmentation ---- */
// Load DS/ES/FS/GS/SS with full protected-mode checks (real mode: base = sel << 4).
void cpu_load_seg(cpu_t *c, int sreg, uint16_t sel);

// Far transfers. opsize = 2 or 4. eip / ret_eip are offsets in the new / current CS.
void cpu_far_jmp (cpu_t *c, uint16_t sel, uint32_t off, int opsize);
void cpu_far_call(cpu_t *c, uint16_t sel, uint32_t off, int opsize, uint32_t ret_eip);
void cpu_far_ret (cpu_t *c, int opsize, uint16_t imm);
void cpu_iret    (cpu_t *c, int opsize);

void cpu_lldt(cpu_t *c, uint16_t sel);
void cpu_ltr (cpu_t *c, uint16_t sel);

/* ---- flags ---- */
// Write EFLAGS from POPF/IRET: only bits in `mask` change, subject to CPL/IOPL rules.
void cpu_set_eflags(cpu_t *c, uint32_t value, uint32_t mask);

/* ---- control registers, MSRs, misc ---- */
// mov crN, value. Returns false for things not emulated yet (paging).
bool     cpu_write_cr(cpu_t *c, int n, uint32_t value);
uint32_t cpu_read_cr (cpu_t *c, int n);
void     cpu_rdmsr(cpu_t *c);   // ECX -> EDX:EAX, #GP on unknown MSRs
void     cpu_wrmsr(cpu_t *c);
void     cpu_cpuid(cpu_t *c);
uint64_t cpu_rdpmc(cpu_t *c, uint32_t idx);   // performance counter idx (0/1)
uint64_t cpu_read_tsc(const cpu_t *c);

// I/O permission for IN/OUT/INS/OUTS: IOPL, then the TSS I/O bitmap.
bool cpu_io_allowed(cpu_t *c, uint16_t port, int size);

static inline bool cpu_pmode(const cpu_t *c) { return (c->cr0 & CR0_PE) != 0; }

#endif
