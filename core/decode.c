#include "decode.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *const op_names[] = {
#define X(n) #n,
    OPLIST(X)
#undef X
};

const char *op_name(op_t op) { return op < OP__COUNT ? op_names[op] : "?"; }

/* ------------------------------------------------------------------ */
/* Byte cursor                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    const uint8_t *p;
    size_t n, pos;
    bool err;
} cur_t;

static uint8_t rd8(cur_t *c)
{
    if (c->pos >= c->n || c->pos >= X86_MAX_INSN_LEN) { c->err = true; return 0; }
    return c->p[c->pos++];
}
static uint16_t rd16(cur_t *c) { uint16_t lo = rd8(c); uint16_t hi = rd8(c); return (uint16_t)(lo | (hi << 8)); }
static uint32_t rd32(cur_t *c) { uint32_t lo = rd16(c); uint32_t hi = rd16(c); return lo | (hi << 16); }

// Read an immediate of `size` bytes, optionally sign-extended to 32 bits.
static uint32_t rd_imm(cur_t *c, int size, bool sx)
{
    switch (size) {
    case 1: { uint8_t v = rd8(c);   return sx ? (uint32_t)(int32_t)(int8_t)v  : v; }
    case 2: { uint16_t v = rd16(c); return sx ? (uint32_t)(int32_t)(int16_t)v : v; }
    default: return rd32(c);
    }
}

/* ------------------------------------------------------------------ */
/* Operand helpers                                                     */
/* ------------------------------------------------------------------ */

static void set_op(operand_t *o, opk_t kind, int reg, int size)
{
    o->kind = (uint8_t)kind;
    o->reg  = (uint8_t)reg;
    o->size = (uint8_t)size;
}

static void decode_modrm(cur_t *c, insn_t *i)
{
    uint8_t m = rd8(c);
    i->has_modrm = true;
    i->mod = m >> 6;
    i->reg = (m >> 3) & 7;
    i->rm  = m & 7;
    if (i->mod == 3) return;

    memref_t *mem = &i->mem;
    mem->base = -1; mem->index = -1; mem->shift = 0; mem->disp = 0;
    int defseg = S_DS;

    if (i->addrsize == 2) {
        static const int8_t bases[8] = { R_EBX, R_EBX, R_EBP, R_EBP, R_ESI, R_EDI, R_EBP, R_EBX };
        static const int8_t idxs[8]  = { R_ESI, R_EDI, R_ESI, R_EDI, -1, -1, -1, -1 };
        mem->base  = bases[i->rm];
        mem->index = idxs[i->rm];
        if (i->mod == 0 && i->rm == 6) {
            mem->base = -1;
            mem->disp = (int16_t)rd16(c);
        } else if (i->mod == 1) {
            mem->disp = (int8_t)rd8(c);
        } else if (i->mod == 2) {
            mem->disp = (int16_t)rd16(c);
        }
        if (mem->base == R_EBP) defseg = S_SS;
    } else {
        if (i->rm == 4) {
            uint8_t sib = rd8(c);
            int index = (sib >> 3) & 7, base = sib & 7;
            mem->shift = sib >> 6;
            mem->index = (index == 4) ? -1 : index;
            if (base == 5 && i->mod == 0) { mem->base = -1; mem->disp = (int32_t)rd32(c); }
            else mem->base = base;
        } else if (i->rm == 5 && i->mod == 0) {
            mem->base = -1;
            mem->disp = (int32_t)rd32(c);
        } else {
            mem->base = i->rm;
        }
        if (i->mod == 1)      mem->disp = (int8_t)rd8(c);
        else if (i->mod == 2) mem->disp = (int32_t)rd32(c);
        if (mem->base == R_ESP || mem->base == R_EBP) defseg = S_SS;
    }
    mem->seg = (uint8_t)(i->seg_override >= 0 ? i->seg_override : defseg);
}

static void set_E(insn_t *i, operand_t *o, int size)
{
    if (i->mod == 3) set_op(o, OPK_REG, i->rm, size);
    else             set_op(o, OPK_MEM, 0, size);
}
static void set_G(insn_t *i, operand_t *o, int size) { set_op(o, OPK_REG, i->reg, size); }

// modrm form with E and G operands. g_is_dst: G is destination (e.g. "add Gv,Ev").
static void form_E_G(cur_t *c, insn_t *i, int size, bool g_is_dst)
{
    decode_modrm(c, i);
    i->opsize = (uint8_t)size;
    set_E(i, g_is_dst ? &i->src : &i->dst, size);
    set_G(i, g_is_dst ? &i->dst : &i->src, size);
}

static const op_t alu_ops[8]   = { OP_ADD, OP_OR, OP_ADC, OP_SBB, OP_AND, OP_SUB, OP_XOR, OP_CMP };
static const op_t shift_ops[8] = { OP_ROL, OP_ROR, OP_RCL, OP_RCR, OP_SHL, OP_SHR, OP_SHL, OP_SAR };

/* ------------------------------------------------------------------ */
/* Two-byte (0F xx) opcodes                                            */
/* ------------------------------------------------------------------ */

static void decode_0f(cur_t *c, insn_t *i)
{
    int osz = i->opsize;
    uint8_t op = rd8(c);
    i->opcode = (uint16_t)(0x0F00 | op);

    if (op >= 0x80 && op <= 0x8F) {
        i->op = OP_JCC; i->cond = op & 15;
        set_op(&i->dst, OPK_REL, 0, osz);
        i->imm = rd_imm(c, osz, true);
        return;
    }
    if (op >= 0x90 && op <= 0x9F) {
        i->op = OP_SETCC; i->cond = op & 15;
        decode_modrm(c, i); i->opsize = 1;
        set_E(i, &i->dst, 1);
        return;
    }
    if (op >= 0x40 && op <= 0x4F) {
        i->op = OP_CMOVCC; i->cond = op & 15;
        form_E_G(c, i, osz, true);
        return;
    }
    if (op >= 0xC8 && op <= 0xCF) {
        i->op = OP_BSWAP; i->opsize = 4;
        set_op(&i->dst, OPK_REG, op & 7, 4);
        return;
    }

    switch (op) {
    case 0x00:   // group 6
        decode_modrm(c, i); i->opsize = 2;
        set_E(i, &i->dst, 2);
        switch (i->reg) {
        case 0: i->op = OP_SLDT; break;
        case 1: i->op = OP_STR;  break;
        case 2: i->op = OP_LLDT; break;
        case 3: i->op = OP_LTR;  break;
        default: i->op = OP_UNSUPPORTED; break;   // VERR / VERW
        }
        return;
    case 0x01:   // group 7
        decode_modrm(c, i);
        if (i->mod == 3 && i->reg != 4 && i->reg != 6) { i->op = OP_UNSUPPORTED; return; }
        set_E(i, &i->dst, (i->reg == 4 || i->reg == 6) ? 2 : 0);
        switch (i->reg) {
        case 0: i->op = OP_SGDT;   break;
        case 1: i->op = OP_SIDT;   break;
        case 2: i->op = OP_LGDT;   break;
        case 3: i->op = OP_LIDT;   break;
        case 4: i->op = OP_SMSW;   break;
        case 6: i->op = OP_LMSW;   break;
        case 7: i->op = OP_INVLPG; break;
        default: i->op = OP_INVALID; break;
        }
        return;
    case 0x02: case 0x03:   // LAR / LSL
        decode_modrm(c, i); i->op = OP_UNSUPPORTED; return;
    case 0x06: i->op = OP_CLTS;   return;
    case 0x08: i->op = OP_INVD;   return;
    case 0x09: i->op = OP_WBINVD; return;
    case 0x0B: i->op = OP_INVALID; return;   // UD2
    case 0x20: case 0x21: case 0x22: case 0x23: {
        decode_modrm(c, i); i->opsize = 4; i->op = OP_MOV;
        opk_t k = (op & 1) ? OPK_DR : OPK_CR;
        if (op < 0x22) { set_op(&i->dst, OPK_REG, i->rm, 4); set_op(&i->src, k, i->reg, 4); }
        else           { set_op(&i->dst, k, i->reg, 4);      set_op(&i->src, OPK_REG, i->rm, 4); }
        return;
    }
    case 0x30: i->op = OP_WRMSR; return;
    case 0x31: i->op = OP_RDTSC; return;
    case 0x32: i->op = OP_RDMSR; return;
    case 0x33: i->op = OP_RDPMC; return;
    case 0x34: case 0x35: i->op = OP_UNSUPPORTED; return;   // SYSENTER / SYSEXIT
    case 0x77: i->op = OP_UNSUPPORTED; return;               // EMMS

    case 0xA0: i->op = OP_PUSH; set_op(&i->src, OPK_SEG, S_FS, 2); return;
    case 0xA1: i->op = OP_POP;  set_op(&i->dst, OPK_SEG, S_FS, 2); return;
    case 0xA8: i->op = OP_PUSH; set_op(&i->src, OPK_SEG, S_GS, 2); return;
    case 0xA9: i->op = OP_POP;  set_op(&i->dst, OPK_SEG, S_GS, 2); return;
    case 0xA2: i->op = OP_CPUID; return;

    case 0xA3: i->op = OP_BT;  form_E_G(c, i, osz, false); return;
    case 0xAB: i->op = OP_BTS; form_E_G(c, i, osz, false); return;
    case 0xB3: i->op = OP_BTR; form_E_G(c, i, osz, false); return;
    case 0xBB: i->op = OP_BTC; form_E_G(c, i, osz, false); return;
    case 0xBA:
        decode_modrm(c, i); i->opsize = (uint8_t)osz;
        set_E(i, &i->dst, osz);
        set_op(&i->src, OPK_IMM, 0, 1);
        i->imm = rd8(c);
        switch (i->reg) {
        case 4: i->op = OP_BT;  break;
        case 5: i->op = OP_BTS; break;
        case 6: i->op = OP_BTR; break;
        case 7: i->op = OP_BTC; break;
        default: i->op = OP_INVALID; break;
        }
        return;
    case 0xBC: i->op = OP_BSF; form_E_G(c, i, osz, true); return;
    case 0xBD: i->op = OP_BSR; form_E_G(c, i, osz, true); return;

    case 0xA4: case 0xA5: case 0xAC: case 0xAD:
        i->op = (op < 0xA8) ? OP_SHLD : OP_SHRD;
        form_E_G(c, i, osz, false);
        if (op & 1) set_op(&i->src2, OPK_REG, 1, 1);          // CL
        else { set_op(&i->src2, OPK_IMM, 0, 1); i->imm = rd8(c); }
        return;

    case 0xAF: i->op = OP_IMUL; form_E_G(c, i, osz, true); return;
    case 0xB0: i->op = OP_CMPXCHG; form_E_G(c, i, 1, false);   return;
    case 0xB1: i->op = OP_CMPXCHG; form_E_G(c, i, osz, false); return;
    case 0xC7:   // group 9: only CMPXCHG8B m64 on a Pentium III
        decode_modrm(c, i);
        if (i->mod == 3 || i->reg != 1) { i->op = OP_INVALID; return; }
        i->op = OP_CMPXCHG8B; i->opsize = 4;
        set_op(&i->dst, OPK_MEM, 0, 8);
        return;
    case 0x18:   // PREFETCHh (SSE hints) and the reserved hint-NOP space: no effect here
    case 0x1F:   // multi-byte NOP (P6)
        decode_modrm(c, i);
        i->op = OP_NOP;
        return;
    case 0xC0: i->op = OP_XADD;    form_E_G(c, i, 1, false);   return;
    case 0xC1: i->op = OP_XADD;    form_E_G(c, i, osz, false); return;

    case 0xB2: case 0xB4: case 0xB5:
        decode_modrm(c, i);
        if (i->mod == 3) { i->op = OP_INVALID; return; }
        i->op = (op == 0xB2) ? OP_LSS : (op == 0xB4) ? OP_LFS : OP_LGS;
        i->opsize = (uint8_t)osz;
        set_G(i, &i->dst, osz); set_op(&i->src, OPK_MEM, 0, osz);
        return;

    case 0xB6: case 0xB7: case 0xBE: case 0xBF: {
        int ssz = (op & 1) ? 2 : 1;
        decode_modrm(c, i);
        i->op = (op < 0xB8) ? OP_MOVZX : OP_MOVSX;
        i->opsize = (uint8_t)osz;
        set_G(i, &i->dst, osz);
        set_E(i, &i->src, ssz);
        return;
    }
    default: break;
    }

    // MMX / SSE / misc: valid encodings we don't execute. Consume modrm (+ imm8)
    // so the length is right and decoding stays in sync.
    bool modrm = (op >= 0x10 && op <= 0x18) || (op >= 0x28 && op <= 0x2F) ||
                 (op >= 0x50 && op <= 0x76) || (op >= 0x78 && op <= 0x7F) ||
                 op == 0xAE || (op >= 0xC2 && op <= 0xC6) || (op >= 0xD0 && op <= 0xFE);
    bool imm8  = (op >= 0x70 && op <= 0x73) || op == 0xC2 || op == 0xC4 || op == 0xC5 || op == 0xC6;
    if (modrm) {
        decode_modrm(c, i);
        if (imm8) (void)rd8(c);
        i->op = OP_UNSUPPORTED;
    } else {
        i->op = OP_INVALID;
    }
}

/* ------------------------------------------------------------------ */
/* One-byte opcodes                                                    */
/* ------------------------------------------------------------------ */

int decode_insn(const uint8_t *buf, size_t avail, bool code32, insn_t *out)
{
    cur_t c = { buf, avail, 0, false };
    insn_t *i = out;
    memset(i, 0, sizeof(*i));
    i->seg_override = -1;
    i->mem.base = -1; i->mem.index = -1;

    int osz = code32 ? 4 : 2;
    int asz = code32 ? 4 : 2;

    // ---- prefixes ----
    bool more = true;
    while (more) {
        uint8_t b = rd8(&c);
        if (c.err) goto fail;
        switch (b) {
        case 0x66: osz ^= 6; break;     // 2 <-> 4
        case 0x67: asz ^= 6; break;
        case 0xF0: i->lock = true; break;
        case 0xF2: i->rep = 2; break;
        case 0xF3: i->rep = 1; break;
        case 0x26: i->seg_override = S_ES; break;
        case 0x2E: i->seg_override = S_CS; break;
        case 0x36: i->seg_override = S_SS; break;
        case 0x3E: i->seg_override = S_DS; break;
        case 0x64: i->seg_override = S_FS; break;
        case 0x65: i->seg_override = S_GS; break;
        default: c.pos--; more = false; break;
        }
    }
    i->opsize = (uint8_t)osz;
    i->addrsize = (uint8_t)asz;

    uint8_t op = rd8(&c);
    if (c.err) goto fail;
    i->opcode = op;

    if (op == 0x0F) {
        decode_0f(&c, i);
        goto done;
    }

    // ---- ALU block 00-3F ----
    if (op < 0x40) {
        if ((op & 7) < 6) {
            i->op = alu_ops[op >> 3];
            switch (op & 7) {
            case 0: form_E_G(&c, i, 1, false);   break;
            case 1: form_E_G(&c, i, osz, false); break;
            case 2: form_E_G(&c, i, 1, true);    break;
            case 3: form_E_G(&c, i, osz, true);  break;
            case 4:
                i->opsize = 1;
                set_op(&i->dst, OPK_REG, R_EAX, 1);
                set_op(&i->src, OPK_IMM, 0, 1);
                i->imm = rd8(&c);
                break;
            default:
                set_op(&i->dst, OPK_REG, R_EAX, osz);
                set_op(&i->src, OPK_IMM, 0, osz);
                i->imm = rd_imm(&c, osz, false);
                break;
            }
        } else if (op < 0x20) {
            if ((op & 7) == 6) { i->op = OP_PUSH; set_op(&i->src, OPK_SEG, (op >> 3) & 3, 2); }
            else               { i->op = OP_POP;  set_op(&i->dst, OPK_SEG, (op >> 3) & 3, 2); }
        } else {
            i->op = OP_UNSUPPORTED;   // DAA DAS AAA AAS
        }
        goto done;
    }

    switch (op) {
    case 0x40 ... 0x47: i->op = OP_INC; set_op(&i->dst, OPK_REG, op & 7, osz); break;
    case 0x48 ... 0x4F: i->op = OP_DEC; set_op(&i->dst, OPK_REG, op & 7, osz); break;
    case 0x50 ... 0x57: i->op = OP_PUSH; set_op(&i->src, OPK_REG, op & 7, osz); break;
    case 0x58 ... 0x5F: i->op = OP_POP;  set_op(&i->dst, OPK_REG, op & 7, osz); break;
    case 0x60: i->op = OP_PUSHA; break;
    case 0x61: i->op = OP_POPA;  break;
    case 0x62: case 0x63:   // BOUND, ARPL
        decode_modrm(&c, i); i->op = OP_UNSUPPORTED; break;
    case 0x68:
        i->op = OP_PUSH; set_op(&i->src, OPK_IMM, 0, osz);
        i->imm = rd_imm(&c, osz, false); break;
    case 0x6A:
        i->op = OP_PUSH; set_op(&i->src, OPK_IMM, 0, osz);
        i->imm = rd_imm(&c, 1, true); break;
    case 0x69: case 0x6B:
        decode_modrm(&c, i); i->op = OP_IMUL;
        set_G(i, &i->dst, osz); set_E(i, &i->src, osz);
        set_op(&i->src2, OPK_IMM, 0, osz);
        i->imm = rd_imm(&c, op == 0x69 ? osz : 1, op == 0x6B);
        break;
    case 0x6C: case 0x6D: case 0x6E: case 0x6F:
        i->op = (op < 0x6E) ? OP_INS : OP_OUTS;
        i->opsize = (op & 1) ? (uint8_t)osz : 1;
        break;

    case 0x70 ... 0x7F:
        i->op = OP_JCC; i->cond = op & 15;
        set_op(&i->dst, OPK_REL, 0, osz);
        i->imm = rd_imm(&c, 1, true); break;

    case 0x80: case 0x81: case 0x83: {
        int sz = (op == 0x80) ? 1 : osz;
        decode_modrm(&c, i); i->opsize = (uint8_t)sz;
        i->op = alu_ops[i->reg];
        set_E(i, &i->dst, sz);
        set_op(&i->src, OPK_IMM, 0, sz);
        i->imm = rd_imm(&c, op == 0x81 ? sz : 1, op == 0x83);
        break;
    }
    case 0x84: i->op = OP_TEST; form_E_G(&c, i, 1, false);   break;
    case 0x85: i->op = OP_TEST; form_E_G(&c, i, osz, false); break;
    case 0x86: i->op = OP_XCHG; form_E_G(&c, i, 1, false);   break;
    case 0x87: i->op = OP_XCHG; form_E_G(&c, i, osz, false); break;
    case 0x88: i->op = OP_MOV;  form_E_G(&c, i, 1, false);   break;
    case 0x89: i->op = OP_MOV;  form_E_G(&c, i, osz, false); break;
    case 0x8A: i->op = OP_MOV;  form_E_G(&c, i, 1, true);    break;
    case 0x8B: i->op = OP_MOV;  form_E_G(&c, i, osz, true);  break;
    case 0x8C:
        decode_modrm(&c, i); i->op = OP_MOV;
        set_E(i, &i->dst, i->mod == 3 ? osz : 2);
        set_op(&i->src, OPK_SEG, i->reg, 2);
        break;
    case 0x8D:
        decode_modrm(&c, i);
        if (i->mod == 3) { i->op = OP_INVALID; break; }
        i->op = OP_LEA; set_G(i, &i->dst, osz); set_op(&i->src, OPK_MEM, 0, osz);
        break;
    case 0x8E:
        decode_modrm(&c, i); i->op = OP_MOV;
        set_op(&i->dst, OPK_SEG, i->reg, 2);
        set_E(i, &i->src, 2);
        break;
    case 0x8F:
        decode_modrm(&c, i);
        if (i->reg != 0) { i->op = OP_INVALID; break; }
        i->op = OP_POP; set_E(i, &i->dst, osz);
        break;

    case 0x90: i->op = OP_NOP; break;
    case 0x91 ... 0x97:
        i->op = OP_XCHG;
        set_op(&i->dst, OPK_REG, R_EAX, osz);
        set_op(&i->src, OPK_REG, op & 7, osz);
        break;
    case 0x98: i->op = OP_CBW; break;
    case 0x99: i->op = OP_CWD; break;
    case 0x9A:
        i->op = OP_CALLF; set_op(&i->dst, OPK_FARPTR, 0, osz);
        i->imm = rd_imm(&c, osz, false); i->imm2 = rd16(&c); break;
    case 0x9B: i->op = OP_FWAIT; break;
    case 0x9C: i->op = OP_PUSHF; break;
    case 0x9D: i->op = OP_POPF;  break;
    case 0x9E: i->op = OP_SAHF;  break;
    case 0x9F: i->op = OP_LAHF;  break;

    case 0xA0 ... 0xA3: {
        int sz = (op & 1) ? osz : 1;
        i->op = OP_MOV; i->opsize = (uint8_t)sz;
        i->mem.base = -1; i->mem.index = -1;
        i->mem.disp = (int32_t)rd_imm(&c, asz, false);
        i->mem.seg = (uint8_t)(i->seg_override >= 0 ? i->seg_override : S_DS);
        operand_t *m = (op & 2) ? &i->dst : &i->src;
        operand_t *a = (op & 2) ? &i->src : &i->dst;
        set_op(m, OPK_MEM, 0, sz);
        set_op(a, OPK_REG, R_EAX, sz);
        break;
    }
    case 0xA4: case 0xA5: i->op = OP_MOVS; i->opsize = (op & 1) ? (uint8_t)osz : 1; break;
    case 0xA6: case 0xA7: i->op = OP_CMPS; i->opsize = (op & 1) ? (uint8_t)osz : 1; break;
    case 0xAA: case 0xAB: i->op = OP_STOS; i->opsize = (op & 1) ? (uint8_t)osz : 1; break;
    case 0xAC: case 0xAD: i->op = OP_LODS; i->opsize = (op & 1) ? (uint8_t)osz : 1; break;
    case 0xAE: case 0xAF: i->op = OP_SCAS; i->opsize = (op & 1) ? (uint8_t)osz : 1; break;
    case 0xA8:
        i->op = OP_TEST; i->opsize = 1;
        set_op(&i->dst, OPK_REG, R_EAX, 1); set_op(&i->src, OPK_IMM, 0, 1);
        i->imm = rd8(&c); break;
    case 0xA9:
        i->op = OP_TEST;
        set_op(&i->dst, OPK_REG, R_EAX, osz); set_op(&i->src, OPK_IMM, 0, osz);
        i->imm = rd_imm(&c, osz, false); break;

    case 0xB0 ... 0xB7:
        i->op = OP_MOV; i->opsize = 1;
        set_op(&i->dst, OPK_REG, op & 7, 1); set_op(&i->src, OPK_IMM, 0, 1);
        i->imm = rd8(&c); break;
    case 0xB8 ... 0xBF:
        i->op = OP_MOV;
        set_op(&i->dst, OPK_REG, op & 7, osz); set_op(&i->src, OPK_IMM, 0, osz);
        i->imm = rd_imm(&c, osz, false); break;

    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        int sz = (op & 1) ? osz : 1;
        decode_modrm(&c, i); i->opsize = (uint8_t)sz;
        i->op = shift_ops[i->reg];
        set_E(i, &i->dst, sz);
        if (op < 0xD0)      { set_op(&i->src, OPK_IMM, 0, 1); i->imm = rd8(&c); }
        else if (op < 0xD2) { set_op(&i->src, OPK_IMM, 0, 1); i->imm = 1; }
        else                { set_op(&i->src, OPK_REG, 1, 1); }   // CL
        break;
    }
    case 0xC2: i->op = OP_RET;  set_op(&i->src, OPK_IMM, 0, 2); i->imm = rd16(&c); break;
    case 0xC3: i->op = OP_RET;  break;
    case 0xCA: i->op = OP_RETF; set_op(&i->src, OPK_IMM, 0, 2); i->imm = rd16(&c); break;
    case 0xCB: i->op = OP_RETF; break;
    case 0xC4: case 0xC5:
        decode_modrm(&c, i);
        if (i->mod == 3) { i->op = OP_INVALID; break; }
        i->op = (op == 0xC4) ? OP_LES : OP_LDS;
        set_G(i, &i->dst, osz); set_op(&i->src, OPK_MEM, 0, osz);
        break;
    case 0xC6: case 0xC7: {
        int sz = (op & 1) ? osz : 1;
        decode_modrm(&c, i); i->opsize = (uint8_t)sz;
        if (i->reg != 0) { i->op = OP_INVALID; break; }
        i->op = OP_MOV; set_E(i, &i->dst, sz);
        set_op(&i->src, OPK_IMM, 0, sz);
        i->imm = rd_imm(&c, sz, false);
        break;
    }
    case 0xC8:
        i->op = OP_ENTER; set_op(&i->src, OPK_IMM, 0, 2);
        i->imm = rd16(&c); i->imm2 = rd8(&c); break;
    case 0xC9: i->op = OP_LEAVE; break;
    case 0xCC: i->op = OP_INT3; break;
    case 0xCD: i->op = OP_INT; set_op(&i->src, OPK_IMM, 0, 1); i->imm = rd8(&c); break;
    case 0xCE: i->op = OP_INTO; break;
    case 0xCF: i->op = OP_IRET; break;

    case 0xD4: case 0xD5: i->op = OP_UNSUPPORTED; (void)rd8(&c); break;   // AAM AAD
    case 0xD6: i->op = OP_UNSUPPORTED; break;                              // SALC
    case 0xD7: i->op = OP_XLAT; break;
    case 0xD8 ... 0xDF:   // x87 escape
        decode_modrm(&c, i); i->op = OP_FPU;
        if (i->mod != 3) set_E(i, &i->dst, 0);    // register forms name ST(i), kept in rm
        break;

    case 0xE0: i->op = OP_LOOPNE; goto rel8;
    case 0xE1: i->op = OP_LOOPE;  goto rel8;
    case 0xE2: i->op = OP_LOOP;   goto rel8;
    case 0xE3: i->op = OP_JCXZ;   goto rel8;
    case 0xEB: i->op = OP_JMP;
    rel8:
        set_op(&i->dst, OPK_REL, 0, osz);
        i->imm = rd_imm(&c, 1, true); break;
    case 0xE8: i->op = OP_CALL; set_op(&i->dst, OPK_REL, 0, osz); i->imm = rd_imm(&c, osz, true); break;
    case 0xE9: i->op = OP_JMP;  set_op(&i->dst, OPK_REL, 0, osz); i->imm = rd_imm(&c, osz, true); break;
    case 0xEA:
        i->op = OP_JMPF; set_op(&i->dst, OPK_FARPTR, 0, osz);
        i->imm = rd_imm(&c, osz, false); i->imm2 = rd16(&c); break;

    case 0xE4: case 0xE5: {   // IN acc, imm8
        int sz = (op & 1) ? osz : 1;
        i->op = OP_IN; i->opsize = (uint8_t)sz;
        set_op(&i->dst, OPK_REG, R_EAX, sz); set_op(&i->src, OPK_IMM, 0, 1);
        i->imm = rd8(&c); break;
    }
    case 0xE6: case 0xE7: {   // OUT imm8, acc
        int sz = (op & 1) ? osz : 1;
        i->op = OP_OUT; i->opsize = (uint8_t)sz;
        set_op(&i->dst, OPK_IMM, 0, 1); set_op(&i->src, OPK_REG, R_EAX, sz);
        i->imm = rd8(&c); break;
    }
    case 0xEC: case 0xED: {   // IN acc, DX
        int sz = (op & 1) ? osz : 1;
        i->op = OP_IN; i->opsize = (uint8_t)sz;
        set_op(&i->dst, OPK_REG, R_EAX, sz); set_op(&i->src, OPK_REG, R_EDX, 2);
        break;
    }
    case 0xEE: case 0xEF: {   // OUT DX, acc
        int sz = (op & 1) ? osz : 1;
        i->op = OP_OUT; i->opsize = (uint8_t)sz;
        set_op(&i->dst, OPK_REG, R_EDX, 2); set_op(&i->src, OPK_REG, R_EAX, sz);
        break;
    }

    case 0xF4: i->op = OP_HLT; break;
    case 0xF5: i->op = OP_CMC; break;
    case 0xF8: i->op = OP_CLC; break;
    case 0xF9: i->op = OP_STC; break;
    case 0xFA: i->op = OP_CLI; break;
    case 0xFB: i->op = OP_STI; break;
    case 0xFC: i->op = OP_CLD; break;
    case 0xFD: i->op = OP_STD; break;

    case 0xF6: case 0xF7: {   // group 3
        int sz = (op & 1) ? osz : 1;
        decode_modrm(&c, i); i->opsize = (uint8_t)sz;
        set_E(i, &i->dst, sz);
        switch (i->reg) {
        case 0: case 1:
            i->op = OP_TEST; set_op(&i->src, OPK_IMM, 0, sz);
            i->imm = rd_imm(&c, sz, false); break;
        case 2: i->op = OP_NOT;  break;
        case 3: i->op = OP_NEG;  break;
        case 4: i->op = OP_MUL;  break;
        case 5: i->op = OP_IMUL; break;
        case 6: i->op = OP_DIV;  break;
        default: i->op = OP_IDIV; break;
        }
        break;
    }
    case 0xFE: case 0xFF: {   // groups 4 / 5
        int sz = (op & 1) ? osz : 1;
        decode_modrm(&c, i); i->opsize = (uint8_t)sz;
        switch (i->reg) {
        case 0: i->op = OP_INC; set_E(i, &i->dst, sz); break;
        case 1: i->op = OP_DEC; set_E(i, &i->dst, sz); break;
        case 2: if (op == 0xFE) { i->op = OP_INVALID; break; }
                i->op = OP_CALL; set_E(i, &i->dst, sz); break;
        case 3: if (op == 0xFE || i->mod == 3) { i->op = OP_INVALID; break; }
                i->op = OP_CALLF; set_E(i, &i->dst, sz); break;
        case 4: if (op == 0xFE) { i->op = OP_INVALID; break; }
                i->op = OP_JMP; set_E(i, &i->dst, sz); break;
        case 5: if (op == 0xFE || i->mod == 3) { i->op = OP_INVALID; break; }
                i->op = OP_JMPF; set_E(i, &i->dst, sz); break;
        case 6: if (op == 0xFE) { i->op = OP_INVALID; break; }
                i->op = OP_PUSH; set_E(i, &i->src, sz); break;
        default: i->op = OP_INVALID; break;
        }
        break;
    }
    default:
        i->op = OP_INVALID;
        break;
    }

done:
    if (c.err) goto fail;
    i->len = (uint8_t)c.pos;
    return (int)c.pos;

fail:
    memset(out, 0, sizeof(*out));
    return 0;
}

/* ------------------------------------------------------------------ */
/* Trace formatter                                                     */
/* ------------------------------------------------------------------ */

static const char *const r8n[8]   = { "al","cl","dl","bl","ah","ch","dh","bh" };
static const char *const r16n[8]  = { "ax","cx","dx","bx","sp","bp","si","di" };
static const char *const r32n[8]  = { "eax","ecx","edx","ebx","esp","ebp","esi","edi" };
static const char *const sregn[8] = { "es","cs","ss","ds","fs","gs","?","?" };
static const char *const ccn[16]  = { "o","no","b","ae","e","ne","be","a",
                                      "s","ns","p","np","l","ge","le","g" };

typedef struct { char *buf; size_t n, len; } sb_t;

static void app(sb_t *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    size_t room = s->len < s->n ? s->n - s->len : 0;
    int w = vsnprintf(s->buf + s->len, room, fmt, ap);
    va_end(ap);
    if (w > 0) s->len += (size_t)w;
}

static const char *regname(int reg, int size)
{
    return size == 1 ? r8n[reg & 7] : size == 2 ? r16n[reg & 7] : r32n[reg & 7];
}

static void fmt_operand(sb_t *s, const insn_t *i, const operand_t *o, uint32_t eip)
{
    switch (o->kind) {
    case OPK_REG: app(s, "%s", regname(o->reg, o->size)); break;
    case OPK_SEG: app(s, "%s", sregn[o->reg & 7]); break;
    case OPK_CR:  app(s, "cr%d", o->reg); break;
    case OPK_DR:  app(s, "dr%d", o->reg); break;
    case OPK_IMM: {
        uint32_t v = i->imm;
        if (o->size == 1) v &= 0xFF; else if (o->size == 2) v &= 0xFFFF;
        app(s, "0x%x", v);
        break;
    }
    case OPK_REL: {
        uint32_t t = eip + i->len + i->imm;
        if (o->size == 2) t &= 0xFFFF;
        app(s, "0x%x", t);
        break;
    }
    case OPK_FARPTR: app(s, "0x%x:0x%x", i->imm2, i->imm); break;
    case OPK_MEM: {
        const memref_t *m = &i->mem;
        const char *const *names = (i->addrsize == 2) ? r16n : r32n;
        bool first = true;
        app(s, "[%s:", sregn[m->seg & 7]);
        if (m->base >= 0)  { app(s, "%s", names[m->base]); first = false; }
        if (m->index >= 0) {
            app(s, "%s%s", first ? "" : "+", names[m->index]);
            if (m->shift) app(s, "*%d", 1 << m->shift);
            first = false;
        }
        if (first) {
            uint32_t d = (uint32_t)m->disp;
            if (i->addrsize == 2) d &= 0xFFFF;
            app(s, "0x%x", d);
        } else if (m->disp < 0) {
            app(s, "-0x%x", (uint32_t)(-(int64_t)m->disp));
        } else if (m->disp > 0) {
            app(s, "+0x%x", (uint32_t)m->disp);
        }
        app(s, "]");
        break;
    }
    default: break;
    }
}

void insn_format(const insn_t *i, uint32_t eip, char *buf, size_t n)
{
    sb_t s = { buf, n, 0 };
    if (n) buf[0] = 0;

    if (i->lock) app(&s, "lock ");
    if (i->rep) {
        bool cmp = (i->op == OP_CMPS || i->op == OP_SCAS);
        app(&s, i->rep == 2 ? "repne " : (cmp ? "repe " : "rep "));
    }

    char name[24];
    size_t k = 0;
    const char *base = op_name(i->op);
    const char *prefix = "";
    if (i->op == OP_JCC) { prefix = "j"; base = ccn[i->cond & 15]; }
    else if (i->op == OP_SETCC)  { prefix = "set";  base = ccn[i->cond & 15]; }
    else if (i->op == OP_CMOVCC) { prefix = "cmov"; base = ccn[i->cond & 15]; }
    for (const char *p = prefix; *p && k < sizeof(name) - 2; p++) name[k++] = *p;
    for (const char *p = base;   *p && k < sizeof(name) - 2; p++) name[k++] = (char)tolower((unsigned char)*p);
    if (i->op == OP_MOVS || i->op == OP_CMPS || i->op == OP_STOS || i->op == OP_LODS ||
        i->op == OP_SCAS || i->op == OP_INS  || i->op == OP_OUTS)
        name[k++] = (i->opsize == 1) ? 'b' : (i->opsize == 2) ? 'w' : 'd';
    name[k] = 0;
    app(&s, "%s", name);

    const operand_t *ops[3] = { &i->dst, &i->src, &i->src2 };
    bool firstop = true;
    for (int j = 0; j < 3; j++) {
        if (ops[j]->kind == OPK_NONE) continue;
        app(&s, firstop ? " " : ", ");
        fmt_operand(&s, i, ops[j], eip);
        firstop = false;
    }
}
