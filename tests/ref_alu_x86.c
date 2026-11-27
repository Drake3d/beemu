// Reference results for tests/test_alu.c, computed by the real x86 instructions.
// Built for x86-64; on Apple Silicon it runs under Rosetta 2.
// Output, one vector per line (hex): form a c d flags_in -> eax ecx edx flags_out
#include "alu_forms.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#if !defined(__x86_64__)
int main(void) { return 77; }
#else

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 16);
}

static const uint32_t SPECIAL[] = {
    0, 1, 2, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF, 0x10000,
    0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFF80, 0xFFFF8000,
};
#define N_SPECIAL (sizeof SPECIAL / sizeof SPECIAL[0])

static uint32_t value(void)
{
    uint32_t r = rnd();
    switch (r & 3) {
    case 0:  return SPECIAL[rnd() % N_SPECIAL];
    case 1:  return rnd() & 0xFF;
    default: return rnd();
    }
}

static void exec(int form, uint32_t *a, uint32_t *c, uint32_t *d, uint64_t *f)
{
    uint32_t A = *a, C = *c, D = *d;
    uint64_t F = *f;
    switch (form) {
#define X(name, txt, bytes, size, rule) \
    case F_##name: \
        __asm__ volatile("pushq %3\n\tpopfq\n\t" txt "\n\tpushfq\n\tpopq %3" \
                         : "+a"(A), "+c"(C), "+d"(D), "+r"(F) : : "cc", "memory"); \
        break;
    ALU_FORMS(X)
#undef X
    }
    *a = A; *c = C; *d = D; *f = F;
}

// Skip inputs that would raise #DE, and SHLD/SHRD counts the manual leaves undefined.
static int skip(int form, uint32_t a, uint32_t c, uint32_t d)
{
    switch (form) {
    case F_DIV8:  { uint32_t v = c & 0xFF;   return !v || (a & 0xFFFF) / v > 0xFF; }
    case F_DIV16: { uint32_t v = c & 0xFFFF; return !v || (((d & 0xFFFF) << 16) | (a & 0xFFFF)) / v > 0xFFFF; }
    case F_DIV32: return !c || ((((uint64_t)d << 32) | a) / c) > 0xFFFFFFFFull;
    case F_IDIV8: {
        int32_t v = (int8_t)c, n = (int16_t)a;
        if (!v) return 1;
        int32_t q = n / v; return q > 127 || q < -128;
    }
    case F_IDIV16: {
        int32_t v = (int16_t)c, n = (int32_t)(((d & 0xFFFF) << 16) | (a & 0xFFFF));
        if (!v || (n == INT32_MIN && v == -1)) return 1;
        int32_t q = n / v; return q > 32767 || q < -32768;
    }
    case F_IDIV32: {
        int64_t v = (int32_t)c, n = (int64_t)(((uint64_t)d << 32) | a);
        if (!v || (n == INT64_MIN && v == -1)) return 1;
        int64_t q = n / v; return q > INT32_MAX || q < INT32_MIN;
    }
    case F_SHLD16: case F_SHRD16: return (c & 31) > 16;
    default: return 0;
    }
}

int main(int argc, char **argv)
{
    int per_form = argc > 1 ? atoi(argv[1]) : 3000;
    for (int form = 0; form < F__COUNT; form++) {
        int is_div = form >= F_DIV8 && form <= F_IDIV32;
        for (int i = 0; i < per_form; i++) {
            uint32_t a = value(), c = value(), d = value();
            if (rnd() & 1) c &= 63;                      // small counts and bit offsets
            if (is_div && (rnd() & 1)) d = rnd() & 3;    // keep many quotients in range
            if (skip(form, a, c, d)) continue;
            uint64_t fin = (rnd() & 0x8D5u) | 2u;        // random arithmetic flags
            uint32_t A = a, C = c, D = d;
            uint64_t F = fin;
            exec(form, &A, &C, &D, &F);
            printf("%d %x %x %x %x %x %x %x %x\n", form, a, c, d, (uint32_t)fin, A, C, D, (uint32_t)F);
        }
    }
    return 0;
}
#endif
