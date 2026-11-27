// Guest CPU state for a Pentium 3-class x86-32 core.
// Designed for N CPUs from day one: no global CPU state anywhere.
#ifndef CPU_H
#define CPU_H

#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>

#define MAX_CPUS 8

struct mem_bus;   // mem.h
struct io_bus;    // io.h

// General register indices (x86 encoding order)
enum { R_EAX = 0, R_ECX, R_EDX, R_EBX, R_ESP, R_EBP, R_ESI, R_EDI };
// Segment register indices
enum { S_ES = 0, S_CS, S_SS, S_DS, S_FS, S_GS };

// EFLAGS bits
#define EFL_CF (1u << 0)
#define EFL_PF (1u << 2)
#define EFL_AF (1u << 4)
#define EFL_ZF (1u << 6)
#define EFL_SF (1u << 7)
#define EFL_TF (1u << 8)
#define EFL_IF (1u << 9)
#define EFL_DF (1u << 10)
#define EFL_OF (1u << 11)
#define EFL_IOPL (3u << 12)
#define EFL_NT (1u << 14)
#define EFL_RF (1u << 16)
#define EFL_VM (1u << 17)
#define EFL_AC (1u << 18)
#define EFL_VIF (1u << 19)
#define EFL_VIP (1u << 20)
#define EFL_ID (1u << 21)

// CR0 bits
#define CR0_PE (1u << 0)
#define CR0_MP (1u << 1)
#define CR0_EM (1u << 2)
#define CR0_TS (1u << 3)
#define CR0_ET (1u << 4)
#define CR0_NE (1u << 5)
#define CR0_WP (1u << 16)
#define CR0_AM (1u << 18)
#define CR0_NW (1u << 29)
#define CR0_CD (1u << 30)
#define CR0_PG (1u << 31)

// Lazy flags: record the last flag-setting op and its operands;
// compute CF/PF/AF/ZF/SF/OF only when something actually reads them.
//
// State layout (see flags.c):
//   cc_dst  = result
//   cc_src1 = first operand  (INC/DEC: the old CF, 0 or 1)
//   cc_src2 = second operand (shifts: the count, already masked to 5 bits and nonzero)
// ADC/SBB recover their carry-in from dst/src1/src2, so no extra field is needed.
//   MUL/IMUL (from generated code): cc_src1 = 1 if the product overflowed the low half
//   (CF = OF = it), SF/ZF/PF from the low result, AF = 0 (undefined on hardware).
// Operations not listed (rotates, BT, ..., and MUL/IMUL in C helpers) are computed eagerly:
// the helper writes the flags into eflags and sets CC_OP_EFLAGS.
//
// Order matters: each family has three sizes, 8/16/32, in that order.
typedef enum {
    CC_OP_EFLAGS = 0,   // flags already materialized in eflags
    CC_OP_ADD8,   CC_OP_ADD16,   CC_OP_ADD32,
    CC_OP_ADC8,   CC_OP_ADC16,   CC_OP_ADC32,
    CC_OP_SUB8,   CC_OP_SUB16,   CC_OP_SUB32,
    CC_OP_SBB8,   CC_OP_SBB16,   CC_OP_SBB32,
    CC_OP_LOGIC8, CC_OP_LOGIC16, CC_OP_LOGIC32,
    CC_OP_INC8,   CC_OP_INC16,   CC_OP_INC32,
    CC_OP_DEC8,   CC_OP_DEC16,   CC_OP_DEC32,
    CC_OP_SHL8,   CC_OP_SHL16,   CC_OP_SHL32,
    CC_OP_SHR8,   CC_OP_SHR16,   CC_OP_SHR32,
    CC_OP_SAR8,   CC_OP_SAR16,   CC_OP_SAR32,
    CC_OP_MUL8,   CC_OP_MUL16,   CC_OP_MUL32,   // MUL/IMUL: dst = low result, src1 = overflow (CF = OF)
    CC_OP__COUNT
} cc_op_t;

// CR4 bits
#define CR4_TSD (1u << 2)
#define CR4_DE  (1u << 3)
#define CR4_PSE (1u << 4)
#define CR4_PAE (1u << 5)
#define CR4_PGE (1u << 7)

// Software TLB (see system.c): direct-mapped by linear page number.
#define TLB_SIZE 1024
#define TLB_U 0x01        // user access allowed (U/S in every level)
#define TLB_W 0x02        // writes allowed (R/W in every level)
#define TLB_D 0x04        // dirty bit already set in the page table
#define TLB_G 0x08        // global page (CR4.PGE): survives CR3 writes
typedef struct {
    uint32_t vpn1;        // linear page number + 1 (0 = empty)
    uint32_t pfn;         // physical page address
    uint8_t  flags;
} tlb_entry_t;

// Fast TLB, read inline by generated code: for pages that are plain RAM (and, for
// writes, not ROM and not holding translated code), host address = addend + linear
// address. tag[user * 2 + write] is the linear page number + 1 when that access is
// allowed, else 0. Filled by the slow path after a successful access (see system.c).
#define FTLB_SIZE 1024
typedef struct {
    uint32_t tag[4];
    uint64_t addend;
    uint32_t vpn1;        // linear page number + 1 the entry describes (0 = empty)
    uint32_t phys;        // physical page number
} ftlb_entry_t;           // 32 bytes: generated code indexes with LSL #5

// Jump cache, read inline by generated code at ret / indirect jumps: linear address
// and block mode -> translated block body. Filled by the dispatcher; cleared with the
// fast TLB (mapping changes) and on full JIT flushes. A stale entry for an
// invalidated block is harmless: that body's first word jumps to the exit stub.
#define JC_SIZE 4096
typedef struct {
    uint32_t lin, mode;
    const uint32_t *body;
} jc_entry_t;             // 16 bytes: generated code indexes with LSL #4

// Segment descriptor attribute bits, at the same positions as in a GDT entry's
// high dword (seg_t.flags keeps that dword's attribute bits).
#define SEG_TYPE(f)  (((f) >> 8) & 0xFu)
#define SEG_S        (1u << 12)   // 1 = code/data, 0 = system
#define SEG_DPL(f)   (((f) >> 13) & 3u)
#define SEG_P        (1u << 15)
#define SEG_DB       (1u << 22)   // default operand/address size: 1 = 32-bit
#define SEG_G        (1u << 23)

typedef struct {
    uint16_t selector;
    uint32_t base;
    uint32_t limit;
    uint32_t flags;      // cached descriptor attributes
} seg_t;

typedef struct {
    uint32_t base;
    uint16_t limit;
} dtr_t;

typedef struct {
    // --- core registers ---
    uint32_t gpr[8];
    uint32_t eip;
    uint32_t eflags;     // materialized bits (valid except lazily-tracked flags)

    // --- lazy flag state ---
    cc_op_t  cc_op;
    uint32_t cc_dst, cc_src1, cc_src2;

    ftlb_entry_t *ftlb_base;  // = ftlb (a pointer at a small offset, for generated code)
    jc_entry_t   *jc_base;    // = jc
    uint32_t *chain_from;     // block chaining: unlinked exit slot just taken (see jit.c)
    int32_t   chain_budget;   // chained block exits left before returning to the dispatcher

    // --- segmentation / system ---
    seg_t    seg[6];
    dtr_t    gdtr, idtr;
    seg_t    ldtr, tr;
    uint32_t cr0, cr2, cr3, cr4;
    uint32_t dr[8];
    uint8_t  cpl;
    bool     halted;
    bool     smc_hit;         // a JIT store modified translated code (checked after the instruction)

    // --- FPU / MMX / SSE (Pentium 3) ---
    // x87 stored as raw 80-bit extended; exactness vs. speed decision pending
    uint8_t  st[8][10];
    uint16_t fpu_cw, fpu_sw, fpu_tw;   // st[] is indexed by physical register; TOP is fpu_sw bits 11-13
    uint32_t fpu_ip, fpu_dp;           // last instruction / operand pointers (FNSTENV)
    uint16_t fpu_cs, fpu_ds, fpu_op;
    uint32_t mxcsr;
    uint8_t  xmm[8][16];

    // --- SMP identity ---
    int      cpu_index;       // 0 .. n_cpus-1
    uint8_t  apic_id;

    // --- MSRs ---
    int64_t  tsc_offset;      // guest TSC = host nanoseconds + tsc_offset
    uint32_t apic_base;       // IA32_APIC_BASE (low dword)
    bool     has_apic;        // local APIC present (multiprocessor machines): CPUID reports it
    uint32_t sysenter_cs, sysenter_esp, sysenter_eip;
    uint32_t perf_sel[2];     // P6 PerfEvtSel0/1 (MSR 0x186/0x187)
    uint64_t perf_ctr[2];     // PerfCtr0/1 (MSR 0xC1/0xC2), value at perf_base[i]
    uint64_t perf_base[2];    // TSC when the counter was last written or (re)started

    // --- machine wiring (kept across cpu_reset) ---
    struct mem_bus *mem;      // physical memory bus, used by JIT helpers
    struct io_bus  *io;       // port I/O bus (may be NULL)
    int    (*intr_ack)(void *opaque);   // INTA cycle: returns the vector (the PIC)
    void    *intr_opaque;

    // --- interrupt state ---
    bool     intr_line;       // INTR input asserted (driven by the PIC)
    bool     intr_shadow;     // MOV SS / POP SS: hold interrupts for one more block
    bool     exit_request;    // a device wants jit_run to return (JIT_EXIT_REQUEST)

    // --- fault unwinding (see system.h) ---
    jmp_buf  fault_jmp;       // set by jit_run; cpu_fault longjmps here after delivering
    uint8_t  delivering;      // 0, or 1/2 while delivering a fault / a double fault
    uint32_t stop;            // nonzero: jit_run returns this jit_exit_t after the longjmp

    // --- paging ---
    uint8_t  sys_access;      // nonzero: implicit supervisor accesses (descriptor tables, TSS,
                              // interrupt delivery) even at CPL 3
    tlb_entry_t tlb[TLB_SIZE];
    ftlb_entry_t ftlb[FTLB_SIZE];
    jc_entry_t   jc[JC_SIZE];

    // TODO: local APIC pointer.
} cpu_t;

// True if the current code segment defaults to 32-bit operand/address size.
static inline bool cpu_code32(const cpu_t *c)
{
    return (c->cr0 & 1u) && (c->seg[S_CS].flags & SEG_DB);
}

// True if the stack segment is 32-bit (ESP), false for 16-bit (SP).
static inline bool cpu_stack32(const cpu_t *c)
{
    return (c->cr0 & 1u) && (c->seg[S_SS].flags & SEG_DB);
}

void cpu_init(cpu_t *c, int index);
void cpu_reset(cpu_t *c);     // real-mode reset state (CS:IP = F000:FFF0)

#endif
