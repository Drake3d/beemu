#include "helpers.h"
#include "flags.h"
#include "fpu.h"
#include "io.h"
#include "jit.h"
#include "system.h"
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Operand access                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    cpu_t         *c;
    const insn_t  *in;
    uint32_t       lin;    // linear address of the memory operand, computed once
} hx_t;

static uint32_t msk(int size)  { return size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu; }
static uint32_t bits_of(int size) { return 8u * (uint32_t)size; }

static uint32_t sext(uint32_t v, int size)
{
    if (size == 1) return (uint32_t)(int32_t)(int8_t)v;
    if (size == 2) return (uint32_t)(int32_t)(int16_t)v;
    return v;
}

static uint32_t reg_rd(const cpu_t *c, int r, int size)
{
    if (size == 4) return c->gpr[r];
    if (size == 2) return c->gpr[r] & 0xFFFFu;
    return r < 4 ? c->gpr[r] & 0xFFu : (c->gpr[r - 4] >> 8) & 0xFFu;
}

static void reg_wr(cpu_t *c, int r, int size, uint32_t v)
{
    if (size == 4)      c->gpr[r] = v;
    else if (size == 2) c->gpr[r] = (c->gpr[r] & 0xFFFF0000u) | (v & 0xFFFFu);
    else if (r < 4)     c->gpr[r] = (c->gpr[r] & 0xFFFFFF00u) | (v & 0xFFu);
    else                c->gpr[r - 4] = (c->gpr[r - 4] & 0xFFFF00FFu) | ((v & 0xFFu) << 8);
}

static uint32_t ea_off(const cpu_t *c, const insn_t *in)
{
    const memref_t *m = &in->mem;
    uint32_t a = (uint32_t)m->disp;
    if (m->base >= 0)  a += c->gpr[m->base];
    if (m->index >= 0) a += c->gpr[m->index] << m->shift;
    return in->addrsize == 2 ? (a & 0xFFFFu) : a;
}

static uint32_t op_rd(hx_t *h, const operand_t *o)
{
    switch (o->kind) {
    case OPK_REG: return reg_rd(h->c, o->reg, o->size);
    case OPK_IMM: return h->in->imm & msk(o->size);
    case OPK_MEM: return cpu_read_lin(h->c, h->lin, o->size);
    case OPK_SEG: return h->c->seg[o->reg].selector;
    default:      return 0;
    }
}

static void op_wr(hx_t *h, const operand_t *o, uint32_t v)
{
    if (o->kind == OPK_REG)      reg_wr(h->c, o->reg, o->size, v);
    else if (o->kind == OPK_MEM) cpu_write_lin(h->c, h->lin, v & msk(o->size), o->size);
}

/* ------------------------------------------------------------------ */
/* Flags                                                               */
/* ------------------------------------------------------------------ */

static void lazy(cpu_t *c, cc_op_t fam8, int size, uint32_t dst, uint32_t s1, uint32_t s2)
{
    c->cc_op = (cc_op_t)(fam8 + (size == 1 ? 0 : size == 2 ? 1 : 2));
    c->cc_dst = dst; c->cc_src1 = s1; c->cc_src2 = s2;
}

// SF, ZF, PF for a result of the given size.
static uint32_t szp(uint32_t r, int size)
{
    r &= msk(size);
    uint32_t f = 0;
    if (r == 0) f |= EFL_ZF;
    if (r >> (bits_of(size) - 1)) f |= EFL_SF;
    uint32_t p = r & 0xFFu;
    p ^= p >> 4; p &= 0xFu;
    if (!((0x6996u >> p) & 1u)) f |= EFL_PF;
    return f;
}

// Replace the flags in `mask` with `bits`, keeping the others.
static void set_flags(cpu_t *c, uint32_t mask, uint32_t bits)
{
    uint32_t f = cpu_calc_flags(c);
    c->eflags = (f & ~mask) | (bits & mask);
    c->cc_op = CC_OP_EFLAGS;
}

static bool privileged(const cpu_t *c) { return !cpu_pmode(c) || c->cpl == 0; }
static void need_cpl0(cpu_t *c) { if (!privileged(c)) cpu_fault(c, EXC_GP, 0); }
static void need_pmode(cpu_t *c) { if (!cpu_pmode(c) || (c->eflags & EFL_VM)) cpu_fault(c, EXC_UD, 0); }

/* ------------------------------------------------------------------ */
/* Shifts and rotates                                                  */
/* ------------------------------------------------------------------ */

static void do_shift(hx_t *h)
{
    cpu_t *c = h->c;
    const insn_t *in = h->in;
    int size = in->dst.size;
    uint32_t bits = bits_of(size), M = msk(size), S = 1u << (bits - 1);
    uint32_t n = op_rd(h, &in->src) & 31u;
    if (n == 0) return;                                   // count 0: nothing changes, not even flags
    uint32_t v = op_rd(h, &in->dst) & M, r, cf, of;

    switch (in->op) {
    case OP_SHL: r = (v << n) & M; op_wr(h, &in->dst, r); lazy(c, CC_OP_SHL8, size, r, v, n); return;
    case OP_SHR: r = v >> n;       op_wr(h, &in->dst, r); lazy(c, CC_OP_SHR8, size, r, v, n); return;
    case OP_SAR: r = (uint32_t)((int32_t)sext(v, size) >> n) & M;
                 op_wr(h, &in->dst, r); lazy(c, CC_OP_SAR8, size, r, v, n); return;
    case OP_ROL: {
        uint32_t k = n % bits;
        r = k ? ((v << k) | (v >> (bits - k))) & M : v;
        cf = r & 1u;
        of = ((r & S) != 0) ^ cf;
        break;
    }
    case OP_ROR: {
        uint32_t k = n % bits;
        r = k ? ((v >> k) | (v << (bits - k))) & M : v;
        cf = (r & S) != 0;
        of = cf ^ ((r >> (bits - 2)) & 1u);
        break;
    }
    case OP_RCL: case OP_RCR: {
        uint32_t k = size == 4 ? n : n % (bits + 1);
        if (k == 0) return;
        uint64_t W = (1ull << (bits + 1)) - 1;            // bits+1 wide: CF:value
        uint64_t old_cf = cpu_get_cf(c);
        uint64_t x = (old_cf << bits) | v;
        if (in->op == OP_RCL) x = ((x << k) | (x >> (bits + 1 - k))) & W;
        else                  x = ((x >> k) | (x << (bits + 1 - k))) & W;
        r = (uint32_t)x & M;
        cf = (uint32_t)(x >> bits) & 1u;
        of = in->op == OP_RCL ? (((r & S) != 0) ^ cf) : (((v & S) != 0) ^ (uint32_t)old_cf);
        break;
    }
    default: return;
    }
    op_wr(h, &in->dst, r);
    set_flags(c, EFL_CF | EFL_OF, (cf ? EFL_CF : 0) | (of ? EFL_OF : 0));
}

static void do_shld(hx_t *h)
{
    cpu_t *c = h->c;
    const insn_t *in = h->in;
    int size = in->dst.size;
    uint32_t bits = bits_of(size), M = msk(size), S = 1u << (bits - 1);
    uint32_t n = (in->src2.kind == OPK_IMM ? in->imm : reg_rd(c, R_ECX, 1)) & 31u;
    if (n == 0) return;
    uint32_t d = op_rd(h, &in->dst) & M, s = op_rd(h, &in->src) & M, r, cf;
    if (size == 4) {
        if (in->op == OP_SHLD) { r = (d << n) | (s >> (32 - n)); cf = (d >> (32 - n)) & 1u; }
        else                   { r = (d >> n) | (s << (32 - n)); cf = (d >> (n - 1)) & 1u; }
    } else {   // 16-bit; counts above 16 are undefined on real hardware
        if (in->op == OP_SHLD) {
            uint64_t x = ((uint64_t)d << 16) | s;
            r = (uint32_t)((x << n) >> 16) & M;
            cf = (uint32_t)((x << n) >> 32) & 1u;
        } else {
            uint64_t x = ((uint64_t)s << 16) | d;
            r = (uint32_t)(x >> n) & M;
            cf = (uint32_t)(x >> (n - 1)) & 1u;
        }
    }
    op_wr(h, &in->dst, r);
    uint32_t of = ((r ^ d) & S) != 0;
    set_flags(c, EFL_ARITH, szp(r, size) | (cf ? EFL_CF : 0) | (of ? EFL_OF : 0));
}

/* ------------------------------------------------------------------ */
/* Multiply / divide                                                   */
/* ------------------------------------------------------------------ */

static void do_mul(hx_t *h)
{
    cpu_t *c = h->c;
    const insn_t *in = h->in;
    bool sgn = in->op == OP_IMUL;

    if (sgn && in->src.kind != OPK_NONE) {               // imul r, r/m [, imm]: truncated result
        int size = in->dst.size;
        uint32_t a, b;
        if (in->src2.kind == OPK_IMM) { a = op_rd(h, &in->src); b = in->imm; }
        else                          { a = reg_rd(c, in->dst.reg, size); b = op_rd(h, &in->src); }
        int64_t p = (int64_t)(int32_t)sext(a & msk(size), size) * (int64_t)(int32_t)sext(b & msk(size), size);
        uint32_t r = (uint32_t)p & msk(size);
        bool ovf = (int64_t)(int32_t)sext(r, size) != p;
        reg_wr(c, in->dst.reg, size, r);
        set_flags(c, EFL_ARITH, szp(r, size) | (ovf ? EFL_CF | EFL_OF : 0));
        return;
    }
    int size = in->dst.size;
    uint32_t v = op_rd(h, &in->dst), lo;
    bool ovf;
    if (size == 1) {
        uint32_t al = reg_rd(c, R_EAX, 1);
        uint32_t p = sgn ? (uint32_t)((int32_t)(int8_t)al * (int32_t)(int8_t)v) : al * v;
        reg_wr(c, R_EAX, 2, p);
        lo = p & 0xFFu;
        ovf = sgn ? (sext(lo, 1) & 0xFFFFu) != (p & 0xFFFFu) : (p & 0xFF00u) != 0;
    } else if (size == 2) {
        uint32_t ax = reg_rd(c, R_EAX, 2);
        uint32_t p = sgn ? (uint32_t)((int32_t)(int16_t)ax * (int32_t)(int16_t)v) : ax * v;
        reg_wr(c, R_EAX, 2, p);
        reg_wr(c, R_EDX, 2, p >> 16);
        lo = p & 0xFFFFu;
        ovf = sgn ? sext(lo, 2) != p : (p >> 16) != 0;
    } else {
        uint64_t p = sgn ? (uint64_t)((int64_t)(int32_t)c->gpr[R_EAX] * (int64_t)(int32_t)v)
                         : (uint64_t)c->gpr[R_EAX] * v;
        c->gpr[R_EAX] = (uint32_t)p;
        c->gpr[R_EDX] = (uint32_t)(p >> 32);
        lo = (uint32_t)p;
        ovf = sgn ? (int64_t)(int32_t)lo != (int64_t)p : (p >> 32) != 0;
    }
    // SF/ZF/PF/AF are undefined; computed from the low half like later Intel cores.
    set_flags(c, EFL_ARITH, szp(lo, size) | (ovf ? EFL_CF | EFL_OF : 0));
}

static void do_div(hx_t *h)
{
    cpu_t *c = h->c;
    const insn_t *in = h->in;
    int size = in->dst.size;
    uint32_t v = op_rd(h, &in->dst) & msk(size);
    if (v == 0) cpu_fault(c, EXC_DE, 0);
    bool sgn = in->op == OP_IDIV;
    uint32_t bits = bits_of(size);

    uint64_t num;
    if (size == 1)      num = reg_rd(c, R_EAX, 2);
    else if (size == 2) num = ((uint64_t)reg_rd(c, R_EDX, 2) << 16) | reg_rd(c, R_EAX, 2);
    else                num = ((uint64_t)c->gpr[R_EDX] << 32) | c->gpr[R_EAX];

    uint32_t q, r;
    if (!sgn) {
        uint64_t qq = num / v;
        if (qq >> bits) cpu_fault(c, EXC_DE, 0);
        q = (uint32_t)qq; r = (uint32_t)(num % v);
    } else {
        int64_t n;
        if (size == 1)      n = (int16_t)num;
        else if (size == 2) n = (int32_t)num;
        else                n = (int64_t)num;
        int64_t d = (int32_t)sext(v, size);
        if (n == INT64_MIN && d == -1) cpu_fault(c, EXC_DE, 0);
        int64_t qq = n / d, rr = n % d;
        int64_t lim = (int64_t)1 << (bits - 1);
        if (qq >= lim || qq < -lim) cpu_fault(c, EXC_DE, 0);
        q = (uint32_t)qq; r = (uint32_t)rr;
    }
    if (size == 1) { reg_wr(c, R_EAX, 1, q); reg_wr(c, 4 /*AH*/, 1, r); }
    else           { reg_wr(c, R_EAX, size, q); reg_wr(c, R_EDX, size, r); }
    // Flags are undefined after DIV/IDIV; left unchanged.
}

/* ------------------------------------------------------------------ */
/* Bit operations, exchanges                                           */
/* ------------------------------------------------------------------ */

static void do_bt(hx_t *h)
{
    cpu_t *c = h->c;
    const insn_t *in = h->in;
    int size = in->dst.size;
    uint32_t bits = bits_of(size), bit;
    if (in->src.kind == OPK_IMM) {
        bit = in->imm & (bits - 1);
    } else {
        uint32_t off = reg_rd(c, in->src.reg, size);
        bit = off & (bits - 1);
        if (in->dst.kind == OPK_MEM) {
            // A register bit offset can reach outside the operand: signed, in operand-size units.
            int32_t so = (int32_t)sext(off, size);
            h->lin += (uint32_t)((so >> (size == 2 ? 4 : 5)) * size);
        }
    }
    uint32_t v = op_rd(h, &in->dst), m = 1u << bit;
    bool cf = (v & m) != 0;
    switch (in->op) {
    case OP_BTS: op_wr(h, &in->dst, v | m); break;
    case OP_BTR: op_wr(h, &in->dst, v & ~m); break;
    case OP_BTC: op_wr(h, &in->dst, v ^ m); break;
    default: break;
    }
    set_flags(c, EFL_CF, cf ? EFL_CF : 0);
}

static void do_bsf(hx_t *h)
{
    const insn_t *in = h->in;
    int size = in->dst.size;
    uint32_t v = op_rd(h, &in->src) & msk(size);
    if (v == 0) { set_flags(h->c, EFL_ZF, EFL_ZF); return; }   // destination left unchanged
    uint32_t idx = in->op == OP_BSF ? (uint32_t)__builtin_ctz(v) : 31u - (uint32_t)__builtin_clz(v);
    reg_wr(h->c, in->dst.reg, size, idx);
    set_flags(h->c, EFL_ZF, 0);
}

static void do_cmpxchg(hx_t *h)
{
    cpu_t *c = h->c;
    const insn_t *in = h->in;
    int size = in->dst.size;
    uint32_t acc = reg_rd(c, R_EAX, size), d = op_rd(h, &in->dst) & msk(size);
    lazy(c, CC_OP_SUB8, size, d - acc, d, acc);   // flags of CMP dest, accumulator
    if (acc == d) {
        op_wr(h, &in->dst, op_rd(h, &in->src));
    } else {
        if (in->dst.kind == OPK_MEM) op_wr(h, &in->dst, d);   // the bus always sees a write
        reg_wr(c, R_EAX, size, d);
    }
}

static void do_cmpxchg8b(hx_t *h)
{
    cpu_t *c = h->c;
    cpu_probe_write(c, h->lin, 8);                         // locked RMW: write access is checked first
    uint32_t lo = cpu_read_lin(c, h->lin, 4), hi = cpu_read_lin(c, h->lin + 4, 4);
    if (lo == c->gpr[R_EAX] && hi == c->gpr[R_EDX]) {
        cpu_write_lin(c, h->lin, c->gpr[R_EBX], 4);
        cpu_write_lin(c, h->lin + 4, c->gpr[R_ECX], 4);
        set_flags(c, EFL_ZF, EFL_ZF);
    } else {
        cpu_write_lin(c, h->lin, lo, 4);
        cpu_write_lin(c, h->lin + 4, hi, 4);
        c->gpr[R_EAX] = lo; c->gpr[R_EDX] = hi;
        set_flags(c, EFL_ZF, 0);
    }
}

static void do_xadd(hx_t *h)
{
    const insn_t *in = h->in;
    int size = in->dst.size;
    uint32_t d = op_rd(h, &in->dst), s = op_rd(h, &in->src), r = (d + s) & msk(size);
    op_wr(h, &in->src, d);
    op_wr(h, &in->dst, r);
    lazy(h->c, CC_OP_ADD8, size, r, d, s);
}

/* ------------------------------------------------------------------ */
/* String instructions                                                 */
/* ------------------------------------------------------------------ */

#define STR_CHUNK 65536u   // REP iterations per call before yielding to the dispatcher

static void advance(cpu_t *c, int r, uint32_t am, int32_t step)
{
    c->gpr[r] = (c->gpr[r] & ~am) | ((c->gpr[r] + (uint32_t)step) & am);
}

static uint32_t do_string(hx_t *h, const jit_hrec_t *rec)
{
    cpu_t *c = h->c;
    const insn_t *in = h->in;
    int size = in->opsize;
    uint32_t am = in->addrsize == 2 ? 0xFFFFu : 0xFFFFFFFFu;
    int32_t step = (c->eflags & EFL_DF) ? -size : size;
    int sseg = in->seg_override >= 0 ? in->seg_override : S_DS;
    bool rep = in->rep != 0;
    uint16_t port = (uint16_t)c->gpr[R_EDX];

    if ((in->op == OP_INS || in->op == OP_OUTS) && !cpu_io_allowed(c, port, size))
        cpu_fault(c, EXC_GP, 0);

    for (uint32_t n = STR_CHUNK;; n--) {
        uint32_t cnt = c->gpr[R_ECX] & am;
        if (rep && cnt == 0) return 0;
        uint32_t src = c->seg[sseg].base + (c->gpr[R_ESI] & am);
        uint32_t dst = c->seg[S_ES].base + (c->gpr[R_EDI] & am);
        bool stop = false;

        switch (in->op) {
        case OP_MOVS:
            cpu_write_lin(c, dst, cpu_read_lin(c, src, size), size);
            advance(c, R_ESI, am, step); advance(c, R_EDI, am, step);
            break;
        case OP_STOS:
            cpu_write_lin(c, dst, reg_rd(c, R_EAX, size), size);
            advance(c, R_EDI, am, step);
            break;
        case OP_LODS:
            reg_wr(c, R_EAX, size, cpu_read_lin(c, src, size));
            advance(c, R_ESI, am, step);
            break;
        case OP_CMPS: case OP_SCAS: {
            uint32_t a = in->op == OP_CMPS ? cpu_read_lin(c, src, size) : reg_rd(c, R_EAX, size);
            uint32_t b = cpu_read_lin(c, dst, size);
            lazy(c, CC_OP_SUB8, size, a - b, a, b);
            if (in->op == OP_CMPS) advance(c, R_ESI, am, step);
            advance(c, R_EDI, am, step);
            stop = in->rep == 1 ? (a != b) : (a == b);      // REPE stops on mismatch, REPNE on match
            break;
        }
        case OP_INS:
            cpu_write_lin(c, dst, io_read(c->io, port, size), size);
            advance(c, R_EDI, am, step);
            break;
        case OP_OUTS:
            io_write(c->io, port, cpu_read_lin(c, src, size), size);
            advance(c, R_ESI, am, step);
            break;
        default:
            return 0;
        }
        if (!rep) return 0;
        cnt = (cnt - 1) & am;
        c->gpr[R_ECX] = (c->gpr[R_ECX] & ~am) | cnt;
        if (stop || cnt == 0) return 0;
        if (n == 1) { c->eip = rec->eip; return JIT_EXIT_OK; }  // resume this REP in a later block
    }
}

/* ------------------------------------------------------------------ */
/* Stack frames and flag transfers                                     */
/* ------------------------------------------------------------------ */

static void do_enter(cpu_t *c, const insn_t *in)
{
    uint32_t size = in->imm & 0xFFFFu;
    int level = (int)(in->imm2 & 31u), osz = in->opsize;
    bool big = cpu_stack32(c);
    if (big) cpu_probe_write(c, c->seg[S_SS].base + c->gpr[R_ESP] - (uint32_t)(osz * (level + 1)) - size,
                             (uint32_t)(osz * (level + 1)) + size);   // whole frame first: no partial ENTER
    cpu_push(c, reg_rd(c, R_EBP, osz), osz);
    uint32_t frame = big ? c->gpr[R_ESP] : (c->gpr[R_ESP] & 0xFFFFu);
    if (level > 0) {
        uint32_t bp = c->gpr[R_EBP];
        for (int i = 1; i < level; i++) {
            bp = big ? bp - (uint32_t)osz : ((bp & 0xFFFF0000u) | ((bp - (uint32_t)osz) & 0xFFFFu));
            uint32_t v = cpu_read_lin(c, c->seg[S_SS].base + (big ? bp : (bp & 0xFFFFu)), osz);
            cpu_push(c, v, osz);
        }
        cpu_push(c, frame, osz);
    }
    reg_wr(c, R_EBP, osz, frame);
    if (big) c->gpr[R_ESP] -= size;
    else     reg_wr(c, R_ESP, 2, c->gpr[R_ESP] - size);
}

static void do_pusha(cpu_t *c, int osz)
{
    if (cpu_stack32(c)) cpu_probe_write(c, c->seg[S_SS].base + c->gpr[R_ESP] - 8u * (uint32_t)osz, 8u * (uint32_t)osz);
    uint32_t sp = reg_rd(c, R_ESP, osz);
    for (int r = R_EAX; r <= R_EDI; r++)
        cpu_push(c, r == R_ESP ? sp : reg_rd(c, r, osz), osz);
}

static void do_popa(cpu_t *c, int osz)
{
    for (int r = R_EDI; r >= R_EAX; r--) {
        uint32_t v = cpu_pop(c, osz);
        if (r != R_ESP) reg_wr(c, r, osz, v);
    }
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

bool helper_supports(const insn_t *in, bool *ends)
{
    *ends = false;
    switch (in->op) {
    case OP_ROL: case OP_ROR: case OP_RCL: case OP_RCR: case OP_SHL: case OP_SHR: case OP_SAR:
    case OP_SHLD: case OP_SHRD: case OP_MUL: case OP_IMUL: case OP_DIV: case OP_IDIV:
    case OP_BT: case OP_BTS: case OP_BTR: case OP_BTC: case OP_BSF: case OP_BSR: case OP_BSWAP:
    case OP_CMPXCHG: case OP_CMPXCHG8B: case OP_XADD:
    case OP_CBW: case OP_CWD: case OP_LAHF: case OP_SAHF: case OP_PUSHF:
    case OP_PUSHA: case OP_POPA: case OP_ENTER: case OP_XLAT:
    case OP_MOVS: case OP_CMPS: case OP_STOS: case OP_LODS: case OP_SCAS: case OP_INS:
    case OP_IN: case OP_CPUID: case OP_RDTSC: case OP_RDPMC: case OP_RDMSR: case OP_WRMSR:
    case OP_LGDT: case OP_LIDT: case OP_SGDT: case OP_SIDT: case OP_LLDT: case OP_SLDT:
    case OP_LTR: case OP_STR: case OP_SMSW: case OP_CLTS: case OP_INVD: case OP_WBINVD: case OP_INVLPG:
    case OP_LES: case OP_LDS: case OP_LFS: case OP_LGS:
    case OP_FPU: case OP_FWAIT:
        return true;
    case OP_OUT: case OP_OUTS:          // device writes can raise IRQs or request a reset
    case OP_HLT: case OP_POPF: case OP_LMSW: case OP_LSS:
    case OP_JMPF: case OP_CALLF: case OP_RETF: case OP_IRET: case OP_INT: case OP_INT3: case OP_INTO:
        *ends = true;
        return true;
    case OP_MOV:
        if (in->dst.kind == OPK_CR) { *ends = true; return true; }
        if (in->src.kind == OPK_CR || in->dst.kind == OPK_DR || in->src.kind == OPK_DR) return true;
        if (in->dst.kind == OPK_SEG) { *ends = in->dst.reg == S_SS; return true; }
        return false;
    case OP_POP:
        if (in->dst.kind == OPK_SEG) { *ends = in->dst.reg == S_SS; return true; }
        return false;
    default:
        return false;
    }
}

// Far pointer from memory: offset (opsize bytes) then a 16-bit selector.
static void far_ptr(hx_t *h, uint16_t *sel, uint32_t *off)
{
    int osz = h->in->opsize;
    *off = cpu_read_lin(h->c, h->lin, osz);
    *sel = (uint16_t)cpu_read_lin(h->c, h->lin + (uint32_t)osz, 2);
}

uint32_t helper_exec(cpu_t *c, const jit_hrec_t *rec)
{
    const insn_t *in = &rec->in;
    hx_t h = { c, in, 0 };
    c->eip = rec->eip;                    // faults report the instruction's own address
    if (in->dst.kind == OPK_MEM || in->src.kind == OPK_MEM)
        h.lin = c->seg[in->mem.seg].base + ea_off(c, in);
    int osz = in->opsize;

    switch (in->op) {
    case OP_ROL: case OP_ROR: case OP_RCL: case OP_RCR:
    case OP_SHL: case OP_SHR: case OP_SAR:   do_shift(&h); return 0;
    case OP_SHLD: case OP_SHRD:               do_shld(&h); return 0;
    case OP_MUL: case OP_IMUL:                do_mul(&h); return 0;
    case OP_DIV: case OP_IDIV:                do_div(&h); return 0;
    case OP_BT: case OP_BTS: case OP_BTR: case OP_BTC: do_bt(&h); return 0;
    case OP_BSF: case OP_BSR:                 do_bsf(&h); return 0;
    case OP_BSWAP: c->gpr[in->dst.reg] = __builtin_bswap32(c->gpr[in->dst.reg]); return 0;
    case OP_CMPXCHG:                          do_cmpxchg(&h); return 0;
    case OP_CMPXCHG8B:                        do_cmpxchg8b(&h); return 0;
    case OP_XADD:                             do_xadd(&h); return 0;

    case OP_CBW:
        if (osz == 2) reg_wr(c, R_EAX, 2, sext(reg_rd(c, R_EAX, 1), 1));
        else          c->gpr[R_EAX] = sext(reg_rd(c, R_EAX, 2), 2);
        return 0;
    case OP_CWD:
        if (osz == 2) reg_wr(c, R_EDX, 2, (c->gpr[R_EAX] & 0x8000u) ? 0xFFFFu : 0);
        else          c->gpr[R_EDX] = (c->gpr[R_EAX] & 0x80000000u) ? 0xFFFFFFFFu : 0;
        return 0;
    case OP_LAHF:
        reg_wr(c, 4 /*AH*/, 1, (cpu_calc_flags(c) & 0xD5u) | 2u);
        return 0;
    case OP_SAHF:
        set_flags(c, EFL_SF | EFL_ZF | EFL_AF | EFL_PF | EFL_CF, reg_rd(c, 4 /*AH*/, 1));
        return 0;
    case OP_PUSHF: {
        uint32_t f = cpu_calc_flags(c);
        cpu_push(c, osz == 4 ? (f & 0x00FCFFFFu) : (f & 0xFFFFu), osz);   // VM and RF read as 0
        return 0;
    }
    case OP_POPF:
        cpu_set_eflags(c, cpu_pop(c, osz), osz == 4 ? 0xFFFFFFFFu : 0xFFFFu);
        c->eip = rec->next_eip;
        return JIT_EXIT_OK;
    case OP_HLT:
        need_cpl0(c);
        c->halted = true;
        c->eip = rec->next_eip;
        return JIT_EXIT_HALT;
    case OP_PUSHA: do_pusha(c, osz); return 0;
    case OP_POPA:  do_popa(c, osz);  return 0;
    case OP_ENTER: do_enter(c, in);  return 0;
    case OP_XLAT: {
        int seg = in->seg_override >= 0 ? in->seg_override : S_DS;
        uint32_t am = in->addrsize == 2 ? 0xFFFFu : 0xFFFFFFFFu;
        uint32_t off = (c->gpr[R_EBX] + reg_rd(c, R_EAX, 1)) & am;
        reg_wr(c, R_EAX, 1, cpu_read_lin(c, c->seg[seg].base + off, 1));
        return 0;
    }

    case OP_MOVS: case OP_CMPS: case OP_STOS: case OP_LODS: case OP_SCAS: case OP_INS:
        return do_string(&h, rec);
    case OP_OUTS: {
        uint32_t r = do_string(&h, rec);
        if (r) return r;                 // unfinished REP: resumes at this instruction
        c->eip = rec->next_eip;
        return JIT_EXIT_OK;
    }

    case OP_IN: case OP_OUT: {
        const operand_t *p = in->op == OP_IN ? &in->src : &in->dst;
        uint16_t port = (uint16_t)(p->kind == OPK_IMM ? in->imm : c->gpr[R_EDX]);
        if (!cpu_io_allowed(c, port, osz)) cpu_fault(c, EXC_GP, 0);
        if (in->op == OP_IN) { reg_wr(c, R_EAX, osz, io_read(c->io, port, osz)); return 0; }
        io_write(c->io, port, reg_rd(c, R_EAX, osz), osz);
        c->eip = rec->next_eip;
        return JIT_EXIT_OK;
    }

    case OP_CPUID: cpu_cpuid(c); return 0;
    case OP_FPU:   return fpu_exec(c, in, h.lin);
    case OP_FWAIT: fpu_wait(c); return 0;
    case OP_RDTSC: {
        if ((c->cr4 & 4u) && !privileged(c)) cpu_fault(c, EXC_GP, 0);   // CR4.TSD
        uint64_t t = cpu_read_tsc(c);
        c->gpr[R_EAX] = (uint32_t)t; c->gpr[R_EDX] = (uint32_t)(t >> 32);
        return 0;
    }
    case OP_RDMSR: need_cpl0(c); cpu_rdmsr(c); return 0;
    case OP_RDPMC: {
        if (!(c->cr4 & (1u << 8)) && !privileged(c)) cpu_fault(c, EXC_GP, 0);   // CR4.PCE
        if (c->gpr[R_ECX] > 1) cpu_fault(c, EXC_GP, 0);
        uint64_t v = cpu_rdpmc(c, c->gpr[R_ECX]);
        c->gpr[R_EAX] = (uint32_t)v; c->gpr[R_EDX] = (uint32_t)(v >> 32) & 0xFF;
        return 0;
    }
    case OP_WRMSR: need_cpl0(c); cpu_wrmsr(c); return 0;

    case OP_LGDT: case OP_LIDT: {
        need_cpl0(c);
        uint32_t limit = cpu_read_lin(c, h.lin, 2), base = cpu_read_lin(c, h.lin + 2, 4);
        if (osz == 2) base &= 0x00FFFFFFu;
        dtr_t *t = in->op == OP_LGDT ? &c->gdtr : &c->idtr;
        t->base = base; t->limit = (uint16_t)limit;
        return 0;
    }
    case OP_SGDT: case OP_SIDT: {
        const dtr_t *t = in->op == OP_SGDT ? &c->gdtr : &c->idtr;
        cpu_write_lin(c, h.lin, t->limit, 2);
        cpu_write_lin(c, h.lin + 2, osz == 2 ? (t->base & 0x00FFFFFFu) : t->base, 4);
        return 0;
    }
    case OP_LLDT: need_pmode(c); need_cpl0(c); cpu_lldt(c, (uint16_t)op_rd(&h, &in->dst)); return 0;
    case OP_LTR:  need_pmode(c); need_cpl0(c); cpu_ltr(c, (uint16_t)op_rd(&h, &in->dst)); return 0;
    case OP_SLDT: need_pmode(c); op_wr(&h, &in->dst, c->ldtr.selector); return 0;
    case OP_STR:  need_pmode(c); op_wr(&h, &in->dst, c->tr.selector); return 0;
    case OP_SMSW: op_wr(&h, &in->dst, c->cr0); return 0;
    case OP_LMSW: {
        need_cpl0(c);
        uint32_t v = op_rd(&h, &in->dst) & 0xFu;
        uint32_t cr0 = (c->cr0 & ~0xEu) | v | (c->cr0 & CR0_PE);    // LMSW can set PE, never clear it
        if (!cpu_write_cr(c, 0, cr0)) return JIT_EXIT_UNSUPPORTED;
        c->eip = rec->next_eip;
        return JIT_EXIT_OK;
    }
    case OP_CLTS: need_cpl0(c); c->cr0 &= ~CR0_TS; return 0;
    case OP_INVD: case OP_WBINVD: need_cpl0(c); return 0;     // no caches
    case OP_INVLPG: need_cpl0(c); cpu_tlb_invlpg(c, h.lin); return 0;

    case OP_MOV:
        if (in->dst.kind == OPK_SEG) {
            if (in->dst.reg == S_CS || in->dst.reg > S_GS) cpu_fault(c, EXC_UD, 0);
            cpu_load_seg(c, in->dst.reg, (uint16_t)op_rd(&h, &in->src));
            if (in->dst.reg == S_SS) { c->intr_shadow = true; c->eip = rec->next_eip; return JIT_EXIT_OK; }
            return 0;
        }
        if (in->dst.kind == OPK_CR || in->src.kind == OPK_CR) {
            int n = in->dst.kind == OPK_CR ? in->dst.reg : in->src.reg;
            if (n == 1 || n > 4) cpu_fault(c, EXC_UD, 0);
            need_cpl0(c);
            if (in->src.kind == OPK_CR) { c->gpr[in->dst.reg] = cpu_read_cr(c, n); return 0; }
            if (!cpu_write_cr(c, n, c->gpr[in->src.reg])) return JIT_EXIT_UNSUPPORTED;
            c->eip = rec->next_eip;
            return JIT_EXIT_OK;
        }
        if (in->dst.kind == OPK_DR || in->src.kind == OPK_DR) {
            int n = in->dst.kind == OPK_DR ? in->dst.reg : in->src.reg;
            if ((n == 4 || n == 5) && !(c->cr4 & 8u)) n += 2;           // DR4/5 alias DR6/7 unless CR4.DE
            else if (n == 4 || n == 5) cpu_fault(c, EXC_UD, 0);
            need_cpl0(c);
            if (in->src.kind == OPK_DR) c->gpr[in->dst.reg] = c->dr[n];
            else                        c->dr[n] = c->gpr[in->src.reg];  // no hardware breakpoints yet
            return 0;
        }
        return JIT_EXIT_UNSUPPORTED;

    case OP_POP: {        // pop sreg: ESP only moves once the load succeeded
        uint32_t esp0 = c->gpr[R_ESP];
        uint32_t v = cpu_pop(c, osz), esp1 = c->gpr[R_ESP];
        c->gpr[R_ESP] = esp0;
        cpu_load_seg(c, in->dst.reg, (uint16_t)v);
        c->gpr[R_ESP] = esp1;
        if (in->dst.reg == S_SS) { c->intr_shadow = true; c->eip = rec->next_eip; return JIT_EXIT_OK; }
        return 0;
    }
    case OP_LES: case OP_LDS: case OP_LSS: case OP_LFS: case OP_LGS: {
        static const int sreg[] = { [OP_LES] = S_ES, [OP_LDS] = S_DS, [OP_LSS] = S_SS,
                                    [OP_LFS] = S_FS, [OP_LGS] = S_GS };
        uint16_t sel; uint32_t off;
        far_ptr(&h, &sel, &off);
        cpu_load_seg(c, sreg[in->op], sel);
        reg_wr(c, in->dst.reg, osz, off);
        if (in->op == OP_LSS) { c->eip = rec->next_eip; return JIT_EXIT_OK; }
        return 0;
    }

    case OP_JMPF: case OP_CALLF: {
        uint16_t sel; uint32_t off;
        if (in->dst.kind == OPK_FARPTR) { sel = (uint16_t)in->imm2; off = in->imm; }
        else far_ptr(&h, &sel, &off);
        if (in->op == OP_JMPF) cpu_far_jmp(c, sel, off, osz);
        else                   cpu_far_call(c, sel, off, osz, rec->next_eip);
        return JIT_EXIT_OK;
    }
    case OP_RETF: cpu_far_ret(c, osz, (uint16_t)(in->src.kind == OPK_IMM ? in->imm : 0)); return JIT_EXIT_OK;
    case OP_IRET: cpu_iret(c, osz); return JIT_EXIT_OK;
    case OP_INT: {
        static int trace = -1;
        if (trace < 0) { const char *e = getenv("BEEMU_TRACE_INT"); trace = e && *e == '1'; }
        if (trace)                                   // debugging aid: log software interrupts
            fprintf(stderr, "INT %02X at %04X:%08X eax=%08X ebx=%08X ecx=%08X edx=%08X\n", in->imm & 0xFFu,
                    c->seg[S_CS].selector, c->eip, c->gpr[R_EAX], c->gpr[R_EBX], c->gpr[R_ECX], c->gpr[R_EDX]);
        cpu_interrupt(c, (int)(in->imm & 0xFFu), rec->next_eip, true, false, 0);
        return JIT_EXIT_OK;
    }
    case OP_INT3: cpu_interrupt(c, EXC_BP, rec->next_eip, true, false, 0); return JIT_EXIT_OK;
    case OP_INTO:
        if (cpu_calc_flags(c) & EFL_OF) cpu_interrupt(c, EXC_OF, rec->next_eip, true, false, 0);
        else c->eip = rec->next_eip;
        return JIT_EXIT_OK;

    default:
        return JIT_EXIT_UNSUPPORTED;
    }
}
