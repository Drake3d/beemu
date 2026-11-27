#include "fpu_math.h"
#include <stdbool.h>

// All arithmetic here runs in round-to-nearest; callers save and restore the
// SoftFloat rounding mode and flags around these functions.

static float128_t q_from_i(int64_t v) { return i64_to_f128(v); }
static float128_t q_add(float128_t a, float128_t b) { return f128_add(a, b); }
static float128_t q_sub(float128_t a, float128_t b) { return f128_sub(a, b); }
static float128_t q_mul(float128_t a, float128_t b) { return f128_mul(a, b); }
static float128_t q_div(float128_t a, float128_t b) { return f128_div(a, b); }
static bool       q_neg_p(float128_t a) { return a.v[1] >> 63; }
static float128_t q_neg(float128_t a) { a.v[1] ^= 0x8000000000000000ull; return a; }
static float128_t q_abs(float128_t a) { a.v[1] &= 0x7FFFFFFFFFFFFFFFull; return a; }
static int        q_exp(float128_t a) { return (int)((a.v[1] >> 48) & 0x7FFF) - 16383; }
static bool       q_zero(float128_t a) { return !((a.v[1] << 1) | a.v[0]); }

// x * 2^n (no overflow handling: only used on moderate values)
static float128_t q_ldexp(float128_t a, int n)
{
    if (q_zero(a)) return a;
    a.v[1] += (uint64_t)(int64_t)n << 48;
    return a;
}

// Sum of a series until terms stop mattering at quad precision.
static bool small_rel(float128_t term, float128_t sum)
{
    return q_zero(term) || (!q_zero(sum) && q_exp(term) < q_exp(sum) - 118);
}

// The series stopped at a nonzero term too small to change the sum: move the sum by
// one quad ulp toward that term, so the result is known to be inexact and to lie on
// the right side of sum when it is rounded to the x87's 64-bit significand.
static float128_t nudge(float128_t sum, float128_t term)
{
    if (q_zero(term) || q_zero(sum)) return sum;
    bool up = q_neg_p(term) == q_neg_p(sum);           // the tail increases |sum|
    uint64_t lo = sum.v[0], hi = sum.v[1];
    if (up) { if (++lo == 0) hi++; }
    else    { if (lo-- == 0) hi--; }
    sum.v[0] = lo; sum.v[1] = hi;
    return sum;
}

// atan(x) for |x| <= 1: halve the argument three times, then the Taylor series.
static float128_t q_atan_small(float128_t x)
{
    float128_t one = q_from_i(1);
    int halvings = 0;
    while (q_exp(x) > -4 && halvings < 4) {                // |x| >= 1/16
        x = q_div(x, q_add(one, f128_sqrt(q_add(one, q_mul(x, x)))));
        halvings++;
    }
    float128_t x2 = q_mul(x, x), p = x, sum = x;
    for (int n = 1; n < 200; n++) {
        p = q_mul(p, x2);
        float128_t term = q_div(p, q_from_i(2 * n + 1));
        if (n & 1) term = q_neg(term);
        if (small_rel(term, sum)) { sum = nudge(sum, term); break; }
        sum = q_add(sum, term);
    }
    return q_ldexp(sum, halvings);
}

float128_t q_pi(void)
{
    static bool done;
    static float128_t pi;
    if (!done) {           // Machin: pi = 16 atan(1/5) - 4 atan(1/239)
        float128_t a = q_atan_small(q_div(q_from_i(1), q_from_i(5)));
        float128_t b = q_atan_small(q_div(q_from_i(1), q_from_i(239)));
        pi = q_sub(q_mul(q_from_i(16), a), q_mul(q_from_i(4), b));
        done = true;
    }
    return pi;
}

// 2 atanh(s) = ln((1+s)/(1-s))
static float128_t q_2atanh(float128_t s)
{
    float128_t s2 = q_mul(s, s), p = s, sum = s;
    for (int n = 1; n < 400; n++) {
        p = q_mul(p, s2);
        float128_t term = q_div(p, q_from_i(2 * n + 1));
        if (small_rel(term, sum)) { sum = nudge(sum, term); break; }
        sum = q_add(sum, term);
    }
    return q_ldexp(sum, 1);
}

float128_t q_ln2(void)
{
    static bool done;
    static float128_t ln2;
    if (!done) { ln2 = q_2atanh(q_div(q_from_i(1), q_from_i(3))); done = true; }   // ln 2 = 2 atanh(1/3)
    return ln2;
}

static float128_t sin_series(float128_t r)
{
    float128_t r2 = q_mul(r, r), term = r, sum = r;
    for (int n = 1; n < 60; n++) {
        term = q_neg(q_div(q_mul(term, r2), q_from_i((2 * n) * (2 * n + 1))));
        if (small_rel(term, sum)) { sum = nudge(sum, term); break; }
        sum = q_add(sum, term);
    }
    return sum;
}

static float128_t cos_series(float128_t r)
{
    float128_t r2 = q_mul(r, r), one = q_from_i(1), term = one, sum = one;
    for (int n = 1; n < 60; n++) {
        term = q_neg(q_div(q_mul(term, r2), q_from_i((2 * n - 1) * (2 * n))));
        if (small_rel(term, sum)) { sum = nudge(sum, term); break; }
        sum = q_add(sum, term);
    }
    return sum;
}

void q_sincos(float128_t x, float128_t *s, float128_t *c)
{
    // Intel's reduction constant is pi with a 66-bit significand:
    //   pi66 = N * 2^-66, N = 0xC90FDAA22168C234C (68-bit pattern, low two bits zero)
    // so pi66/2 = N * 2^-67 = P_hi * 2^-32 + L * 2^-67, with P_hi the top 33 bits of
    // P = 0xC90FDAA22168C234 and L = (low 31 bits of P) * 16 + 0xC. Both halves are
    // short enough that k * half is exact in quad precision.
    const uint64_t P = 0xC90FDAA22168C234ull;
    float128_t p_hi = q_ldexp(ui64_to_f128(P >> 31), -32);
    float128_t p_lo = q_ldexp(ui64_to_f128(((P & 0x7FFFFFFFull) << 4) | 0xC), -67);
    float128_t half_pi = q_add(p_hi, p_lo);
    bool neg = q_neg_p(x);
    float128_t ax = q_abs(x);
    // k = nearest integer to |x| / (pi/2)
    float128_t kq = f128_roundToInt(q_div(ax, half_pi), softfloat_round_near_even, false);
    int64_t k = f128_to_i64(kq, softfloat_round_near_even, false);
    // r = |x| - k*hi - k*lo, each product exact (k < 2^64, pieces <= 35 bits)
    float128_t r = f128_mulAdd(q_neg(kq), p_hi, ax);
    r = f128_mulAdd(q_neg(kq), p_lo, r);
    float128_t sr = sin_series(r), cr = cos_series(r);
    float128_t sv, cv;
    switch (k & 3) {
    case 0:  sv = sr;        cv = cr;        break;
    case 1:  sv = cr;        cv = q_neg(sr); break;
    case 2:  sv = q_neg(sr); cv = q_neg(cr); break;
    default: sv = q_neg(cr); cv = sr;        break;
    }
    *s = neg ? q_neg(sv) : sv;
    *c = cv;
}

float128_t q_atan2(float128_t y, float128_t x)
{
    float128_t ay = q_abs(y), ax = q_abs(x), t;
    float128_t pi = q_pi(), half_pi = q_ldexp(pi, -1);
    if (f128_le(ay, ax)) t = q_atan_small(q_div(ay, ax));
    else                 t = q_sub(half_pi, q_atan_small(q_div(ax, ay)));
    if (q_neg_p(x)) t = q_sub(pi, t);
    return q_neg_p(y) ? q_neg(t) : t;
}

float128_t q_exp2m1(float128_t x)
{
    float128_t y = q_mul(x, q_ln2()), term = y, sum = y;   // e^y - 1 = sum y^n / n!
    for (int n = 2; n < 80; n++) {
        term = q_div(q_mul(term, y), q_from_i(n));
        if (small_rel(term, sum)) { sum = nudge(sum, term); break; }
        sum = q_add(sum, term);
    }
    return sum;
}

float128_t q_log2(float128_t x)
{
    // x = m * 2^e with m in [sqrt(1/2), sqrt(2)); ln m = 2 atanh((m-1)/(m+1))
    int bias = 0;
    if (!((x.v[1] >> 48) & 0x7FFF)) { x = q_mul(x, q_ldexp(q_from_i(1), 256)); bias = 256; }   // quad denormal
    int e = q_exp(x);
    float128_t m = q_ldexp(x, -e);
    e -= bias;
    // compare m with sqrt(2) (as a double: exact enough to pick a side of the split)
    if (f128_lt(f64_to_f128((float64_t){ 0x3FF6A09E667F3BCDull }), m)) { m = q_ldexp(m, -1); e++; }
    float128_t one = q_from_i(1);
    float128_t l = q_2atanh(q_div(q_sub(m, one), q_add(m, one)));
    return q_add(q_from_i(e), q_div(l, q_ln2()));
}

float128_t q_log2p1(float128_t x)
{
    float128_t two = q_from_i(2);
    return q_div(q_2atanh(q_div(x, q_add(two, x))), q_ln2());
}
