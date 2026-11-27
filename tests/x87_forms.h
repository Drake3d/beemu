// x87 instruction forms cross-checked against x87 running under Rosetta 2
// (tests/ref_x87_x86.c) and through beemu's JIT (tests/test_x87.c).
//
// Every vector runs the same machine code in both places (registers hold pointers,
// so the bytes mean the same in 32-bit and 64-bit mode):
//   fninit; fldcw [esi]; fld tword [ebx] (b); fld tword [ecx] (a);
//   <form bytes>; fnstsw [edx]; fstp tword [eax] (r0); fstp tword [eax+16] (r1); fninit
// so before the form ST0 = a and ST1 = b. Memory forms use [edi] (16 bytes, compared too).
//
// X(name, form bytes, input generator, status bits the manual defines for the form)
#ifndef X87_FORMS_H
#define X87_FORMS_H

enum { G_ANY, G_TRIG, G_UNIT, G_POS, G_SMALL, G_INT, G_M32, G_M64, G_MI16, G_MI32, G_MI64, G_BCD, G_NONE };

// Status word masks: exceptions + SF always; C1 (rounding direction / stack) where defined.
#define S_EXC   0x007F
#define S_C1    0x0200
#define S_CC    0x4700         // C0 C1 C2 C3
#define S_ARITH (S_EXC | S_C1)
#define S_TRIG  (S_EXC | S_C1 | 0x0400)
#define S_ALL   (S_EXC | S_CC)

#define XB(...) { __VA_ARGS__ }
#define X87_FORMS(X) \
    X(FADD,    XB(0xD8,0xC1), G_ANY,  S_ARITH) \
    X(FSUB,    XB(0xD8,0xE1), G_ANY,  S_ARITH) \
    X(FSUBR,   XB(0xD8,0xE9), G_ANY,  S_ARITH) \
    X(FMUL,    XB(0xD8,0xC9), G_ANY,  S_ARITH) \
    X(FDIV,    XB(0xD8,0xF1), G_ANY,  S_ARITH) \
    X(FDIVR,   XB(0xD8,0xF9), G_ANY,  S_ARITH) \
    X(FADDP,   XB(0xDE,0xC1), G_ANY,  S_ARITH) \
    X(FSUBP,   XB(0xDE,0xE9), G_ANY,  S_ARITH) \
    X(FSUBRP,  XB(0xDE,0xE1), G_ANY,  S_ARITH) \
    X(FDIVP,   XB(0xDE,0xF9), G_ANY,  S_ARITH) \
    X(FDIVRP,  XB(0xDE,0xF1), G_ANY,  S_ARITH) \
    X(FADD_M32,  XB(0xD8,0x07), G_M32, S_ARITH) \
    X(FMUL_M64,  XB(0xDC,0x0F), G_M64, S_ARITH) \
    X(FDIVR_M64, XB(0xDC,0x3F), G_M64, S_ARITH) \
    X(FIADD_M32, XB(0xDA,0x07), G_MI32, S_ARITH) \
    X(FIDIV_M16, XB(0xDE,0x37), G_MI16, S_ARITH) \
    X(FSQRT,   XB(0xD9,0xFA), G_ANY,  S_ARITH) \
    X(FRNDINT, XB(0xD9,0xFC), G_ANY,  S_ARITH) \
    X(FCHS,    XB(0xD9,0xE0), G_ANY,  S_ARITH) \
    X(FABS,    XB(0xD9,0xE1), G_ANY,  S_ARITH) \
    X(FSCALE,  XB(0xD9,0xFD), G_ANY,  S_ARITH) \
    X(FXTRACT, XB(0xD9,0xF4), G_ANY,  S_ARITH) \
    X(FPREM,   XB(0xD9,0xF8), G_ANY,  S_ALL)   \
    X(FPREM1,  XB(0xD9,0xF5), G_ANY,  S_ALL)   \
    X(FCOM,    XB(0xD8,0xD1), G_ANY,  S_ALL)   \
    X(FCOMP,   XB(0xD8,0xD9), G_ANY,  S_ALL)   \
    X(FUCOM,   XB(0xDD,0xE1), G_ANY,  S_ALL)   \
    X(FUCOMPP, XB(0xDA,0xE9), G_ANY,  S_ALL)   \
    X(FCOMPP,  XB(0xDE,0xD9), G_ANY,  S_ALL)   \
    X(FCOM_M32,  XB(0xD8,0x17), G_M32, S_ALL)  \
    X(FICOM_M32, XB(0xDA,0x17), G_MI32, S_ALL) \
    X(FCOMI,   XB(0xDB,0xF1), G_ANY,  S_EXC | S_C1) \
    X(FUCOMI,  XB(0xDB,0xE9), G_ANY,  S_EXC | S_C1) \
    X(FCOMIP,  XB(0xDF,0xF1), G_ANY,  S_EXC | S_C1) \
    X(FTST,    XB(0xD9,0xE4), G_ANY,  S_ALL)   \
    X(FXAM,    XB(0xD9,0xE5), G_ANY,  S_ALL)   \
    X(FXCH,    XB(0xD9,0xC9), G_ANY,  S_EXC | S_C1) \
    X(FLD_ST1, XB(0xD9,0xC1), G_ANY,  S_EXC | S_C1) \
    X(FST_M32,   XB(0xD9,0x17), G_ANY, S_ARITH) \
    X(FSTP_M64,  XB(0xDD,0x1F), G_ANY, S_ARITH) \
    X(FSTP_M80,  XB(0xDB,0x3F), G_ANY, S_EXC)   \
    X(FIST_M16,  XB(0xDF,0x17), G_INT, S_ARITH) \
    X(FISTP_M32, XB(0xDB,0x1F), G_INT, S_ARITH) \
    X(FISTP_M64, XB(0xDF,0x3F), G_INT, S_ARITH) \
    X(FBSTP,     XB(0xDF,0x37), G_INT, S_EXC)   \
    X(FLD_M32,   XB(0xD9,0x07), G_M32, S_EXC)   \
    X(FLD_M64,   XB(0xDD,0x07), G_M64, S_EXC)   \
    X(FILD_M16,  XB(0xDF,0x07), G_MI16, S_EXC)  \
    X(FILD_M64,  XB(0xDF,0x2F), G_MI64, S_EXC)  \
    X(FBLD,      XB(0xDF,0x27), G_BCD, S_EXC)   \
    X(FLDPI,   XB(0xD9,0xEB), G_NONE, S_EXC)   \
    X(FLDL2T,  XB(0xD9,0xE9), G_NONE, S_EXC)   \
    X(FLDL2E,  XB(0xD9,0xEA), G_NONE, S_EXC)   \
    X(FLDLG2,  XB(0xD9,0xEC), G_NONE, S_EXC)   \
    X(FLDLN2,  XB(0xD9,0xED), G_NONE, S_EXC)   \
    X(F2XM1,   XB(0xD9,0xF0), G_UNIT, S_ARITH) \
    X(FYL2X,   XB(0xD9,0xF1), G_POS,  S_ARITH) \
    X(FYL2XP1, XB(0xD9,0xF9), G_SMALL, S_ARITH) \
    X(FPATAN,  XB(0xD9,0xF3), G_ANY,  S_ARITH) \
    X(FSIN,    XB(0xD9,0xFE), G_TRIG, S_TRIG)  \
    X(FCOS,    XB(0xD9,0xFF), G_TRIG, S_TRIG)  \
    X(FPTAN,   XB(0xD9,0xF2), G_TRIG, S_TRIG)  \
    X(FSINCOS, XB(0xD9,0xFB), G_TRIG, S_TRIG)

// Forms whose results are transcendental approximations (compared with 1-ulp tolerance).
#define X87_IS_TRANSCENDENTAL(f) ((f) == F87_F2XM1 || (f) == F87_FYL2X || (f) == F87_FYL2XP1 || \
    (f) == F87_FPATAN || (f) == F87_FSIN || (f) == F87_FCOS || (f) == F87_FPTAN || (f) == F87_FSINCOS)

enum {
#define X(name, bytes, gen, mask) F87_##name,
    X87_FORMS(X)
#undef X
    F87__COUNT
};

// The scaffolding around each form (see the top of this file).
#define X87_PRE  0xDB,0xE3, 0xD9,0x2E, 0xDB,0x2B, 0xDB,0x29
#define X87_POST 0xDD,0x3A, 0xDB,0x38, 0xDB,0x78,0x10, 0xDB,0xE3

#endif
