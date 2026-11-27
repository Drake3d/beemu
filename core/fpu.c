#include "fpu.h"
#include "flags.h"
#include "fpu_math.h"
#include "jit.h"
#include "softfloat.h"
#include "system.h"
#include <stdlib.h>
#include <string.h>

typedef extFloat80_t fx;

// Status word
#define SW_IE 0x0001
#define SW_DE 0x0002
#define SW_ZE 0x0004
#define SW_OE 0x0008
#define SW_UE 0x0010
#define SW_PE 0x0020
#define SW_SF 0x0040
#define SW_ES 0x0080
#define SW_C0 0x0100
#define SW_C1 0x0200
#define SW_C2 0x0400
#define SW_C3 0x4000
#define SW_B  0x8000
#define SW_CC (SW_C0 | SW_C1 | SW_C2 | SW_C3)

enum { TAG_VALID = 0, TAG_ZERO = 1, TAG_SPECIAL = 2, TAG_EMPTY = 3 };

/* ------------------------------------------------------------------ */
/* Values                                                              */
/* ------------------------------------------------------------------ */

static fx mk(uint16_t se, uint64_t sig) { fx v; v.signExp = se; v.signif = sig; return v; }
static const fx INDEFINITE = { .signExp = 0xFFFF, .signif = 0xC000000000000000ull };   // the "real indefinite" QNaN

// Evaluate both checks (each may record a stack fault or exception), then combine.
static bool both(bool a, bool b) { return a && b; }

static int  fexp(fx v)      { return v.signExp & 0x7FFF; }
static bool fsign(fx v)     { return v.signExp >> 15; }
static bool is_nan(fx v)    { return fexp(v) == 0x7FFF && (v.signif << 1); }
static bool is_snan(fx v)   { return is_nan(v) && !(v.signif & 0x4000000000000000ull); }
static bool is_inf(fx v)    { return fexp(v) == 0x7FFF && !(v.signif << 1) && (v.signif >> 63); }
static bool is_zero(fx v)   { return fexp(v) == 0 && v.signif == 0; }
static bool is_denorm(fx v) { return fexp(v) == 0 && v.signif != 0; }     // includes pseudo-denormals
// Encodings the 387 and later reject: unnormals, pseudo-NaN, pseudo-infinity.
static bool is_unsupported(fx v) { return fexp(v) != 0 && !(v.signif >> 63); }
static fx   fneg(fx v)      { v.signExp ^= 0x8000; return v; }
static fx   fabs_(fx v)     { v.signExp &= 0x7FFF; return v; }

static int tag_of(fx v)
{
    if (fexp(v) == 0x7FFF) return TAG_SPECIAL;
    if (fexp(v) == 0) return v.signif ? TAG_SPECIAL : TAG_ZERO;
    return (v.signif >> 63) ? TAG_VALID : TAG_SPECIAL;
}

/* ------------------------------------------------------------------ */
/* Register stack                                                      */
/* ------------------------------------------------------------------ */

static int  top(const cpu_t *c)            { return (c->fpu_sw >> 11) & 7; }
static void set_top(cpu_t *c, int t)       { c->fpu_sw = (uint16_t)((c->fpu_sw & ~0x3800) | ((t & 7) << 11)); }
static int  phys(const cpu_t *c, int i)    { return (top(c) + i) & 7; }
static int  ptag(const cpu_t *c, int p)    { return (c->fpu_tw >> (2 * p)) & 3; }
static void set_ptag(cpu_t *c, int p, int t) { c->fpu_tw = (uint16_t)((c->fpu_tw & ~(3u << (2 * p))) | ((unsigned)t << (2 * p))); }
static bool empty(const cpu_t *c, int i)   { return ptag(c, phys(c, i)) == TAG_EMPTY; }

static fx preg(const cpu_t *c, int p)
{
    fx v;
    memcpy(&v.signif, c->st[p], 8);
    v.signExp = (uint16_t)(c->st[p][8] | c->st[p][9] << 8);
    return v;
}

static void set_preg(cpu_t *c, int p, fx v)
{
    memcpy(c->st[p], &v.signif, 8);
    c->st[p][8] = (uint8_t)v.signExp;
    c->st[p][9] = (uint8_t)(v.signExp >> 8);
    set_ptag(c, p, tag_of(v));
}

static fx   st(const cpu_t *c, int i)      { return preg(c, phys(c, i)); }
static void set_st(cpu_t *c, int i, fx v)  { set_preg(c, phys(c, i), v); }

static void pop(cpu_t *c)
{
    set_ptag(c, top(c), TAG_EMPTY);
    set_top(c, top(c) + 1);
}

/* ------------------------------------------------------------------ */
/* Exceptions                                                          */
/* ------------------------------------------------------------------ */

// Per-instruction state: accumulated exception bits and the condition codes to set.
typedef struct {
    cpu_t   *c;
    uint16_t exc;           // IE..PE, SF
    uint16_t cc;            // C0..C3 values
    uint16_t cc_mask;       // which C bits this instruction defines
    bool     mem_den;       // the memory operand was a denormal (DE decided with the other operand)
    bool     de_from_mem;   // DE came from converting a memory operand (survives ZE, not IE)
} op_t_;

static bool masked(const cpu_t *c, uint16_t bits) { return (bits & ~c->fpu_cw & 0x3F) == 0; }

static void set_c1(op_t_ *o, bool v) { o->cc_mask |= SW_C1; if (v) o->cc |= SW_C1; else o->cc &= (uint16_t)~SW_C1; }

static void commit(op_t_ *o)
{
    cpu_t *c = o->c;
    c->fpu_sw = (uint16_t)((c->fpu_sw & ~o->cc_mask) | (o->cc & o->cc_mask));
    c->fpu_sw |= o->exc & 0x7F;
    if (c->fpu_sw & ~c->fpu_cw & 0x3F) c->fpu_sw |= SW_ES | SW_B;
}

// Stack fault: underflow (empty source) or overflow (full destination).
static void stack_fault(op_t_ *o, bool overflow)
{
    o->exc |= SW_IE | SW_SF;
    set_c1(o, overflow);
}

static const uint8_t RC_TO_SF[4] = { softfloat_round_near_even, softfloat_round_min, softfloat_round_max, softfloat_round_minMag };
static const uint8_t PC_TO_BITS[4] = { 32, 80, 64, 80 };

static void sf_begin(const cpu_t *c, bool precision_control)
{
    softfloat_exceptionFlags = 0;
    softfloat_roundingMode = RC_TO_SF[(c->fpu_cw >> 10) & 3];
    extF80_roundingPrecision = precision_control ? PC_TO_BITS[(c->fpu_cw >> 8) & 3] : 80;
}

static uint16_t sf_flags(void)
{
    uint8_t f = softfloat_exceptionFlags;
    return (uint16_t)(((f & softfloat_flag_invalid) ? SW_IE : 0) | ((f & softfloat_flag_infinite) ? SW_ZE : 0) |
                      ((f & softfloat_flag_overflow) ? SW_OE : 0) | ((f & softfloat_flag_underflow) ? SW_UE : 0) |
                      ((f & softfloat_flag_inexact) ? SW_PE : 0));
}

// Operand checks, with x87 precedence: an unsupported encoding is invalid (IE); a
// denormal operand flags DE only when no operand is a NaN or unsupported.
// Returns false if any operand is unsupported.
static bool check_operands(op_t_ *o, const fx *v, int n)
{
    bool bad = false, nan = false, den = o->mem_den;
    o->de_from_mem = o->mem_den;
    o->mem_den = false;
    for (int i = 0; i < n; i++) {
        bad |= is_unsupported(v[i]);
        nan |= is_nan(v[i]);
        den |= is_denorm(v[i]);
    }
    if (bad) { o->exc |= SW_IE; return false; }
    if (den && !nan) o->exc |= SW_DE;
    return true;
}
static bool check_operand(op_t_ *o, fx v) { return check_operands(o, &v, 1); }
static bool check2(op_t_ *o, fx a, fx b) { fx v[2] = { a, b }; return check_operands(o, v, 2); }

/* ------------------------------------------------------------------ */
/* Arithmetic core                                                     */
/* ------------------------------------------------------------------ */

typedef enum { A_ADD, A_MUL, A_COM, A_COMP, A_SUB, A_SUBR, A_DIV, A_DIVR } aop_t;   // ModRM reg order

static fx sf_op(aop_t op, fx a, fx b)
{
    switch (op) {
    case A_ADD:  return extF80_add(a, b);
    case A_MUL:  return extF80_mul(a, b);
    case A_SUB:  return extF80_sub(a, b);
    case A_SUBR: return extF80_sub(b, a);
    case A_DIV:  return extF80_div(a, b);
    default:     return extF80_div(b, a);
    }
}

// dst = a op b with all x87 rules. Returns false if the destination must stay unchanged
// (unmasked invalid/denormal/zero-divide).
static bool arith(op_t_ *o, aop_t op, fx a, fx b, fx *res)
{
    cpu_t *c = o->c;
    if (!check2(o, a, b)) {
        if (!masked(c, SW_IE)) return false;
        *res = INDEFINITE;
        return true;
    }
    sf_begin(c, true);
    fx r = sf_op(op, a, b);
    uint16_t f = sf_flags();
    // Invalid reports alone. Divide-by-zero suppresses DE from register operands only: a
    // memory denormal raises DE while it is converted, before the division.
    if ((f & SW_IE) || ((f & SW_ZE) && !o->de_from_mem)) o->exc &= (uint16_t)~SW_DE;
    if (!masked(c, o->exc & SW_DE)) return false;            // unmasked denormal operand: no result
    if ((f & (SW_IE | SW_ZE)) && !masked(c, f & (SW_IE | SW_ZE))) { o->exc |= f & (SW_IE | SW_ZE); return false; }
    if ((f & SW_PE) && softfloat_extF80RoundKnown) {         // C1: SoftFloat reports the rounding direction
        set_c1(o, softfloat_extF80RoundedUp);
    } else if (f & SW_PE) {                                   // tiny/overflow paths: round toward zero to compare
        uint8_t rm = softfloat_roundingMode;
        softfloat_roundingMode = softfloat_round_minMag;
        fx t = sf_op(op, a, b);
        softfloat_roundingMode = rm;
        set_c1(o, t.signif != r.signif || fexp(t) != fexp(r));
    } else {
        set_c1(o, false);
    }
    o->exc |= f;
    *res = r;
    return true;
}

// Compare: 0 = a > b, 1 = a < b, 2 = equal, 3 = unordered.
static int compare(op_t_ *o, fx a, fx b, bool quiet_nan)
{
    if (!check2(o, a, b)) return 3;
    if (is_nan(a) || is_nan(b)) {
        if (!quiet_nan || is_snan(a) || is_snan(b)) o->exc |= SW_IE;
        return 3;
    }
    if (extF80_eq(a, b)) return 2;
    return extF80_lt(a, b) ? 1 : 0;
}

static void set_compare_cc(op_t_ *o, int r)
{
    static const uint16_t CC[4] = { 0, SW_C0, SW_C3, SW_C0 | SW_C2 | SW_C3 };
    o->cc_mask |= SW_CC;
    o->cc = CC[r];
}

static void set_compare_eflags(cpu_t *c, int r)
{
    static const uint32_t F[4] = { 0, EFL_CF, EFL_ZF, EFL_ZF | EFL_PF | EFL_CF };
    uint32_t f = cpu_calc_flags(c) & ~(EFL_ZF | EFL_PF | EFL_CF | EFL_OF | EFL_SF | EFL_AF);
    c->eflags = f | F[r];
    c->cc_op = CC_OP_EFLAGS;
}

/* ------------------------------------------------------------------ */
/* Memory operands                                                     */
/* ------------------------------------------------------------------ */

static uint64_t rd64(cpu_t *c, uint32_t a) { return cpu_read_lin(c, a, 4) | (uint64_t)cpu_read_lin(c, a + 4, 4) << 32; }
static void wr64(cpu_t *c, uint32_t a, uint64_t v) { cpu_write_lin(c, a, (uint32_t)v, 4); cpu_write_lin(c, a + 4, (uint32_t)(v >> 32), 4); }
static fx rd80(cpu_t *c, uint32_t a) { return mk((uint16_t)cpu_read_lin(c, a + 8, 2), rd64(c, a)); }
static void wr80(cpu_t *c, uint32_t a, fx v) { wr64(c, a, v.signif); cpu_write_lin(c, a + 8, v.signExp, 2); }

// Load a 32/64-bit float as extended (SNaN -> IE + quieted, denormal -> DE). Returns false
// when an unmasked exception means nothing is loaded.
static bool load_real(op_t_ *o, uint32_t a, int size, fx *out, bool arith_operand)
{
    cpu_t *c = o->c;
    sf_begin(c, false);
    bool den, nan;
    uint64_t nan_sig;
    uint16_t sign;
    if (size == 4) {
        float32_t f = { cpu_read_lin(c, a, 4) };
        den = !(f.v & 0x7F800000) && (f.v & 0x7FFFFF);
        nan = (f.v & 0x7F800000) == 0x7F800000 && (f.v & 0x7FFFFF);
        nan_sig = 0x8000000000000000ull | (uint64_t)(f.v & 0x7FFFFF) << 40;
        sign = (uint16_t)(f.v >> 16 & 0x8000);
        *out = f32_to_extF80(f);
    } else {
        float64_t f = { rd64(c, a) };
        den = !(f.v & 0x7FF0000000000000ull) && (f.v & 0xFFFFFFFFFFFFFull);
        nan = (f.v & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (f.v & 0xFFFFFFFFFFFFFull);
        nan_sig = 0x8000000000000000ull | (f.v & 0xFFFFFFFFFFFFFull) << 11;
        sign = (uint16_t)(f.v >> 48 & 0x8000);
        *out = f64_to_extF80(f);
    }
    if (arith_operand) {
        // The operation sees the operand as it is: an SNaN stays signaling, so NaN
        // selection (larger significand) and IE come from the operation itself.
        softfloat_exceptionFlags = 0;
        if (nan) *out = mk((uint16_t)(sign | 0x7FFF), nan_sig);
        o->mem_den = den;                                          // DE decided together with ST0
        return true;
    }
    o->exc |= sf_flags();
    if (den && !(o->exc & SW_IE)) o->exc |= SW_DE;
    return masked(c, o->exc & (SW_IE | SW_DE));
}

static fx load_int(cpu_t *c, uint32_t a, int size)
{
    if (size == 2) return i32_to_extF80((int16_t)cpu_read_lin(c, a, 2));
    if (size == 4) return i32_to_extF80((int32_t)cpu_read_lin(c, a, 4));
    return i64_to_extF80((int64_t)rd64(c, a));
}

// FST/FSTP m32/m64: round with RC; unmasked IE/OE/UE store nothing.
static void store_real(op_t_ *o, uint32_t a, int size, fx v)
{
    cpu_t *c = o->c;
    if (is_unsupported(v)) {                         // (stores never report DE)
        o->exc |= SW_IE;
        if (!masked(c, SW_IE)) return;
        v = INDEFINITE;
    }
    sf_begin(c, false);
    uint64_t bits, chop;
    if (size == 4) bits = extF80_to_f32(v).v;
    else           bits = extF80_to_f64(v).v;
    uint16_t f = sf_flags();
    if (!masked(c, f & (SW_IE | SW_OE | SW_UE))) { o->exc |= f; return; }
    if (f & SW_PE) {
        uint8_t rm = softfloat_roundingMode;
        softfloat_roundingMode = softfloat_round_minMag;
        chop = size == 4 ? extF80_to_f32(v).v : extF80_to_f64(v).v;
        softfloat_roundingMode = rm;
        set_c1(o, chop != bits);
    } else {
        set_c1(o, false);
    }
    o->exc |= f;
    if (size == 4) cpu_write_lin(c, a, (uint32_t)bits, 4);
    else           wr64(c, a, bits);
}

// FIST/FISTP: round with RC; out of range or NaN -> IE and the integer indefinite.
static void store_int(op_t_ *o, uint32_t a, int size, fx v)
{
    cpu_t *c = o->c;
    int64_t r;
    sf_begin(c, false);
    if (is_unsupported(v)) { softfloat_raiseFlags(softfloat_flag_invalid); r = INT64_MIN; }
    else r = extF80_to_i64(v, softfloat_roundingMode, true);   // (no DE for stores)
    uint16_t f = sf_flags();
    int64_t lim = size == 2 ? 32767 : size == 4 ? INT32_MAX : INT64_MAX;
    if (!(f & SW_IE) && (r > lim || r < -lim - 1)) { f |= SW_IE; f &= (uint16_t)~SW_PE; }
    if (f & SW_IE) {
        f &= (uint16_t)~SW_PE;
        o->exc |= f;
        if (!masked(c, SW_IE)) return;
        r = size == 2 ? INT16_MIN : size == 4 ? INT32_MIN : INT64_MIN;     // integer indefinite
    } else if (f & SW_PE) {
        int64_t t = extF80_to_i64(v, softfloat_round_minMag, false);
        set_c1(o, t != r);
        o->exc |= f;
    } else {
        set_c1(o, false);
    }
    if (size == 2) cpu_write_lin(c, a, (uint16_t)r, 2);
    else if (size == 4) cpu_write_lin(c, a, (uint32_t)r, 4);
    else wr64(c, a, (uint64_t)r);
}

/* ------------------------------------------------------------------ */
/* Push / write helpers                                                */
/* ------------------------------------------------------------------ */

static void push(op_t_ *o, fx v)
{
    cpu_t *c = o->c;
    int t = (top(c) - 1) & 7;
    if (ptag(c, t) != TAG_EMPTY) {                 // stack overflow
        stack_fault(o, true);
        if (!masked(c, SW_IE)) return;
        v = INDEFINITE;
    }
    set_top(c, t);
    set_st(c, 0, v);
}

// Read ST(i), handling stack underflow. Returns false if the instruction must stop
// (unmasked); otherwise *v is the value (indefinite when it was empty).
static bool src(op_t_ *o, int i, fx *v)
{
    if (!empty(o->c, i)) { *v = st(o->c, i); return true; }
    stack_fault(o, false);
    *v = INDEFINITE;
    return masked(o->c, SW_IE);
}

/* ------------------------------------------------------------------ */
/* Transcendentals and other special operations                        */
/* ------------------------------------------------------------------ */

// Result of an overflow in the given direction under the current RC: infinity, or the
// largest finite value when rounding goes toward zero.
static fx overflow_result(const cpu_t *c, bool neg)
{
    int rc = (c->fpu_cw >> 10) & 3;
    bool to_inf = rc == 0 || (rc == 1 && neg) || (rc == 2 && !neg);
    fx r = to_inf ? mk(0x7FFF, 0x8000000000000000ull) : mk(0x7FFE, 0xFFFFFFFFFFFFFFFFull);
    return neg ? fneg(r) : r;
}

// Round a quad result to extended (RC, no PC), set C1 and the exception flags.
static fx from_quad(op_t_ *o, float128_t q)
{
    sf_begin(o->c, false);
    fx r = f128_to_extF80(q);
    uint16_t f = sf_flags();
    if (f & SW_PE) {
        float128_t back = extF80_to_f128(r);
        q.v[1] &= 0x7FFFFFFFFFFFFFFFull;
        back.v[1] &= 0x7FFFFFFFFFFFFFFFull;
        bool up = f128_lt(q, back);
        set_c1(o, up);
    } else {
        set_c1(o, false);
    }
    o->exc |= f;
    return r;
}

// NaN propagation for one or two operands (x87 rules via SoftFloat: add of NaNs propagates).
// NaN result for operations with NaN operands, by the x87 rules SoftFloat's 8086
// specialization implements (QNaN over SNaN, then the larger significand).
static fx propagate_nan(op_t_ *o, fx a, fx b)
{
    if (is_snan(a) || is_snan(b)) o->exc |= SW_IE;
    if (!is_nan(a)) a = b;
    if (!is_nan(b)) b = a;
    uint8_t fl = softfloat_exceptionFlags;
    fx r = extF80_add(a, b);
    softfloat_exceptionFlags = fl;
    return r;
}

// Quad-precision work runs in round-to-nearest and must not leak flags into the instruction.
#define QUAD_BEGIN uint8_t q_rm_ = softfloat_roundingMode; softfloat_roundingMode = softfloat_round_near_even
#define QUAD_END   softfloat_exceptionFlags = 0; softfloat_roundingMode = q_rm_

static float128_t to_q(fx v) { return extF80_to_f128(v); }

static void op_sincos(op_t_ *o, int which)        // 0 sin, 1 cos, 2 sincos, 3 ptan
{
    cpu_t *c = o->c;
    fx x;
    if (!src(o, 0, &x)) return;
    if (empty(c, 0)) {                              // masked underflow
        set_st(c, 0, INDEFINITE);
        if (which >= 2) push(o, INDEFINITE);
        return;
    }
    if (!check_operand(o, x) || is_inf(x)) {
        o->exc |= SW_IE;
        if (!masked(c, SW_IE)) return;
        set_st(c, 0, INDEFINITE);
        if (which >= 2) push(o, INDEFINITE);
        o->cc_mask |= SW_C2;
        return;
    }
    if (is_nan(x)) {
        fx n = propagate_nan(o, x, x);
        if (!masked(c, o->exc & SW_IE)) return;
        set_st(c, 0, n);
        if (which >= 2) push(o, n);
        o->cc_mask |= SW_C2;
        return;
    }
    o->cc_mask |= SW_C2;
    if (fexp(x) >= 0x3FFF + 63) { o->cc |= SW_C2; return; }      // |x| >= 2^63: out of range, unchanged
    if (!masked(c, o->exc & SW_DE)) return;
    if (which >= 2 && !empty(c, 7)) {                              // the push would overflow
        stack_fault(o, true);
        if (!masked(c, SW_IE)) return;
        set_st(c, 0, INDEFINITE);
        push(o, INDEFINITE);
        return;
    }
    QUAD_BEGIN;
    float128_t s, co;
    if (is_zero(x)) { s = to_q(x); co = i32_to_f128(1); }
    else if (fexp(x) < 0x3FFF - 40) {
        // sin x = x - x^3/6: just below |x|; cos x = 1 - x^2/2: just below 1. The next
        // terms vanish even in quad here, so step one quad ulp in the right direction.
        s = to_q(x);
        if (s.v[0]-- == 0) s.v[1]--;
        co = i32_to_f128(1);
        if (co.v[0]-- == 0) co.v[1]--;
    }
    else q_sincos(to_q(x), &s, &co);
    float128_t t = which == 3 ? f128_div(s, co) : s;
    if (which == 3 && fexp(x) < 0x3FFF - 40 && !is_zero(x)) {
        // tan(x) = x + x^3/3 + ...: just above |x|, beyond quad precision for tiny x.
        t = to_q(x);
        if (++t.v[0] == 0) t.v[1]++;
    }
    QUAD_END;
    switch (which) {
    case 0: set_st(c, 0, from_quad(o, s)); break;
    case 1: set_st(c, 0, from_quad(o, co)); break;
    case 2: set_st(c, 0, from_quad(o, s)); push(o, from_quad(o, co)); break;
    default: set_st(c, 0, from_quad(o, t)); push(o, i32_to_extF80(1)); break;
    }
}

static void op_patan(op_t_ *o)                     // ST1 = atan2(ST1, ST0), pop
{
    cpu_t *c = o->c;
    fx x, y;
    if (!both(src(o, 0, &x), src(o, 1, &y))) return;
    fx r;
    if (empty(c, 0) || empty(c, 1)) r = INDEFINITE;
    else if (!check2(o, x, y)) r = INDEFINITE;
    else if (is_nan(x) || is_nan(y)) r = propagate_nan(o, y, x);
    else {
        if (!masked(c, o->exc & SW_DE)) return;
        QUAD_BEGIN;
        float128_t pi = q_pi(), q;
        bool sy = fsign(y), sx = fsign(x);
        if (is_inf(y) && is_inf(x))  q = f128_div(sx ? f128_mul(pi, i32_to_f128(3)) : pi, i32_to_f128(4));
        else if (is_inf(y))          q = f128_div(pi, i32_to_f128(2));
        else if (is_inf(x))          q = sx ? pi : i32_to_f128(0);
        else if (is_zero(y))         q = sx ? pi : i32_to_f128(0);
        else if (is_zero(x))         q = f128_div(pi, i32_to_f128(2));
        else if (!sx && (int)fexp(y) - (int)fexp(x) < -16500) {
            // |y/x| underflows even quad: the result is that tiny ratio, nonzero; use the
            // smallest quad denormal so rounding to extended follows RC and flags UE/PE.
            q.v[1] = 0; q.v[0] = 1;
        }
        else                         q = q_atan2(to_q(fabs_(y)), to_q(x));
        if (sy) q.v[1] |= 0x8000000000000000ull; else q.v[1] &= 0x7FFFFFFFFFFFFFFFull;
        if ((is_zero(y) && !sx) || (is_inf(x) && !sx && !is_inf(y))) {      // exact signed zero
            q = to_q(sy ? fneg(mk(0, 0)) : mk(0, 0));
        }
        QUAD_END;
        r = from_quad(o, q);
    }
    if (!masked(c, o->exc & (SW_IE | SW_DE))) return;
    set_st(c, 1, r);
    pop(c);
}

static void op_f2xm1(op_t_ *o)
{
    cpu_t *c = o->c;
    fx x;
    if (!src(o, 0, &x)) return;
    fx r;
    if (empty(c, 0) || !check_operand(o, x)) r = INDEFINITE;
    else if (is_nan(x)) r = propagate_nan(o, x, x);
    else if (is_inf(x)) r = fsign(x) ? i32_to_extF80(-1) : x;
    else if (is_zero(x)) r = x;
    else {
        if (!masked(c, o->exc & SW_DE)) return;
        QUAD_BEGIN;
        float128_t q = q_exp2m1(to_q(x));
        QUAD_END;
        r = from_quad(o, q);
    }
    if (!masked(c, o->exc & (SW_IE | SW_DE))) return;
    set_st(c, 0, r);
}

// FYL2X (plus1 = false) and FYL2XP1: ST1 = ST1 * log2(ST0 [+ 1]), pop.
static void op_yl2x(op_t_ *o, bool plus1)
{
    cpu_t *c = o->c;
    fx x, y;
    if (!both(src(o, 0, &x), src(o, 1, &y))) return;
    fx r;
    if (empty(c, 0) || empty(c, 1)) r = INDEFINITE;
    else if (!check2(o, x, y)) r = INDEFINITE;
    else if (is_nan(x) || is_nan(y)) r = propagate_nan(o, x, y);
    else {
        if (!masked(c, o->exc & SW_DE)) return;
        QUAD_BEGIN;
        float128_t l, qy = to_q(y);
        bool invalid = false, zdiv = false;
        if (!plus1) {
            if (fsign(x) && !is_zero(x)) invalid = true;
            else if (is_zero(x)) { l = to_q(mk(0xFFFF, 0x8000000000000000ull)); zdiv = !is_zero(y) && !is_inf(y); invalid = is_zero(y); }
            else if (is_inf(x)) l = to_q(x);
            else l = q_log2(to_q(x));
        } else {
            // Defined for |x| < 1 - sqrt(2)/2; outside it the manual leaves the result
            // undefined. Give the IEEE answer: log2(1 + x) is invalid below -1, -inf at -1.
            fx m1 = mk(0xBFFF, 0x8000000000000000ull);
            if (fsign(x) && !is_zero(x) && (is_inf(x) || extF80_lt(x, m1))) invalid = true;
            else if (extF80_eq(x, m1)) { l = to_q(mk(0xFFFF, 0x8000000000000000ull)); zdiv = !is_zero(y) && !is_inf(y); invalid = is_zero(y); }
            else if (is_inf(x)) l = to_q(x);
            else l = is_zero(x) ? to_q(x) : q_log2p1(to_q(x));
        }
        float128_t q = {{ 0, 0 }};
        uint8_t qf = 0;
        if (!invalid) {
            softfloat_exceptionFlags = 0;
            q = f128_mul(qy, l);                   // inf * 0 -> invalid
            qf = softfloat_exceptionFlags;
            invalid = qf & softfloat_flag_invalid;
        }
        QUAD_END;
        if (qf & softfloat_flag_overflow) o->exc |= SW_OE | SW_PE;     // beyond even the quad range
        if (invalid || zdiv) o->exc &= (uint16_t)~SW_DE;    // IE / ZE are reported alone
        if (invalid) { o->exc |= SW_IE; r = INDEFINITE; }
        else if ((qf & softfloat_flag_underflow) && !((q.v[1] << 1) | q.v[0])) {
            // Below even the quad range: a tiny nonzero result that rounds per RC (UE, PE).
            q.v[1] = (fsign(y) ^ (l.v[1] >> 63)) ? 0x8000000000000000ull : 0;
            q.v[0] = 1;
            r = from_quad(o, q);
        }
        else if (qf & softfloat_flag_overflow) {
            r = overflow_result(c, q.v[1] >> 63);
            set_c1(o, is_inf(r));
        } else {
            if (zdiv) o->exc |= SW_ZE;
            r = from_quad(o, q);
        }
    }
    if (!masked(c, o->exc & (SW_IE | SW_DE | SW_ZE))) return;
    set_st(c, 1, r);
    pop(c);
}

static void op_scale(op_t_ *o)
{
    cpu_t *c = o->c;
    fx x, s;
    if (!both(src(o, 0, &x), src(o, 1, &s))) return;
    fx r;
    if (empty(c, 0) || empty(c, 1)) r = INDEFINITE;
    else if (!check2(o, x, s)) r = INDEFINITE;
    else if (is_nan(x) || is_nan(s)) r = propagate_nan(o, x, s);
    else if (is_inf(s)) {
        if (fsign(s)) {                                      // x * 2^-inf
            if (is_inf(x)) { o->exc |= SW_IE; r = INDEFINITE; }
            else r = fsign(x) ? fneg(mk(0, 0)) : mk(0, 0);
        } else {                                             // x * 2^+inf
            if (is_zero(x)) { o->exc |= SW_IE; r = INDEFINITE; }
            else r = fsign(x) ? mk(0xFFFF, 0x8000000000000000ull) : mk(0x7FFF, 0x8000000000000000ull);
        }
    } else if (is_zero(x) || is_inf(x)) {
        r = x;
    } else {
        if (!masked(c, o->exc & SW_DE)) return;
        sf_begin(c, false);
        int64_t n;
        if (fexp(s) >= 0x3FFF + 20) n = fsign(s) ? -70000 : 70000;     // far beyond the exponent range
        else n = extF80_to_i64(s, softfloat_round_minMag, false);
        softfloat_exceptionFlags = 0;
        // Normalize x to exponent 0x3FFF (exact), then scale by the remaining power of two in
        // at most two steps: the first stays inside the exponent range (exact), only the
        // second can overflow, underflow or round.
        int ex = fexp(x);
        uint64_t sig = x.signif;
        if (ex == 0) { int sh = __builtin_clzll(sig); sig <<= sh; ex = 1 - sh; }
        int64_t N = n + (ex - 0x3FFF);
        if (N > 16383 + 70) N = 16383 + 70;
        if (N < -16383 - 70) N = -16383 - 70;
        fx v = mk((uint16_t)((x.signExp & 0x8000) | 0x3FFF), sig);
        if (N > 16000 || N < -16000) {
            int first = N > 0 ? 16000 : -16000;
            v = extF80_mul(v, mk((uint16_t)(0x3FFF + first), 0x8000000000000000ull));
            N -= first;
        }
        fx before = v, p2 = mk((uint16_t)(0x3FFF + N), 0x8000000000000000ull);
        softfloat_exceptionFlags = 0;
        v = extF80_mul(before, p2);
        uint16_t f = sf_flags();
        o->exc |= f;
        if (f & SW_PE) {                                   // C1: was the rounding upward?
            uint8_t rm = softfloat_roundingMode;
            softfloat_roundingMode = softfloat_round_minMag;
            fx t = extF80_mul(before, p2);
            softfloat_roundingMode = rm;
            softfloat_exceptionFlags = 0;
            set_c1(o, t.signif != v.signif || fexp(t) != fexp(v));
        } else set_c1(o, false);
        r = v;
    }
    if (!masked(c, o->exc & (SW_IE | SW_DE | SW_OE | SW_UE))) return;
    set_st(c, 0, r);
}

static void op_xtract(op_t_ *o)
{
    cpu_t *c = o->c;
    fx x;
    if (!src(o, 0, &x)) return;
    fx e, m;
    if (empty(c, 0) || !check_operand(o, x)) e = m = INDEFINITE;
    else if (is_nan(x)) e = m = propagate_nan(o, x, x);
    else if (is_inf(x)) { e = fabs_(x); m = x; }
    else if (is_zero(x)) { o->exc |= SW_ZE; e = mk(0xFFFF, 0x8000000000000000ull); m = x; }
    else {
        if (!masked(c, o->exc & SW_DE)) return;
        int ex = fexp(x);
        uint64_t sig = x.signif;
        if (ex == 0) {                                       // denormal: normalize
            int sh = __builtin_clzll(sig);
            sig <<= sh;
            ex = 1 - sh;
        }
        e = i32_to_extF80(ex - 0x3FFF);
        m = mk((uint16_t)((x.signExp & 0x8000) | 0x3FFF), sig);
    }
    if (!masked(c, o->exc & (SW_IE | SW_DE | SW_ZE))) return;
    if (!empty(c, 7)) {
        stack_fault(o, true);
        if (!masked(c, SW_IE)) return;
        e = m = INDEFINITE;
    }
    set_st(c, 0, e);
    push(o, m);
}

// FPREM (round = false, truncating quotient) and FPREM1 (IEEE remainder).
static void op_prem(op_t_ *o, bool ieee)
{
    cpu_t *c = o->c;
    fx a, b;
    if (!both(src(o, 0, &a), src(o, 1, &b))) return;
    o->cc_mask |= SW_CC;
    o->cc = 0;
    fx r;
    if (empty(c, 0) || empty(c, 1)) r = INDEFINITE;
    else if (!check2(o, a, b)) r = INDEFINITE;
    else if (is_nan(a) || is_nan(b)) r = propagate_nan(o, a, b);
    else if (is_inf(a) || is_zero(b)) { o->exc = (uint16_t)((o->exc & ~SW_DE) | SW_IE); r = INDEFINITE; }
    else if (is_zero(a) || is_inf(b)) r = a;
    else {
        if (!masked(c, o->exc & SW_DE)) return;
        // Normalize both to (64-bit significand with top bit set, unbiased exponent).
        int ea = fexp(a), eb = fexp(b);
        uint64_t sa = a.signif, sb = b.signif;
        if (!ea) { int s = __builtin_clzll(sa); sa <<= s; ea = 1 - s; }
        if (!eb) { int s = __builtin_clzll(sb); sb <<= s; eb = 1 - s; }
        int d = ea - eb;
        bool neg = fsign(a);
        uint64_t q = 0;
        unsigned __int128 rem;
        int re;                                              // exponent of rem's unit
        if (d < 0) {
            // |a| < |b|: the quotient is 0 and the remainder is a, except that FPREM1
            // rounds the quotient to 1 when |a| > |b|/2 (only possible for d = -1:
            // then 2|a| and |b| share an exponent and compare as sa vs sb).
            rem = sa; re = ea;
            if (ieee && d == -1 && sa > sb) {
                rem = ((unsigned __int128)sb << 1) - sa;     // |b| - |a| in units of a's ulp
                neg = !neg;
                q = 1;
            }
        } else {
            int n = d;
            bool partial = d >= 64;
            if (partial) n = 32 + ((d - 64) & 31);           // reduce the exponent difference to 32..63
            // remainder of sa * 2^n by sb (an exact integer division in 128 bits)
            unsigned __int128 num = (unsigned __int128)sa << n;
            unsigned __int128 qq = num / sb;
            rem = num % sb;
            q = (uint64_t)qq;
            re = eb + (partial ? d - n : 0);
            if (ieee && !partial) {
                unsigned __int128 twice = rem << 1;
                if (twice > sb || (twice == sb && (q & 1))) { rem = sb - rem; q++; neg = !neg; }
            }
            if (partial) { o->cc |= SW_C2; q = 0; }
        }
        if (!(o->cc & SW_C2)) {
            if (q & 1) o->cc |= SW_C1;
            if (q & 2) o->cc |= SW_C3;
            if (q & 4) o->cc |= SW_C0;
        }
        // Pack rem (an integer in units of 2^(re - 0x3FFF - 63)) as an extended value: exact.
        if (rem == 0) r = mk(fsign(a) ? 0x8000 : 0, 0);
        else {
            int sh = 0;
            while (!(rem >> 63)) { rem <<= 1; sh++; }
            while (rem >> 64) { rem >>= 1; sh--; }           // (cannot lose bits: rem < 2^65 shifted from small values)
            int e = re - sh;
            uint64_t sig = (uint64_t)rem;
            if (e <= 0) { sig >>= (1 - e); e = 0; }          // exact denormal
            r = mk((uint16_t)((neg ? 0x8000 : 0) | e), sig);
        }
        set_c1(o, (o->cc & SW_C1) != 0);
    }
    if (!masked(c, o->exc & (SW_IE | SW_DE))) return;
    set_st(c, 0, r);
}

/* ------------------------------------------------------------------ */
/* BCD                                                                 */
/* ------------------------------------------------------------------ */

static void op_fbld(op_t_ *o, uint32_t a)
{
    cpu_t *c = o->c;
    int64_t v = 0;
    for (int i = 8; i >= 0; i--) {
        uint8_t b = (uint8_t)cpu_read_lin(c, a + (uint32_t)i, 1);
        v = v * 100 + (b >> 4) * 10 + (b & 15);
    }
    bool neg = cpu_read_lin(c, a + 9, 1) & 0x80;
    fx r = i64_to_extF80(v);
    if (neg) r = fneg(r);
    push(o, r);
}

static void op_fbstp(op_t_ *o, uint32_t a)
{
    cpu_t *c = o->c;
    fx v;
    if (!src(o, 0, &v)) return;
    sf_begin(c, false);
    bool bad = empty(c, 0) || is_unsupported(v) || is_nan(v) || is_inf(v);
    uint64_t n = 0;                                                    // magnitude
    if (!bad) {
        int64_t s = extF80_to_i64(v, softfloat_roundingMode, true);     // round the signed value
        n = s < 0 ? 0 - (uint64_t)s : (uint64_t)s;                      // INT64_MIN (invalid) stays in range
        bad = (softfloat_exceptionFlags & softfloat_flag_invalid) || n > 999999999999999999ull;
        if (!bad) o->exc |= sf_flags();
    }
    if (bad) {
        o->exc |= SW_IE;
        if (!masked(c, SW_IE)) return;
        // packed BCD indefinite: FFFF C000 0000 0000 0000
        for (int i = 0; i < 7; i++) cpu_write_lin(c, a + (uint32_t)i, 0, 1);
        cpu_write_lin(c, a + 7, 0xC0, 1);
        cpu_write_lin(c, a + 8, 0xFF, 1);
        cpu_write_lin(c, a + 9, 0xFF, 1);
        pop(c);
        return;
    }
    for (int i = 0; i < 9; i++) {
        int lo = (int)(n % 10); n /= 10;
        int hi = (int)(n % 10); n /= 10;
        cpu_write_lin(c, a + (uint32_t)i, (uint32_t)(hi << 4 | lo), 1);
    }
    cpu_write_lin(c, a + 9, fsign(v) ? 0x80 : 0, 1);
    pop(c);
}

/* ------------------------------------------------------------------ */
/* Environment                                                         */
/* ------------------------------------------------------------------ */

// FLD1, FLDL2T, FLDL2E, FLDPI, FLDLG2, FLDLN2, FLDZ (D9 E8..EE), rounded according to RC
// like the 387 and later.
static fx fpu_const(const cpu_t *c, int m)
{
    int rc = (c->fpu_cw >> 10) & 3;
    bool down = rc == 1 || rc == 3, up = rc == 2;
    switch (m) {
    case 0xE8: return mk(0x3FFF, 0x8000000000000000ull);                                                // 1
    case 0xE9: return mk(0x4000, up ? 0xD49A784BCD1B8AFFull : 0xD49A784BCD1B8AFEull);                   // log2(10)
    case 0xEA: return mk(0x3FFF, down ? 0xB8AA3B295C17F0BBull : 0xB8AA3B295C17F0BCull);                 // log2(e)
    case 0xEB: return mk(0x4000, down ? 0xC90FDAA22168C234ull : 0xC90FDAA22168C235ull);                 // pi
    case 0xEC: return mk(0x3FFD, down ? 0x9A209A84FBCFF798ull : 0x9A209A84FBCFF799ull);                 // log10(2)
    case 0xED: return mk(0x3FFE, down ? 0xB17217F7D1CF79ABull : 0xB17217F7D1CF79ACull);                 // ln(2)
    default:   return mk(0, 0);                                                                         // 0
    }
}

void fpu_init(cpu_t *c)
{
    c->fpu_cw = 0x037F;
    c->fpu_sw = 0;
    c->fpu_tw = 0xFFFF;
    c->fpu_ip = c->fpu_dp = 0;
    c->fpu_cs = c->fpu_ds = c->fpu_op = 0;
}

// Tag word as stored by FNSTENV/FNSAVE: recomputed from the register contents.
static uint16_t full_tags(const cpu_t *c)
{
    uint16_t t = 0;
    for (int p = 0; p < 8; p++) {
        int tg = ptag(c, p) == TAG_EMPTY ? TAG_EMPTY : tag_of(preg(c, p));
        t |= (uint16_t)(tg << (2 * p));
    }
    return t;
}

static uint32_t store_env(cpu_t *c, const insn_t *in, uint32_t a)
{
    bool big = in->opsize == 4, pm = cpu_pmode(c) && !(c->eflags & EFL_VM);
    int n = big ? 4 : 2;
    uint32_t v[7];
    v[0] = c->fpu_cw; v[1] = c->fpu_sw; v[2] = full_tags(c);
    if (pm) {
        v[3] = c->fpu_ip; v[4] = c->fpu_cs | (big ? (uint32_t)(c->fpu_op & 0x7FF) << 16 : 0);
        v[5] = c->fpu_dp; v[6] = c->fpu_ds;
    } else {
        v[3] = c->fpu_ip & 0xFFFF; v[4] = (c->fpu_ip >> 4 & 0xF000) | (c->fpu_op & 0x7FF);
        v[5] = c->fpu_dp & 0xFFFF; v[6] = c->fpu_dp >> 4 & 0xF000;
    }
    for (int i = 0; i < 7; i++) {
        uint32_t w = v[i];
        if (big && i < 3) w |= 0xFFFF0000u;          // reserved upper halves read as ones
        cpu_write_lin(c, a + (uint32_t)(i * n), big ? w : (w & 0xFFFF), n);
    }
    return (uint32_t)(7 * n);
}

static uint32_t load_env(cpu_t *c, const insn_t *in, uint32_t a)
{
    bool big = in->opsize == 4, pm = cpu_pmode(c) && !(c->eflags & EFL_VM);
    int n = big ? 4 : 2;
    uint32_t v[7];
    for (int i = 0; i < 7; i++) v[i] = cpu_read_lin(c, a + (uint32_t)(i * n), n);
    c->fpu_cw = (uint16_t)v[0];
    c->fpu_sw = (uint16_t)v[1];
    c->fpu_tw = (uint16_t)v[2];
    if (pm) { c->fpu_ip = v[3]; c->fpu_cs = (uint16_t)v[4]; c->fpu_op = (uint16_t)(v[4] >> 16 & 0x7FF); c->fpu_dp = v[5]; c->fpu_ds = (uint16_t)v[6]; }
    else    { c->fpu_ip = (v[3] & 0xFFFF) | (v[4] & 0xF000) << 4; c->fpu_op = (uint16_t)(v[4] & 0x7FF); c->fpu_dp = (v[5] & 0xFFFF) | (v[6] & 0xF000) << 4; }
    // ES/B follow the unmasked exception bits.
    if (c->fpu_sw & ~c->fpu_cw & 0x3F) c->fpu_sw |= SW_ES | SW_B;
    else c->fpu_sw &= (uint16_t)~(SW_ES | SW_B);
    return (uint32_t)(7 * n);
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

void fpu_wait(cpu_t *c)
{
    if ((c->cr0 & (CR0_TS | CR0_MP)) == (CR0_TS | CR0_MP)) cpu_fault(c, EXC_NM, 0);
    if ((c->fpu_sw & SW_ES) && (c->cr0 & CR0_NE)) cpu_fault(c, EXC_MF, 0);
}


/* ------------------------------------------------------------------ */
/* Fast paths                                                          */
/* ------------------------------------------------------------------ */
// The hot x87 traffic in BeOS (its mixer and decoders, measured) is loads, stores, integer
// conversions and plain arithmetic with every exception masked. For ordinary values (normal or
// zero, results in range) these are done here, producing exactly the state the general code
// would (same result, PE, C1 = rounded away from zero); anything else is left to it.
// Two callers: fpu_exec's front end (memory through cpu_read_lin/cpu_write_lin, which may
// fault), and the entry points for generated code at the end of this section ("lean": memory
// through the fast TLB only, so they never fault and change nothing when they decline).
// tests/test_x87.c checks both against x87 under Rosetta and against the general code.

static int g_fast = -1;                                         // -1: not decided yet (environment)
static bool g_native = true;                                    // native add/sub/mul/div (BEEMU_NO_FPU_NATIVE)
void fpu_set_fast_paths(bool on) { g_fast = on; }
void fpu_set_native(bool on) { g_native = on; }
static bool fast_on(void)
{
    if (g_fast < 0) { g_fast = getenv("BEEMU_NO_FPU_FAST") == NULL; g_native = getenv("BEEMU_NO_FPU_NATIVE") == NULL; }
    return g_fast > 0;
}

// Rounding increment for the RC field: does a value with discarded bits `rest` (out of
// `half` * 2) round away from zero? `odd` is the kept part's low bit (ties to even).
static bool round_up(int rc, bool sign, uint64_t rest, uint64_t half, bool odd)
{
    if (!rest) return false;
    switch (rc) {
    case 0:  return rest > half || (rest == half && odd);     // nearest even
    case 1:  return sign;                                      // down (toward -inf)
    case 2:  return !sign;                                     // up (toward +inf)
    default: return false;                                     // chop
    }
}

static void fast_status(cpu_t *c, bool inexact, bool up)
{
    c->fpu_sw = (uint16_t)((c->fpu_sw & ~SW_C1) | (up ? SW_C1 : 0) | (inexact ? SW_PE : 0));
}

static bool ordinary(fx v) { int e = fexp(v); return (e == 0 && v.signif == 0) || (e != 0 && e != 0x7FFF && (v.signif >> 63)); }

/* ---- memory ---- */
static inline bool m_rd(cpu_t *c, uint32_t a, int size, bool lean, uint64_t *v)
{
    if (lean) {
        uint8_t *h = cpu_fast_host(c, a, size, 0);
        if (!h) return false;
        if (size == 8) { memcpy(v, h, 8); return true; }
        if (size == 4) { uint32_t x; memcpy(&x, h, 4); *v = x; return true; }
        uint16_t x; memcpy(&x, h, 2); *v = x; return true;
    }
    *v = size == 8 ? rd64(c, a) : cpu_read_lin(c, a, size);
    return true;
}

static inline bool m_wr(cpu_t *c, uint32_t a, int size, bool lean, uint64_t v)
{
    if (lean) {
        uint8_t *h = cpu_fast_host(c, a, size, 1);
        if (!h) return false;
        if (size == 8) memcpy(h, &v, 8);
        else if (size == 4) { uint32_t x = (uint32_t)v; memcpy(h, &x, 4); }
        else { uint16_t x = (uint16_t)v; memcpy(h, &x, 2); }
        return true;
    }
    if (size == 8) wr64(c, a, v); else cpu_write_lin(c, a, (uint32_t)v, size);
    return true;
}

/* ---- values ---- */
// float32/float64 bits -> extended, exactly; false for NaN, infinity, denormal.
static bool from_real(uint64_t f, int size, fx *v)
{
    if (size == 4) {
        uint32_t e = (uint32_t)(f >> 23 & 0xFF), m = (uint32_t)(f & 0x7FFFFF);
        uint16_t sg = (uint16_t)(f >> 16 & 0x8000);
        if (e == 0 && m == 0) { *v = mk(sg, 0); return true; }
        if (e == 0 || e == 0xFF) return false;
        *v = mk((uint16_t)(sg | (e - 127 + 16383)), 0x8000000000000000ull | (uint64_t)m << 40);
        return true;
    }
    uint32_t e = (uint32_t)(f >> 52 & 0x7FF);
    uint64_t m = f & 0xFFFFFFFFFFFFFull;
    uint16_t sg = (uint16_t)(f >> 48 & 0x8000);
    if (e == 0 && m == 0) { *v = mk(sg, 0); return true; }
    if (e == 0 || e == 0x7FF) return false;
    *v = mk((uint16_t)(sg | (e - 1023 + 16383)), 0x8000000000000000ull | m << 11);
    return true;
}

static fx from_int(int64_t i)                                   // always exact
{
    if (i == 0) return mk(0, 0);
    uint64_t mag = i < 0 ? (uint64_t)0 - (uint64_t)i : (uint64_t)i;
    int lz = __builtin_clzll(mag);
    return mk((uint16_t)((i < 0 ? 0x8000 : 0) | (16383 + 63 - lz)), mag << lz);
}

static int64_t int_of(uint64_t raw, int size)                   // memory integer, sign-extended
{
    return size == 2 ? (int16_t)raw : size == 4 ? (int32_t)raw : (int64_t)raw;
}

// Extended -> float32/float64 bits with RC; false unless v is ordinary and the result a normal
// float or zero (overflow/underflow are the general code's).
static bool to_real(fx v, int size, int rc, uint64_t *bits, bool *inexact, bool *up)
{
    bool sign = fsign(v);
    int e = fexp(v);
    *inexact = *up = false;
    if (e == 0 && v.signif == 0) { *bits = size == 4 ? (uint64_t)sign << 31 : (uint64_t)sign << 63; return true; }
    if (e == 0 || e == 0x7FFF || !(v.signif >> 63)) return false;
    int shift = size == 4 ? 40 : 11, bias = size == 4 ? 127 : 1023, emax = size == 4 ? 254 : 2046;
    int te = e - 16383 + bias;
    uint64_t m = v.signif >> shift, rest = v.signif & ((1ull << shift) - 1);
    *up = round_up(rc, sign, rest, 1ull << (shift - 1), m & 1);
    *inexact = rest != 0;
    if (*up && ++m >> (64 - shift)) { m >>= 1; te++; }          // carried into a new bit
    if (te < 1 || te > emax) return false;
    uint64_t frac = m & ((1ull << (63 - shift)) - 1);
    *bits = size == 4 ? (uint64_t)sign << 31 | (uint64_t)te << 23 | frac
                      : (uint64_t)sign << 63 | (uint64_t)te << 52 | frac;
    return true;
}

// Extended -> integer with RC; false unless v is ordinary and the result fits.
static bool to_int(fx v, int size, int rc, uint64_t *out, bool *inexact, bool *up)
{
    bool sign = fsign(v);
    int e = fexp(v);
    uint64_t mag = 0;
    *inexact = *up = false;
    if (!(e == 0 && v.signif == 0)) {
        if (e == 0 || e == 0x7FFF || !(v.signif >> 63)) return false;
        int ue = e - 16383;                                     // value = sig * 2^(ue - 63)
        if (ue >= 63) return false;
        uint64_t ip, frac;                                      // integer part, fraction as 0.64 fixed point
        if (ue >= 0) { ip = v.signif >> (63 - ue); frac = v.signif << (ue + 1); }
        else if (ue == -1) { ip = 0; frac = v.signif; }
        else { ip = 0; frac = 1; }                              // below one half: only "nonzero" matters
        *up = round_up(rc, sign, frac, 1ull << 63, ip & 1);
        *inexact = frac != 0;
        mag = ip + *up;
    }
    uint64_t lim = size == 2 ? 32767 : size == 4 ? 0x7FFFFFFF : 0x7FFFFFFFFFFFFFFFull;
    if (mag > lim + (sign ? 1 : 0)) return false;               // out of range: IE in the general path
    *out = sign ? (uint64_t)0 - mag : mag;
    return true;
}

// compare() for ordinary operands (normal or zero), which raise nothing: 0 a > b, 1 a < b, 2 equal.
static int cmp_ord(fx a, fx b)
{
    bool za = is_zero(a), zb = is_zero(b), sa = fsign(a), sb = fsign(b);
    if (za && zb) return 2;
    if (za) return sb ? 0 : 1;
    if (zb) return sa ? 1 : 0;
    if (sa != sb) return sa ? 1 : 0;
    int ea = fexp(a), eb = fexp(b);
    if (ea == eb && a.signif == b.signif) return 2;
    bool mag_less = ea != eb ? ea < eb : a.signif < b.signif;
    return mag_less != sa ? 1 : 0;
}

static void fast_compare_cc(cpu_t *c, int r)
{
    static const uint16_t CC[3] = { 0, SW_C0, SW_C3 };
    c->fpu_sw = (uint16_t)((c->fpu_sw & ~SW_CC) | CC[r]);
}

static bool m_rd80(cpu_t *c, uint32_t a, bool lean, fx *v)
{
    if (!lean) { *v = rd80(c, a); return true; }
    uint8_t *h = cpu_fast_host(c, a, 10, 0);
    if (!h) return false;
    uint16_t se;
    memcpy(&v->signif, h, 8);
    memcpy(&se, h + 8, 2);
    v->signExp = se;
    return true;
}

static bool m_wr80(cpu_t *c, uint32_t a, bool lean, fx v)
{
    if (!lean) { wr80(c, a, v); return true; }
    uint8_t *h = cpu_fast_host(c, a, 10, 1);
    if (!h) return false;
    memcpy(h, &v.signif, 8);
    memcpy(h + 8, &v.signExp, 2);
    return true;
}

/* ---- instructions ---- */
static bool f_push(cpu_t *c, fx v)                              // only into an empty slot
{
    int t = (top(c) - 1) & 7;
    if (ptag(c, t) != TAG_EMPTY) return false;
    set_top(c, t);
    set_preg(c, t, v);
    return true;
}

static bool f_fld(cpu_t *c, uint32_t a, int size, bool lean)    // FLD m32/m64
{
    uint64_t raw;
    fx v;
    if (ptag(c, (top(c) - 1) & 7) != TAG_EMPTY) return false;
    return m_rd(c, a, size, lean, &raw) && from_real(raw, size, &v) && f_push(c, v);
}

static bool f_fild(cpu_t *c, uint32_t a, int size, bool lean)   // FILD m16/m32/m64
{
    uint64_t raw;
    if (ptag(c, (top(c) - 1) & 7) != TAG_EMPTY) return false;
    return m_rd(c, a, size, lean, &raw) && f_push(c, from_int(int_of(raw, size)));
}

static bool f_fst(cpu_t *c, uint32_t a, int size, bool popafter, bool lean)   // FST(P) m32/m64
{
    int p = top(c);
    uint64_t bits;
    bool inexact, up;
    if (ptag(c, p) == TAG_EMPTY || !to_real(preg(c, p), size, (c->fpu_cw >> 10) & 3, &bits, &inexact, &up)) return false;
    if (!m_wr(c, a, size, lean, bits)) return false;
    fast_status(c, inexact, up);
    if (popafter) pop(c);
    return true;
}

static bool f_fist(cpu_t *c, uint32_t a, int size, bool popafter, bool lean)  // FIST(P) m16/m32/m64
{
    int p = top(c);
    uint64_t r;
    bool inexact, up;
    if (ptag(c, p) == TAG_EMPTY || !to_int(preg(c, p), size, (c->fpu_cw >> 10) & 3, &r, &inexact, &up)) return false;
    if (!m_wr(c, a, size, lean, r)) return false;
    fast_status(c, inexact, up);
    if (popafter) pop(c);
    return true;
}


/* ---- exact native arithmetic for the common case ----
 * Both operands normal (not zero), precision control 64 bits (PC = 3, BeOS's usual 037F
 * control word), result normal: the same bits, PE and rounding direction (C1) as SoftFloat,
 * with plain 64/128-bit integer arithmetic. Anything else returns false and SoftFloat does it. */
typedef unsigned __int128 u128;

// Round sig (top bit set) with the discarded part described by rest/half under RC; e may grow.
static bool nat_round(int rc, bool sign, uint64_t *sig, int *e, uint64_t rest, uint64_t half, bool *inexact, bool *up)
{
    *inexact = rest != 0;
    *up = round_up(rc, sign, rest, half, *sig & 1);
    if (*up && ++*sig == 0) { *sig = 0x8000000000000000ull; ++*e; }
    return *e >= 1 && *e <= 0x7FFE;
}

static bool nat_normal(fx v) { int e = fexp(v); return e != 0 && e != 0x7FFF && (v.signif >> 63); }

static bool native_arith(const cpu_t *c, aop_t op, fx a, fx b, fx *r, bool *inexact, bool *up)
{
    if (((c->fpu_cw >> 8) & 3) != 3 || !nat_normal(a) || !nat_normal(b)) return false;
    int rc = (c->fpu_cw >> 10) & 3;
    bool sa = fsign(a), sb = fsign(b);
    int ea = fexp(a), eb = fexp(b);
    uint64_t sig;
    int e;
    switch (op) {
    case A_MUL: {
        bool sign = sa != sb;
        u128 p = (u128)a.signif * b.signif;                     // [2^126, 2^128)
        e = ea + eb - 16383;
        if (p >> 127) e++; else p <<= 1;
        sig = (uint64_t)(p >> 64);
        if (!nat_round(rc, sign, &sig, &e, (uint64_t)p, 1ull << 63, inexact, up)) return false;
        *r = mk((uint16_t)((sign ? 0x8000 : 0) | e), sig);
        return true;
    }
    case A_DIV: case A_DIVR: {
        if (op == A_DIVR) { fx t = a; a = b; b = t; ea = fexp(a); eb = fexp(b); }
        bool sign = sa != sb;
        e = ea - eb + 16383;
        u128 n;
        if (a.signif >= b.signif) n = (u128)a.signif << 63;
        else { n = (u128)a.signif << 64; e--; }
        u128 q = n / b.signif;                                  // [2^63, 2^64)
        uint64_t rem = (uint64_t)(n - q * b.signif), half_d = b.signif - rem;
        sig = (uint64_t)q;
        // Discarded fraction rem / b against one half: encode as 0, 1 (< half), 2 (= half), 3 (> half).
        uint64_t rest = rem == 0 ? 0 : rem < half_d ? 1 : rem == half_d ? 2 : 3;
        if (!nat_round(rc, sign, &sig, &e, rest, 2, inexact, up)) return false;
        *r = mk((uint16_t)((sign ? 0x8000 : 0) | e), sig);
        return true;
    }
    default: break;
    }
    // ADD / SUB / SUBR: x + y with y's sign flipped for subtraction.
    if (op == A_SUBR) { fx t = a; a = b; b = t; bool ts = sa; sa = sb; sb = ts; int te = ea; ea = eb; eb = te; }
    if (op == A_SUB || op == A_SUBR) sb = !sb;
    // big = the larger magnitude
    bool swap = eb > ea || (eb == ea && b.signif > a.signif);
    uint64_t mb_ = swap ? a.signif : b.signif, mB = swap ? b.signif : a.signif;
    int eB = swap ? eb : ea, d = swap ? eb - ea : ea - eb;
    bool sB = swap ? sb : sa, ss = swap ? sa : sb;
    u128 B = (u128)mB << 64, S;
    bool sticky = false;
    if (d == 0) S = (u128)mb_ << 64;
    else if (d < 64) S = (u128)mb_ << (64 - d);
    else if (d < 128) { S = (u128)mb_ >> (d - 64); sticky = d > 64 && (mb_ & ((1ull << (d - 64)) - 1)) != 0; }
    else { S = 0; sticky = true; }
    e = eB;
    u128 x;
    if (sB == ss) {
        x = B + S;
        if (x < B) {                                            // carry out of bit 127
            sticky |= (bool)(x & 1);
            x = (x >> 1) | ((u128)1 << 127);
            e++;
        }
    } else {
        x = B - S - (sticky ? 1 : 0);                           // a sticky remainder borrows one
        if (x == 0) return false;                               // exact zero: sign rules, leave to SoftFloat
        int hi = (int)(x >> 64 ? __builtin_clzll((uint64_t)(x >> 64)) : 64 + __builtin_clzll((uint64_t)x));
        x <<= hi;                                               // exact: hi > 1 only when d <= 1 (no sticky)
        e -= hi;
    }
    sig = (uint64_t)(x >> 64);
    uint64_t rest = (uint64_t)x | (sticky ? 1 : 0);
    if (!nat_round(rc, sB, &sig, &e, rest, 1ull << 63, inexact, up)) return false;
    *r = mk((uint16_t)((sB ? 0x8000 : 0) | e), sig);
    return true;
}

// dst = a op b for ordinary operands when SoftFloat reports nothing but (maybe) inexact and
// knows the rounding direction; otherwise false (nothing changed) and the general code does it.
static bool fast_arith(cpu_t *c, aop_t op, int dst, fx a, fx b, bool popafter)
{
    if (!ordinary(a) || !ordinary(b)) return false;
    fx nr;
    bool n_inexact, n_up;
    if (g_native && native_arith(c, op, a, b, &nr, &n_inexact, &n_up)) {
        set_st(c, dst, nr);
        if (popafter) pop(c);
        fast_status(c, n_inexact, n_up);
        return true;
    }
    sf_begin(c, true);
    fx r = sf_op(op, a, b);
    uint8_t f = softfloat_exceptionFlags;
    if (f & (uint8_t)~softfloat_flag_inexact) return false;
    bool up = false;
    if (f) { if (!softfloat_extF80RoundKnown) return false; up = softfloat_extF80RoundedUp; }
    set_st(c, dst, r);
    if (popafter) pop(c);
    fast_status(c, f != 0, up);
    return true;
}

// ST0 op= m32 real (D8), m64 real (DC), m32 int (DA), m16 int (DE); FCOM(P) with them.
static bool f_arith_mem(cpu_t *c, int esc, aop_t op, uint32_t a, bool lean)
{
    uint64_t raw;
    fx b;
    if (empty(c, 0)) return false;
    int size = esc == 0 ? 4 : esc == 4 ? 8 : esc == 2 ? 4 : 2;
    if (!m_rd(c, a, size, lean, &raw)) return false;
    if (esc == 0 || esc == 4) { if (!from_real(raw, size, &b)) return false; }
    else b = from_int(int_of(raw, size));
    if (op == A_COM || op == A_COMP) {
        fx x = st(c, 0);
        if (!ordinary(x)) return false;
        fast_compare_cc(c, cmp_ord(x, b));
        if (op == A_COMP) pop(c);
        return true;
    }
    return fast_arith(c, op, 0, st(c, 0), b, false);
}

// FCOM-family compare of ST0 with ST(i), both present and ordinary. eflags: FCOMI-style
// (ZF PF CF, OF SF AF cleared, C1 cleared); otherwise C0 C2 C3 (C1 cleared). Then n pops.
static bool f_com_reg(cpu_t *c, int i, bool eflags, int pops)
{
    if (empty(c, 0) || empty(c, i)) return false;
    fx a = st(c, 0), b = st(c, i);
    if (!ordinary(a) || !ordinary(b)) return false;
    int r = cmp_ord(a, b);
    if (eflags) { set_compare_eflags(c, r); c->fpu_sw &= (uint16_t)~SW_C1; }
    else fast_compare_cc(c, r);
    while (pops--) pop(c);
    return true;
}

// Register forms: arithmetic (D8, DC, DE), FLD ST(i), FXCH.
static bool fast_reg(cpu_t *c, int esc, int reg, int i)
{
    if ((esc == 0 || esc == 4 || esc == 6) && reg != 2 && reg != 3) {
        if (empty(c, 0) || empty(c, i)) return false;
        if (esc == 0) return fast_arith(c, (aop_t)reg, 0, st(c, 0), st(c, i), false);
        static const aop_t REV[8] = { A_ADD, A_MUL, A_COM, A_COMP, A_SUBR, A_SUB, A_DIVR, A_DIV };
        return fast_arith(c, REV[reg], i, st(c, i), st(c, 0), esc == 6);
    }
    if (esc == 1 && reg == 0) {                                 // FLD ST(i)
        if (empty(c, i)) return false;
        return f_push(c, st(c, i));
    }
    if (esc == 1 && reg == 1) {                                 // FXCH ST(i)
        if (empty(c, 0) || empty(c, i)) return false;
        fx x0 = st(c, 0), xi = st(c, i);
        set_st(c, 0, xi);
        set_st(c, i, x0);
        c->fpu_sw &= (uint16_t)~SW_C1;
        return true;
    }
    int m = 0xC0 | reg << 3 | i;
    if (((esc == 5 || esc == 7) && (reg == 2 || reg == 3)) || (esc == 1 && reg == 3)) {   // FST(P) ST(i) + aliases
        if (empty(c, 0)) return false;
        set_st(c, i, st(c, 0));
        if (!(esc == 5 && reg == 2)) pop(c);
        return true;
    }
    if (esc == 1 && (m == 0xE0 || m == 0xE1)) {                 // FCHS, FABS
        if (empty(c, 0)) return false;
        fx v = st(c, 0);
        set_st(c, 0, m == 0xE0 ? fneg(v) : fabs_(v));
        c->fpu_sw &= (uint16_t)~SW_C1;
        return true;
    }
    if (esc == 1 && m >= 0xE8 && m <= 0xEE) {                   // FLD1 ... FLDZ
        if (ptag(c, (top(c) - 1) & 7) != TAG_EMPTY) return false;
        return f_push(c, fpu_const(c, m));
    }
    if (esc == 0 && (reg == 2 || reg == 3)) return f_com_reg(c, i, false, reg == 3);         // FCOM(P) ST(i)
    if (esc == 5 && (reg == 4 || reg == 5)) return f_com_reg(c, i, false, reg == 5);         // FUCOM(P) ST(i)
    if ((esc == 6 && m == 0xD9) || (esc == 2 && m == 0xE9)) return f_com_reg(c, 1, false, 2); // FCOMPP, FUCOMPP
    if ((esc == 3 || esc == 7) && (reg == 5 || reg == 6)) return f_com_reg(c, i, true, esc == 7);   // F(U)COMI(P)
    return false;
}

// Register forms fast_reg can do (it may still decline at run time).
static bool fast_reg_form(int esc, int reg, int i)
{
    int m = 0xC0 | reg << 3 | i;
    if ((esc == 0 || esc == 4 || esc == 6) && reg != 2 && reg != 3) return true;
    if (esc == 1 && reg <= 1) return true;
    if (((esc == 5 || esc == 7) && (reg == 2 || reg == 3)) || (esc == 1 && reg == 3)) return true;
    if (esc == 1 && (m == 0xE0 || m == 0xE1 || (m >= 0xE8 && m <= 0xEE))) return true;
    if ((esc == 0 && (reg == 2 || reg == 3)) || (esc == 5 && (reg == 4 || reg == 5))) return true;
    if ((esc == 6 && m == 0xD9) || (esc == 2 && m == 0xE9)) return true;
    return (esc == 3 || esc == 7) && (reg == 5 || reg == 6);
}

// Memory forms; true if done (with `lean`, false means nothing changed).
static bool fast_mem(cpu_t *c, int esc, int reg, uint32_t lin, bool lean)
{
    if (esc == 0 || esc == 2 || esc == 4 || esc == 6)
        return f_arith_mem(c, esc, (aop_t)reg, lin, lean);
    switch (esc << 3 | reg) {
    case 3 << 3 | 5: {                                          // FLD m80: no conversion, no exceptions
        fx v;
        if (ptag(c, (top(c) - 1) & 7) != TAG_EMPTY) return false;
        return m_rd80(c, lin, lean, &v) && f_push(c, v);
    }
    case 3 << 3 | 7:                                            // FSTP m80
        if (empty(c, 0) || !m_wr80(c, lin, lean, st(c, 0))) return false;
        pop(c);
        return true;
    case 1 << 3 | 0: return f_fld(c, lin, 4, lean);             // FLD m32
    case 5 << 3 | 0: return f_fld(c, lin, 8, lean);             // FLD m64
    case 1 << 3 | 2: return f_fst(c, lin, 4, false, lean);      // FST m32
    case 1 << 3 | 3: return f_fst(c, lin, 4, true, lean);       // FSTP m32
    case 5 << 3 | 2: return f_fst(c, lin, 8, false, lean);      // FST m64
    case 5 << 3 | 3: return f_fst(c, lin, 8, true, lean);       // FSTP m64
    case 7 << 3 | 0: return f_fild(c, lin, 2, lean);            // FILD m16
    case 3 << 3 | 0: return f_fild(c, lin, 4, lean);            // FILD m32
    case 7 << 3 | 5: return f_fild(c, lin, 8, lean);            // FILD m64
    case 7 << 3 | 2: return f_fist(c, lin, 2, false, lean);     // FIST m16
    case 7 << 3 | 3: return f_fist(c, lin, 2, true, lean);      // FISTP m16
    case 3 << 3 | 2: return f_fist(c, lin, 4, false, lean);     // FIST m32
    case 3 << 3 | 3: return f_fist(c, lin, 4, true, lean);      // FISTP m32
    case 7 << 3 | 7: return f_fist(c, lin, 8, true, lean);      // FISTP m64
    default: return false;
    }
}

/* ---- entry points for generated code ---- */
// Common case: every exception masked, nothing pending (#MF), FPU present (#NM).
static bool lean_ok(const cpu_t *c)
{
    return (c->fpu_cw & 0x3F) == 0x3F && !(c->fpu_sw & SW_ES) && !(c->cr0 & (CR0_EM | CR0_TS)) && fast_on();
}

uint32_t fpu_jit_mem(cpu_t *c, uint32_t lin, uint32_t off, uint32_t eip, uint32_t op_seg)
{
    uint16_t op = (uint16_t)op_seg;
    int seg = (int)(op_seg >> 16), esc = op >> 8 & 7, reg = op >> 3 & 7;
    if (esc == 1 && (reg == 5 || reg == 7)) {                   // FLDCW / FNSTCW: control, no pointers
        if (c->cr0 & (CR0_EM | CR0_TS)) return 1;
        uint64_t v;
        if (reg == 7) return m_wr(c, lin, 2, true, c->fpu_cw) ? 0 : 1;
        if (!m_rd(c, lin, 2, true, &v)) return 1;
        c->fpu_cw = (uint16_t)v;
        if (c->fpu_sw & ~c->fpu_cw & 0x3F) c->fpu_sw |= SW_ES | SW_B;
        else c->fpu_sw &= (uint16_t)~(SW_ES | SW_B);
        return 0;
    }
    if (!lean_ok(c) || !fast_mem(c, esc, reg, lin, true)) return 1;
    c->fpu_ip = eip;
    c->fpu_cs = c->seg[S_CS].selector;
    c->fpu_op = op;
    c->fpu_dp = off;
    c->fpu_ds = c->seg[seg].selector;
    return 0;
}

uint32_t fpu_jit_reg(cpu_t *c, uint32_t eip, uint32_t op)
{
    if ((op & 0x7FF) == 0x7E0) {                                // FNSTSW AX: control, no pointers
        if (c->cr0 & (CR0_EM | CR0_TS)) return 1;
        c->gpr[R_EAX] = (c->gpr[R_EAX] & 0xFFFF0000u) | c->fpu_sw;
        return 0;
    }
    if (!lean_ok(c) || !fast_reg(c, op >> 8 & 7, op >> 3 & 7, op & 7)) return 1;
    c->fpu_ip = eip;
    c->fpu_cs = c->seg[S_CS].selector;
    c->fpu_op = (uint16_t)op;
    return 0;
}

bool fpu_jit_handles(const insn_t *in)
{
    int esc = in->opcode & 7, reg = in->reg;
    if (in->mod == 3) return fast_reg_form(esc, reg, in->rm) || (esc == 7 && reg == 4 && in->rm == 0);   // + FNSTSW AX
    if (esc == 0 || esc == 2 || esc == 4 || esc == 6) return true;
    switch (esc << 3 | reg) {
    case 3 << 3 | 5: case 3 << 3 | 7:
    case 1 << 3 | 0: case 5 << 3 | 0: case 1 << 3 | 2: case 1 << 3 | 3: case 5 << 3 | 2: case 5 << 3 | 3:
    case 7 << 3 | 0: case 3 << 3 | 0: case 7 << 3 | 5: case 7 << 3 | 2: case 7 << 3 | 3: case 3 << 3 | 2:
    case 3 << 3 | 3: case 7 << 3 | 7: case 1 << 3 | 5: case 1 << 3 | 7:
        return true;
    default:
        return false;
    }
}

// Non-waiting control instructions: no #MF check, no pointer update.
static bool control_insn(const insn_t *in)
{
    int esc = in->opcode & 7, reg = in->reg;
    if (in->mod != 3) return (esc == 1 && reg >= 4) || (esc == 5 && (reg == 4 || reg == 6 || reg == 7));
    uint8_t m = (uint8_t)(0xC0 | reg << 3 | in->rm);
    return (esc == 3 && m >= 0xE0 && m <= 0xE4) || (esc == 7 && m == 0xE0);
}

// ST(0) op= operand, or ST(i) op= ST(0); `pop` afterwards.
static void binop(op_t_ *o, aop_t op, int dst, fx a, fx b, bool popafter)
{
    fx r;
    if (arith(o, op, a, b, &r)) {
        set_st(o->c, dst, r);
        if (popafter) pop(o->c);
    }
}

__attribute__((noinline)) static uint32_t fpu_exec_general(cpu_t *c, const insn_t *in, uint32_t lin)
{
    if (c->cr0 & (CR0_EM | CR0_TS)) cpu_fault(c, EXC_NM, 0);      // no FPU / lazy context switch
    bool ctl = control_insn(in);
    if (!ctl && (c->fpu_sw & SW_ES) && (c->cr0 & CR0_NE)) cpu_fault(c, EXC_MF, 0);

    int esc = in->opcode & 7, reg = in->reg, i = in->rm;
    bool mem = in->mod != 3;
    if (!ctl) {                                                    // last-instruction pointers
        c->fpu_ip = c->eip;
        c->fpu_cs = c->seg[S_CS].selector;
        c->fpu_op = (uint16_t)(esc << 8 | in->mod << 6 | reg << 3 | in->rm);
        if (mem) { c->fpu_dp = lin - c->seg[in->mem.seg].base; c->fpu_ds = c->seg[in->mem.seg].selector; }
    }
    op_t_ o = { c, 0, 0, 0, false, false };
    fx a, b, v;

    if (mem) {
        switch (esc << 3 | reg) {
        /* D8 / DC / DA / DE: arithmetic with a memory operand */
        case 0 << 3 | 0: case 0 << 3 | 1: case 0 << 3 | 2: case 0 << 3 | 3:
        case 0 << 3 | 4: case 0 << 3 | 5: case 0 << 3 | 6: case 0 << 3 | 7:
        case 4 << 3 | 0: case 4 << 3 | 1: case 4 << 3 | 2: case 4 << 3 | 3:
        case 4 << 3 | 4: case 4 << 3 | 5: case 4 << 3 | 6: case 4 << 3 | 7:
        case 2 << 3 | 0: case 2 << 3 | 1: case 2 << 3 | 2: case 2 << 3 | 3:
        case 2 << 3 | 4: case 2 << 3 | 5: case 2 << 3 | 6: case 2 << 3 | 7:
        case 6 << 3 | 0: case 6 << 3 | 1: case 6 << 3 | 2: case 6 << 3 | 3:
        case 6 << 3 | 4: case 6 << 3 | 5: case 6 << 3 | 6: case 6 << 3 | 7: {
            if (esc == 0 || esc == 4) { if (!load_real(&o, lin, esc == 0 ? 4 : 8, &b, true)) break; }
            else b = load_int(c, lin, esc == 2 ? 4 : 2);
            if (!src(&o, 0, &a)) break;
            if (reg == A_COM || reg == A_COMP) {
                set_compare_cc(&o, empty(c, 0) ? 3 : compare(&o, a, b, false));
                if (masked(c, o.exc & (SW_IE | SW_DE)) && reg == A_COMP) pop(c);
                break;
            }
            if (empty(c, 0)) { set_st(c, 0, INDEFINITE); break; }
            binop(&o, (aop_t)reg, 0, a, b, false);
            break;
        }
        /* D9 */
        case 1 << 3 | 0: if (load_real(&o, lin, 4, &v, false)) push(&o, v); break;    // FLD m32
        case 1 << 3 | 2: case 1 << 3 | 3:                                                // FST(P) m32
            if (!src(&o, 0, &v)) break;
            store_real(&o, lin, 4, v);
            if (reg == 3 && masked(c, o.exc & (SW_IE | SW_OE | SW_UE))) pop(c);
            break;
        case 1 << 3 | 4: load_env(c, in, lin); return 0;                                 // FLDENV
        case 1 << 3 | 5:                                                                 // FLDCW
            c->fpu_cw = (uint16_t)cpu_read_lin(c, lin, 2);
            if (c->fpu_sw & ~c->fpu_cw & 0x3F) c->fpu_sw |= SW_ES | SW_B;
            else c->fpu_sw &= (uint16_t)~(SW_ES | SW_B);
            return 0;
        case 1 << 3 | 6: store_env(c, in, lin); c->fpu_cw |= 0x3F; return 0;            // FNSTENV
        case 1 << 3 | 7: cpu_write_lin(c, lin, c->fpu_cw, 2); return 0;                  // FNSTCW
        /* DB */
        case 3 << 3 | 0: push(&o, load_int(c, lin, 4)); break;                          // FILD m32
        case 3 << 3 | 2: case 3 << 3 | 3:                                                // FIST(P) m32
            if (!src(&o, 0, &v)) break;
            store_int(&o, lin, 4, v);
            if (reg == 3 && masked(c, o.exc & SW_IE)) pop(c);
            break;
        case 3 << 3 | 5: push(&o, rd80(c, lin)); break;                                 // FLD m80 (no conversion)
        case 3 << 3 | 7:                                                                 // FSTP m80
            if (!src(&o, 0, &v)) break;
            wr80(c, lin, v);
            pop(c);
            break;
        /* DD */
        case 5 << 3 | 0: if (load_real(&o, lin, 8, &v, false)) push(&o, v); break;    // FLD m64
        case 5 << 3 | 2: case 5 << 3 | 3:                                                // FST(P) m64
            if (!src(&o, 0, &v)) break;
            store_real(&o, lin, 8, v);
            if (reg == 3 && masked(c, o.exc & (SW_IE | SW_OE | SW_UE))) pop(c);
            break;
        case 5 << 3 | 4: {                                                               // FRSTOR
            uint32_t p = lin + load_env(c, in, lin);
            for (int k = 0; k < 8; k++) {
                fx r = rd80(c, p + 10u * (uint32_t)k);
                int ph = phys(c, k);
                memcpy(c->st[ph], &r.signif, 8);
                c->st[ph][8] = (uint8_t)r.signExp; c->st[ph][9] = (uint8_t)(r.signExp >> 8);
            }
            return 0;
        }
        case 5 << 3 | 6: {                                                               // FNSAVE
            uint32_t p = lin + store_env(c, in, lin);
            for (int k = 0; k < 8; k++) wr80(c, p + 10u * (uint32_t)k, st(c, k));
            fpu_init(c);
            return 0;
        }
        case 5 << 3 | 7: cpu_write_lin(c, lin, c->fpu_sw, 2); return 0;                 // FNSTSW m16
        /* DF */
        case 7 << 3 | 0: push(&o, load_int(c, lin, 2)); break;                          // FILD m16
        case 7 << 3 | 2: case 7 << 3 | 3:                                                // FIST(P) m16
            if (!src(&o, 0, &v)) break;
            store_int(&o, lin, 2, v);
            if (reg == 3 && masked(c, o.exc & SW_IE)) pop(c);
            break;
        case 7 << 3 | 4: op_fbld(&o, lin); break;                                       // FBLD
        case 7 << 3 | 5: push(&o, load_int(c, lin, 8)); break;                          // FILD m64
        case 7 << 3 | 6: op_fbstp(&o, lin); break;                                      // FBSTP
        case 7 << 3 | 7:                                                                 // FISTP m64
            if (!src(&o, 0, &v)) break;
            store_int(&o, lin, 8, v);
            if (masked(c, o.exc & SW_IE)) pop(c);
            break;
        default:
            return JIT_EXIT_UNSUPPORTED;      // FISTTP (SSE3): not on a Pentium III
        }
        commit(&o);
        return 0;
    }

    uint8_t m = (uint8_t)(0xC0 | reg << 3 | i);
    switch (esc) {
    case 0:                                   // D8: ST0 = ST0 op ST(i)
        if (!both(src(&o, 0, &a), src(&o, i, &b))) break;
        if (reg == A_COM || reg == A_COMP) {
            set_compare_cc(&o, empty(c, 0) || empty(c, i) ? 3 : compare(&o, a, b, false));
            if (masked(c, o.exc & (SW_IE | SW_DE)) && reg == A_COMP) pop(c);
            break;
        }
        if (empty(c, 0) || empty(c, i)) { set_st(c, 0, INDEFINITE); break; }
        binop(&o, (aop_t)reg, 0, a, b, false);
        break;
    case 4: case 6: {                         // DC / DE: ST(i) = ST(i) op ST0 (sub/div reversed), DE pops
        bool p = esc == 6;
        if (reg == 2 || reg == 3) {           // FCOM2/FCOMP3 (DC), FCOMP5/FCOMPP (DE)
            if (esc == 6 && reg == 3 && i != 1) return JIT_EXIT_UNSUPPORTED;
            if (!both(src(&o, 0, &a), src(&o, i, &b))) break;
            set_compare_cc(&o, empty(c, 0) || empty(c, i) ? 3 : compare(&o, a, b, false));
            if (!masked(c, o.exc & (SW_IE | SW_DE))) break;
            if (reg == 3 || esc == 6) pop(c);
            if (esc == 6 && reg == 3) pop(c);
            break;
        }
        static const aop_t REV[8] = { A_ADD, A_MUL, A_COM, A_COMP, A_SUBR, A_SUB, A_DIVR, A_DIV };
        if (!both(src(&o, i, &a), src(&o, 0, &b))) break;
        if (empty(c, 0) || empty(c, i)) { set_st(c, i, INDEFINITE); if (p) pop(c); break; }
        binop(&o, REV[reg], i, a, b, p);
        break;
    }
    case 1:                                   // D9
        switch (reg) {
        case 0:                               // FLD ST(i)
            if (!src(&o, i, &v)) break;
            push(&o, v);
            break;
        case 1: {                             // FXCH ST(i)
            fx x0, xi;
            if (!both(src(&o, 0, &x0), src(&o, i, &xi))) break;
            set_st(c, 0, xi);
            set_st(c, i, x0);
            if (!o.exc) set_c1(&o, false);
            break;
        }
        case 2:                               // FNOP (D9 D0)
            if (i) return JIT_EXIT_UNSUPPORTED;
            break;
        case 3:                               // FSTP1 (undocumented alias of FSTP ST(i))
            if (!src(&o, 0, &v)) break;
            set_st(c, i, v);
            pop(c);
            break;
        default:
            switch (m) {
            case 0xE0: case 0xE1:             // FCHS, FABS
                if (!src(&o, 0, &v)) break;
                set_st(c, 0, m == 0xE0 ? fneg(v) : fabs_(v));
                set_c1(&o, false);
                break;
            case 0xE4:                        // FTST
                if (!src(&o, 0, &v)) { break; }
                set_compare_cc(&o, empty(c, 0) ? 3 : compare(&o, v, mk(0, 0), false));
                break;
            case 0xE5: {                      // FXAM
                fx x = st(c, 0);
                uint16_t cc;
                if (empty(c, 0))            cc = SW_C3 | SW_C0;
                else if (is_unsupported(x)) cc = 0;
                else if (is_nan(x))         cc = SW_C0;
                else if (is_inf(x))         cc = SW_C2 | SW_C0;
                else if (is_zero(x))        cc = SW_C3;
                else if (is_denorm(x))      cc = SW_C3 | SW_C2;
                else                        cc = SW_C2;
                if (fsign(x)) cc |= SW_C1;
                o.cc_mask = SW_CC;
                o.cc = cc;
                break;
            }
            case 0xE8: case 0xE9: case 0xEA: case 0xEB: case 0xEC: case 0xED: case 0xEE:
                push(&o, fpu_const(c, m));
                break;
            case 0xF0: op_f2xm1(&o); break;
            case 0xF1: op_yl2x(&o, false); break;
            case 0xF2: op_sincos(&o, 3); break;           // FPTAN
            case 0xF3: op_patan(&o); break;
            case 0xF4: op_xtract(&o); break;
            case 0xF5: op_prem(&o, true); break;          // FPREM1
            case 0xF6: set_top(c, top(c) - 1); set_c1(&o, false); break;   // FDECSTP
            case 0xF7: set_top(c, top(c) + 1); set_c1(&o, false); break;   // FINCSTP
            case 0xF8: op_prem(&o, false); break;         // FPREM
            case 0xF9: op_yl2x(&o, true); break;          // FYL2XP1
            case 0xFA: {                                  // FSQRT
                if (!src(&o, 0, &v)) break;
                if (empty(c, 0)) { set_st(c, 0, INDEFINITE); break; }
                if (!check_operand(&o, v)) { if (masked(c, SW_IE)) set_st(c, 0, INDEFINITE); break; }
                sf_begin(c, true);
                fx r = extF80_sqrt(v);
                uint16_t f = sf_flags();
                if (f & SW_IE) o.exc &= (uint16_t)~SW_DE;
                if (!masked(c, o.exc & SW_DE)) break;
                if ((f & SW_IE) && !masked(c, SW_IE)) { o.exc |= f; break; }
                if (f & SW_PE) {
                    uint8_t rm = softfloat_roundingMode;
                    softfloat_roundingMode = softfloat_round_minMag;
                    fx t = extF80_sqrt(v);
                    softfloat_roundingMode = rm;
                    set_c1(&o, t.signif != r.signif || fexp(t) != fexp(r));
                } else set_c1(&o, false);
                o.exc |= f;
                set_st(c, 0, r);
                break;
            }
            case 0xFB: op_sincos(&o, 2); break;           // FSINCOS
            case 0xFC: {                                  // FRNDINT
                if (!src(&o, 0, &v)) break;
                if (empty(c, 0)) { set_st(c, 0, INDEFINITE); break; }
                if (!check_operand(&o, v)) { if (masked(c, SW_IE)) set_st(c, 0, INDEFINITE); break; }
                if (!masked(c, o.exc & SW_DE)) break;
                sf_begin(c, false);
                fx r = extF80_roundToInt(v, softfloat_roundingMode, true);
                uint16_t f = sf_flags();
                if ((f & SW_IE) && !masked(c, SW_IE)) { o.exc |= f; break; }
                set_c1(&o, (f & SW_PE) && extF80_lt(fabs_(v), fabs_(r)));
                o.exc |= f;
                set_st(c, 0, r);
                break;
            }
            case 0xFD: op_scale(&o); break;
            case 0xFE: op_sincos(&o, 0); break;           // FSIN
            case 0xFF: op_sincos(&o, 1); break;           // FCOS
            default: return JIT_EXIT_UNSUPPORTED;         // D9 E2, E3, E6, E7, EF: invalid
            }
            break;
        }
        break;
    case 2:                                   // DA: FCMOVcc, FUCOMPP
        if (m == 0xE9) {
            if (!both(src(&o, 0, &a), src(&o, 1, &b))) break;
            set_compare_cc(&o, empty(c, 0) || empty(c, 1) ? 3 : compare(&o, a, b, true));
            if (masked(c, o.exc & (SW_IE | SW_DE))) { pop(c); pop(c); }
            break;
        }
        if (reg > 3) return JIT_EXIT_UNSUPPORTED;
        /* fall through */
    case 3:                                   // DB: FCMOVNcc, control, FUCOMI, FCOMI
        if (esc == 3 && reg >= 4) {
            if (m == 0xE3) { fpu_init(c); return 0; }                    // FNINIT
            if (m == 0xE2) { c->fpu_sw &= 0x7F00; return 0; }            // FNCLEX (clears ES, B too)
            if (m >= 0xE0 && m <= 0xE4) return 0;                         // FNENI, FNDISI, FNSETPM: no-ops
            if (reg == 5 || reg == 6) {                                   // FUCOMI, FCOMI
                if (!both(src(&o, 0, &a), src(&o, i, &b))) break;
                int r = empty(c, 0) || empty(c, i) ? 3 : compare(&o, a, b, reg == 5);
                if (masked(c, o.exc & (SW_IE | SW_DE))) set_compare_eflags(c, r);
                set_c1(&o, false);
                break;
            }
            return JIT_EXIT_UNSUPPORTED;
        }
        {   // FCMOVcc: B E BE U (DA) and NB NE NBE NU (DB)
            static const int COND[4] = { 2, 4, 6, 10 };                    // x86 condition codes B, E, BE, P
            int cond = COND[reg & 3] | (esc == 3);
            if (!src(&o, i, &v)) break;
            if (empty(c, 0)) { stack_fault(&o, false); if (masked(c, SW_IE)) set_st(c, 0, INDEFINITE); break; }
            if (cpu_eval_cond(c, cond)) set_st(c, 0, v);
            break;
        }
    case 5:                                   // DD: FFREE, FXCH4, FST, FSTP, FUCOM, FUCOMP
        switch (reg) {
        case 0: set_ptag(c, phys(c, i), TAG_EMPTY); break;                 // FFREE
        case 1: {                                                          // FXCH4 alias
            fx x0, xi;
            if (!both(src(&o, 0, &x0), src(&o, i, &xi))) break;
            set_st(c, 0, xi); set_st(c, i, x0);
            break;
        }
        case 2: case 3:                                                    // FST / FSTP ST(i)
            if (!src(&o, 0, &v)) break;
            set_st(c, i, v);
            if (reg == 3) pop(c);
            break;
        case 4: case 5:                                                    // FUCOM / FUCOMP
            if (!both(src(&o, 0, &a), src(&o, i, &b))) break;
            set_compare_cc(&o, empty(c, 0) || empty(c, i) ? 3 : compare(&o, a, b, true));
            if (reg == 5 && masked(c, o.exc & (SW_IE | SW_DE))) pop(c);
            break;
        default: return JIT_EXIT_UNSUPPORTED;
        }
        break;
    case 7:                                   // DF
        if (m == 0xE0) { c->gpr[R_EAX] = (c->gpr[R_EAX] & 0xFFFF0000u) | c->fpu_sw; return 0; }   // FNSTSW AX
        switch (reg) {
        case 0: set_ptag(c, phys(c, i), TAG_EMPTY); pop(c); break;         // FFREEP
        case 1: {                                                          // FXCH7 alias
            fx x0, xi;
            if (!both(src(&o, 0, &x0), src(&o, i, &xi))) break;
            set_st(c, 0, xi); set_st(c, i, x0);
            break;
        }
        case 2: case 3:                                                    // FSTP8 / FSTP9 aliases
            if (!src(&o, 0, &v)) break;
            set_st(c, i, v);
            pop(c);
            break;
        case 5: case 6: {                                                  // FUCOMIP, FCOMIP
            if (!both(src(&o, 0, &a), src(&o, i, &b))) break;
            int r = empty(c, 0) || empty(c, i) ? 3 : compare(&o, a, b, reg == 5);
            if (masked(c, o.exc & (SW_IE | SW_DE))) { set_compare_eflags(c, r); pop(c); }
            set_c1(&o, false);
            break;
        }
        default: return JIT_EXIT_UNSUPPORTED;
        }
        break;
    }
    commit(&o);
    return 0;
}

// Front end: the instructions BeOS uses most (measured) are done here with the same checks,
// in the same order, as the general code; everything else goes there. Keeping this function
// small matters: the general one has a large frame, and most x87 instructions are cheap.
// (Generated code calls fpu_jit_mem/fpu_jit_reg first; this is for what they decline.)
uint32_t fpu_exec(cpu_t *c, const insn_t *in, uint32_t lin)
{
    if (c->cr0 & (CR0_EM | CR0_TS)) return fpu_exec_general(c, in, lin);
    int esc = in->opcode & 7, reg = in->reg;
    if (in->mod == 3) {
        if (fast_on() && (c->fpu_cw & 0x3F) == 0x3F && !(c->fpu_sw & SW_ES) &&
            fast_reg_form(esc, reg, in->rm)) {                                                 // none of these are control
            c->fpu_ip = c->eip;
            c->fpu_cs = c->seg[S_CS].selector;
            c->fpu_op = (uint16_t)(esc << 8 | in->mod << 6 | reg << 3 | in->rm);
            if (fast_reg(c, esc, reg, in->rm)) return 0;
        }
        return fpu_exec_general(c, in, lin);
    }
    if (esc == 1 && reg == 7) { cpu_write_lin(c, lin, c->fpu_cw, 2); return 0; }            // FNSTCW (no #MF check)
    if (esc == 1 && reg == 5) {                                                             // FLDCW (no #MF check)
        c->fpu_cw = (uint16_t)cpu_read_lin(c, lin, 2);
        if (c->fpu_sw & ~c->fpu_cw & 0x3F) c->fpu_sw |= SW_ES | SW_B;
        else c->fpu_sw &= (uint16_t)~(SW_ES | SW_B);
        return 0;
    }
    if (fast_on() && (c->fpu_cw & 0x3F) == 0x3F && !(c->fpu_sw & SW_ES)) {               // all masked, no #MF due
        c->fpu_ip = c->eip;                                                                 // last-instruction pointers
        c->fpu_cs = c->seg[S_CS].selector;
        c->fpu_op = (uint16_t)(esc << 8 | in->mod << 6 | reg << 3 | in->rm);
        c->fpu_dp = lin - c->seg[in->mem.seg].base;
        c->fpu_ds = c->seg[in->mem.seg].selector;
        if (fast_mem(c, esc, reg, lin, false)) return 0;
    }
    return fpu_exec_general(c, in, lin);
}
