// Reference x87 results for tests/test_x87.c, computed by running each form's
// machine code on x86 (under Rosetta 2 on Apple Silicon). See tests/x87_forms.h.
// Output per vector (hex):
//   form cw a_se a_sig b_se b_sig mem0 mem1 -> r0_se r0_sig r1_se r1_sig sw eflags mem0 mem1
#include "x87_forms.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#if !defined(__x86_64__)
int main(void) { return 77; }
#else

typedef struct { uint64_t sig; uint16_t se; uint8_t pad[6]; } ext_t;   // 80-bit value + padding to 16

static uint64_t g_rng = 0x2545F4914F6CDD1Dull;
static uint64_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return g_rng; }

static ext_t mk(int sign, int exp, uint64_t sig) { ext_t v = { sig, (uint16_t)((sign ? 0x8000 : 0) | exp), {0} }; return v; }

static ext_t special(void)
{
    int s = (int)(rnd() & 1);
    switch (rnd() % 10) {
    case 0:  return mk(s, 0, 0);                                         // zero
    case 1:  return mk(s, 0x7FFF, 0x8000000000000000ull);                // infinity
    case 2:  return mk(s, 0x7FFF, 0xC000000000000000ull | (rnd() >> 3)); // QNaN
    case 3:  return mk(s, 0x7FFF, 0x8000000000000000ull | ((rnd() >> 3) | 1)); // SNaN
    case 4:  return mk(s, 0, rnd() >> 1);                                // denormal
    case 5:  return mk(s, 0x7FFE, rnd() | 0x8000000000000000ull);        // near overflow
    case 6:  return mk(s, 1, rnd() | 0x8000000000000000ull);             // smallest normals
    case 7:  return mk(s, 0x3FFF, 0x8000000000000000ull);                // +-1
    case 8:  return mk(s, 0x3FFF + (int)(rnd() % 8), (rnd() & 0xFF00000000000000ull) | 0x8000000000000000ull);  // short significand
    default: return mk(s, 0x3FFF + (int)(rnd() % 64) - 32, rnd() >> 1);  // unnormal (invalid encoding)
    }
}

static ext_t normal(int lo, int hi)
{
    return mk((int)(rnd() & 1), 0x3FFF + lo + (int)(rnd() % (uint64_t)(hi - lo + 1)), rnd() | 0x8000000000000000ull);
}

static ext_t any_value(void)
{
    uint64_t r = rnd() % 16;
    if (r < 3) return special();
    if (r < 5) return normal(-16382, 16383);
    return normal(-40, 40);
}

static void gen(int g, ext_t *a, ext_t *b, uint64_t mem[2])
{
    *b = any_value();
    mem[0] = rnd(); mem[1] = rnd();
    switch (g) {
    case G_TRIG:
        *a = (rnd() % 16 == 0) ? special() : (rnd() % 16 == 0) ? normal(40, 70) : normal(-20, 8);
        break;
    case G_UNIT:
        *a = (rnd() % 16 == 0) ? special() : (rnd() % 16 == 0) ? mk((int)(rnd() & 1), 0x3FFF, 0x8000000000000000ull) : normal(-30, -1);
        break;
    case G_POS:
        *a = (rnd() % 16 == 0) ? special() : normal(-60, 60);
        a->se &= 0x7FFF;
        break;
    case G_SMALL:
        *a = (rnd() % 16 == 0) ? special() : normal(-40, -3);
        break;
    case G_INT: {
        *a = (rnd() % 16 == 0) ? special() : normal(-2, 66);
        if (rnd() % 4 == 0) {                                // x.5: rounding ties
            int e = (int)(rnd() % 40) + 1;
            uint64_t sig = (rnd() | 0x8000000000000000ull) & ~((1ull << (62 - e)) - 1);
            sig |= 1ull << (62 - e);
            *a = mk((int)(rnd() & 1), 0x3FFF + e, sig);
        }
        break;
    }
    case G_M32:
        *a = any_value();
        if (rnd() % 8 == 0) { uint32_t sp[] = { 0, 0x80000000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x7F800001u, 0x00000001u, 0x007FFFFFu };
                              mem[0] = (mem[0] & ~0xFFFFFFFFull) | sp[rnd() % 8]; }
        break;
    case G_M64:
        *a = any_value();
        if (rnd() % 8 == 0) { uint64_t sp[] = { 0, 0x8000000000000000ull, 0x7FF0000000000000ull, 0x7FF8000000000000ull,
                                                0x7FF0000000000001ull, 0x0000000000000001ull, 0x000FFFFFFFFFFFFFull, 0x3FF0000000000000ull };
                              mem[0] = sp[rnd() % 8]; }
        break;
    case G_MI16: case G_MI32: case G_MI64:
        *a = any_value();
        if (rnd() % 8 == 0) mem[0] = 0;
        break;
    case G_BCD: {
        *a = any_value();
        uint8_t *p = (uint8_t *)mem;
        memset(mem, 0, 16);
        for (int i = 0; i < 9; i++) p[i] = (uint8_t)((rnd() % 10) << 4 | (rnd() % 10));
        p[9] = (rnd() & 1) ? 0x80 : 0;
        break;
    }
    default:
        *a = any_value();
        if (rnd() % 4 == 0 && (a->se & 0x7FFF) != 0) {       // b close to a: cancellation
            *b = *a;
            b->sig ^= rnd() >> (int)(rnd() % 64);
            b->sig |= 0x8000000000000000ull;
            if (rnd() & 1) b->se ^= 0x8000;
        }
        break;
    }
}

int main(int argc, char **argv)
{
    int per_form = argc > 1 ? atoi(argv[1]) : 2000;
    static const uint8_t PRE[] = { X87_PRE }, POST[] = { X87_POST };
    static const int gens[F87__COUNT] = {
#define X(name, bytes, gen, mask) gen,
        X87_FORMS(X)
#undef X
    };
    static const uint8_t *codes[F87__COUNT];
    static size_t lens[F87__COUNT];
#define X(name, bytes, gen, mask) { static const uint8_t b_[] = bytes; codes[F87_##name] = b_; lens[F87_##name] = sizeof b_; }
    X87_FORMS(X)
#undef X
    uint8_t *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (page == MAP_FAILED) return 77;

    for (int f = 0; f < F87__COUNT; f++) {
        // The same bytes test_x87 runs, plus a ret: esi=&cw ebx=&b ecx=&a edx=&sw eax=r edi=mem.
        size_t n = 0;
        memcpy(page + n, PRE, sizeof PRE); n += sizeof PRE;
        memcpy(page + n, codes[f], lens[f]); n += lens[f];
        memcpy(page + n, POST, sizeof POST); n += sizeof POST;
        page[n] = 0xC3;
        for (int v = 0; v < per_form; v++) {
            ext_t a, b;
            uint64_t mem[2];
            gen(gens[f], &a, &b, mem);
            static const int pcs[3] = { 0, 2, 3 };
            uint16_t cw = (uint16_t)(0x0040 | 0x3F | pcs[rnd() % 3] << 8 | (int)(rnd() % 4) << 10);
            ext_t r[2];
            memset(r, 0, sizeof r);
            uint16_t sw = 0;
            uint64_t memout[2] = { mem[0], mem[1] };
            uint64_t fl;
            __asm__ volatile("call *%[fn]\n\tpushfq\n\tpopq %[fl]"
                             : [fl] "=r"(fl)
                             : [fn] "r"(page), "S"(&cw), "b"(&b), "c"(&a), "d"(&sw), "a"(r), "D"(memout)
                             : "memory", "cc");
            printf("%d %x %x %llx %x %llx %llx %llx %x %llx %x %llx %x %llx %llx %llx\n", f, cw,
                   a.se, (unsigned long long)a.sig, b.se, (unsigned long long)b.sig,
                   (unsigned long long)mem[0], (unsigned long long)mem[1],
                   r[0].se, (unsigned long long)r[0].sig, r[1].se, (unsigned long long)r[1].sig,
                   sw, (unsigned long long)fl, (unsigned long long)memout[0], (unsigned long long)memout[1]);
        }
    }
    return 0;
}
#endif
