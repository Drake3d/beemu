// x87 checks.
//  1. Exact values: transcendentals against correctly rounded results computed with
//     60-digit decimal arithmetic (sin/cos/tan reduced with Intel's 66-bit pi like the
//     hardware), in all four rounding modes. Must match bit for bit.
//  2. Cross-check against x87 under Rosetta 2: the vectors from ref_x87_x86 run through
//     the JIT with the same machine code; results, the status bits the manual defines,
//     EFLAGS for FCOMI and memory outputs must match. Rosetta's own transcendentals are
//     not ulp-accurate (its FSIN(-1) is 2 ulps off) and it omits PE for some of them, so
//     for those forms results may differ by 2 ulps and PE/C1 are not compared.
// Part 2 exits 77 (skipped) when the reference can't run.
#include "cpu.h"
#include "flags.h"
#include "decode.h"
#include "fpu.h"
#include "system.h"
#include "jit.h"
#include "mem.h"
#include "x87_forms.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const char *NAMES[] = {
#define X(name, bytes, gen, mask) #name,
    X87_FORMS(X)
#undef X
};
static const uint16_t MASKS[] = {
#define X(name, bytes, gen, mask) mask,
    X87_FORMS(X)
#undef X
};

enum { SW_IE_ = 0x01, SW_ZE_ = 0x04, SW_OE_ = 0x08 };
enum { A_CODE = 0x1000, A_CW = 0x8000, A_B = 0x8010, A_A = 0x8020, A_SW = 0x8030, A_R = 0x8040, A_MEM = 0x8060 };

static void put80(mem_bus_t *bus, uint32_t a, uint16_t se, uint64_t sig)
{
    mem_write(bus, a, (uint32_t)sig, 4);
    mem_write(bus, a + 4, (uint32_t)(sig >> 32), 4);
    mem_write(bus, a + 8, se, 2);
}

static uint64_t get64(mem_bus_t *bus, uint32_t a) { return mem_read(bus, a, 4) | (uint64_t)mem_read(bus, a + 4, 4) << 32; }

// form, rounding control, which result slot (0 = r0, 1 = r1), ST0 (a), ST1 (b), expected result
static const struct { int form, rc, slot; uint16_t ase; uint64_t asig; uint16_t bse; uint64_t bsig; uint16_t rse; uint64_t rsig; } EXACT[] = {
    { F87_FSIN, 0, 0, 0x3fff, 0x8000000000000000ull, 0x0000, 0x0000000000000000ull, 0x3ffe, 0xd76aa47848677021ull },
    { F87_FSIN, 1, 0, 0x3fff, 0x8000000000000000ull, 0x0000, 0x0000000000000000ull, 0x3ffe, 0xd76aa47848677020ull },
    { F87_FSIN, 2, 0, 0x3fff, 0x8000000000000000ull, 0x0000, 0x0000000000000000ull, 0x3ffe, 0xd76aa47848677021ull },
    { F87_FSIN, 3, 0, 0x3fff, 0x8000000000000000ull, 0x0000, 0x0000000000000000ull, 0x3ffe, 0xd76aa47848677020ull },
    { F87_FCOS, 0, 0, 0x3fff, 0x8000000000000000ull, 0x0000, 0x0000000000000000ull, 0x3ffe, 0x8a51407da8345c92ull },
    { F87_FCOS, 1, 0, 0x3fff, 0x8000000000000000ull, 0x0000, 0x0000000000000000ull, 0x3ffe, 0x8a51407da8345c91ull },
    { F87_FCOS, 2, 0, 0x3fff, 0x8000000000000000ull, 0x0000, 0x0000000000000000ull, 0x3ffe, 0x8a51407da8345c92ull },
    { F87_FCOS, 3, 0, 0x3fff, 0x8000000000000000ull, 0x0000, 0x0000000000000000ull, 0x3ffe, 0x8a51407da8345c91ull },
    { F87_FSIN, 0, 0, 0x3ffe, 0xc000000000000000ull, 0x0000, 0x0000000000000000ull, 0x3ffe, 0xae7fe0b5fc786b2eull },
    { F87_FSIN, 1, 0, 0x3ffe, 0xc000000000000000ull, 0x0000, 0x0000000000000000ull, 0x3ffe, 0xae7fe0b5fc786b2dull },
    { F87_FSIN, 2, 0, 0x3ffe, 0xc000000000000000ull, 0x0000, 0x0000000000000000ull, 0x3ffe, 0xae7fe0b5fc786b2eull },
    { F87_FSIN, 3, 0, 0x3ffe, 0xc000000000000000ull, 0x0000, 0x0000000000000000ull, 0x3ffe, 0xae7fe0b5fc786b2dull },
    { F87_FSIN, 0, 0, 0x4005, 0xc800000000000000ull, 0x0000, 0x0000000000000000ull, 0xbffe, 0x81a12dbc626dc036ull },
    { F87_FSIN, 1, 0, 0x4005, 0xc800000000000000ull, 0x0000, 0x0000000000000000ull, 0xbffe, 0x81a12dbc626dc037ull },
    { F87_FSIN, 2, 0, 0x4005, 0xc800000000000000ull, 0x0000, 0x0000000000000000ull, 0xbffe, 0x81a12dbc626dc036ull },
    { F87_FSIN, 3, 0, 0x4005, 0xc800000000000000ull, 0x0000, 0x0000000000000000ull, 0xbffe, 0x81a12dbc626dc036ull },
    { F87_FCOS, 0, 0, 0xc000, 0xc000000000000000ull, 0x0000, 0x0000000000000000ull, 0xbffe, 0xfd7025f42f2e9308ull },
    { F87_FCOS, 1, 0, 0xc000, 0xc000000000000000ull, 0x0000, 0x0000000000000000ull, 0xbffe, 0xfd7025f42f2e9308ull },
    { F87_FCOS, 2, 0, 0xc000, 0xc000000000000000ull, 0x0000, 0x0000000000000000ull, 0xbffe, 0xfd7025f42f2e9307ull },
    { F87_FCOS, 3, 0, 0xc000, 0xc000000000000000ull, 0x0000, 0x0000000000000000ull, 0xbffe, 0xfd7025f42f2e9307ull },
    { F87_FPTAN, 0, 1, 0x3ffe, 0x8000000000000000ull, 0x0000, 0x0000000000000000ull, 0x3ffe, 0x8bda7adf9a3a5219ull },
    { F87_FPTAN, 1, 1, 0x3ffe, 0x8000000000000000ull, 0x0000, 0x0000000000000000ull, 0x3ffe, 0x8bda7adf9a3a5218ull },
    { F87_FPTAN, 2, 1, 0x3ffe, 0x8000000000000000ull, 0x0000, 0x0000000000000000ull, 0x3ffe, 0x8bda7adf9a3a5219ull },
    { F87_FPTAN, 3, 1, 0x3ffe, 0x8000000000000000ull, 0x0000, 0x0000000000000000ull, 0x3ffe, 0x8bda7adf9a3a5218ull },
    { F87_FPATAN, 0, 0, 0x3fff, 0x8000000000000000ull, 0x3fff, 0x8000000000000000ull, 0x3ffe, 0xc90fdaa22168c235ull },
    { F87_FPATAN, 1, 0, 0x3fff, 0x8000000000000000ull, 0x3fff, 0x8000000000000000ull, 0x3ffe, 0xc90fdaa22168c234ull },
    { F87_FPATAN, 2, 0, 0x3fff, 0x8000000000000000ull, 0x3fff, 0x8000000000000000ull, 0x3ffe, 0xc90fdaa22168c235ull },
    { F87_FPATAN, 3, 0, 0x3fff, 0x8000000000000000ull, 0x3fff, 0x8000000000000000ull, 0x3ffe, 0xc90fdaa22168c234ull },
    { F87_FPATAN, 0, 0, 0xc000, 0x8000000000000000ull, 0x3fff, 0x8000000000000000ull, 0x4000, 0xab63739cbfad72ccull },
    { F87_FPATAN, 1, 0, 0xc000, 0x8000000000000000ull, 0x3fff, 0x8000000000000000ull, 0x4000, 0xab63739cbfad72ccull },
    { F87_FPATAN, 2, 0, 0xc000, 0x8000000000000000ull, 0x3fff, 0x8000000000000000ull, 0x4000, 0xab63739cbfad72cdull },
    { F87_FPATAN, 3, 0, 0xc000, 0x8000000000000000ull, 0x3fff, 0x8000000000000000ull, 0x4000, 0xab63739cbfad72ccull },
    { F87_F2XM1, 0, 0, 0x3ffe, 0x8000000000000000ull, 0x0000, 0x0000000000000000ull, 0x3ffd, 0xd413cccfe7799211ull },
    { F87_F2XM1, 1, 0, 0x3ffe, 0x8000000000000000ull, 0x0000, 0x0000000000000000ull, 0x3ffd, 0xd413cccfe7799211ull },
    { F87_F2XM1, 2, 0, 0x3ffe, 0x8000000000000000ull, 0x0000, 0x0000000000000000ull, 0x3ffd, 0xd413cccfe7799212ull },
    { F87_F2XM1, 3, 0, 0x3ffe, 0x8000000000000000ull, 0x0000, 0x0000000000000000ull, 0x3ffd, 0xd413cccfe7799211ull },
    { F87_F2XM1, 0, 0, 0xbffd, 0x8000000000000000ull, 0x0000, 0x0000000000000000ull, 0xbffc, 0xa2ec0cd4a58a542full },
    { F87_F2XM1, 1, 0, 0xbffd, 0x8000000000000000ull, 0x0000, 0x0000000000000000ull, 0xbffc, 0xa2ec0cd4a58a5430ull },
    { F87_F2XM1, 2, 0, 0xbffd, 0x8000000000000000ull, 0x0000, 0x0000000000000000ull, 0xbffc, 0xa2ec0cd4a58a542full },
    { F87_F2XM1, 3, 0, 0xbffd, 0x8000000000000000ull, 0x0000, 0x0000000000000000ull, 0xbffc, 0xa2ec0cd4a58a542full },
    { F87_FYL2X, 0, 0, 0x4000, 0xc000000000000000ull, 0x3fff, 0x8000000000000000ull, 0x3fff, 0xcae00d1cfdeb43d0ull },
    { F87_FYL2X, 1, 0, 0x4000, 0xc000000000000000ull, 0x3fff, 0x8000000000000000ull, 0x3fff, 0xcae00d1cfdeb43cfull },
    { F87_FYL2X, 2, 0, 0x4000, 0xc000000000000000ull, 0x3fff, 0x8000000000000000ull, 0x3fff, 0xcae00d1cfdeb43d0ull },
    { F87_FYL2X, 3, 0, 0x4000, 0xc000000000000000ull, 0x3fff, 0x8000000000000000ull, 0x3fff, 0xcae00d1cfdeb43cfull },
    { F87_FYL2X, 0, 0, 0x4002, 0xa000000000000000ull, 0xbffe, 0x8000000000000000ull, 0xbfff, 0xd49a784bcd1b8afeull },
    { F87_FYL2X, 1, 0, 0x4002, 0xa000000000000000ull, 0xbffe, 0x8000000000000000ull, 0xbfff, 0xd49a784bcd1b8affull },
    { F87_FYL2X, 2, 0, 0x4002, 0xa000000000000000ull, 0xbffe, 0x8000000000000000ull, 0xbfff, 0xd49a784bcd1b8afeull },
    { F87_FYL2X, 3, 0, 0x4002, 0xa000000000000000ull, 0xbffe, 0x8000000000000000ull, 0xbfff, 0xd49a784bcd1b8afeull },
    { F87_FYL2XP1, 0, 0, 0x3ffd, 0x8000000000000000ull, 0x3fff, 0x8000000000000000ull, 0x3ffd, 0xa4d3c25e68dc57f2ull },
    { F87_FYL2XP1, 1, 0, 0x3ffd, 0x8000000000000000ull, 0x3fff, 0x8000000000000000ull, 0x3ffd, 0xa4d3c25e68dc57f2ull },
    { F87_FYL2XP1, 2, 0, 0x3ffd, 0x8000000000000000ull, 0x3fff, 0x8000000000000000ull, 0x3ffd, 0xa4d3c25e68dc57f3ull },
    { F87_FYL2XP1, 3, 0, 0x3ffd, 0x8000000000000000ull, 0x3fff, 0x8000000000000000ull, 0x3ffd, 0xa4d3c25e68dc57f2ull },
    { F87_F2XM1, 0, 0, 0x0000, 0x7acb0491ad3b8d2eull, 0x0000, 0x0000000000000000ull, 0x0000, 0x551d180fdd931380ull },
    { F87_F2XM1, 1, 0, 0x0000, 0x7acb0491ad3b8d2eull, 0x0000, 0x0000000000000000ull, 0x0000, 0x551d180fdd931380ull },
    { F87_F2XM1, 2, 0, 0x0000, 0x7acb0491ad3b8d2eull, 0x0000, 0x0000000000000000ull, 0x0000, 0x551d180fdd931381ull },
    { F87_F2XM1, 3, 0, 0x0000, 0x7acb0491ad3b8d2eull, 0x0000, 0x0000000000000000ull, 0x0000, 0x551d180fdd931380ull },
    { F87_F2XM1, 0, 0, 0x8000, 0x59b6da3737f6cdf0ull, 0x0000, 0x0000000000000000ull, 0x8000, 0x3e2f68b3a0d70f3eull },
    { F87_F2XM1, 1, 0, 0x8000, 0x59b6da3737f6cdf0ull, 0x0000, 0x0000000000000000ull, 0x8000, 0x3e2f68b3a0d70f3eull },
    { F87_F2XM1, 2, 0, 0x8000, 0x59b6da3737f6cdf0ull, 0x0000, 0x0000000000000000ull, 0x8000, 0x3e2f68b3a0d70f3dull },
    { F87_F2XM1, 3, 0, 0x8000, 0x59b6da3737f6cdf0ull, 0x0000, 0x0000000000000000ull, 0x8000, 0x3e2f68b3a0d70f3dull },
    { F87_FYL2XP1, 0, 0, 0xbfdb, 0x8d99e0ec4a017d4eull, 0x3fec, 0xc33c30490021e01full, 0xbfc9, 0x9bcc082e819ebb5cull },
    { F87_FYL2XP1, 1, 0, 0xbfdb, 0x8d99e0ec4a017d4eull, 0x3fec, 0xc33c30490021e01full, 0xbfc9, 0x9bcc082e819ebb5cull },
    { F87_FYL2XP1, 2, 0, 0xbfdb, 0x8d99e0ec4a017d4eull, 0x3fec, 0xc33c30490021e01full, 0xbfc9, 0x9bcc082e819ebb5bull },
    { F87_FYL2XP1, 3, 0, 0xbfdb, 0x8d99e0ec4a017d4eull, 0x3fec, 0xc33c30490021e01full, 0xbfc9, 0x9bcc082e819ebb5bull },
    { F87_FPREM1, 0, 0, 0xc023, 0xed5eb8b2540fc4ccull, 0xbfe4, 0x947c3131307f7ed0ull, 0x3fe1, 0x880782f94a31a700ull },
    { F87_FPREM1, 1, 0, 0xc023, 0xed5eb8b2540fc4ccull, 0xbfe4, 0x947c3131307f7ed0ull, 0x3fe1, 0x880782f94a31a700ull },
    { F87_FPREM1, 2, 0, 0xc023, 0xed5eb8b2540fc4ccull, 0xbfe4, 0x947c3131307f7ed0ull, 0x3fe1, 0x880782f94a31a700ull },
    { F87_FPREM1, 3, 0, 0xc023, 0xed5eb8b2540fc4ccull, 0xbfe4, 0x947c3131307f7ed0ull, 0x3fe1, 0x880782f94a31a700ull },
    // FADD of zero-exponent operands (once a shift by 64 inside SoftFloat): signed zeros, denormal sums
    { F87_FADD,   0, 0, 0x0000, 0x0000000000000000ull, 0x0000, 0x0000000000000000ull, 0x0000, 0x0000000000000000ull },
    { F87_FADD,   0, 0, 0x8000, 0x0000000000000000ull, 0x8000, 0x0000000000000000ull, 0x8000, 0x0000000000000000ull },
    { F87_FADD,   1, 0, 0x8000, 0x0000000000000000ull, 0x8000, 0x0000000000000000ull, 0x8000, 0x0000000000000000ull },
    { F87_FADD,   0, 0, 0x0000, 0x0000000000000001ull, 0x0000, 0x0000000000000001ull, 0x0000, 0x0000000000000002ull },
    { F87_FADD,   0, 0, 0x0000, 0x4000000000000000ull, 0x0000, 0x4000000000000000ull, 0x0001, 0x8000000000000000ull },
};

// Same value, or finite normal values of the same sign within a relative 2^-50
// (compared on the integer significands; long double is only a double on this host).
static bool near_equal(uint16_t ase, uint64_t asig, uint16_t bse, uint64_t bsig)
{
    if (ase == bse && asig == bsig) return true;
    if ((ase ^ bse) & 0x8000) return false;
    int ea = ase & 0x7FFF, eb = bse & 0x7FFF;
    if (ea == 0x7FFF || eb == 0x7FFF || ea == 0 || eb == 0) return false;
    if (ea < eb) { int t = ea; ea = eb; eb = t; uint64_t u = asig; asig = bsig; bsig = u; }
    if (ea - eb > 1) return false;
    unsigned __int128 x = (unsigned __int128)asig << (ea - eb), y = bsig;
    unsigned __int128 d = x > y ? x - y : y - x;
    return d <= (x >> 50);
}

static mem_bus_t *g_bus;
static jit_t *g_jit;

// Run one form with the given inputs; outputs as in the reference.
static jit_exit_t run_form(int form, uint16_t cw, uint16_t ase, uint64_t asig, uint16_t bse, uint64_t bsig,
                           uint64_t m0, uint64_t m1, uint32_t host_flags, cpu_t *cpu)
{
    mem_bus_t *bus = g_bus;
    mem_write(bus, A_CW, cw, 2);
    put80(bus, A_A, ase, asig);
    put80(bus, A_B, bse, bsig);
    mem_write(bus, A_MEM, (uint32_t)m0, 4); mem_write(bus, A_MEM + 4, (uint32_t)(m0 >> 32), 4);
    mem_write(bus, A_MEM + 8, (uint32_t)m1, 4); mem_write(bus, A_MEM + 12, (uint32_t)(m1 >> 32), 4);
    mem_write(bus, A_SW, 0, 2);
    for (int i = 0; i < 32; i += 4) mem_write(bus, A_R + (uint32_t)i, 0, 4);
    cpu_init(cpu, 0);
    cpu->mem = bus;
    cpu->cr0 |= CR0_PE | CR0_NE;
    cpu->seg[S_CS].base = 0; cpu->seg[S_CS].flags |= SEG_DB; cpu->seg[S_CS].limit = 0xFFFFFFFFu;
    cpu->seg[S_SS].flags |= SEG_DB;
    cpu->gpr[R_ESP] = 0x9000;
    cpu->gpr[R_ESI] = A_CW; cpu->gpr[R_EBX] = A_B; cpu->gpr[R_ECX] = A_A;
    cpu->gpr[R_EDX] = A_SW; cpu->gpr[R_EAX] = A_R; cpu->gpr[R_EDI] = A_MEM;
    cpu->eflags = (host_flags & ~0x8D5u) | 2u;
    cpu->eip = A_CODE + 64u * (uint32_t)form;
    return jit_run(g_jit, cpu, 50);
}

/* Fast paths vs general code (core/fpu.c): same instruction, same random state, both ways. */
static uint64_t g_x = 0x9E3779B97F4A7C15ull;
static uint64_t xr(void) { g_x ^= g_x << 13; g_x ^= g_x >> 7; g_x ^= g_x << 17; return g_x; }

static void rand80(uint16_t *se, uint64_t *sig)
{
    uint64_t r = xr();
    uint16_t sign = (uint16_t)(r & 0x8000);
    uint64_t m = xr();
    switch (r >> 16 & 15) {
    case 0: *se = sign; *sig = 0; return;                                        // zero
    case 1: *se = (uint16_t)(sign | 0x7FFF); *sig = 0x8000000000000000ull; return; // infinity
    case 2: *se = (uint16_t)(sign | 0x7FFF); *sig = 0x8000000000000000ull | (m >> 2) | 1; return;   // NaN
    case 3: *se = sign; *sig = m >> (r >> 24 & 63); return;                      // denormal / pseudo-denormal
    case 4: *se = (uint16_t)(sign | (1 + (r >> 24 & 0x7FFD))); *sig = m & 0x7FFFFFFFFFFFFFFFull; return;   // unsupported
    default: break;
    }
    static const int centers[] = { 16383 - 126, 16383 + 127, 16383 - 1022, 16383 + 1023, 16383, 16383 + 15, 16383 + 31, 16383 + 63, 16383 - 2, 16383 - 149, 16383 - 1074 };
    int e = centers[(r >> 20) % 11] + (int)(r >> 32 & 7) - 4;
    if ((r >> 40 & 3) == 0) e = 16383 + (int)(r >> 42 & 127) - 64;
    m |= 0x8000000000000000ull;
    switch (r >> 48 & 7) {                                      // rounding-boundary patterns
    case 0: m = (m & ~0xFFFFFFFFFFull) | 0x8000000000ull; break;             // float32 tie
    case 1: m = (m & ~0x7FFull) | 0x400ull; break;                           // float64 tie
    case 2: m |= 0xFFFFFFFFFFull; break;                                     // all ones below float32
    case 3: { int ue = e - 16383; if (ue >= 0 && ue < 62) { uint64_t keep = ~0ull << (63 - ue); m = (m & keep) | (1ull << (62 - ue)); } break; }  // x.5
    case 4: m &= ~0ull << (20 + (r >> 52 & 31)); break;                     // short significand: exact products, ties
    default: break;
    }
    *se = (uint16_t)(sign | e);
    *sig = m;
}

static int tag80(uint16_t se, uint64_t sig)
{
    int e = se & 0x7FFF;
    if (e == 0x7FFF) return 2;
    if (e == 0) return sig ? 2 : 1;
    return (sig >> 63) ? 0 : 2;
}

static long g_jit_cases;
static int g_jit_fails;

static int fast_vs_general(mem_bus_t *bus, long iters)
{
    static const struct { const char *name; uint8_t b[2]; } F[] = {
        { "FLD m32", { 0xD9, 0x07 } }, { "FLD m64", { 0xDD, 0x07 } }, { "FST m32", { 0xD9, 0x17 } },
        { "FSTP m32", { 0xD9, 0x1F } }, { "FST m64", { 0xDD, 0x17 } }, { "FSTP m64", { 0xDD, 0x1F } },
        { "FILD m16", { 0xDF, 0x07 } }, { "FILD m32", { 0xDB, 0x07 } }, { "FILD m64", { 0xDF, 0x2F } },
        { "FIST m16", { 0xDF, 0x17 } }, { "FISTP m16", { 0xDF, 0x1F } }, { "FIST m32", { 0xDB, 0x17 } },
        { "FISTP m32", { 0xDB, 0x1F } }, { "FISTP m64", { 0xDF, 0x3F } },
        { "FLD m80", { 0xDB, 0x2F } }, { "FSTP m80", { 0xDB, 0x3F } },
        { "FCOM m32", { 0xD8, 0x17 } }, { "FCOMP m64", { 0xDC, 0x1F } }, { "FICOM m32", { 0xDA, 0x17 } },
        { "FICOMP m16", { 0xDE, 0x1F } }, { "FLDCW", { 0xD9, 0x2F } }, { "FNSTCW", { 0xD9, 0x3F } },
    };
    // Register forms beyond arithmetic / FLD / FXCH (byte 1 gets ST(i) or'ed in where it has one).
    static const struct { uint8_t b0, b1; bool has_i; } R[] = {
        { 0xDD, 0xD0, true }, { 0xDD, 0xD8, true }, { 0xDF, 0xD0, true }, { 0xDF, 0xD8, true }, { 0xD9, 0xD8, true },
        { 0xD9, 0xE0, false }, { 0xD9, 0xE1, false }, { 0xD9, 0xE8, false }, { 0xD9, 0xE9, false }, { 0xD9, 0xEA, false },
        { 0xD9, 0xEB, false }, { 0xD9, 0xEC, false }, { 0xD9, 0xED, false }, { 0xD9, 0xEE, false },
        { 0xD8, 0xD0, true }, { 0xD8, 0xD8, true }, { 0xDD, 0xE0, true }, { 0xDD, 0xE8, true },
        { 0xDE, 0xD9, false }, { 0xDA, 0xE9, false }, { 0xDB, 0xE8, true }, { 0xDB, 0xF0, true },
        { 0xDF, 0xE8, true }, { 0xDF, 0xF0, true }, { 0xDF, 0xE0, false },
    };
    const int nr = (int)(sizeof R / sizeof R[0]);
    const int nf = (int)(sizeof F / sizeof F[0]);
    const uint32_t A = 0x3000;
    int fails = 0, jfails = 0;
    long jit_ok = 0;
    static cpu_t c0, c1;
    for (long it = 0; it < iters; it++) {
        int f = (int)(xr() % (uint64_t)nf);
        insn_t in;
        uint8_t bytes[16] = { F[f].b[0], F[f].b[1] };
        static const uint8_t AREG[6] = { 0, 1, 4, 5, 6, 7 };
        char name[32];
        snprintf(name, sizeof name, "%s", F[f].name);
        switch (xr() % 5) {                                     // or an arithmetic / register form
        case 4: {
            int k = (int)(xr() % (uint64_t)nr);
            bytes[0] = R[k].b0; bytes[1] = (uint8_t)(R[k].b1 | (R[k].has_i ? (xr() & 7) : 0));
            snprintf(name, sizeof name, "%02X %02X", bytes[0], bytes[1]);
            break;
        }
        case 0: {                                               // ST0 op= m32 / m64 / m32int / m16int
            static const uint8_t ESC[4] = { 0xD8, 0xDC, 0xDA, 0xDE };
            bytes[0] = ESC[xr() % 4]; bytes[1] = (uint8_t)(AREG[xr() % 6] << 3 | 7);
            snprintf(name, sizeof name, "%02X %02X [mem]", bytes[0], bytes[1]);
            break;
        }
        case 1: {                                               // D8 / DC / DE register forms
            static const uint8_t ESC[3] = { 0xD8, 0xDC, 0xDE };
            bytes[0] = ESC[xr() % 3]; bytes[1] = (uint8_t)(0xC0 | AREG[xr() % 6] << 3 | (xr() & 7));
            snprintf(name, sizeof name, "%02X %02X", bytes[0], bytes[1]);
            break;
        }
        case 2:                                                 // FLD ST(i) / FXCH ST(i)
            bytes[0] = 0xD9; bytes[1] = (uint8_t)((xr() & 1 ? 0xC8 : 0xC0) | (xr() & 7));
            snprintf(name, sizeof name, "%02X %02X", bytes[0], bytes[1]);
            break;
        default: break;
        }
        decode_insn(bytes, sizeof bytes, true, &in);
        cpu_init(&c0, 0);
        c0.mem = bus;
        c0.cr0 &= ~(uint32_t)(CR0_EM | CR0_TS | CR0_NE);
        uint64_t r = xr();
        c0.fpu_cw = (uint16_t)(0x0040 | ((r & 7) ? 0x3F : (r >> 3 & 0x3F)) | (r >> 9 & 3) << 8 | (r >> 11 & 3) << 10);
        c0.fpu_sw = (uint16_t)((r >> 13 & 7) << 11 | (r >> 16 & 0x4700) | (r >> 32 & 0x3F));
        if (c0.fpu_sw & ~c0.fpu_cw & 0x3F) c0.fpu_sw |= 0x8080;
        c0.fpu_tw = 0;
        for (int p = 0; p < 8; p++) {
            uint16_t se; uint64_t sig;
            rand80(&se, &sig);
            memcpy(c0.st[p], &sig, 8);
            c0.st[p][8] = (uint8_t)se; c0.st[p][9] = (uint8_t)(se >> 8);
            int t = (xr() % 5 == 0) ? 3 : tag80(se, sig);
            c0.fpu_tw = (uint16_t)(c0.fpu_tw | t << (2 * p));
        }
        // Register arithmetic: sometimes put the operands' exponents a chosen distance apart (the
        // alignment cases of an add: 0, 1, 64 and 128 bits are boundaries), the smaller often an
        // exact power of two (ties and sticky bits at the boundary).
        if (in.mod == 3 && (bytes[0] == 0xD8 || bytes[0] == 0xDC || bytes[0] == 0xDE) && xr() % 2) {
            static const int D[] = { 0, 1, 2, 63, 64, 65, 127, 128, 129 };
            int t0 = (c0.fpu_sw >> 11) & 7, pi = (t0 + in.rm) & 7;
            int big = xr() % 2 ? t0 : pi, small = big == t0 ? pi : t0;
            int eb = (c0.st[big][8] | c0.st[big][9] << 8) & 0x7FFF, es = eb - D[xr() % 9];
            if (small != big && eb > 200 && eb < 0x7F00 && es > 0) {
                uint64_t sig = xr() % 2 ? 0x8000000000000000ull : xr() | 0x8000000000000000ull;
                uint16_t se = (uint16_t)((xr() & 0x8000) | es);
                memcpy(c0.st[small], &sig, 8);
                c0.st[small][8] = (uint8_t)se; c0.st[small][9] = (uint8_t)(se >> 8);
                c0.fpu_tw = (uint16_t)(c0.fpu_tw & ~(3 << (2 * small)));
            }
        }
        // Memory operand: float bits near edges, integers, or anything.
        uint64_t m = xr();
        switch (xr() % 6) {
        case 0: m = (m & 0x807FFFFFull) | (uint64_t)((xr() % 3 == 0) ? (xr() % 2 ? 0xFF : 0) : (1 + xr() % 254)) << 23; break;
        case 1: m = (m & 0x800FFFFFFFFFFFFFull) | (uint64_t)((xr() % 3 == 0) ? (xr() % 2 ? 0x7FF : 0) : (1 + xr() % 2046)) << 52; break;
        case 2: m = (uint64_t)(int64_t)(int32_t)(m >> (xr() % 32)); break;
        case 3: m = xr() % 4 == 0 ? 0x8000000000000000ull : (uint64_t)(int64_t)(int16_t)m; break;
        default: break;
        }
        c0.eip = 0x1234;
        c0.gpr[R_EAX] = (uint32_t)xr();
        c0.eflags = (uint32_t)(xr() & 0x8D5) | 2;
        uint64_t m_hi = xr();
        if (xr() % 2) m_hi = (m_hi & ~0x7FFFull) | (16383 + xr() % 64 - 32);   // plausible m80 exponents
        mem_write(bus, A + 8, (uint32_t)m_hi, 4); mem_write(bus, A + 12, (uint32_t)(m_hi >> 32), 4);
        mem_write(bus, A, (uint32_t)m, 4); mem_write(bus, A + 4, (uint32_t)(m >> 32), 4);
        if (xr() % 8) { cpu_write_lin(&c0, A, cpu_read_lin(&c0, A, 4), 4); }   // usually in the fast TLB (JIT entry needs it)
        static cpu_t c2, orig;
        c2 = c0;
        orig = c0;
        c1 = c0;
        uint8_t mem0[16], mem1[16], mem_in[16];
        mem_read_block(bus, A, mem_in, 16);
        for (int k = 0; k < 2; k++) {
            mem_write_block(bus, A, mem_in, 16);
            fpu_set_fast_paths(k == 0);
            fpu_exec(k == 0 ? &c0 : &c1, &in, A);
            mem_read_block(bus, A, k == 0 ? mem0 : mem1, 16);
        }
        // The JIT entry points: when they accept, the result must equal the general code's.
        uint8_t mem2[16];
        mem_write_block(bus, A, mem_in, 16);
        fpu_set_fast_paths(true);
        fpu_set_fast_paths(true);
        uint32_t fop = (uint32_t)(in.opcode & 7) << 8 | (uint32_t)in.mod << 6 | (uint32_t)in.reg << 3 | (uint32_t)in.rm;
        uint32_t jr = in.mod == 3 ? fpu_jit_reg(&c2, c2.eip, fop)
                                  : fpu_jit_mem(&c2, A, A - c2.seg[in.mem.seg].base, c2.eip, fop | (uint32_t)in.mem.seg << 16);
        mem_read_block(bus, A, mem2, 16);
        if (jr == 0) {
            jit_ok++;
            bool js = memcmp(c2.st, c1.st, sizeof c2.st) == 0 && c2.fpu_sw == c1.fpu_sw && c2.fpu_tw == c1.fpu_tw &&
                      c2.fpu_cw == c1.fpu_cw && memcmp(mem2, mem1, 16) == 0 && c2.fpu_ip == c1.fpu_ip &&
                      c2.gpr[R_EAX] == c1.gpr[R_EAX] && cpu_calc_flags(&c2) == cpu_calc_flags(&c1) &&
                      c2.fpu_cs == c1.fpu_cs && c2.fpu_op == c1.fpu_op &&
                      (in.mod == 3 || (c2.fpu_dp == c1.fpu_dp && c2.fpu_ds == c1.fpu_ds));
            if (!js && jfails++ < 8)
                printf("JIT ENTRY FAIL %-12s cw=%04x sw jit %04x general %04x, tw %04x/%04x, ip %x/%x op %x/%x\n", name,
                       c1.fpu_cw, c2.fpu_sw, c1.fpu_sw, c2.fpu_tw, c1.fpu_tw, c2.fpu_ip, c1.fpu_ip, c2.fpu_op, c1.fpu_op);
        } else if (memcmp(&c2, &orig, sizeof c2) || memcmp(mem2, mem_in, 16)) {
            if (jfails++ < 8) printf("JIT ENTRY FAIL %-12s declined but changed state\n", name);
        }
        // Generated code (inline x87 moves, else the entry points, else helper_exec), from the
        // same state in flat 32-bit protected mode, instruction at the same eip.
        {
            static cpu_t c3;
            c3 = orig;
            c3.ftlb_base = c3.ftlb;
            c3.jc_base = c3.jc;
            c3.cr0 |= CR0_PE;
            c3.seg[S_CS].base = 0; c3.seg[S_CS].flags |= SEG_DB; c3.seg[S_CS].limit = 0xFFFFFFFFu;
            c3.seg[S_SS].flags |= SEG_DB;
            c3.gpr[R_EDI] = A;
            uint8_t code[8] = { bytes[0], bytes[1], 0xF4 };
            mem_write_block(bus, 0x1234, code, 3);
            mem_write_block(bus, A, mem_in, 16);
            c3.eip = 0x1234;
            jit_exit_t x = jit_run(g_jit, &c3, 4);
            uint8_t mem3[16];
            mem_read_block(bus, A, mem3, 16);
            bool ok3 = x == JIT_EXIT_HALT && memcmp(c3.st, c1.st, sizeof c3.st) == 0 && c3.fpu_sw == c1.fpu_sw &&
                       c3.fpu_tw == c1.fpu_tw && c3.fpu_cw == c1.fpu_cw && memcmp(mem3, mem1, 16) == 0 &&
                       c3.fpu_ip == c1.fpu_ip && c3.fpu_cs == c1.fpu_cs && c3.fpu_op == c1.fpu_op &&
                       c3.fpu_dp == c1.fpu_dp && c3.fpu_ds == c1.fpu_ds &&
                       c3.gpr[R_EAX] == c1.gpr[R_EAX] && cpu_calc_flags(&c3) == cpu_calc_flags(&c1);
            g_jit_cases++;
            if (!ok3 && g_jit_fails++ < 8)
                printf("GENERATED CODE FAIL %-12s cw=%04x sw jit %04x general %04x, tw %04x/%04x, ip %x/%x op %x/%x dp %x/%x exit %d\n",
                       name, c1.fpu_cw, c3.fpu_sw, c1.fpu_sw, c3.fpu_tw, c1.fpu_tw, c3.fpu_ip, c1.fpu_ip, c3.fpu_op,
                       c1.fpu_op, c3.fpu_dp, c1.fpu_dp, x);
        }
        bool same = memcmp(c0.st, c1.st, sizeof c0.st) == 0 && c0.fpu_sw == c1.fpu_sw && c0.fpu_tw == c1.fpu_tw &&
                    c0.fpu_cw == c1.fpu_cw && memcmp(mem0, mem1, 16) == 0 && c0.gpr[R_EAX] == c1.gpr[R_EAX] &&
                    cpu_calc_flags(&c0) == cpu_calc_flags(&c1);
        if (!same && fails++ < 8) {
            int t = (c0.fpu_sw >> 11) & 7;   // after; show the old ST0 slot from the input
            (void)t;
            printf("FAST FAIL %-12s cw=%04x sw fast %04x general %04x, tw %04x/%04x, mem %016llx/%016llx\n", name,
                   c1.fpu_cw, c0.fpu_sw, c1.fpu_sw, c0.fpu_tw, c1.fpu_tw,
                   (unsigned long long)(mem0[0] | (uint64_t)mem0[1] << 8 | (uint64_t)mem0[2] << 16 | (uint64_t)mem0[3] << 24 |
                                        (uint64_t)mem0[4] << 32 | (uint64_t)mem0[5] << 40 | (uint64_t)mem0[6] << 48 | (uint64_t)mem0[7] << 56),
                   (unsigned long long)(mem1[0] | (uint64_t)mem1[1] << 8 | (uint64_t)mem1[2] << 16 | (uint64_t)mem1[3] << 24 |
                                        (uint64_t)mem1[4] << 32 | (uint64_t)mem1[5] << 40 | (uint64_t)mem1[6] << 48 | (uint64_t)mem1[7] << 56));
        }
    }
    fpu_set_fast_paths(true);
    printf("fast paths vs general code: %ld random cases, %d differ; JIT entry points took %ld of them, %d differ\n",
           iters, fails, jit_ok, jfails);
    printf("generated code vs general code: %ld cases, %d differ\n", g_jit_cases, g_jit_fails);
    return fails + jfails + g_jit_fails;
}

int main(int argc, char **argv)
{
#if !defined(__aarch64__)
    printf("(skipped: host is not ARM64)\n");
    return 77;
#else
    mem_bus_t *bus = g_bus = mem_create();
    uint8_t *ram = mem_alloc_host(1 << 20);
    mem_map_ram(bus, 0, 1 << 20, ram);
    jit_t *jit = g_jit = jit_create(bus, 8 << 20);
    static const uint8_t PRE[] = { X87_PRE }, POST[] = { X87_POST };
    {
        uint32_t f = 0;
#define X(name, bytes, gen, mask) { static const uint8_t b_[] = bytes; uint32_t a = A_CODE + 64 * f++; \
        mem_write_block(bus, a, PRE, sizeof PRE); a += sizeof PRE; \
        mem_write_block(bus, a, b_, sizeof b_); a += sizeof b_; \
        mem_write_block(bus, a, POST, sizeof POST); a += sizeof POST; mem_write(bus, a, 0xF4, 1); }
        X87_FORMS(X)
#undef X
    }

    /* ---- 1. exact transcendental values ---- */
    cpu_t cpu;
    int exact_fail = 0, n_exact = (int)(sizeof EXACT / sizeof EXACT[0]);
    for (int k = 0; k < n_exact; k++) {
        uint16_t cw = (uint16_t)(0x037F | EXACT[k].rc << 10);
        jit_exit_t x = run_form(EXACT[k].form, cw, EXACT[k].ase, EXACT[k].asig, EXACT[k].bse, EXACT[k].bsig, 0, 0, 0, &cpu);
        uint32_t ra = A_R + 16u * (uint32_t)EXACT[k].slot;
        uint16_t se = (uint16_t)mem_read(bus, ra + 8, 2);
        uint64_t sig = get64(bus, ra);
        if (x != JIT_EXIT_HALT || se != EXACT[k].rse || sig != EXACT[k].rsig) {
            exact_fail++;
            printf("EXACT FAIL %-8s rc=%d a=%04x:%016llx b=%04x:%016llx want %04x:%016llx got %04x:%016llx\n",
                   NAMES[EXACT[k].form], EXACT[k].rc, EXACT[k].ase, (unsigned long long)EXACT[k].asig,
                   EXACT[k].bse, (unsigned long long)EXACT[k].bsig, EXACT[k].rse,
                   (unsigned long long)EXACT[k].rsig, se, (unsigned long long)sig);
        }
    }
    printf("exact values: %d checks, %d failed\n", n_exact, exact_fail);

    // FBSTP of 2^64: the integer conversion returns INT64_MIN (invalid), so the masked
    // response is the packed BCD indefinite (and the magnitude must not be negated as signed).
    {
        jit_exit_t x = run_form(F87_FBSTP, 0x037F, 0x403F, 0x8000000000000000ull, 0, 0, 0, 0, 0, &cpu);
        bool ok = x == JIT_EXIT_HALT && get64(bus, A_MEM) == 0xC000000000000000ull &&
                  mem_read(bus, A_MEM + 8, 2) == 0xFFFF && (mem_read(bus, A_SW, 2) & SW_IE_);
        if (!ok) { exact_fail++; printf("FBSTP of 2^64 did not store the BCD indefinite with IE\n"); }
    }

    if (fast_vs_general(bus, 4000000)) exact_fail++;

    /* ---- 2. cross-check against Rosetta ---- */
    const char *count = argc > 1 ? argv[1] : "2000";
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "%s %s", REF_X87, count);
    FILE *ref = popen(cmd, "r");
    if (!ref) return exact_fail ? 1 : 77;
    long vectors = 0, fails = 0, ulp1 = 0, per[F87__COUNT] = { 0 }, bad[F87__COUNT] = { 0 }, near[F87__COUNT] = { 0 };
    int form;
    unsigned cw, ase, bse, r0se, r1se, sw;
    unsigned long long asig, bsig, m0, m1, r0sig, r1sig, fl, o0, o1;
    while (fscanf(ref, "%d %x %x %llx %x %llx %llx %llx %x %llx %x %llx %x %llx %llx %llx",
                  &form, &cw, &ase, &asig, &bse, &bsig, &m0, &m1, &r0se, &r0sig, &r1se, &r1sig, &sw, &fl, &o0, &o1) == 16) {
        if (form < 0 || form >= F87__COUNT) continue;
        jit_exit_t x = run_form(form, (uint16_t)cw, (uint16_t)ase, asig, (uint16_t)bse, bsig, m0, m1, (uint32_t)fl, &cpu);

        uint16_t got_r0se = (uint16_t)mem_read(bus, A_R + 8, 2), got_r1se = (uint16_t)mem_read(bus, A_R + 24, 2);
        uint64_t got_r0 = get64(bus, A_R), got_r1 = get64(bus, A_R + 16);
        uint16_t got_sw = (uint16_t)mem_read(bus, A_SW, 2);
        uint64_t got_m0 = get64(bus, A_MEM), got_m1 = get64(bus, A_MEM + 8);
        uint32_t got_fl = cpu_calc_flags(&cpu);
        uint16_t mask = MASKS[form];
        bool comi = form == F87_FCOMI || form == F87_FUCOMI || form == F87_FCOMIP;

        bool exact = x == JIT_EXIT_HALT && got_r0se == r0se && got_r0 == r0sig && got_r1se == r1se && got_r1 == r1sig &&
                     ((got_sw ^ sw) & mask) == 0 && got_m0 == o0 && got_m1 == o1 &&
                     (!comi || ((got_fl ^ fl) & (EFL_ZF | EFL_PF | EFL_CF)) == 0);
        // Transcendentals: Rosetta computes some of them in double precision, so only a
        // relative agreement of 2^-50 is required (part 1 checks exact values); PE/C1 not compared.
        // Rosetta's FYL2XP1 is far off for small arguments (log2(1+x) in double precision
        // loses most of x), so for that form only the exception flags are compared.
        // Rosetta also truncates F2XM1 of denormals and reports no flags for them (checked
        // against exact values: ours rounds correctly and flags UE/PE).
        // Rosetta limits found by checking against exact values (ours were right each time):
        //  - F2XM1 of tiny inputs (denormal results): truncated, no UE/PE;
        //  - FYL2X with a zero/infinite multiplier: no DE; FYL2XP1: double-precision log;
        //  - FPREM/FPREM1 with exponent differences beyond a double quotient: wrong remainder.
        bool tiny_in = (ase & 0x7FFF) < 0x3FFF - 64;
        bool lenient = form == F87_FYL2XP1 || (form == F87_F2XM1 && tiny_in);
        uint16_t flag_mask = (form == F87_FYL2XP1 || form == F87_FYL2X || (form == F87_F2XM1 && tiny_in))
                             ? (SW_IE_ | SW_ZE_ | SW_OE_) : 0xFFFF;
        if ((form == F87_FPREM || form == F87_FPREM1) && (int)(ase & 0x7FFF) - (int)(bse & 0x7FFF) > 52 &&
            (ase & 0x7FFF) != 0x7FFF && (bse & 0x7FFF) != 0 && bsig) { per[form]++; continue; }
        // Undefined inputs: F2XM1 needs |x| <= 1 (Rosetta returns -0.5 for -inf, for example),
        // FYL2XP1 |x| < 1 - sqrt(2)/2.
        if (form == F87_FYL2XP1 && (ase & 0x7FFF) >= 0x3FFF && !((ase & 0x7FFF) == 0x7FFF && (asig << 1))) { per[form]++; continue; }
        if (form == F87_F2XM1 && (ase & 0x7FFF) >= 0x3FFF && !((ase & 0x7FFF) == 0x3FFF && asig == 0x8000000000000000ull) &&
            !((ase & 0x7FFF) == 0x7FFF && (asig << 1))) { per[form]++; continue; }
        bool close = !exact && X87_IS_TRANSCENDENTAL(form) && x == JIT_EXIT_HALT &&
                     (lenient || (near_equal(got_r0se, got_r0, (uint16_t)r0se, r0sig) &&
                                  near_equal(got_r1se, got_r1, (uint16_t)r1se, r1sig))) &&
                     ((got_sw ^ sw) & mask & flag_mask & ~(S_C1 | 0x20)) == 0;
        vectors++;
        per[form]++;
        if (close) { ulp1++; near[form]++; continue; }
        if (!exact) {
            fails++;
            if (bad[form]++ < 4)
                printf("FAIL %-9s cw=%04x a=%04x:%016llx b=%04x:%016llx m=%016llx\n"
                       "     want r0=%04x:%016llx r1=%04x:%016llx sw=%04x m=%016llx fl=%03llx\n"
                       "     got  r0=%04x:%016llx r1=%04x:%016llx sw=%04x m=%016llx fl=%03x (mask %04x, exit %d)\n",
                       NAMES[form], cw, ase, asig, bse, bsig, m0, r0se, r0sig, r1se, r1sig, sw & mask, o0,
                       fl & 0x8D5, got_r0se, (unsigned long long)got_r0, got_r1se, (unsigned long long)got_r1,
                       got_sw & mask, (unsigned long long)got_m0, got_fl & 0x8D5, mask, x);
        }
    }
    int status = pclose(ref);
    if (!vectors) { printf("(skipped: the x87 reference produced no output, status %d)\n", status); return exact_fail ? 1 : 77; }
    for (int f = 0; f < F87__COUNT; f++) {
        if (!per[f]) { printf("FAIL: no vectors for %s\n", NAMES[f]); fails++; continue; }
        if (bad[f] || near[f]) printf("%-9s %5ld vectors: %ld mismatches, %ld within 2^-50\n", NAMES[f], per[f], bad[f], near[f]);
    }
    jit_destroy(jit);
    mem_free_host(ram);
    mem_destroy(bus);
    printf("%ld vectors over %d forms, %ld failed, %ld transcendental results within 2^-50\n",
           vectors, F87__COUNT, fails, ulp1);
    return (fails || exact_fail) ? 1 : 0;
#endif
}
