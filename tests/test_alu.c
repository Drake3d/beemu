// Cross-check of instruction semantics against real x86 behavior: runs the
// vectors produced by ref_alu_x86 (x86-64, under Rosetta 2 on Apple Silicon)
// through the JIT and compares registers and every flag the manual defines for
// that case. Exits 77 (skipped) when the reference can't run.
#include "alu_forms.h"
#include "cpu.h"
#include "flags.h"
#include "jit.h"
#include "mem.h"
#include <stdio.h>
#include <string.h>

#define X(name, txt, bytes, size, rule) static const uint8_t code_##name[] = bytes;
ALU_FORMS(X)
#undef X

static const struct { const char *name; const uint8_t *code; int len, size, rule; } FORMS[] = {
#define X(name, txt, bytes, size, rule) { #name, code_##name, (int)sizeof code_##name, size, rule },
    ALU_FORMS(X)
#undef X
};

#define ALL (EFL_CF | EFL_PF | EFL_AF | EFL_ZF | EFL_SF | EFL_OF)

// Flags whose value the Intel manual defines for this form and count.
static uint32_t defined_flags(int rule, int size, uint32_t c)
{
    uint32_t n = c & 31, bits = 8u * (uint32_t)size, of1 = n == 1 ? EFL_OF : 0;
    switch (rule) {
    case FL_ALL:   return ALL;
    case FL_LOGIC: return ALL & ~EFL_AF;
    case FL_SHIFT: return n == 0 ? ALL : EFL_PF | EFL_ZF | EFL_SF | (n < bits ? EFL_CF : 0) | of1;
    case FL_SAR:   return n == 0 ? ALL : EFL_PF | EFL_ZF | EFL_SF | EFL_CF | of1;
    case FL_ROT:   return n == 0 ? ALL : (ALL & ~EFL_OF) | of1;
    case FL_RCX: {
        uint32_t k = size == 4 ? n : n % (bits + 1);
        return k == 0 ? ALL : (ALL & ~EFL_OF) | of1;
    }
    case FL_SHD:   return n == 0 ? ALL : EFL_PF | EFL_ZF | EFL_SF | EFL_CF | of1;
    case FL_MUL:   return EFL_CF | EFL_OF;
    case FL_CF:    return EFL_CF;
    case FL_ZF:    return EFL_ZF;
    case FL_SZPC:  return EFL_PF | EFL_ZF | EFL_SF | EFL_CF;      // immediate count 2..size-1
    case FL_SZPCO: return EFL_PF | EFL_ZF | EFL_SF | EFL_CF | EFL_OF;   // count 1
    default:       return 0;
    }
}

int main(void)
{
#if !defined(__aarch64__)
    printf("(skipped: host is not ARM64)\n");
    return 77;
#else
    FILE *ref = popen(REF_ALU " 3000", "r");
    if (!ref) { printf("(skipped: cannot run %s)\n", REF_ALU); return 77; }

    mem_bus_t *bus = mem_create();
    uint8_t *ram = mem_alloc_host(1 << 20);
    mem_map_ram(bus, 0, 1 << 20, ram);
    jit_t *jit = jit_create(bus, 4 << 20);
    for (int f = 0; f < F__COUNT; f++) {
        mem_write_block(bus, 0x1000 + 16u * (uint32_t)f, FORMS[f].code, (size_t)FORMS[f].len);
        mem_write(bus, 0x1000 + 16u * (uint32_t)f + (uint32_t)FORMS[f].len, 0xF4, 1);   // hlt
    }

    cpu_t cpu;
    long vectors = 0, fails = 0, per_form[F__COUNT] = { 0 }, fail_form[F__COUNT] = { 0 };
    int form;
    uint32_t a, c, d, fin, ea, ec, ed, ef;
    while (fscanf(ref, "%d %x %x %x %x %x %x %x %x", &form, &a, &c, &d, &fin, &ea, &ec, &ed, &ef) == 9) {
        if (form < 0 || form >= F__COUNT) continue;
        cpu_init(&cpu, 0);
        cpu.mem = bus;
        cpu.cr0 |= CR0_PE;
        cpu.seg[S_CS].base = 0; cpu.seg[S_CS].flags |= SEG_DB; cpu.seg[S_CS].limit = 0xFFFFFFFFu;
        cpu.seg[S_SS].flags |= SEG_DB;
        cpu.gpr[R_ESP] = 0x8000;
        cpu.gpr[R_EAX] = a; cpu.gpr[R_ECX] = c; cpu.gpr[R_EDX] = d;
        cpu.eflags = fin | 2u;
        cpu.eip = 0x1000 + 16u * (uint32_t)form;
        jit_exit_t x = jit_run(jit, &cpu, 10);

        uint32_t mask = defined_flags(FORMS[form].rule, FORMS[form].size, c);
        uint32_t got = cpu_calc_flags(&cpu);
        bool eax_defined = !((form == F_BSF16 || form == F_BSF32 || form == F_BSR32) &&
                             (c & (form == F_BSF16 ? 0xFFFFu : 0xFFFFFFFFu)) == 0);
        bool ok = x == JIT_EXIT_HALT && cpu.gpr[R_ECX] == ec && cpu.gpr[R_EDX] == ed &&
                  (!eax_defined || cpu.gpr[R_EAX] == ea) && ((got ^ ef) & mask) == 0;
        vectors++;
        per_form[form]++;
        if (!ok) {
            fails++;
            if (fail_form[form]++ < 3)
                printf("FAIL %-9s a=%08x c=%08x d=%08x f=%03x: want %08x %08x %08x f=%03x, got %08x %08x %08x f=%03x (mask %03x, exit %d)\n",
                       FORMS[form].name, a, c, d, fin, ea, ec, ed, ef & mask,
                       cpu.gpr[R_EAX], cpu.gpr[R_ECX], cpu.gpr[R_EDX], got & mask, mask, x);
        }
    }
    int status = pclose(ref);
    if (vectors == 0) { printf("(skipped: the x86 reference produced no output, status %d)\n", status); return 77; }
    for (int f = 0; f < F__COUNT; f++)
        if (per_form[f] == 0) { printf("FAIL: no vectors for %s\n", FORMS[f].name); fails++; }
        else if (fail_form[f]) printf("%-9s %ld / %ld failed\n", FORMS[f].name, fail_form[f], per_form[f]);

    jit_destroy(jit);
    mem_free_host(ram);
    mem_destroy(bus);
    printf("%ld vectors over %d forms, %ld failed\n", vectors, F__COUNT, fails);
    return fails ? 1 : 0;
#endif
}
