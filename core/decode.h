// x86-32 instruction decoder (Pentium 3-class integer ISA + system ops).
//
// Pure function: bytes in, insn_t out. No CPU state, no memory access.
// The translator fetches up to 15 bytes through the MMU and hands them here.
//
// x87 escapes (D8-DF) decode to OP_FPU with opcode, mod/reg/rm and the memory
// operand (dst, size 0) filled in; the helper interprets them. Other valid but
// unemulated opcodes (MMX/SSE, BCD...) decode to OP_UNSUPPORTED with the
// correct length, so the decoder never loses sync on them.
#ifndef DECODE_H
#define DECODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cpu.h"

#define X86_MAX_INSN_LEN 15

#define OPLIST(X) \
    X(INVALID) X(UNSUPPORTED) \
    X(ADD) X(OR) X(ADC) X(SBB) X(AND) X(SUB) X(XOR) X(CMP) \
    X(INC) X(DEC) X(NOT) X(NEG) X(MUL) X(IMUL) X(DIV) X(IDIV) \
    X(PUSH) X(POP) X(PUSHA) X(POPA) X(PUSHF) X(POPF) \
    X(JCC) X(SETCC) X(CMOVCC) \
    X(TEST) X(XCHG) X(MOV) X(LEA) X(LES) X(LDS) X(LSS) X(LFS) X(LGS) \
    X(NOP) X(CBW) X(CWD) X(SAHF) X(LAHF) X(XLAT) \
    X(MOVS) X(CMPS) X(STOS) X(LODS) X(SCAS) X(INS) X(OUTS) \
    X(ROL) X(ROR) X(RCL) X(RCR) X(SHL) X(SHR) X(SAR) X(SHLD) X(SHRD) \
    X(RET) X(RETF) X(LEAVE) X(ENTER) X(INT3) X(INT) X(INTO) X(IRET) \
    X(CALL) X(CALLF) X(JMP) X(JMPF) X(LOOP) X(LOOPE) X(LOOPNE) X(JCXZ) \
    X(IN) X(OUT) X(HLT) X(CLC) X(STC) X(CMC) X(CLI) X(STI) X(CLD) X(STD) \
    X(BT) X(BTS) X(BTR) X(BTC) X(BSF) X(BSR) X(BSWAP) \
    X(CMPXCHG) X(CMPXCHG8B) X(XADD) X(MOVZX) X(MOVSX) \
    X(SGDT) X(SIDT) X(LGDT) X(LIDT) X(SMSW) X(LMSW) X(INVLPG) \
    X(SLDT) X(STR) X(LLDT) X(LTR) \
    X(CLTS) X(INVD) X(WBINVD) X(WRMSR) X(RDTSC) X(RDMSR) X(RDPMC) X(CPUID) \
    X(FPU) X(FWAIT)

typedef enum {
#define X(n) OP_##n,
    OPLIST(X)
#undef X
    OP__COUNT
} op_t;

typedef enum {
    OPK_NONE = 0,
    OPK_REG,      // reg = register index, size = 1/2/4 (size 1: 0-3 = AL..BL, 4-7 = AH..BH)
    OPK_MEM,      // address described by insn_t.mem
    OPK_IMM,      // value in insn_t.imm (sign-extended where the encoding says so)
    OPK_REL,      // branch displacement in insn_t.imm; target = eip + len + imm
    OPK_SEG,      // segment register, reg = S_ES..S_GS
    OPK_CR,       // control register, reg = 0..7
    OPK_DR,       // debug register, reg = 0..7
    OPK_FARPTR,   // seg:off, offset in imm, selector in imm2
} opk_t;

typedef struct {
    uint8_t kind;   // opk_t
    uint8_t size;   // operand size in bytes (0 = n/a)
    uint8_t reg;
} operand_t;

typedef struct {
    int8_t  base;    // R_E?? or -1.  In 16-bit addressing this is a 16-bit register.
    int8_t  index;   // R_E?? or -1
    uint8_t shift;   // index scale as log2 (0..3)
    uint8_t seg;     // effective segment (override or default), S_ES..S_GS
    int32_t disp;
} memref_t;

typedef struct {
    op_t     op;
    uint16_t opcode;       // raw opcode; two-byte opcodes are 0x0Fxx
    uint8_t  len;          // total length in bytes
    uint8_t  opsize;       // effective operation size: 1, 2 or 4
    uint8_t  addrsize;     // 2 or 4
    int8_t   seg_override; // -1 if none, else S_ES..S_GS
    bool     lock;
    uint8_t  rep;          // 0 = none, 1 = REP/REPE (F3), 2 = REPNE (F2)
    uint8_t  cond;         // condition code 0..15 for JCC / SETCC / CMOVCC
    bool     has_modrm;
    uint8_t  mod, reg, rm;
    operand_t dst, src, src2;   // destination first, Intel-style
    memref_t mem;
    uint32_t imm;
    uint32_t imm2;
} insn_t;

// Decode one instruction.
//   buf/avail: instruction bytes (avail may exceed 15; only what is needed is read)
//   code32:    true if the code segment's default operand/address size is 32-bit
// Returns the instruction length, or 0 if the bytes ran out (or exceeded 15).
// Illegal opcodes return their consumed length with op == OP_INVALID.
int decode_insn(const uint8_t *buf, size_t avail, bool code32, insn_t *out);

const char *op_name(op_t op);

// Human-readable form for traces, e.g. "mov eax, [ss:esp+0x4]".
// eip is the address of the instruction (used to print branch targets).
void insn_format(const insn_t *i, uint32_t eip, char *buf, size_t n);

#endif
