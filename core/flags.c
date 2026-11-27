#include "flags.h"

enum { K_ADD, K_ADC, K_SUB, K_SBB, K_LOGIC, K_INC, K_DEC, K_SHL, K_SHR, K_SAR, K_MUL };

static uint32_t parity_flag(uint32_t v)
{
    v &= 0xFF;
    v ^= v >> 4;
    v &= 0xF;
    return ((0x6996u >> v) & 1u) ? 0 : EFL_PF;   // PF set when the low byte has even parity
}

uint32_t cpu_calc_flags(const cpu_t *c)
{
    unsigned op = (unsigned)c->cc_op;
    if (op == CC_OP_EFLAGS || op >= CC_OP__COUNT) return c->eflags;

    unsigned idx  = op - 1;
    unsigned kind = idx / 3;
    unsigned bits = 8u << (idx % 3);
    uint32_t M = bits == 32 ? 0xFFFFFFFFu : (1u << bits) - 1u;
    uint32_t S = 1u << (bits - 1);
    uint32_t r = c->cc_dst & M, a = c->cc_src1 & M, b = c->cc_src2 & M;
    uint32_t cf = 0, of = 0, af = 0;

    switch (kind) {
    case K_ADD:
        cf = r < a;
        af = (a ^ b ^ r) & 0x10;
        of = ((a ^ r) & (b ^ r) & S) != 0;
        break;
    case K_ADC: {
        uint32_t cin = (r - a - b) & M;       // r = a + b + cin
        cf = cin ? (r <= a) : (r < a);
        af = (a ^ b ^ r) & 0x10;
        of = ((a ^ r) & (b ^ r) & S) != 0;
        break;
    }
    case K_SUB:
        cf = a < b;
        af = (a ^ b ^ r) & 0x10;
        of = ((a ^ b) & (a ^ r) & S) != 0;
        break;
    case K_SBB: {
        uint32_t cin = (a - b - r) & M;       // r = a - b - cin
        cf = cin ? (a <= b) : (a < b);
        af = (a ^ b ^ r) & 0x10;
        of = ((a ^ b) & (a ^ r) & S) != 0;
        break;
    }
    case K_LOGIC:
        break;
    case K_INC:
        cf = c->cc_src1 & 1;                  // CF is preserved: src1 holds the old CF
        of = (r == S);
        af = ((r & 0xF) == 0);
        break;
    case K_DEC:
        cf = c->cc_src1 & 1;
        of = (r == S - 1);
        af = ((r & 0xF) == 0xF);
        break;
    case K_SHL: {
        uint32_t n = c->cc_src2;              // 1..31
        cf = (n <= bits) ? ((a >> (bits - n)) & 1u) : 0;
        of = ((r & S) != 0) ^ cf;             // architecturally defined for n == 1
        break;
    }
    case K_SHR: {
        uint32_t n = c->cc_src2;
        cf = (n <= bits) ? ((a >> (n - 1)) & 1u) : 0;
        of = (a & S) != 0;                    // defined for n == 1
        break;
    }
    case K_SAR: {
        uint32_t n = c->cc_src2;
        int32_t sa = (int32_t)(a << (32 - bits)) >> (32 - bits);   // sign-extend to 32 bits
        cf = (n >= bits) ? (uint32_t)((a & S) != 0) : (uint32_t)(((uint32_t)sa >> (n - 1)) & 1u);
        of = 0;
        break;
    }
    case K_MUL:
        cf = of = c->cc_src1 & 1;
        break;
    default:
        break;
    }

    uint32_t f = parity_flag(r);
    if (r == 0)  f |= EFL_ZF;
    if (r & S)   f |= EFL_SF;
    if (cf)      f |= EFL_CF;
    if (af)      f |= EFL_AF;
    if (of)      f |= EFL_OF;
    return (c->eflags & ~EFL_ARITH) | f;
}

bool cpu_eval_cond(const cpu_t *c, int cond)
{
    uint32_t f = cpu_calc_flags(c);
    bool cf = f & EFL_CF, zf = f & EFL_ZF, sf = f & EFL_SF, of = f & EFL_OF, pf = f & EFL_PF;
    bool r;
    switch ((cond & 15) >> 1) {
    case 0: r = of; break;                    // O
    case 1: r = cf; break;                    // B
    case 2: r = zf; break;                    // E
    case 3: r = cf || zf; break;              // BE
    case 4: r = sf; break;                    // S
    case 5: r = pf; break;                    // P
    case 6: r = sf != of; break;              // L
    default: r = zf || (sf != of); break;     // LE
    }
    return (cond & 1) ? !r : r;
}

void cpu_flush_flags(cpu_t *c)
{
    c->eflags = cpu_calc_flags(c);
    c->cc_op = CC_OP_EFLAGS;
}

uint32_t cpu_get_cf(const cpu_t *c)
{
    return cpu_calc_flags(c) & EFL_CF ? 1u : 0u;
}
