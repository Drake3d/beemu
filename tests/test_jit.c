// JIT tests.
//  1. Encoder words checked against known AArch64 encodings   (any host)
//  2. Translation structure, block cache, SMC invalidation    (any host)
//  3. Actually running generated code and checking guest state (ARM64 hosts only)
#include "a64.h"
#include "cpu.h"
#include "flags.h"
#include "jit.h"
#include "mem.h"
#include <stdio.h>
#include <string.h>

static int g_fail, g_run;
#define CHECK(cond, msg) do { g_run++; if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); g_fail++; } } while (0)

static void setup_cpu(cpu_t *cpu, uint32_t eip)
{
    cpu_init(cpu, 0);
    cpu->cr0 |= 1;                    // protected mode
    cpu->seg[S_CS].flags |= SEG_DB;   // 32-bit code segment
    cpu->seg[S_CS].base = 0;
    cpu->seg[S_CS].limit = 0xFFFFFFFFu;
    cpu->eflags |= EFL_IF;
    cpu->eip = eip;
}

static jit_exit_t run_prog(jit_t *jit, mem_bus_t *bus, cpu_t *cpu, uint32_t at,
                           const uint8_t *code, size_t n)
{
    mem_write_block(bus, at, code, n);
    setup_cpu(cpu, at);
    cpu->mem = bus;
    return jit_run(jit, cpu, 100);
}

// mov eax,0x12345678 ; mov ebx,1 ; add eax,ebx ; xor ecx,ecx ; mov edx,eax ; cli ; jmp +0 ; hlt
static const uint8_t PROG[] = {
    0xB8, 0x78, 0x56, 0x34, 0x12,
    0xBB, 0x01, 0x00, 0x00, 0x00,
    0x01, 0xD8,
    0x31, 0xC9,
    0x89, 0xC2,
    0xFA,
    0xEB, 0x00,
    0xF4,
};

int main(void)
{
    /* ---- 1. encoders ---- */
    CHECK(a64_enc_movz(0, 1, 0)           == 0x52800020u, "movz w0,#1");
    CHECK(a64_enc_movk(0, 0x1234, 1)      == 0x72A24680u, "movk w0,#0x1234,lsl16");
    CHECK(a64_enc_ldr_w(1, 0, 4)          == 0xB9400401u, "ldr w1,[x0,#4]");
    CHECK(a64_enc_str_w(2, 0, 8)          == 0xB9000802u, "str w2,[x0,#8]");
    CHECK(a64_enc_strb(9, 0, 16)          == 0x39004009u, "strb w9,[x0,#16]");
    CHECK(a64_enc_alu(A64_OP_EOR, 3, 4, 5)  == 0x4A050083u, "eor w3,w4,w5");
    CHECK(a64_enc_alu(A64_OP_ADD, 11, 9, 10) == 0x0B0A012Bu, "add w11,w9,w10");
    CHECK(a64_enc_alu(A64_OP_SUB, 11, 9, 10) == 0x4B0A012Bu, "sub w11,w9,w10");
    CHECK(a64_enc_alu(A64_OP_AND, 11, 9, 10) == 0x0A0A012Bu, "and w11,w9,w10");
    CHECK(a64_enc_alu(A64_OP_ORR, 11, 9, 10) == 0x2A0A012Bu, "orr w11,w9,w10");
    // fast-TLB path and block chaining (words from clang's assembler)
    CHECK(a64_enc_ubfx(10, 20, 12, 10)        == 0x530C568Au, "ubfx w10,w20,#12,#10");
    CHECK(a64_enc_add_x_lsl(11, 11, 10, 5)    == 0x8B0A156Bu, "add x11,x11,x10,lsl #5");
    CHECK(a64_enc_cmp_w(12, 10)               == 0x6B0A019Fu, "cmp w12,w10");
    CHECK(a64_enc_ldr_uxtw(4, 0, 11, 20)      == 0xB8744960u, "ldr w0,[x11,w20,uxtw]");
    CHECK(a64_enc_ldr_uxtw(2, 0, 11, 20)      == 0x78744960u, "ldrh w0,[x11,w20,uxtw]");
    CHECK(a64_enc_ldr_uxtw(1, 0, 11, 20)      == 0x38744960u, "ldrb w0,[x11,w20,uxtw]");
    CHECK(a64_enc_str_uxtw(4, 21, 11, 20)     == 0xB8344975u, "str w21,[x11,w20,uxtw]");
    CHECK(a64_enc_str_uxtw(2, 21, 11, 20)     == 0x78344975u, "strh w21,[x11,w20,uxtw]");
    CHECK(a64_enc_str_uxtw(1, 21, 11, 20)     == 0x38344975u, "strb w21,[x11,w20,uxtw]");
    CHECK(a64_enc_bfi(25, 9, 8, 8)            == 0x33181D39u, "bfi w25,w9,#8,#8");
    CHECK(a64_enc_bfi(26, 23, 0, 16)          == 0x33003EFAu, "bfi w26,w23,#0,#16");
    CHECK(a64_enc_bfi(27, 0, 0, 8)            == 0x33001C1Bu, "bfi w27,w0,#0,#8");
    CHECK(a64_enc_stp_off(25, 26, A64_SP, 64) == 0xA9046BF9u, "stp x25,x26,[sp,#64]");
    CHECK(a64_enc_ldp_off(27, 28, A64_SP, 80) == 0xA94573FBu, "ldp x27,x28,[sp,#80]");
    CHECK(a64_enc_bl(-2)                      == 0x97FFFFFEu, "bl .-8");
    CHECK(a64_enc_clz_x(5, 16)                == 0xDAC01205u, "clz x5,x16");
    CHECK(a64_enc_mul_x(17, 6, 8)             == 0x9B087CD1u, "mul x17,x6,x8");
    CHECK(a64_enc_umulh(16, 6, 8)             == 0x9BC87CD0u, "umulh x16,x6,x8");
    CHECK(a64_enc_cmp_x_imm(22, 64)           == 0xF10102DFu, "cmp x22,#64");
    CHECK((a64_enc_alu(A64_OP_ADDS, 16, 6, 16) | A64_X) == 0xAB1000D0u, "adds x16,x6,x16");
    CHECK((a64_enc_alu(A64_OP_SUBS, 17, A64_ZR, 22) | A64_X) == 0xEB1603F1u, "negs x17,x22");
    CHECK((a64_enc_alu(A64_OP_SBC, 16, 6, 16) | A64_X) == 0xDA1000D0u, "sbc x16,x6,x16");
    CHECK((a64_enc_alu(A64_OP_ADD, 5, 6, 7) | A64_X) == 0x8B0700C5u, "add x5,x6,x7");
    CHECK((a64_enc_alu(A64_OP_SUB, 5, 6, 7) | A64_X) == 0xCB0700C5u, "sub x5,x6,x7");
    CHECK((a64_enc_alu(A64_OP_BIC, 5, 6, 7) | A64_X) == 0x8A2700C5u, "bic x5,x6,x7");
    CHECK((a64_enc_alu(A64_OP_EOR, 5, 6, 7) | A64_X) == 0xCA0700C5u, "eor x5,x6,x7");
    CHECK((a64_enc_alu(A64_OP_AND, 5, 6, 7) | A64_X) == 0x8A0700C5u, "and x5,x6,x7");
    CHECK((a64_enc_alu_lsl(A64_OP_ORR, 5, 6, 7, 52) | A64_X) == 0xAA07D0C5u, "orr x5,x6,x7,lsl #52");
    CHECK((a64_enc_alu_lsr(A64_OP_ORR, 5, 6, 7, 63) | A64_X) == 0xAA47FCC5u, "orr x5,x6,x7,lsr #63");
    CHECK((a64_enc_lslv(5, 6, 7) | A64_X)     == 0x9AC720C5u, "lsl x5,x6,x7");
    CHECK((a64_enc_lsrv(5, 6, 7) | A64_X)     == 0x9AC724C5u, "lsr x5,x6,x7");
    CHECK((a64_enc_csel(5, 6, 7, A64_EQ) | A64_X) == 0x9A8700C5u, "csel x5,x6,x7,eq");
    CHECK(a64_enc_bcond(A64_HI, 2)            == 0x54000048u, "b.hi +8");
    CHECK(a64_enc_bcond(A64_LE, 2)            == 0x5400004Du, "b.le +8");
    CHECK(a64_enc_subs_imm(10, 10, 1)         == 0x7100054Au, "subs w10,w10,#1");
    CHECK(a64_enc_adr(10, -4)                 == 0x10FFFFEAu, "adr x10,.-4");
    CHECK(a64_enc_ubfx(10, 10, 9, 1)          == 0x5309254Au, "ubfx w10,w10,#9,#1");
    CHECK(a64_enc_cset(0, A64_LO)             == 0x1A9F27E0u, "cset w0,lo");
    CHECK(a64_enc_cmn_w(9, 10)                == 0x2B0A013Fu, "cmn w9,w10");
    CHECK(a64_enc_tst_w(9, 9)                 == 0x6A09013Fu, "tst w9,w9");
    CHECK(a64_enc_br(12)                      == 0xD61F0180u, "br x12");
    CHECK(a64_enc_alu_lsr(A64_OP_ADD, 11, 11, 10, 12) == 0x0B4A316Bu, "add w11,w11,w10,lsr #12");
    CHECK(a64_enc_alu(A64_OP_BIC, 9, 9, 10)  == 0x0A2A0129u, "bic w9,w9,w10");
    CHECK(a64_enc_mov_w(1, 2)             == 0x2A0203E1u, "mov w1,w2");
    CHECK(A64_RET                         == 0xD65F03C0u, "ret");
    CHECK(a64_enc_mvn_w(0, 1)             == 0x2A2103E0u, "mvn w0,w1");
    CHECK(a64_enc_add_imm(0, 1, 16)       == 0x11004020u, "add w0,w1,#16");
    CHECK(a64_enc_sub_imm(0, 1, 1)        == 0x51000420u, "sub w0,w1,#1");
    CHECK(a64_enc_alu_lsl(A64_OP_ADD, 0, 1, 2, 2) == 0x0B020820u, "add w0,w1,w2,lsl#2");
    CHECK(a64_enc_alu(A64_OP_SUB, 0, A64_ZR, 1) == 0x4B0103E0u, "neg w0,w1");
    CHECK(a64_enc_uxtb(0, 1)              == 0x53001C20u, "uxtb w0,w1");
    CHECK(a64_enc_uxth(0, 1)              == 0x53003C20u, "uxth w0,w1");
    CHECK(a64_enc_sxtb(0, 1)              == 0x13001C20u, "sxtb w0,w1");
    CHECK(a64_enc_sxth(0, 1)              == 0x13003C20u, "sxth w0,w1");
    CHECK(a64_enc_lsl_imm(0, 1, 4)        == 0x531C6C20u, "lsl w0,w1,#4");
    CHECK(a64_enc_lsr_imm(0, 1, 8)        == 0x53087C20u, "lsr w0,w1,#8");
    CHECK(a64_enc_asr_imm(0, 1, 5)        == 0x13057C20u, "asr w0,w1,#5");
    CHECK(a64_enc_lslv(2, 3, 4)           == 0x1AC42062u, "lslv w2,w3,w4");
    CHECK(a64_enc_lsrv(2, 3, 4)           == 0x1AC42462u, "lsrv w2,w3,w4");
    CHECK(a64_enc_asrv(2, 3, 4)           == 0x1AC42862u, "asrv w2,w3,w4");
    CHECK(a64_enc_umull(5, 6, 7)          == 0x9BA77CC5u, "umull x5,w6,w7");
    CHECK(a64_enc_smull(5, 6, 7)          == 0x9B277CC5u, "smull x5,w6,w7");
    CHECK(a64_enc_lsr_x_imm(8, 9, 32)     == 0xD360FD28u, "lsr x8,x9,#32");
    CHECK(a64_enc_cmp_x_sxtw(10, 11)      == 0xEB2BC15Fu, "cmp x10,w11,sxtw");
    CHECK(a64_enc_lsl_x_imm(1, 2, 40)     == 0xD3585C41u, "lsl x1,x2,#40");
    CHECK(a64_enc_orr_x(3, 4, 5)          == 0xAA050083u, "orr x3,x4,x5");
    CHECK(a64_enc_cbz_x(6, 2)             == 0xB4000046u, "cbz x6,.+8");
    CHECK(a64_enc_cbnz_x(7, 2)            == 0xB5000047u, "cbnz x7,.+8");
    CHECK(a64_enc_ldr_uxtw(8, 8, 19, 9)   == 0xF8694A68u, "ldr x8,[x19,w9,uxtw]");
    CHECK(a64_enc_str_uxtw(8, 8, 19, 9)   == 0xF8294A68u, "str x8,[x19,w9,uxtw]");
    CHECK(a64_enc_ubfx_x(10, 11, 52, 11)  == 0xD374F96Au, "ubfx x10,x11,#52,#11");
    CHECK(a64_enc_cmp_x(5, 10)            == 0xEB0A00BFu, "cmp x5,x10");
    CHECK(a64_enc_ldrh(1, 0, 2)           == 0x79400401u, "ldrh w1,[x0,#2]");
    CHECK(a64_enc_strh(1, 0, 2)           == 0x79000401u, "strh w1,[x0,#2]");
    CHECK(a64_enc_ldrb(1, 0, 1)           == 0x39400401u, "ldrb w1,[x0,#1]");
    CHECK(a64_enc_movz_x(16, 0x1234, 0)   == 0xD2824690u, "movz x16,#0x1234");
    CHECK(a64_enc_movk_x(16, 0xFFFF, 3)   == 0xF2FFFFF0u, "movk x16,#0xffff,lsl48");
    CHECK(a64_enc_mov_x(19, 0)            == 0xAA0003F3u, "mov x19,x0");
    CHECK(a64_enc_stp_pre(29, 30, A64_SP, -16) == 0xA9BF7BFDu, "stp x29,x30,[sp,#-16]!");
    CHECK(a64_enc_ldp_post(29, 30, A64_SP, 16) == 0xA8C17BFDu, "ldp x29,x30,[sp],#16");
    CHECK(a64_enc_stp_off(21, 22, A64_SP, 16)  == 0xA9015BF5u, "stp x21,x22,[sp,#16]");
    CHECK(a64_enc_ldp_off(21, 22, A64_SP, 16)  == 0xA9415BF5u, "ldp x21,x22,[sp,#16]");
    CHECK(a64_enc_str_x(30, A64_SP, 48)   == 0xF9001BFEu, "str x30,[sp,#48]");
    CHECK(a64_enc_ldr_x(30, A64_SP, 48)   == 0xF9401BFEu, "ldr x30,[sp,#48]");
    CHECK(a64_enc_blr(16)                 == 0xD63F0200u, "blr x16");
    CHECK(a64_enc_cbz_w(0, 2)             == 0x34000040u, "cbz w0,#8");
    CHECK(a64_enc_cbnz_w(1, -1)           == 0x35FFFFE1u, "cbnz w1,#-4");
    CHECK(a64_enc_b(2)                    == 0x14000002u, "b #8");
    CHECK(a64_enc_b(-1)                   == 0x17FFFFFFu, "b #-4");
    CHECK(a64_enc_bcond(A64_NE, 2)        == 0x54000041u, "b.ne #8");
    CHECK(a64_enc_cmp_imm(0, 0)           == 0x7100001Fu, "cmp w0,#0");
    CHECK(a64_enc_csel(0, 1, 2, A64_NE)   == 0x1A821020u, "csel w0,w1,w2,ne");

    /* ---- 2. translation ---- */
    mem_bus_t *bus = mem_create();
    uint8_t *ram = mem_alloc_host(1 << 20);
    mem_map_ram(bus, 0, 1 << 20, ram);
    mem_write_block(bus, 0x1000, PROG, sizeof PROG);

    jit_t *jit = jit_create(bus, 1 << 20);
    CHECK(jit != NULL, "jit_create");

    cpu_t cpu;
    setup_cpu(&cpu, 0x1000);

    jit_set_regcache(jit, false);           // exact words below assume registers in cpu_t
    jit_block_info b1;
    CHECK(jit_get_block(jit, &cpu, &b1), "translate block 1");
    CHECK(b1.exit == JIT_EXIT_OK, "block 1 exits OK (ends at jmp)");
    CHECK(b1.guest_insns == 7, "block 1 has 7 guest insns");
    CHECK(b1.guest_bytes == 19, "block 1 covers 19 guest bytes");
    CHECK(b1.end_eip == 0x1013, "block 1 ends at 0x1013");
    CHECK(!b1.dyn_exit, "block 1 has a static exit");
    CHECK(b1.code[0] == 0xA9BA53F3u, "prologue: stp x19,x20,[sp,#-96]!");
    CHECK(b1.code[4] == 0xA9046BF9u, "prologue: stp x25,x26,[sp,#64]");
    CHECK(b1.code[6] == 0xAA0003F3u, "prologue: mov x19,x0");
    CHECK(b1.code[7] == 0x528ACF15u, "movz w21,#0x5678");
    CHECK(b1.code[8] == 0x72A24695u, "movk w21,#0x1234,lsl16");
    CHECK(b1.code[9] == 0xB9000275u, "str w21,[x19] (eax)");
    CHECK(b1.code[b1.nwords - 8] == 0x52800020u, "exit code OK");
    CHECK(b1.code[b1.nwords - 2] == 0xA8C653F3u, "epilogue: ldp x19,x20,[sp],#96");
    CHECK(b1.code[b1.nwords - 1] == A64_RET, "ends with ret");
    jit_set_regcache(jit, true);            // the rest runs with the register cache
    {   // the same block with the cache: eax is loaded into w25 first, then written there
        jit_block_info bc;
        CHECK(jit_get_block(jit, &cpu, &bc), "translate block 1 with the register cache");
        CHECK(bc.code[7] == 0xB9400279u && bc.code[10] == 0x2A1503F9u, "regcache: ldr w25,[x19]; ...; mov w25,w21");
    }

    cpu.eip = 0x1013;
    jit_block_info b2;
    CHECK(jit_get_block(jit, &cpu, &b2), "translate block 2");
    CHECK(b2.exit == JIT_EXIT_HALT, "block 2 is HALT");
    CHECK(b2.guest_insns == 1 && b2.dyn_exit, "block 2 is the single hlt (a helper call)");
    CHECK(b2.code[b2.nwords - 1] == A64_RET, "block 2 ends with ret");

    // cache hit: same code, no new translation
    uint64_t before = jit_blocks_translated(jit);
    jit_block_info b2b;
    CHECK(jit_get_block(jit, &cpu, &b2b) && b2b.code == b2.code, "cache hit returns same code");
    CHECK(jit_blocks_translated(jit) == before, "cache hit does not retranslate");

    // unsupported / invalid instructions end the block with eip at the culprit
    static const uint8_t BAD16[] = { 0x0F, 0x6F, 0xC1 };          // movq mm0,mm1 (MMX: not emulated yet)
    static const uint8_t BADMEM[] = { 0x0F, 0x28, 0xC1 };         // movaps xmm0,xmm1 (SSE)
    static const uint8_t UD2[] = { 0x0F, 0x0B };
    mem_write_block(bus, 0x2000, BAD16, sizeof BAD16);
    mem_write_block(bus, 0x2100, BADMEM, sizeof BADMEM);
    mem_write_block(bus, 0x2200, UD2, sizeof UD2);
    jit_block_info bu;
    cpu.eip = 0x2000;
    CHECK(jit_get_block(jit, &cpu, &bu), "translate unsupported");
    CHECK(bu.exit == JIT_EXIT_UNSUPPORTED && bu.guest_insns == 0 && bu.end_eip == 0x2000, "MMX unsupported at 0x2000");
    cpu.eip = 0x2100;
    CHECK(jit_get_block(jit, &cpu, &bu) && bu.exit == JIT_EXIT_UNSUPPORTED && bu.end_eip == 0x2100, "SSE unsupported");
    cpu.eip = 0x2200;
    CHECK(jit_get_block(jit, &cpu, &bu) && bu.exit == JIT_EXIT_INVALID && bu.end_eip == 0x2200, "ud2 invalid");

    // self-modifying code: writing to a translated page flushes the cache
    before = jit_blocks_translated(jit);
    CHECK(bus->page_flags[1] & PG_CODE, "page 0x1000 flagged as code");
    mem_write(bus, 0x1006, 0x02, 1);                 // patch the imm32 of "mov ebx": 2 instead of 1
    CHECK(!(bus->page_flags[1] & PG_CODE), "flag cleared by flush");
    cpu.eip = 0x1000;
    jit_block_info b3;
    CHECK(jit_get_block(jit, &cpu, &b3), "retranslate after write");
    CHECK(jit_blocks_translated(jit) == before + 1, "block was retranslated");
    CHECK(bus->page_flags[1] & PG_CODE, "page flagged again");
    // a write to the same page away from any translated bytes keeps the blocks
    before = jit_blocks_translated(jit);
    mem_write(bus, 0x1F00, 0xAA, 1);
    CHECK(bus->page_flags[1] & PG_CODE, "data write elsewhere in a code page keeps it flagged");
    CHECK(jit_get_block(jit, &cpu, &b3) && jit_blocks_translated(jit) == before, "and does not retranslate");

    // the cache key includes the stack size: same address, 16-bit stack -> new block
    before = jit_blocks_translated(jit);
    cpu.seg[S_SS].flags |= SEG_DB;
    CHECK(jit_get_block(jit, &cpu, &b3), "32-bit stack block");
    cpu.seg[S_SS].flags &= ~SEG_DB;
    CHECK(jit_get_block(jit, &cpu, &b3), "16-bit stack block");
    CHECK(jit_blocks_translated(jit) == before + 1, "stack size is part of the cache key");

    // jnz: the block has two exits, each storing its own eip
    static const uint8_t JNZ[] = { 0x75, 0x10 };                 // jnz +0x10
    mem_write_block(bus, 0x2400, JNZ, sizeof JNZ);
    cpu.eip = 0x2400;
    CHECK(jit_get_block(jit, &cpu, &bu) && bu.dyn_exit && bu.end_eip == 0x2402, "jcc block: falls through to 0x2402");
    int rets = 0, cbz = 0;
    for (uint32_t i = 0; i < bu.nwords; i++) {
        rets += bu.code[i] == A64_RET;
        cbz += (bu.code[i] & 0xFF00001Fu) == 0x34000000u;      // cbz w0, ...
    }
    CHECK(rets == 2 && cbz == 1, "jcc block: one cbz, two returns");

    // ret ends with a computed eip
    static const uint8_t RET[] = { 0xC3 };
    mem_write_block(bus, 0x2300, RET, sizeof RET);
    cpu.eip = 0x2300;
    CHECK(jit_get_block(jit, &cpu, &bu) && bu.dyn_exit && bu.exit == JIT_EXIT_OK, "ret is a dynamic exit");

    /* ---- 3. execution (ARM64 hosts) ---- */
#if defined(__aarch64__)
    mem_write(bus, 0x1006, 0x01, 1);                 // restore the immediate: ebx = 1
    jit_flush(jit);
    setup_cpu(&cpu, 0x1000);
    jit_exit_t x = jit_run(jit, &cpu, 100);
    CHECK(x == JIT_EXIT_HALT, "run ends in HALT");
    CHECK(cpu.gpr[R_EAX] == 0x12345679u, "eax = 0x12345679");
    CHECK(cpu.gpr[R_EBX] == 1, "ebx = 1");
    CHECK(cpu.gpr[R_ECX] == 0, "ecx = 0 (xor)");
    CHECK(cpu.gpr[R_EDX] == 0x12345679u, "edx = eax");
    CHECK(!(cpu.eflags & EFL_IF), "cli cleared IF");
    CHECK(cpu.halted, "cpu halted");
    CHECK(cpu.eip == 0x1014, "eip after hlt");
    CHECK(cpu.cc_op == CC_OP_LOGIC32 && cpu.cc_dst == 0, "lazy flags from xor");

    // sub/cmp flag recording + sti/std
    static const uint8_t PROG2[] = {
        0xB8, 0x0A, 0x00, 0x00, 0x00,        // mov eax,10
        0x83, 0xE8, 0x03,                    // sub eax,3
        0x83, 0xF8, 0x07,                    // cmp eax,7
        0xFB,                                // sti
        0xFD,                                // std
        0xF4,                                // hlt
    };
    mem_write_block(bus, 0x3000, PROG2, sizeof PROG2);
    setup_cpu(&cpu, 0x3000);
    cpu.eflags &= ~EFL_IF;
    x = jit_run(jit, &cpu, 100);
    CHECK(x == JIT_EXIT_HALT && cpu.gpr[R_EAX] == 7, "sub result");
    CHECK(cpu.cc_op == CC_OP_SUB32 && cpu.cc_dst == 0 && cpu.cc_src1 == 7 && cpu.cc_src2 == 7, "cmp lazy flags");
    CHECK((cpu.eflags & EFL_IF) && (cpu.eflags & EFL_DF), "sti and std set bits");

    // stops cleanly on an unsupported instruction
    setup_cpu(&cpu, 0x2100);
    x = jit_run(jit, &cpu, 10);
    CHECK(x == JIT_EXIT_UNSUPPORTED && cpu.eip == 0x2100, "run stops at unsupported insn");

    // 8/16-bit registers: AH write merges, 16-bit add carries out
    static const uint8_t P_NARROW[] = {
        0xB8, 0x44, 0x33, 0x22, 0x11,        // mov eax,0x11223344
        0xB4, 0x55,                          // mov ah,0x55
        0x66, 0xB9, 0xCD, 0xAB,              // mov cx,0xabcd
        0x88, 0xE3,                          // mov bl,ah
        0x66, 0x01, 0xC8,                    // add ax,cx
        0xF4,
    };
    x = run_prog(jit, bus, &cpu, 0x4000, P_NARROW, sizeof P_NARROW);
    CHECK(x == JIT_EXIT_HALT, "narrow: halts");
    CHECK(cpu.gpr[R_EAX] == 0x11220111u, "narrow: eax = 0x11220111");
    CHECK((cpu.gpr[R_EBX] & 0xFF) == 0x55, "narrow: bl = ah");
    CHECK((cpu.gpr[R_ECX] & 0xFFFF) == 0xABCD, "narrow: cx");
    CHECK(cpu.cc_op == CC_OP_ADD16, "narrow: cc_op ADD16");
    CHECK(cpu_calc_flags(&cpu) & EFL_CF, "narrow: 16-bit carry out");
    CHECK(!(cpu_calc_flags(&cpu) & EFL_ZF), "narrow: ZF clear");

    // memory operands, SIB addressing, movzx/movsx, lea, inc/xchg on memory
    static const uint8_t P_MEM[] = {
        0xBB, 0x00, 0x50, 0x00, 0x00,                // mov ebx,0x5000
        0xC7, 0x43, 0x08, 0x78, 0x56, 0x34, 0x12,    // mov dword [ebx+8],0x12345678
        0x83, 0x43, 0x08, 0x02,                      // add dword [ebx+8],2
        0x8B, 0x43, 0x08,                            // mov eax,[ebx+8]
        0xBE, 0x01, 0x00, 0x00, 0x00,                // mov esi,1
        0xC6, 0x44, 0xB3, 0x10, 0x80,                // mov byte [ebx+esi*4+0x10],0x80
        0x0F, 0xB6, 0x0D, 0x14, 0x50, 0x00, 0x00,    // movzx ecx,byte [0x5014]
        0x0F, 0xBE, 0x15, 0x14, 0x50, 0x00, 0x00,    // movsx edx,byte [0x5014]
        0x8D, 0x7C, 0xB3, 0x10,                      // lea edi,[ebx+esi*4+0x10]
        0xFF, 0x43, 0x08,                            // inc dword [ebx+8]
        0x87, 0x43, 0x08,                            // xchg [ebx+8],eax
        0xF4,
    };
    x = run_prog(jit, bus, &cpu, 0x4100, P_MEM, sizeof P_MEM);
    CHECK(x == JIT_EXIT_HALT, "mem: halts");
    CHECK(mem_read(bus, 0x5008, 4) == 0x1234567Au, "mem: [ebx+8] after add and xchg");
    CHECK(cpu.gpr[R_EAX] == 0x1234567Bu, "mem: eax from xchg (after inc)");
    CHECK(mem_read(bus, 0x5014, 1) == 0x80, "mem: SIB byte store");
    CHECK(cpu.gpr[R_ECX] == 0x80, "mem: movzx");
    CHECK(cpu.gpr[R_EDX] == 0xFFFFFF80u, "mem: movsx");
    CHECK(cpu.gpr[R_EDI] == 0x5014, "mem: lea");
    CHECK(cpu.cc_op == CC_OP_INC32, "mem: cc_op from inc");

    // stack: push imm, call rel, push ebp / mov ebp,esp / [ebp+8] / leave / ret 4
    static const uint8_t P_STACK[] = {
        0xBC, 0x00, 0x80, 0x00, 0x00,        // 6000: mov esp,0x8000
        0x6A, 0x11,                          // 6005: push 0x11
        0xE8, 0x03, 0x00, 0x00, 0x00,        // 6007: call 0x600f
        0x89, 0xE3,                          // 600c: mov ebx,esp
        0xF4,                                // 600e: hlt
        0x55,                                // 600f: push ebp
        0x89, 0xE5,                          //       mov ebp,esp
        0x8B, 0x45, 0x08,                    //       mov eax,[ebp+8]
        0xC9,                                //       leave
        0xC2, 0x04, 0x00,                    //       ret 4
    };
    x = run_prog(jit, bus, &cpu, 0x6000, P_STACK, sizeof P_STACK);
    CHECK(x == JIT_EXIT_HALT, "stack: halts");
    CHECK(cpu.gpr[R_EAX] == 0x11, "stack: argument via [ebp+8]");
    CHECK(cpu.gpr[R_EBX] == 0x8000 && cpu.gpr[R_ESP] == 0x8000, "stack: ret 4 pops the argument");
    CHECK(cpu.gpr[R_EBP] == 0, "stack: leave restores ebp");
    CHECK(mem_read(bus, 0x7FF8, 4) == 0x600C, "stack: return address pushed");
    CHECK(cpu.eip == 0x600F, "stack: eip after hlt");

    // indirect call through a register
    static const uint8_t P_ICALL[] = {
        0xBC, 0x00, 0x80, 0x00, 0x00,        // 6100: mov esp,0x8000
        0xB8, 0x0D, 0x61, 0x00, 0x00,        // 6105: mov eax,0x610d
        0xFF, 0xD0,                          // 610a: call eax
        0xF4,                                // 610c: hlt
        0xB9, 0x07, 0x00, 0x00, 0x00,        // 610d: mov ecx,7
        0xC3,                                //       ret
    };
    x = run_prog(jit, bus, &cpu, 0x6100, P_ICALL, sizeof P_ICALL);
    CHECK(x == JIT_EXIT_HALT && cpu.gpr[R_ECX] == 7, "icall: callee ran");
    CHECK(cpu.eip == 0x610D && cpu.gpr[R_ESP] == 0x8000, "icall: returned to hlt");

    // ADC/SBB carry-in, NEG/NOT, STC/CMC
    static const uint8_t P_CARRY[] = {
        0xB8, 0xFF, 0xFF, 0xFF, 0xFF,        // mov eax,-1
        0x83, 0xC0, 0x01,                    // add eax,1      CF=1
        0xBB, 0x05, 0x00, 0x00, 0x00,        // mov ebx,5
        0x83, 0xD3, 0x00,                    // adc ebx,0      ebx=6
        0xF9,                                // stc
        0xB9, 0x0A, 0x00, 0x00, 0x00,        // mov ecx,10
        0x83, 0xD9, 0x03,                    // sbb ecx,3      ecx=6
        0xF7, 0xDB,                          // neg ebx        ebx=-6, CF=1
        0xF7, 0xD1,                          // not ecx        ecx=~6
        0xF5,                                // cmc            CF=0
        0xF4,
    };
    x = run_prog(jit, bus, &cpu, 0x6200, P_CARRY, sizeof P_CARRY);
    CHECK(x == JIT_EXIT_HALT, "carry: halts");
    CHECK(cpu.gpr[R_EAX] == 0, "carry: add wrapped");
    CHECK(cpu.gpr[R_EBX] == 0xFFFFFFFAu, "carry: adc then neg");
    CHECK(cpu.gpr[R_ECX] == 0xFFFFFFF9u, "carry: sbb with CF then not");
    CHECK(cpu.cc_op == CC_OP_EFLAGS, "carry: cmc materialized flags");
    CHECK(!(cpu.eflags & EFL_CF) && (cpu.eflags & EFL_SF) && !(cpu.eflags & EFL_ZF), "carry: flags after neg + cmc");

    // a failing instruction emits no code: the block stops cleanly before it
    static const uint8_t P_STOP[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00,        // mov eax,1
        0xDB, 0x08,                          // fisttp dword [eax] (SSE3: not on a Pentium III, stops at run time)
    };
    x = run_prog(jit, bus, &cpu, 0x6300, P_STOP, sizeof P_STOP);
    CHECK(x == JIT_EXIT_UNSUPPORTED && cpu.eip == 0x6305 && cpu.gpr[R_EAX] == 1, "stop before fisttp");

    // real mode: 16-bit code, [bx+si+disp] addressing with DS base, 16-bit stack
    static const uint8_t P_REAL[] = {
        0xBC, 0x00, 0x02,                    // 7c00: mov sp,0x200
        0xBB, 0x10, 0x00,                    // 7c03: mov bx,0x10
        0xBE, 0x02, 0x00,                    // 7c06: mov si,2
        0xC7, 0x40, 0x04, 0xEF, 0xBE,        // 7c09: mov word [bx+si+4],0xbeef
        0xFF, 0x70, 0x04,                    // 7c0e: push word [bx+si+4]
        0x58,                                // 7c11: pop ax
        0xE8, 0x01, 0x00,                    // 7c12: call 0x7c16
        0xF4,                                // 7c15: hlt
        0x40,                                // 7c16: inc ax
        0xC3,                                // 7c17: ret
    };
    mem_write_block(bus, 0x7C00, P_REAL, sizeof P_REAL);
    cpu_init(&cpu, 0);
    cpu.mem = bus;
    cpu.seg[S_CS].selector = 0; cpu.seg[S_CS].base = 0;
    cpu.seg[S_DS].selector = 0x1000; cpu.seg[S_DS].base = 0x10000;
    cpu.gpr[R_ESP] = 0xABCD0000u;
    cpu.eip = 0x7C00;
    x = jit_run(jit, &cpu, 100);
    CHECK(x == JIT_EXIT_HALT, "real: halts");
    CHECK(mem_read(bus, 0x10016, 2) == 0xBEEF, "real: store at DS:bx+si+4");
    CHECK(cpu.gpr[R_EAX] == 0xBEF0, "real: pushed, popped, incremented");
    CHECK(cpu.gpr[R_ESP] == 0xABCD0200u, "real: 16-bit stack keeps ESP's upper half");
    CHECK(mem_read(bus, 0x1FE, 2) == 0x7C15, "real: 16-bit return address on the stack");
    CHECK(cpu.eip == 0x7C16 && cpu.cc_op == CC_OP_INC16, "real: eip and cc_op");

    // block chaining: the loop's back edge is linked, the block budget still applies
    static const uint8_t P_LOOP[] = {
        0xB9, 0xE8, 0x03, 0x00, 0x00,        // 7100: mov ecx,1000
        0x31, 0xC0,                          // 7105: xor eax,eax
        0x01, 0xC8,                          // 7107: add eax,ecx
        0xA3, 0x00, 0x51, 0x00, 0x00,        // 7109: mov [0x5100],eax
        0x49,                                // 710e: dec ecx
        0x75, 0xF6,                          // 710f: jnz 0x7107
        0xF4,                                // 7111: hlt
    };
    uint64_t links = jit_chain_links(jit);
    x = run_prog(jit, bus, &cpu, 0x7100, P_LOOP, sizeof P_LOOP);   // 100-block budget
    CHECK(x == JIT_EXIT_LIMIT && cpu.gpr[R_ECX] > 0 && cpu.gpr[R_ECX] < 1000, "chain: budget stops a linked loop");
    x = jit_run(jit, &cpu, 100000);
    CHECK(x == JIT_EXIT_HALT && cpu.gpr[R_EAX] == 500500 && cpu.gpr[R_ECX] == 0, "chain: loop result");
    CHECK(mem_read(bus, 0x5100, 4) == 500500, "chain: stores through the fast path");
    CHECK(jit_chain_links(jit) > links, "chain: back edge linked");

    // fast path: dword accesses that straddle a page go the slow way, correctly
    static const uint8_t P_CROSS[] = {
        0xB8, 0x44, 0x33, 0x22, 0x11,        // mov eax,0x11223344
        0xA3, 0xFE, 0x5F, 0x00, 0x00,        // mov [0x5ffe],eax
        0xA3, 0xFE, 0x5F, 0x00, 0x00,        // again (fast TLB filled for that page now)
        0x8B, 0x1D, 0xFE, 0x5F, 0x00, 0x00,  // mov ebx,[0x5ffe]
        0x66, 0x8B, 0x0D, 0xFF, 0x5F, 0x00, 0x00, // mov cx,[0x5fff]
        0xF4,
    };
    x = run_prog(jit, bus, &cpu, 0x7180, P_CROSS, sizeof P_CROSS);
    CHECK(x == JIT_EXIT_HALT && cpu.gpr[R_EBX] == 0x11223344u, "cross: dword across a page");
    CHECK((cpu.gpr[R_ECX] & 0xFFFF) == 0x2233 && mem_read(bus, 0x6000, 2) == 0x1122, "cross: word across a page");

    // a page written through the fast path, then run as code, then patched:
    // the patch must not use the stale fast-TLB write permission
    static const uint8_t P_SMC[] = { 0xBC, 0x00, 0x90, 0x00, 0x00, 0xC7, 0x05, 0x00, 0x80, 0x00, 0x00, 0xB8, 0x01, 0x00, 0x00, 0xC6, 0x05, 0x04, 0x80, 0x00, 0x00, 0x00, 0xC6, 0x05, 0x05, 0x80, 0x00, 0x00, 0xC3, 0xE8, 0xDE, 0x0D, 0x00, 0x00, 0x89, 0xC6, 0xC6, 0x05, 0x01, 0x80, 0x00, 0x00, 0x02, 0xE8, 0xD0, 0x0D, 0x00, 0x00, 0xF4 };
    x = run_prog(jit, bus, &cpu, 0x7200, P_SMC, sizeof P_SMC);
    CHECK(x == JIT_EXIT_HALT && cpu.gpr[R_ESI] == 1, "smc: first call ran the written code");
    CHECK(cpu.gpr[R_EAX] == 2, "smc: patched code ran on the second call");

    // a chained jump into a block invalidated through the page its last
    // instruction straddles into goes back to the dispatcher
    uint8_t far[0x22];
    memset(far, 0x90, sizeof far);
    static const uint8_t S_BLK[] = { 0xB8, 0x00, 0x00, 0x00, 0x00, 0xEB, 0x09 };   // afe0: mov eax,0; jmp aff0
    memcpy(far, S_BLK, sizeof S_BLK);
    far[0x10] = 0x40;                                         // aff0: inc eax
    static const uint8_t T_END[] = { 0xB9, 0x11, 0x22, 0x33, 0x44, 0xC3 };          // affc: mov ecx,0x44332211 (ends at b000); ret
    memcpy(far + 0x1C, T_END, sizeof T_END);
    mem_write_block(bus, 0xAFE0, far, sizeof far);
    static const uint8_t P_CALLERS[] = { 0xBC, 0x00, 0x90, 0x00, 0x00, 0xE8, 0xD6, 0x3C, 0x00, 0x00, 0xC6, 0x05, 0x00, 0xB0, 0x00, 0x00, 0x55, 0xE8, 0xCA, 0x3C, 0x00, 0x00, 0xF4 };
    links = jit_chain_links(jit);
    x = run_prog(jit, bus, &cpu, 0x7300, P_CALLERS, sizeof P_CALLERS);
    CHECK(x == JIT_EXIT_HALT && cpu.gpr[R_EAX] == 1, "straddle: inc ran on both calls");
    CHECK(cpu.gpr[R_ECX] == 0x55332211u, "straddle: the patched immediate is used");
    CHECK(jit_chain_links(jit) > links, "straddle: the jump was linked");

    // jump cache: a call/ret loop to another page stays out of the dispatcher;
    // patching the callee still takes effect
    static const uint8_t P_JCLOOP[] = { 0xBC, 0x00, 0x90, 0x00, 0x00, 0x31, 0xF6, 0xB9, 0xE8, 0x03, 0x00, 0x00, 0xE8, 0xEF, 0x11, 0x00, 0x00, 0x01, 0xC6, 0x49, 0x75, 0xF6, 0xC6, 0x05, 0x01, 0x88, 0x00, 0x00, 0x05, 0xE8, 0xDE, 0x11, 0x00, 0x00, 0xF4 };
    static const uint8_t F_RET3[] = { 0xB8, 0x03, 0x00, 0x00, 0x00, 0xC3 };          // 8800: mov eax,3; ret
    mem_write_block(bus, 0x8800, F_RET3, sizeof F_RET3);
    uint64_t disp = jit_dispatches(jit);
    x = run_prog(jit, bus, &cpu, 0x7600, P_JCLOOP, sizeof P_JCLOOP);
    x = x == JIT_EXIT_LIMIT ? jit_run(jit, &cpu, 100000) : x;
    CHECK(x == JIT_EXIT_HALT && cpu.gpr[R_ESI] == 3000 && cpu.gpr[R_EAX] == 5, "jump cache: loop result, then patched callee");
    CHECK(jit_dispatches(jit) - disp < 100, "jump cache: call/ret loop mostly bypasses the dispatcher");

    // jump cache and address spaces: the same linear address maps to different
    // code after mov cr3; the second call must not reuse the first target
    {
        for (uint32_t i = 0; i < 256; i++) mem_write(bus, 0x24000 + 4 * i, (i << 12) | 3, 4);   // 1 MB identity
        mem_write(bus, 0x20000, 0x24003, 4); mem_write(bus, 0x20004, 0x22003, 4); mem_write(bus, 0x22000, 0x30003, 4);
        mem_write(bus, 0x21000, 0x24003, 4); mem_write(bus, 0x21004, 0x23003, 4); mem_write(bus, 0x23000, 0x31003, 4);
        static const uint8_t RET1[] = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };
        static const uint8_t RET2[] = { 0xB8, 0x02, 0x00, 0x00, 0x00, 0xC3 };
        mem_write_block(bus, 0x30000, RET1, sizeof RET1);
        mem_write_block(bus, 0x31000, RET2, sizeof RET2);
        static const uint8_t P_CR3[] = { 0xBC, 0x00, 0x90, 0x00, 0x00, 0xBB, 0x00, 0x10, 0x02, 0x00, 0xE8, 0xF1, 0x88, 0x3F, 0x00, 0x89, 0xC6, 0x0F, 0x22, 0xDB, 0xE8, 0xE7, 0x88, 0x3F, 0x00, 0xF4 };
        mem_write_block(bus, 0x7700, P_CR3, sizeof P_CR3);
        setup_cpu(&cpu, 0x7700);
        cpu.mem = bus;
        cpu.cr3 = 0x20000;
        cpu.cr0 |= CR0_PG;
        x = jit_run(jit, &cpu, 1000);
        CHECK(x == JIT_EXIT_HALT && cpu.gpr[R_ESI] == 1, "cr3: first address space");
        CHECK(cpu.gpr[R_EAX] == 2, "cr3: same linear address, new code after the switch");
    }

    // one physical code page mapped at two linear addresses (BeOS shares add-on text between
    // teams at different bases): position-independent code must see its own address in each
    {
        static const uint8_t PIC[] = { 0xE8, 0x00, 0x00, 0x00, 0x00, 0x58, 0xF4 };   // call next; pop eax; hlt
        mem_write_block(bus, 0x30000, PIC, sizeof PIC);
        mem_write(bus, 0x22000, 0x30003, 4);                  // 0x400000 -> 0x30000
        mem_write(bus, 0x22004, 0x30003, 4);                  // 0x401000 -> 0x30000 as well
        uint32_t want[2] = { 0x400005, 0x401005 }, got[2];
        for (int k = 0; k < 2; k++) {
            setup_cpu(&cpu, 0x400000u + 0x1000u * (uint32_t)k);
            cpu.mem = bus;
            cpu.cr3 = 0x20000;
            cpu.cr0 |= CR0_PG;
            cpu.gpr[R_ESP] = 0x9000;
            x = jit_run(jit, &cpu, 100);
            got[k] = x == JIT_EXIT_HALT ? cpu.gpr[R_EAX] : 0;
        }
        CHECK(got[0] == want[0] && got[1] == want[1], "shared code page: each mapping gets its own return address");
    }

    // inline conditions: op; setcc x16 must agree with the C evaluator (validated
    // against hardware) for every op/size/operand pair; then CF through INC and ADC.
    {
        static const uint32_t V[] = { 0, 1, 2, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF,
                                      0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0x12345678u };
        static const struct { uint8_t op8, op32; bool unary; uint8_t modrm; } OPS[] = {
            { 0x00, 0x01, false, 0xD8 }, { 0x08, 0x09, false, 0xD8 }, { 0x10, 0x11, false, 0xD8 },
            { 0x18, 0x19, false, 0xD8 }, { 0x20, 0x21, false, 0xD8 }, { 0x28, 0x29, false, 0xD8 },
            { 0x30, 0x31, false, 0xD8 }, { 0x38, 0x39, false, 0xD8 }, { 0x84, 0x85, false, 0xD8 },
            { 0xFE, 0xFF, true, 0xC0 }, { 0xFE, 0xFF, true, 0xC8 }, { 0xF6, 0xF7, true, 0xD8 },  // inc dec neg
        };
        int bad = 0, bad_cf = 0, cases = 0;
        for (unsigned o = 0; o < sizeof OPS / sizeof OPS[0]; o++)
        for (int size = 1; size <= 4; size *= 2)
        for (unsigned ia = 0; ia < sizeof V / sizeof V[0]; ia++)
        for (unsigned ib = 0; ib < sizeof V / sizeof V[0]; ib++) {
            uint8_t p[160];
            int n = 0;
            p[n++] = 0xB8; memcpy(p + n, &V[ia], 4); n += 4;            // mov eax,a
            p[n++] = 0xBB; memcpy(p + n, &V[ib], 4); n += 4;            // mov ebx,b
            p[n++] = 0xF9;                                              // stc (carry-in for adc/sbb)
            int op_at = n;
            if (size == 2) p[n++] = 0x66;
            p[n++] = size == 1 ? OPS[o].op8 : OPS[o].op32;
            p[n++] = OPS[o].modrm;
            int op_len = n - op_at;
            uint8_t op_bytes[4];
            memcpy(op_bytes, p + op_at, (size_t)op_len);
            for (int k = 0; k < 16; k++) {                              // setcc byte [0x5200+k]
                p[n++] = 0x0F; p[n++] = (uint8_t)(0x90 + k); p[n++] = 0x05;
                uint32_t a = 0x5200u + (uint32_t)k; memcpy(p + n, &a, 4); n += 4;
            }
            p[n++] = 0xF4;
            x = run_prog(jit, bus, &cpu, 0x7400, p, (size_t)n);
            cases++;
            for (int k = 0; k < 16; k++)
                if (mem_read(bus, 0x5200u + (uint32_t)k, 1) != (cpu_eval_cond(&cpu, k) ? 1u : 0u)) {
                    if (bad++ < 5) printf("  cond %X wrong after op %02X size %d a=%08X b=%08X\n",
                                          k, op_bytes[op_len - 2], size, V[ia], V[ib]);
                }
            uint32_t cf = cpu_eval_cond(&cpu, 2) ? 1 : 0;
            // second program: same op, then inc ecx (keeps CF), setb, adc edx,0
            n = 0;
            p[n++] = 0xB8; memcpy(p + n, &V[ia], 4); n += 4;
            p[n++] = 0xBB; memcpy(p + n, &V[ib], 4); n += 4;
            p[n++] = 0x31; p[n++] = 0xC9;                               // xor ecx,ecx
            p[n++] = 0x31; p[n++] = 0xD2;                               // xor edx,edx
            p[n++] = 0xF9;                                              // stc
            memcpy(p + n, op_bytes, (size_t)op_len); n += op_len;
            p[n++] = 0x41;                                              // inc ecx
            p[n++] = 0x0F; p[n++] = 0x92; p[n++] = 0x05;                // setb [0x5220]
            uint32_t a = 0x5220; memcpy(p + n, &a, 4); n += 4;
            p[n++] = 0x83; p[n++] = 0xD2; p[n++] = 0x00;                // adc edx,0
            p[n++] = 0xF4;
            x = run_prog(jit, bus, &cpu, 0x7400, p, (size_t)n);
            if (mem_read(bus, 0x5220, 1) != cf || cpu.gpr[R_EDX] != cf) {
                if (bad_cf++ < 5) printf("  CF via inc/adc wrong after op %02X size %d a=%08X b=%08X\n",
                                         op_bytes[op_len - 2], size, V[ia], V[ib]);
            }
        }
        CHECK(bad == 0, "inline conditions match the C evaluator");
        CHECK(bad_cf == 0, "inline carry through inc/adc matches");
        printf("(%d op/size/operand cases x 16 conditions)\n", cases);
    }

    printf("(execution tests ran on this ARM64 host)\n");
#else
    printf("(execution tests skipped: host is not ARM64)\n");
#endif

    jit_destroy(jit);
    mem_free_host(ram);
    mem_destroy(bus);
    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
