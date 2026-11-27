// Minimal AArch64 instruction encoders for the JIT.
// Only the handful of forms the translator needs; each returns the 32-bit word.
// Registers are 0..30 (Wn / Xn), 31 = WZR in the ALU forms used here.
#ifndef A64_H
#define A64_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t *p;
    uint32_t *end;
    bool      overflow;   // set if the code buffer ran out of room
} emit_t;

static inline void a64_emit(emit_t *e, uint32_t word)
{
    if (e->p < e->end) *e->p++ = word;
    else e->overflow = true;
}

/* ---- encoders ---- */

// MOVZ Wd, #imm16, LSL #(16*hw)
static inline uint32_t a64_enc_movz(int rd, uint16_t imm, int hw)
{ return 0x52800000u | ((uint32_t)hw << 21) | ((uint32_t)imm << 5) | (uint32_t)rd; }

// MOVK Wd, #imm16, LSL #(16*hw)
static inline uint32_t a64_enc_movk(int rd, uint16_t imm, int hw)
{ return 0x72800000u | ((uint32_t)hw << 21) | ((uint32_t)imm << 5) | (uint32_t)rd; }

// LDR Wt, [Xn, #off]   (off: multiple of 4, 0..16380)
static inline uint32_t a64_enc_ldr_w(int rt, int rn, uint32_t off)
{ assert(!(off & 3) && off <= 16380); return 0xB9400000u | ((off >> 2) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt; }

// STR Wt, [Xn, #off]
static inline uint32_t a64_enc_str_w(int rt, int rn, uint32_t off)
{ assert(!(off & 3) && off <= 16380); return 0xB9000000u | ((off >> 2) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt; }

// STRB Wt, [Xn, #off]  (off: 0..4095)
static inline uint32_t a64_enc_strb(int rt, int rn, uint32_t off)
{ assert(off <= 4095); return 0x39000000u | (off << 10) | ((uint32_t)rn << 5) | (uint32_t)rt; }

// Three-register ALU, 32-bit, no shift: Wd = Wn <op> Wm
#define A64_OP_ADD 0x0B000000u
#define A64_OP_SUB 0x4B000000u
#define A64_OP_AND 0x0A000000u
#define A64_OP_ORR 0x2A000000u
#define A64_OP_EOR 0x4A000000u
#define A64_OP_BIC 0x0A200000u
static inline uint32_t a64_enc_alu(uint32_t opbase, int rd, int rn, int rm)
{ return opbase | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd; }

// Same, with the second register shifted left: Wd = Wn <op> (Wm LSL #sh)
static inline uint32_t a64_enc_alu_lsl(uint32_t opbase, int rd, int rn, int rm, int sh)
{ return a64_enc_alu(opbase, rd, rn, rm) | ((uint32_t)sh << 10); }

// MOV Wd, Wm  (ORR Wd, WZR, Wm)
static inline uint32_t a64_enc_mov_w(int rd, int rm)
{ return 0x2A0003E0u | ((uint32_t)rm << 16) | (uint32_t)rd; }

// MVN Wd, Wm  (ORN Wd, WZR, Wm)
static inline uint32_t a64_enc_mvn_w(int rd, int rm)
{ return 0x2A2003E0u | ((uint32_t)rm << 16) | (uint32_t)rd; }

// ADD / SUB Wd, Wn, #imm12
static inline uint32_t a64_enc_add_imm(int rd, int rn, uint32_t imm)
{ assert(imm < 4096); return 0x11000000u | (imm << 10) | ((uint32_t)rn << 5) | (uint32_t)rd; }
static inline uint32_t a64_enc_sub_imm(int rd, int rn, uint32_t imm)
{ assert(imm < 4096); return 0x51000000u | (imm << 10) | ((uint32_t)rn << 5) | (uint32_t)rd; }

// Bitfield moves (32-bit). The usual aliases are built on these.
static inline uint32_t a64_enc_ubfm(int rd, int rn, int immr, int imms)
{ return 0x53000000u | ((uint32_t)immr << 16) | ((uint32_t)imms << 10) | ((uint32_t)rn << 5) | (uint32_t)rd; }
static inline uint32_t a64_enc_sbfm(int rd, int rn, int immr, int imms)
{ return 0x13000000u | ((uint32_t)immr << 16) | ((uint32_t)imms << 10) | ((uint32_t)rn << 5) | (uint32_t)rd; }
static inline uint32_t a64_enc_uxtb(int rd, int rn) { return a64_enc_ubfm(rd, rn, 0, 7); }
static inline uint32_t a64_enc_uxth(int rd, int rn) { return a64_enc_ubfm(rd, rn, 0, 15); }
static inline uint32_t a64_enc_sxtb(int rd, int rn) { return a64_enc_sbfm(rd, rn, 0, 7); }
static inline uint32_t a64_enc_sxth(int rd, int rn) { return a64_enc_sbfm(rd, rn, 0, 15); }
static inline uint32_t a64_enc_lsl_imm(int rd, int rn, int sh) { return a64_enc_ubfm(rd, rn, (32 - sh) & 31, 31 - sh); }
static inline uint32_t a64_enc_lsr_imm(int rd, int rn, int sh) { return a64_enc_ubfm(rd, rn, sh, 31); }
static inline uint32_t a64_enc_asr_imm(int rd, int rn, int sh) { return a64_enc_sbfm(rd, rn, sh, 31); }

// LSLV / LSRV / ASRV Wd, Wn, Wm  (count = Wm mod 32)
static inline uint32_t a64_enc_shiftv_(uint32_t op2, int rd, int rn, int rm)
{ return 0x1AC02000u | (op2 << 10) | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd; }
static inline uint32_t a64_enc_lslv(int rd, int rn, int rm) { return a64_enc_shiftv_(0, rd, rn, rm); }
static inline uint32_t a64_enc_lsrv(int rd, int rn, int rm) { return a64_enc_shiftv_(1, rd, rn, rm); }
static inline uint32_t a64_enc_asrv(int rd, int rn, int rm) { return a64_enc_shiftv_(2, rd, rn, rm); }

// UMULL / SMULL Xd, Wn, Wm  (64-bit product)
static inline uint32_t a64_enc_umull(int rd, int rn, int rm)
{ return 0x9BA07C00u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd; }
static inline uint32_t a64_enc_smull(int rd, int rn, int rm)
{ return 0x9B207C00u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd; }
// LSR Xd, Xn, #sh  (UBFM, 64-bit)
static inline uint32_t a64_enc_lsr_x_imm(int rd, int rn, int sh)
{ return 0xD340FC00u | ((uint32_t)sh << 16) | ((uint32_t)rn << 5) | (uint32_t)rd; }
// 64-bit forms for the x87 code: LSL Xd, Xn, #sh; UBFX Xd, Xn, #lsb, #width; ORR Xd, Xn, Xm;
// CMP Xn, Xm; CBZ / CBNZ Xt.
static inline uint32_t a64_enc_lsl_x_imm(int rd, int rn, int sh)
{ return 0xD3400000u | ((uint32_t)((64 - sh) & 63) << 16) | ((uint32_t)(63 - sh) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd; }
static inline uint32_t a64_enc_ubfx_x(int rd, int rn, int lsb, int width)
{ return 0xD3400000u | ((uint32_t)lsb << 16) | ((uint32_t)(lsb + width - 1) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd; }
static inline uint32_t a64_enc_orr_x(int rd, int rn, int rm)
{ return 0xAA000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd; }
static inline uint32_t a64_enc_cmp_x(int rn, int rm)
{ return 0xEB00001Fu | ((uint32_t)rm << 16) | ((uint32_t)rn << 5); }
static inline uint32_t a64_enc_cbz_x(int rt, int32_t words)
{ return 0xB4000000u | (((uint32_t)words & 0x7FFFFu) << 5) | (uint32_t)rt; }
static inline uint32_t a64_enc_cbnz_x(int rt, int32_t words)
{ return 0xB5000000u | (((uint32_t)words & 0x7FFFFu) << 5) | (uint32_t)rt; }

// CMP Xn, Wm, SXTW  (does the 64-bit value equal its sign-extended low half?)
static inline uint32_t a64_enc_cmp_x_sxtw(int rn, int rm)
{ return 0xEB20C01Fu | ((uint32_t)rm << 16) | ((uint32_t)rn << 5); }

// LDRH / STRH Wt, [Xn, #off]  (off: multiple of 2, 0..8190)
static inline uint32_t a64_enc_ldrh(int rt, int rn, uint32_t off)
{ assert(!(off & 1) && off <= 8190); return 0x79400000u | ((off >> 1) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt; }
static inline uint32_t a64_enc_strh(int rt, int rn, uint32_t off)
{ assert(!(off & 1) && off <= 8190); return 0x79000000u | ((off >> 1) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt; }

// LDRB Wt, [Xn, #off]  (off: 0..4095)
static inline uint32_t a64_enc_ldrb(int rt, int rn, uint32_t off)
{ assert(off <= 4095); return 0x39400000u | (off << 10) | ((uint32_t)rn << 5) | (uint32_t)rt; }

/* 64-bit forms used for the block prologue/epilogue and helper calls. */

// MOVZ / MOVK Xd, #imm16, LSL #(16*hw)
static inline uint32_t a64_enc_movz_x(int rd, uint16_t imm, int hw)
{ return 0xD2800000u | ((uint32_t)hw << 21) | ((uint32_t)imm << 5) | (uint32_t)rd; }
static inline uint32_t a64_enc_movk_x(int rd, uint16_t imm, int hw)
{ return 0xF2800000u | ((uint32_t)hw << 21) | ((uint32_t)imm << 5) | (uint32_t)rd; }

// MOV Xd, Xm  (ORR Xd, XZR, Xm)
static inline uint32_t a64_enc_mov_x(int rd, int rm)
{ return 0xAA0003E0u | ((uint32_t)rm << 16) | (uint32_t)rd; }

// STP / LDP Xt1, Xt2: pre-index store, post-index load, signed-offset forms.
// off: multiple of 8, -512..504.
static inline uint32_t a64_pair_(uint32_t base, int t1, int t2, int rn, int off)
{ assert(!(off & 7) && off >= -512 && off <= 504);
  return base | (((uint32_t)(off / 8) & 0x7Fu) << 15) | ((uint32_t)t2 << 10) | ((uint32_t)rn << 5) | (uint32_t)t1; }
static inline uint32_t a64_enc_stp_pre (int t1, int t2, int rn, int off) { return a64_pair_(0xA9800000u, t1, t2, rn, off); }
static inline uint32_t a64_enc_ldp_post(int t1, int t2, int rn, int off) { return a64_pair_(0xA8C00000u, t1, t2, rn, off); }
static inline uint32_t a64_enc_stp_off (int t1, int t2, int rn, int off) { return a64_pair_(0xA9000000u, t1, t2, rn, off); }
static inline uint32_t a64_enc_ldp_off (int t1, int t2, int rn, int off) { return a64_pair_(0xA9400000u, t1, t2, rn, off); }

// STR / LDR Xt, [Xn, #off]  (off: multiple of 8, 0..32760)
static inline uint32_t a64_enc_str_x(int rt, int rn, uint32_t off)
{ assert(!(off & 7) && off <= 32760); return 0xF9000000u | ((off >> 3) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt; }
static inline uint32_t a64_enc_ldr_x(int rt, int rn, uint32_t off)
{ assert(!(off & 7) && off <= 32760); return 0xF9400000u | ((off >> 3) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt; }

// Branches. Offsets are in instructions (words), relative to the branch itself.
static inline uint32_t a64_enc_b(int32_t words)
{ return 0x14000000u | ((uint32_t)words & 0x3FFFFFFu); }
static inline uint32_t a64_enc_bcond(int cond, int32_t words)
{ return 0x54000000u | (((uint32_t)words & 0x7FFFFu) << 5) | (uint32_t)cond; }
static inline uint32_t a64_enc_cbz_w(int rt, int32_t words)
{ return 0x34000000u | (((uint32_t)words & 0x7FFFFu) << 5) | (uint32_t)rt; }
static inline uint32_t a64_enc_cbnz_w(int rt, int32_t words)
{ return 0x35000000u | (((uint32_t)words & 0x7FFFFu) << 5) | (uint32_t)rt; }

// Condition codes for B.cond / CSEL
enum { A64_EQ = 0, A64_NE = 1, A64_HS = 2, A64_LO = 3, A64_MI = 4, A64_PL = 5, A64_VS = 6, A64_VC = 7,
       A64_HI = 8, A64_LS = 9, A64_GE = 10, A64_LT = 11, A64_GT = 12, A64_LE = 13 };

// SUBS Wd, Wn, #imm12
static inline uint32_t a64_enc_subs_imm(int rd, int rn, uint32_t imm)
{ assert(imm < 4096); return 0x71000000u | (imm << 10) | ((uint32_t)rn << 5) | (uint32_t)rd; }

// ADR Xd, #bytes  (PC-relative, +-1 MB)
static inline uint32_t a64_enc_adr(int rd, int32_t bytes)
{ return 0x10000000u | (((uint32_t)bytes & 3u) << 29) | ((((uint32_t)bytes >> 2) & 0x7FFFFu) << 5) | (uint32_t)rd; }

// CMN Wn, Wm (ADDS WZR, Wn, Wm) and TST Wn, Wm (ANDS WZR, Wn, Wm)
static inline uint32_t a64_enc_cmn_w(int rn, int rm)
{ return 0x2B00001Fu | ((uint32_t)rm << 16) | ((uint32_t)rn << 5); }
static inline uint32_t a64_enc_tst_w(int rn, int rm)
{ return 0x6A00001Fu | ((uint32_t)rm << 16) | ((uint32_t)rn << 5); }

// CSET Wd, cond  (CSINC Wd, WZR, WZR, !cond)
static inline uint32_t a64_enc_cset(int rd, int cond)
{ return 0x1A9F07E0u | ((uint32_t)(cond ^ 1) << 12) | (uint32_t)rd; }

// Wd = Wn <op> (Wm LSR #sh)
static inline uint32_t a64_enc_alu_lsr(uint32_t opbase, int rd, int rn, int rm, int sh)
{ return a64_enc_alu(opbase, rd, rn, rm) | (1u << 22) | ((uint32_t)sh << 10); }

// BR Xn
static inline uint32_t a64_enc_br(int rn) { return 0xD61F0000u | ((uint32_t)rn << 5); }

// UBFX Wd, Wn, #lsb, #width
static inline uint32_t a64_enc_ubfx(int rd, int rn, int lsb, int width)
{ return a64_enc_ubfm(rd, rn, lsb, lsb + width - 1); }

// ADD Xd, Xn, Xm, LSL #sh
static inline uint32_t a64_enc_add_x_lsl(int rd, int rn, int rm, int sh)
{ return 0x8B000000u | ((uint32_t)rm << 16) | ((uint32_t)sh << 10) | ((uint32_t)rn << 5) | (uint32_t)rd; }

// CMP Wn, Wm  (SUBS WZR, Wn, Wm)
static inline uint32_t a64_enc_cmp_w(int rn, int rm)
{ return 0x6B00001Fu | ((uint32_t)rm << 16) | ((uint32_t)rn << 5); }

// LDR{B,H,} / STR{B,H,} Wt, [Xn, Wm, UXTW]   (size 1, 2 or 4 bytes)
static inline uint32_t a64_size_bits_(int size) { return size == 8 ? 0xC0000000u : size == 4 ? 0x80000000u : size == 2 ? 0x40000000u : 0; }
static inline uint32_t a64_enc_ldr_uxtw(int size, int rt, int rn, int rm)
{ return 0x38604800u | a64_size_bits_(size) | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rt; }
static inline uint32_t a64_enc_str_uxtw(int size, int rt, int rn, int rm)
{ return 0x38204800u | a64_size_bits_(size) | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rt; }

// CMP Wn, #imm12  (SUBS WZR, Wn, #imm)
static inline uint32_t a64_enc_cmp_imm(int rn, uint32_t imm)
{ assert(imm < 4096); return 0x7100001Fu | (imm << 10) | ((uint32_t)rn << 5); }

// CSEL Wd, Wn, Wm, cond   (Wd = cond ? Wn : Wm)
static inline uint32_t a64_enc_csel(int rd, int rn, int rm, int cond)
{ return 0x1A800000u | ((uint32_t)rm << 16) | ((uint32_t)cond << 12) | ((uint32_t)rn << 5) | (uint32_t)rd; }

// BFI Wd, Wn, #lsb, #width  (BFM Wd, Wn, #(-lsb mod 32), #(width - 1))
static inline uint32_t a64_enc_bfi(int rd, int rn, int lsb, int width)
{ return 0x33000000u | ((uint32_t)((32 - lsb) & 31) << 16) | ((uint32_t)(width - 1) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd; }

// BL #words (+-128 MB)
static inline uint32_t a64_enc_bl(int32_t words) { return 0x94000000u | ((uint32_t)words & 0x3FFFFFFu); }

// BLR Xn
static inline uint32_t a64_enc_blr(int rn) { return 0xD63F0000u | ((uint32_t)rn << 5); }

// 64-bit forms for the x87 arithmetic. A64_X turns the three-register ALU forms above (also
// with a shift) and LSLV/LSRV/CSEL into their X-register versions.
#define A64_X 0x80000000u
#define A64_OP_ADDS 0x2B000000u
#define A64_OP_SUBS 0x6B000000u
#define A64_OP_SBC  0x5A000000u      // Rd = Rn - Rm - !C  (no shift allowed)
// CLZ Xd, Xn
static inline uint32_t a64_enc_clz_x(int rd, int rn) { return 0xDAC01000u | ((uint32_t)rn << 5) | (uint32_t)rd; }
// MUL Xd, Xn, Xm (low 64 bits) and UMULH Xd, Xn, Xm (high 64 bits of the unsigned product)
static inline uint32_t a64_enc_mul_x(int rd, int rn, int rm)
{ return 0x9B007C00u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd; }
static inline uint32_t a64_enc_umulh(int rd, int rn, int rm)
{ return 0x9BC07C00u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd; }
// CMP Xn, #imm12
static inline uint32_t a64_enc_cmp_x_imm(int rn, uint32_t imm)
{ assert(imm < 4096); return 0xF100001Fu | (imm << 10) | ((uint32_t)rn << 5); }

#define A64_RET 0xD65F03C0u
#define A64_SP  31     // as a base register in loads/stores
#define A64_ZR  31     // as a data register in ALU forms and STR

/* ---- emit helpers ---- */

// Load an arbitrary 32-bit constant into Wd.
static inline void a64_mov_imm32(emit_t *e, int rd, uint32_t v)
{
    uint16_t lo = (uint16_t)v, hi = (uint16_t)(v >> 16);
    if (hi == 0)       a64_emit(e, a64_enc_movz(rd, lo, 0));
    else if (lo == 0)  a64_emit(e, a64_enc_movz(rd, hi, 1));
    else {
        a64_emit(e, a64_enc_movz(rd, lo, 0));
        a64_emit(e, a64_enc_movk(rd, hi, 1));
    }
}

// Load an arbitrary 64-bit constant (e.g. a helper address) into Xd.
static inline void a64_mov_imm64(emit_t *e, int rd, uint64_t v)
{
    a64_emit(e, a64_enc_movz_x(rd, (uint16_t)v, 0));
    for (int hw = 1; hw < 4; hw++) {
        uint16_t part = (uint16_t)(v >> (16 * hw));
        if (part) a64_emit(e, a64_enc_movk_x(rd, part, hw));
    }
}

#endif
