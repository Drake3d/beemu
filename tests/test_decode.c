// Decoder tests: known encodings -> expected disassembly and length.
#include "decode.h"
#include <stdio.h>
#include <string.h>

static int g_fail, g_run;

static void run(const uint8_t *b, size_t len, bool code32, uint32_t eip, const char *expect)
{
    insn_t i;
    char text[128];
    g_run++;
    int n = decode_insn(b, len, code32, &i);
    if (n != (int)len) {
        printf("FAIL len: expected %zu got %d (want \"%s\")\n", len, n, expect);
        g_fail++;
        return;
    }
    insn_format(&i, eip, text, sizeof text);
    if (strcmp(text, expect) != 0) {
        printf("FAIL text: expected \"%s\" got \"%s\"\n", expect, text);
        g_fail++;
    }
}

#define T16(eip, expect, ...) do { const uint8_t b_[] = { __VA_ARGS__ }; run(b_, sizeof b_, false, eip, expect); } while (0)
#define T32(eip, expect, ...) do { const uint8_t b_[] = { __VA_ARGS__ }; run(b_, sizeof b_, true,  eip, expect); } while (0)

int main(void)
{
    // ---- 16-bit real mode (SeaBIOS-style code) ----
    T16(0, "jmpf 0xf000:0xe05b", 0xEA, 0x5B, 0xE0, 0x00, 0xF0);
    T16(0, "xor ax, ax",         0x31, 0xC0);
    T16(0, "mov ds, ax",         0x8E, 0xD8);
    T16(0, "mov ax, 0x1234",     0xB8, 0x34, 0x12);
    T16(0, "mov eax, 0x12345678", 0x66, 0xB8, 0x78, 0x56, 0x34, 0x12);
    T16(0, "cli",                0xFA);
    T16(0, "cld",                0xFC);
    T16(0, "lgdt [ds:0x7c00]",   0x0F, 0x01, 0x16, 0x00, 0x7C);
    T16(0, "mov eax, cr0",       0x0F, 0x20, 0xC0);
    T16(0, "mov cr0, eax",       0x0F, 0x22, 0xC0);
    T16(0x1000, "jmp 0x1000",    0xEB, 0xFE);
    T16(0x100,  "je 0x107",      0x74, 0x05);
    T16(0, "sete al",            0x0F, 0x94, 0xC0);
    T16(0, "mov al, [ds:0x1234]", 0xA0, 0x34, 0x12);
    T16(0, "mov [ds:bx+si+0x10], ax", 0x89, 0x40, 0x10);
    T16(0, "out 0x80, al",       0xE6, 0x80);
    T16(0, "in al, dx",          0xEC);
    T16(0, "mov [ss:bp+0x4], ax", 0x89, 0x46, 0x04);
    T16(0, "int 0x13",           0xCD, 0x13);

    // ---- 32-bit protected mode ----
    T32(0, "mov eax, [ss:esp+0x4]", 0x8B, 0x44, 0x24, 0x04);
    T32(0, "lea eax, [ds:ecx*4]",   0x8D, 0x04, 0x8D, 0x00, 0x00, 0x00, 0x00);
    T32(0, "movzx eax, cl",         0x0F, 0xB6, 0xC1);
    T32(0, "rep movsb",             0xF3, 0xA4);
    T32(0, "repe cmpsd",            0xF3, 0xA7);
    T32(0, "ret",                   0xC3);
    T32(0, "and esp, 0xfffffff0",   0x83, 0xE4, 0xF0);
    T32(0, "call [ds:0x12345678]",  0xFF, 0x15, 0x78, 0x56, 0x34, 0x12);
    T32(0x2000, "call 0x2000",      0xE8, 0xFB, 0xFF, 0xFF, 0xFF);
    T32(0, "jne 0x106",             0x0F, 0x85, 0x00, 0x01, 0x00, 0x00);
    T32(0, "push 0xffffffff",       0x6A, 0xFF);
    T32(0, "cpuid",                 0x0F, 0xA2);
    T32(0, "lock cmpxchg [ds:edx], ecx", 0xF0, 0x0F, 0xB1, 0x0A);
    T32(0, "mov [ds:0x1000], eax",  0x89, 0x05, 0x00, 0x10, 0x00, 0x00);
    T32(0, "mov eax, [ds:ebx+esi*2+0x10]", 0x8B, 0x44, 0x73, 0x10);
    T32(0, "mov eax, [ss:ebp-0x8]", 0x8B, 0x45, 0xF8);
    T32(0, "shl eax, 0x2",          0xC1, 0xE0, 0x02);
    T32(0, "imul eax, ecx, 0x64",   0x6B, 0xC1, 0x64);
    T32(0, "ltr ax",                0x0F, 0x00, 0xD8);
    T32(0, "bswap edx",             0x0F, 0xCA);
    T32(0, "mov [fs:0x0], eax",     0x64, 0x89, 0x05, 0x00, 0x00, 0x00, 0x00);

    // ---- valid-but-unsupported opcodes must still have the right length ----
    T32(0, "fpu",                   0xD9, 0xEE);                 // fldz (x87 escape, interpreted by the helper)
    T32(0, "fpu [ss:ebp+0x8]",      0xDD, 0x45, 0x08);           // fld qword [ebp+8]
    T32(0, "unsupported",           0x0F, 0x6F, 0xC1);           // movq mm0, mm1
    T32(0, "unsupported",           0x0F, 0x70, 0xC1, 0x1B);     // pshufw (has imm8)

    // ---- truncated input must return 0 ----
    {
        const uint8_t b[] = { 0xB8, 0x34 };
        insn_t i;
        g_run++;
        if (decode_insn(b, sizeof b, false, &i) != 0) { printf("FAIL: truncated imm\n"); g_fail++; }
    }
    {
        const uint8_t b[] = { 0x8B };
        insn_t i;
        g_run++;
        if (decode_insn(b, sizeof b, true, &i) != 0) { printf("FAIL: truncated modrm\n"); g_fail++; }
    }

    printf("%d tests, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
