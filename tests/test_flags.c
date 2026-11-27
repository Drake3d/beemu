// Lazy-flag tests.
//  1. Hand-checked vectors                          (any host)
//  2. Randomized cross-check against the real CPU's flags (x86-64 hosts only)
//     Each case runs the actual instruction with inline asm, reads CF/PF/AF/ZF/SF/OF
//     back with LAHF+SETO, and compares with cpu_calc_flags() on the same lazy state
//     the JIT will record.
#include "flags.h"
#include <stdio.h>
#include <string.h>

static int g_fail, g_run;
#define CHECK(cond, msg) do { g_run++; if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); g_fail++; } } while (0)

static uint32_t flags_of(cc_op_t op, uint32_t dst, uint32_t s1, uint32_t s2)
{
    cpu_t c;
    memset(&c, 0, sizeof c);
    c.cc_op = op; c.cc_dst = dst; c.cc_src1 = s1; c.cc_src2 = s2;
    return cpu_calc_flags(&c) & EFL_ARITH;
}

static void fixed_vectors(void)
{
    // 8-bit: 0xFF + 1 = 0
    CHECK(flags_of(CC_OP_ADD8, 0x00, 0xFF, 0x01) == (EFL_CF | EFL_PF | EFL_AF | EFL_ZF), "add8 0xff+1");
    // 8-bit: 0x7F + 1 = 0x80  (signed overflow)
    CHECK(flags_of(CC_OP_ADD8, 0x80, 0x7F, 0x01) == (EFL_AF | EFL_SF | EFL_OF), "add8 0x7f+1");
    // 32-bit: 0 - 1
    CHECK(flags_of(CC_OP_SUB32, 0xFFFFFFFFu, 0, 1) == (EFL_CF | EFL_PF | EFL_AF | EFL_SF), "sub32 0-1");
    // 32-bit: 0x80000000 - 1 (signed overflow)
    CHECK(flags_of(CC_OP_SUB32, 0x7FFFFFFFu, 0x80000000u, 1) == (EFL_PF | EFL_AF | EFL_OF), "sub32 min-1");
    // INC8 0x7F with CF=1 preserved
    CHECK(flags_of(CC_OP_INC8, 0x80, 1, 0) == (EFL_CF | EFL_AF | EFL_SF | EFL_OF), "inc8 0x7f cf=1");
    // XOR result 0
    CHECK(flags_of(CC_OP_LOGIC32, 0, 0, 0) == (EFL_PF | EFL_ZF), "logic zero");

    // conditions after cmp 5, 7 (32-bit)
    cpu_t c;
    memset(&c, 0, sizeof c);
    c.cc_op = CC_OP_SUB32; c.cc_dst = (uint32_t)(5 - 7); c.cc_src1 = 5; c.cc_src2 = 7;
    CHECK(cpu_eval_cond(&c, 2),  "cmp 5,7: B");
    CHECK(!cpu_eval_cond(&c, 3), "cmp 5,7: !AE");
    CHECK(!cpu_eval_cond(&c, 4), "cmp 5,7: !E");
    CHECK(cpu_eval_cond(&c, 5),  "cmp 5,7: NE");
    CHECK(cpu_eval_cond(&c, 6),  "cmp 5,7: BE");
    CHECK(!cpu_eval_cond(&c, 7), "cmp 5,7: !A");
    CHECK(cpu_eval_cond(&c, 12), "cmp 5,7: L");
    CHECK(!cpu_eval_cond(&c, 15), "cmp 5,7: !G");

    // materialized flags
    cpu_flush_flags(&c);
    CHECK(c.cc_op == CC_OP_EFLAGS && (c.eflags & EFL_CF) && (c.eflags & EFL_SF), "flush stores flags");
    CHECK(cpu_get_cf(&c) == 1, "get_cf");
}

#if defined(__x86_64__)

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rng(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 16);
}

static const uint32_t EDGE[] = {
    0, 1, 2, 0x0F, 0x10, 0x7F, 0x80, 0x81, 0xFF, 0x7FFF, 0x8000, 0xFFFF,
    0x7FFFFFFFu, 0x80000000u, 0x80000001u, 0xFFFFFFFEu, 0xFFFFFFFFu,
};
#define NEDGE ((int)(sizeof EDGE / sizeof EDGE[0]))
#define NRAND 6000

// k-th test pair: all edge pairs first, then random ones.
static void pair(int k, uint32_t *a, uint32_t *b)
{
    if (k < NEDGE * NEDGE) { *a = EDGE[k / NEDGE]; *b = EDGE[k % NEDGE]; }
    else                   { *a = rng(); *b = (rng() & 3) ? rng() : *a; }
}
#define NPAIRS (NEDGE * NEDGE + NRAND)

// LAHF puts SF ZF AF PF CF in AH; SETO puts OF in AL.
static uint32_t native_flags(uint32_t ax)
{
    uint32_t ah = (ax >> 8) & 0xFF, f = 0;
    if (ah & 0x01) f |= EFL_CF;
    if (ah & 0x04) f |= EFL_PF;
    if (ah & 0x10) f |= EFL_AF;
    if (ah & 0x40) f |= EFL_ZF;
    if (ah & 0x80) f |= EFL_SF;
    if (ax & 1)    f |= EFL_OF;
    return f;
}

static void compare(const char *what, int bits, uint32_t a, uint32_t b, uint32_t res,
                    cc_op_t op, uint32_t s1, uint32_t s2, uint32_t ax, uint32_t mask)
{
    uint32_t got = flags_of(op, res, s1, s2) & mask;
    uint32_t want = native_flags(ax) & mask;
    g_run++;
    if (got != want) {
        if (g_fail < 25)
            printf("FAIL %s%d a=%#x b=%#x res=%#x: ours=%#x hardware=%#x\n", what, bits, a, b, res, got, want);
        g_fail++;
    }
}

/* ---- native binary ops: result flags via LAHF/SETO ---- */
#define NATIVE_BIN(FN, MNEM, SFX, T, PRE) \
    static uint32_t FN(T a, T b, T *res) { \
        unsigned ax; \
        __asm__ volatile(PRE "\n\t" MNEM SFX " %[b], %[a]\n\t" "lahf\n\t" "seto %%al" \
                         : [a] "+r"(a), "=&a"(ax) : [b] "r"(b) : "cc"); \
        *res = a; return ax; }

#define BIN_ALL(NAME, MNEM, PRE) \
    NATIVE_BIN(NAME##8,  MNEM, "b", uint8_t,  PRE) \
    NATIVE_BIN(NAME##16, MNEM, "w", uint16_t, PRE) \
    NATIVE_BIN(NAME##32, MNEM, "l", uint32_t, PRE)

BIN_ALL(n_add,  "add", "clc")
BIN_ALL(n_adc0, "adc", "clc")
BIN_ALL(n_adc1, "adc", "stc")
BIN_ALL(n_sub,  "sub", "clc")
BIN_ALL(n_sbb0, "sbb", "clc")
BIN_ALL(n_sbb1, "sbb", "stc")
BIN_ALL(n_and,  "and", "clc")
BIN_ALL(n_or,   "or",  "clc")
BIN_ALL(n_xor,  "xor", "clc")

/* ---- native unary ops (INC / DEC): CF input must be preserved ---- */
#define NATIVE_UN(FN, MNEM, SFX, T, PRE) \
    static uint32_t FN(T a, T *res) { \
        unsigned ax; \
        __asm__ volatile(PRE "\n\t" MNEM SFX " %[a]\n\t" "lahf\n\t" "seto %%al" \
                         : [a] "+r"(a), "=&a"(ax) : : "cc"); \
        *res = a; return ax; }

#define UN_ALL(NAME, MNEM, PRE) \
    NATIVE_UN(NAME##8,  MNEM, "b", uint8_t,  PRE) \
    NATIVE_UN(NAME##16, MNEM, "w", uint16_t, PRE) \
    NATIVE_UN(NAME##32, MNEM, "l", uint32_t, PRE)

UN_ALL(n_inc0, "inc", "clc")
UN_ALL(n_inc1, "inc", "stc")
UN_ALL(n_dec0, "dec", "clc")
UN_ALL(n_dec1, "dec", "stc")

/* ---- native shifts by CL ---- */
#define NATIVE_SH(FN, MNEM, SFX, T) \
    static uint32_t FN(T a, unsigned cnt, T *res) { \
        unsigned ax; \
        __asm__ volatile(MNEM SFX " %%cl, %[a]\n\t" "lahf\n\t" "seto %%al" \
                         : [a] "+r"(a), "=&a"(ax) : "c"(cnt) : "cc"); \
        *res = a; return ax; }

#define SH_ALL(NAME, MNEM) \
    NATIVE_SH(NAME##8,  MNEM, "b", uint8_t) \
    NATIVE_SH(NAME##16, MNEM, "w", uint16_t) \
    NATIVE_SH(NAME##32, MNEM, "l", uint32_t)

SH_ALL(n_shl, "shl")
SH_ALL(n_shr, "shr")
SH_ALL(n_sar, "sar")

#define MASK_ALL    (EFL_CF | EFL_PF | EFL_AF | EFL_ZF | EFL_SF | EFL_OF)
#define MASK_LOGIC  (EFL_CF | EFL_PF | EFL_ZF | EFL_SF | EFL_OF)     // AF is undefined

#define TEST_BIN(LABEL, FN, T, CC, MASK) do { \
    for (int k = 0; k < NPAIRS; k++) { \
        uint32_t ua, ub; pair(k, &ua, &ub); \
        T a = (T)ua, b = (T)ub, res; \
        uint32_t ax = FN(a, b, &res); \
        compare(LABEL, (int)sizeof(T) * 8, a, b, res, CC, a, b, ax, MASK); \
    } } while (0)

#define TEST_UN(LABEL, FN, T, CC, CIN) do { \
    for (int k = 0; k < NEDGE * NEDGE; k++) { \
        uint32_t ua, ub; pair(k, &ua, &ub); \
        T a = (T)ua, res; \
        uint32_t ax = FN(a, &res); \
        compare(LABEL, (int)sizeof(T) * 8, a, 0, res, CC, CIN, 0, ax, MASK_ALL); \
    } } while (0)

#define TEST_SH(LABEL, FN, T, CC) do { \
    int bits_ = (int)sizeof(T) * 8, maxc_ = bits_ == 32 ? 31 : bits_; \
    for (int k = 0; k < NEDGE * NEDGE + 800; k++) { \
        uint32_t ua, ub; pair(k, &ua, &ub); \
        for (int cnt = 1; cnt <= maxc_; cnt++) { \
            T a = (T)ua, res; \
            uint32_t ax = FN(a, (unsigned)cnt, &res); \
            uint32_t mask = EFL_CF | EFL_PF | EFL_ZF | EFL_SF | (cnt == 1 ? EFL_OF : 0); \
            compare(LABEL, bits_, a, (uint32_t)cnt, res, CC, a, (uint32_t)cnt, ax, mask); \
        } } } while (0)

static int cond_native(uint32_t a, uint32_t b, int cond)
{
    unsigned char r = 0;
#define CC(N, SET) case N: __asm__("cmpl %2, %1\n\t" SET " %0" : "=q"(r) : "r"(a), "r"(b) : "cc"); break;
    switch (cond) {
    CC(0, "seto")  CC(1, "setno") CC(2, "setb")  CC(3, "setae")
    CC(4, "sete")  CC(5, "setne") CC(6, "setbe") CC(7, "seta")
    CC(8, "sets")  CC(9, "setns") CC(10, "setp") CC(11, "setnp")
    CC(12, "setl") CC(13, "setge") CC(14, "setle") CC(15, "setg")
    }
#undef CC
    return r;
}

static void native_tests(void)
{
    TEST_BIN("add", n_add8,  uint8_t,  CC_OP_ADD8,  MASK_ALL);
    TEST_BIN("add", n_add16, uint16_t, CC_OP_ADD16, MASK_ALL);
    TEST_BIN("add", n_add32, uint32_t, CC_OP_ADD32, MASK_ALL);
    TEST_BIN("adc/cf0", n_adc08,  uint8_t,  CC_OP_ADC8,  MASK_ALL);
    TEST_BIN("adc/cf0", n_adc016, uint16_t, CC_OP_ADC16, MASK_ALL);
    TEST_BIN("adc/cf0", n_adc032, uint32_t, CC_OP_ADC32, MASK_ALL);
    TEST_BIN("adc/cf1", n_adc18,  uint8_t,  CC_OP_ADC8,  MASK_ALL);
    TEST_BIN("adc/cf1", n_adc116, uint16_t, CC_OP_ADC16, MASK_ALL);
    TEST_BIN("adc/cf1", n_adc132, uint32_t, CC_OP_ADC32, MASK_ALL);
    TEST_BIN("sub", n_sub8,  uint8_t,  CC_OP_SUB8,  MASK_ALL);
    TEST_BIN("sub", n_sub16, uint16_t, CC_OP_SUB16, MASK_ALL);
    TEST_BIN("sub", n_sub32, uint32_t, CC_OP_SUB32, MASK_ALL);
    TEST_BIN("sbb/cf0", n_sbb08,  uint8_t,  CC_OP_SBB8,  MASK_ALL);
    TEST_BIN("sbb/cf0", n_sbb016, uint16_t, CC_OP_SBB16, MASK_ALL);
    TEST_BIN("sbb/cf0", n_sbb032, uint32_t, CC_OP_SBB32, MASK_ALL);
    TEST_BIN("sbb/cf1", n_sbb18,  uint8_t,  CC_OP_SBB8,  MASK_ALL);
    TEST_BIN("sbb/cf1", n_sbb116, uint16_t, CC_OP_SBB16, MASK_ALL);
    TEST_BIN("sbb/cf1", n_sbb132, uint32_t, CC_OP_SBB32, MASK_ALL);
    TEST_BIN("and", n_and8,  uint8_t,  CC_OP_LOGIC8,  MASK_LOGIC);
    TEST_BIN("and", n_and16, uint16_t, CC_OP_LOGIC16, MASK_LOGIC);
    TEST_BIN("and", n_and32, uint32_t, CC_OP_LOGIC32, MASK_LOGIC);
    TEST_BIN("or",  n_or8,   uint8_t,  CC_OP_LOGIC8,  MASK_LOGIC);
    TEST_BIN("or",  n_or16,  uint16_t, CC_OP_LOGIC16, MASK_LOGIC);
    TEST_BIN("or",  n_or32,  uint32_t, CC_OP_LOGIC32, MASK_LOGIC);
    TEST_BIN("xor", n_xor8,  uint8_t,  CC_OP_LOGIC8,  MASK_LOGIC);
    TEST_BIN("xor", n_xor16, uint16_t, CC_OP_LOGIC16, MASK_LOGIC);
    TEST_BIN("xor", n_xor32, uint32_t, CC_OP_LOGIC32, MASK_LOGIC);

    TEST_UN("inc/cf0", n_inc08,  uint8_t,  CC_OP_INC8,  0);
    TEST_UN("inc/cf0", n_inc016, uint16_t, CC_OP_INC16, 0);
    TEST_UN("inc/cf0", n_inc032, uint32_t, CC_OP_INC32, 0);
    TEST_UN("inc/cf1", n_inc18,  uint8_t,  CC_OP_INC8,  1);
    TEST_UN("inc/cf1", n_inc116, uint16_t, CC_OP_INC16, 1);
    TEST_UN("inc/cf1", n_inc132, uint32_t, CC_OP_INC32, 1);
    TEST_UN("dec/cf0", n_dec08,  uint8_t,  CC_OP_DEC8,  0);
    TEST_UN("dec/cf0", n_dec016, uint16_t, CC_OP_DEC16, 0);
    TEST_UN("dec/cf0", n_dec032, uint32_t, CC_OP_DEC32, 0);
    TEST_UN("dec/cf1", n_dec18,  uint8_t,  CC_OP_DEC8,  1);
    TEST_UN("dec/cf1", n_dec116, uint16_t, CC_OP_DEC16, 1);
    TEST_UN("dec/cf1", n_dec132, uint32_t, CC_OP_DEC32, 1);

    TEST_SH("shl", n_shl8,  uint8_t,  CC_OP_SHL8);
    TEST_SH("shl", n_shl16, uint16_t, CC_OP_SHL16);
    TEST_SH("shl", n_shl32, uint32_t, CC_OP_SHL32);
    TEST_SH("shr", n_shr8,  uint8_t,  CC_OP_SHR8);
    TEST_SH("shr", n_shr16, uint16_t, CC_OP_SHR16);
    TEST_SH("shr", n_shr32, uint32_t, CC_OP_SHR32);
    TEST_SH("sar", n_sar8,  uint8_t,  CC_OP_SAR8);
    TEST_SH("sar", n_sar16, uint16_t, CC_OP_SAR16);
    TEST_SH("sar", n_sar32, uint32_t, CC_OP_SAR32);

    // all 16 condition codes after a 32-bit CMP
    for (int k = 0; k < NPAIRS; k++) {
        uint32_t a, b; pair(k, &a, &b);
        cpu_t c; memset(&c, 0, sizeof c);
        c.cc_op = CC_OP_SUB32; c.cc_dst = a - b; c.cc_src1 = a; c.cc_src2 = b;
        for (int cond = 0; cond < 16; cond++) {
            g_run++;
            if ((int)cpu_eval_cond(&c, cond) != cond_native(a, b, cond)) {
                if (g_fail < 25) printf("FAIL cond %d after cmp %#x,%#x\n", cond, a, b);
                g_fail++;
            }
        }
    }
    printf("(hardware cross-check ran on this x86-64 host)\n");
}
#endif

int main(void)
{
    fixed_vectors();
#if defined(__x86_64__)
    native_tests();
#else
    printf("(hardware cross-check skipped: host is not x86-64)\n");
#endif
    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
