// Instruction forms cross-checked against the host CPU (or Rosetta 2) by
// tests/ref_alu_x86.c (x86-64 reference) and tests/test_alu.c (runs the same
// machine code through the JIT).
//
// Register roles: A = EAX (destination), C = ECX (source, or the shift count in
// CL), D = EDX (SHLD/SHRD source, high half for MUL/DIV, CMPXCHG source).
//
// X(name, "AT&T asm for the reference", 32-bit-mode machine code bytes..., size, flag rule)
#ifndef ALU_FORMS_H
#define ALU_FORMS_H

enum { FL_ALL, FL_LOGIC, FL_SHIFT, FL_SAR, FL_ROT, FL_RCX, FL_SHD, FL_MUL, FL_NONE, FL_CF, FL_ZF, FL_SZPC, FL_SZPCO };

#define B(...) { __VA_ARGS__ }
#define ALU_FORMS(X) \
    X(ADD8,   "addb %%cl,%%al",    B(0x00,0xC8), 1, FL_ALL)     \
    X(ADD16,  "addw %%cx,%%ax",    B(0x66,0x01,0xC8), 2, FL_ALL) \
    X(ADD32,  "addl %%ecx,%%eax",  B(0x01,0xC8), 4, FL_ALL)     \
    X(ADC8,   "adcb %%cl,%%al",    B(0x10,0xC8), 1, FL_ALL)     \
    X(ADC32,  "adcl %%ecx,%%eax",  B(0x11,0xC8), 4, FL_ALL)     \
    X(SUB8,   "subb %%cl,%%al",    B(0x28,0xC8), 1, FL_ALL)     \
    X(SUB16,  "subw %%cx,%%ax",    B(0x66,0x29,0xC8), 2, FL_ALL) \
    X(SUB32,  "subl %%ecx,%%eax",  B(0x29,0xC8), 4, FL_ALL)     \
    X(SBB8,   "sbbb %%cl,%%al",    B(0x18,0xC8), 1, FL_ALL)     \
    X(SBB16,  "sbbw %%cx,%%ax",    B(0x66,0x19,0xC8), 2, FL_ALL) \
    X(SBB32,  "sbbl %%ecx,%%eax",  B(0x19,0xC8), 4, FL_ALL)     \
    X(CMP32,  "cmpl %%ecx,%%eax",  B(0x39,0xC8), 4, FL_ALL)     \
    X(AND8,   "andb %%cl,%%al",    B(0x20,0xC8), 1, FL_LOGIC)   \
    X(OR16,   "orw %%cx,%%ax",     B(0x66,0x09,0xC8), 2, FL_LOGIC) \
    X(XOR32,  "xorl %%ecx,%%eax",  B(0x31,0xC8), 4, FL_LOGIC)   \
    X(INC8,   "incb %%al",         B(0xFE,0xC0), 1, FL_ALL)     \
    X(INC16,  "incw %%ax",         B(0x66,0xFF,0xC0), 2, FL_ALL) \
    X(DEC32,  "decl %%eax",        B(0xFF,0xC8), 4, FL_ALL)     \
    X(NEG8,   "negb %%al",         B(0xF6,0xD8), 1, FL_ALL)     \
    X(NEG32,  "negl %%eax",        B(0xF7,0xD8), 4, FL_ALL)     \
    X(NOT16,  "notw %%ax",         B(0x66,0xF7,0xD0), 2, FL_ALL) \
    X(SHL8,   "shlb %%cl,%%al",    B(0xD2,0xE0), 1, FL_SHIFT)   \
    X(SHL16,  "shlw %%cl,%%ax",    B(0x66,0xD3,0xE0), 2, FL_SHIFT) \
    X(SHL32,  "shll %%cl,%%eax",   B(0xD3,0xE0), 4, FL_SHIFT)   \
    X(SHR8,   "shrb %%cl,%%al",    B(0xD2,0xE8), 1, FL_SHIFT)   \
    X(SHR16,  "shrw %%cl,%%ax",    B(0x66,0xD3,0xE8), 2, FL_SHIFT) \
    X(SHR32,  "shrl %%cl,%%eax",   B(0xD3,0xE8), 4, FL_SHIFT)   \
    X(SAR8,   "sarb %%cl,%%al",    B(0xD2,0xF8), 1, FL_SAR)     \
    X(SAR16,  "sarw %%cl,%%ax",    B(0x66,0xD3,0xF8), 2, FL_SAR) \
    X(SAR32,  "sarl %%cl,%%eax",   B(0xD3,0xF8), 4, FL_SAR)     \
    X(ROL8,   "rolb %%cl,%%al",    B(0xD2,0xC0), 1, FL_ROT)     \
    X(ROL16,  "rolw %%cl,%%ax",    B(0x66,0xD3,0xC0), 2, FL_ROT) \
    X(ROL32,  "roll %%cl,%%eax",   B(0xD3,0xC0), 4, FL_ROT)     \
    X(ROR8,   "rorb %%cl,%%al",    B(0xD2,0xC8), 1, FL_ROT)     \
    X(ROR16,  "rorw %%cl,%%ax",    B(0x66,0xD3,0xC8), 2, FL_ROT) \
    X(ROR32,  "rorl %%cl,%%eax",   B(0xD3,0xC8), 4, FL_ROT)     \
    X(RCL8,   "rclb %%cl,%%al",    B(0xD2,0xD0), 1, FL_RCX)     \
    X(RCL16,  "rclw %%cl,%%ax",    B(0x66,0xD3,0xD0), 2, FL_RCX) \
    X(RCL32,  "rcll %%cl,%%eax",   B(0xD3,0xD0), 4, FL_RCX)     \
    X(RCR8,   "rcrb %%cl,%%al",    B(0xD2,0xD8), 1, FL_RCX)     \
    X(RCR16,  "rcrw %%cl,%%ax",    B(0x66,0xD3,0xD8), 2, FL_RCX) \
    X(RCR32,  "rcrl %%cl,%%eax",   B(0xD3,0xD8), 4, FL_RCX)     \
    X(SHLD16, "shldw %%cl,%%dx,%%ax",   B(0x66,0x0F,0xA5,0xD0), 2, FL_SHD) \
    X(SHLD32, "shldl %%cl,%%edx,%%eax", B(0x0F,0xA5,0xD0), 4, FL_SHD) \
    X(SHRD16, "shrdw %%cl,%%dx,%%ax",   B(0x66,0x0F,0xAD,0xD0), 2, FL_SHD) \
    X(SHRD32, "shrdl %%cl,%%edx,%%eax", B(0x0F,0xAD,0xD0), 4, FL_SHD) \
    X(MUL8,   "mulb %%cl",         B(0xF6,0xE1), 1, FL_MUL)     \
    X(MUL16,  "mulw %%cx",         B(0x66,0xF7,0xE1), 2, FL_MUL) \
    X(MUL32,  "mull %%ecx",        B(0xF7,0xE1), 4, FL_MUL)     \
    X(IMUL8,  "imulb %%cl",        B(0xF6,0xE9), 1, FL_MUL)     \
    X(IMUL16, "imulw %%cx",        B(0x66,0xF7,0xE9), 2, FL_MUL) \
    X(IMUL32, "imull %%ecx",       B(0xF7,0xE9), 4, FL_MUL)     \
    X(IMULR16,"imulw %%cx,%%ax",   B(0x66,0x0F,0xAF,0xC1), 2, FL_MUL) \
    X(IMULR32,"imull %%ecx,%%eax", B(0x0F,0xAF,0xC1), 4, FL_MUL) \
    X(IMUL3B, "imull $-3,%%ecx,%%eax", B(0x6B,0xC1,0xFD), 4, FL_MUL) \
    X(IMUL3D, "imull $0x12345,%%ecx,%%eax", B(0x69,0xC1,0x45,0x23,0x01,0x00), 4, FL_MUL) \
    X(SHL32I, "shll $5,%%eax",     B(0xC1,0xE0,0x05), 4, FL_SZPC) \
    X(SHR32I1,"shrl %%eax",        B(0xD1,0xE8), 4, FL_SZPCO)   \
    X(SAR32I, "sarl $31,%%eax",    B(0xC1,0xF8,0x1F), 4, FL_SZPC) \
    X(SHL8I,  "shlb $3,%%al",      B(0xC0,0xE0,0x03), 1, FL_SZPC) \
    X(SAR16I, "sarw $7,%%ax",      B(0x66,0xC1,0xF8,0x07), 2, FL_SZPC) \
    X(SHR16I, "shrw $15,%%ax",     B(0x66,0xC1,0xE8,0x0F), 2, FL_SZPC) \
    X(MULADC, "mull %%ecx; adcl %%ecx,%%eax", B(0xF7,0xE1,0x11,0xC8), 4, FL_ALL) \
    X(IMULADC,"imull %%ecx,%%eax; adcl %%ecx,%%edx", B(0x0F,0xAF,0xC1,0x11,0xCA), 4, FL_ALL) \
    X(SHLADC, "shll $5,%%eax; adcl %%ecx,%%edx", B(0xC1,0xE0,0x05,0x11,0xCA), 4, FL_ALL) \
    X(SHLCLADC,"shll %%cl,%%eax; adcl %%ecx,%%edx", B(0xD3,0xE0,0x11,0xCA), 4, FL_ALL) \
    X(SARCLJ, "sarl %%cl,%%eax; setc %%dl", B(0xD3,0xF8,0x0F,0x92,0xC2), 4, FL_NONE) \
    X(DIV8,   "divb %%cl",         B(0xF6,0xF1), 1, FL_NONE)    \
    X(DIV16,  "divw %%cx",         B(0x66,0xF7,0xF1), 2, FL_NONE) \
    X(DIV32,  "divl %%ecx",        B(0xF7,0xF1), 4, FL_NONE)    \
    X(IDIV8,  "idivb %%cl",        B(0xF6,0xF9), 1, FL_NONE)    \
    X(IDIV16, "idivw %%cx",        B(0x66,0xF7,0xF9), 2, FL_NONE) \
    X(IDIV32, "idivl %%ecx",       B(0xF7,0xF9), 4, FL_NONE)    \
    X(BT16,   "btw %%cx,%%ax",     B(0x66,0x0F,0xA3,0xC8), 2, FL_CF) \
    X(BT32,   "btl %%ecx,%%eax",   B(0x0F,0xA3,0xC8), 4, FL_CF) \
    X(BTS32,  "btsl %%ecx,%%eax",  B(0x0F,0xAB,0xC8), 4, FL_CF) \
    X(BTR32,  "btrl %%ecx,%%eax",  B(0x0F,0xB3,0xC8), 4, FL_CF) \
    X(BTC16,  "btcw %%cx,%%ax",    B(0x66,0x0F,0xBB,0xC8), 2, FL_CF) \
    X(BSF16,  "bsfw %%cx,%%ax",    B(0x66,0x0F,0xBC,0xC1), 2, FL_ZF) \
    X(BSF32,  "bsfl %%ecx,%%eax",  B(0x0F,0xBC,0xC1), 4, FL_ZF) \
    X(BSR32,  "bsrl %%ecx,%%eax",  B(0x0F,0xBD,0xC1), 4, FL_ZF) \
    X(XADD8,  "xaddb %%cl,%%al",   B(0x0F,0xC0,0xC8), 1, FL_ALL) \
    X(XADD32, "xaddl %%ecx,%%eax", B(0x0F,0xC1,0xC8), 4, FL_ALL) \
    X(CMPXCHG32, "cmpxchgl %%edx,%%ecx", B(0x0F,0xB1,0xD1), 4, FL_ALL)

enum {
#define X(name, ...) F_##name,
    ALU_FORMS(X)
#undef X
    F__COUNT
};

#endif
