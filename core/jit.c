#include "jit.h"
#include "a64.h"
#include "decode.h"
#include "flags.h"
#include "fpu.h"
#include "helpers.h"
#include "system.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#if defined(__aarch64__)
#define JIT_HOST_ARM64 1
#include <sys/mman.h>
#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#endif
#endif

#define MAX_BLOCK_INSNS  64
#define TABLE_BITS       14
#define TABLE_SIZE       (1u << TABLE_BITS)
#define MAX_ENTRIES      32768    // blocks between full flushes
#define MIN_FREE_WORDS   4096     // headroom required before translating a block
#define PROLOGUE_WORDS   7

/* Guest-state offsets baked into generated code. All must fit the
 * scaled-immediate forms of LDR/STR/LDRH/STRH/LDRB/STRB. */
#define OFF_GPR(r)      ((uint32_t)(offsetof(cpu_t, gpr) + 4u * (unsigned)(r)))
#define OFF_EIP         ((uint32_t)offsetof(cpu_t, eip))
#define OFF_EFLAGS      ((uint32_t)offsetof(cpu_t, eflags))
#define OFF_CC_OP       ((uint32_t)offsetof(cpu_t, cc_op))
#define OFF_CC_DST      ((uint32_t)offsetof(cpu_t, cc_dst))
#define OFF_CC_SRC1     ((uint32_t)offsetof(cpu_t, cc_src1))
#define OFF_CC_SRC2     ((uint32_t)offsetof(cpu_t, cc_src2))
#define OFF_HALTED      ((uint32_t)offsetof(cpu_t, halted))
#define OFF_SMC         ((uint32_t)offsetof(cpu_t, smc_hit))
#define OFF_FTLB_BASE   ((uint32_t)offsetof(cpu_t, ftlb_base))
#define OFF_JC_BASE     ((uint32_t)offsetof(cpu_t, jc_base))
#define OFF_CHAIN_FROM  ((uint32_t)offsetof(cpu_t, chain_from))
#define OFF_CHAIN_BUDGET ((uint32_t)offsetof(cpu_t, chain_budget))
#define OFF_INTR_LINE   ((uint32_t)offsetof(cpu_t, intr_line))
#define OFF_EXIT_REQ    ((uint32_t)offsetof(cpu_t, exit_request))
#define OFF_SEG_SEL(s)  ((uint32_t)(offsetof(cpu_t, seg) + sizeof(seg_t) * (unsigned)(s) + offsetof(seg_t, selector)))
#define OFF_SEG_BASE(s) ((uint32_t)(offsetof(cpu_t, seg) + sizeof(seg_t) * (unsigned)(s) + offsetof(seg_t, base)))
#define OFF_CR0         ((uint32_t)offsetof(cpu_t, cr0))
#define OFF_ST          ((uint32_t)offsetof(cpu_t, st))
#define OFF_FPU_CW      ((uint32_t)offsetof(cpu_t, fpu_cw))
#define OFF_FPU_SW      ((uint32_t)offsetof(cpu_t, fpu_sw))
#define OFF_FPU_TW      ((uint32_t)offsetof(cpu_t, fpu_tw))
#define OFF_FPU_IP      ((uint32_t)offsetof(cpu_t, fpu_ip))
#define OFF_FPU_DP      ((uint32_t)offsetof(cpu_t, fpu_dp))
#define OFF_FPU_CS      ((uint32_t)offsetof(cpu_t, fpu_cs))
#define OFF_FPU_DS      ((uint32_t)offsetof(cpu_t, fpu_ds))
#define OFF_FPU_OP      ((uint32_t)offsetof(cpu_t, fpu_op))

_Static_assert(sizeof(ftlb_entry_t) == 32 && offsetof(ftlb_entry_t, addend) == 16, "fast TLB layout");
_Static_assert(offsetof(cpu_t, ftlb_base) % 8 == 0 && offsetof(cpu_t, ftlb_base) <= 32760, "ftlb_base offset");
_Static_assert(sizeof(jc_entry_t) == 16 && offsetof(jc_entry_t, body) == 8, "jump cache layout");
_Static_assert(offsetof(cpu_t, jc_base) % 8 == 0 && offsetof(cpu_t, jc_base) <= 32760, "jc_base offset");
_Static_assert(offsetof(cpu_t, chain_from) % 8 == 0 && offsetof(cpu_t, chain_from) <= 32760, "chain_from offset");
_Static_assert(offsetof(cpu_t, chain_budget) % 4 == 0, "chain_budget alignment");
_Static_assert(sizeof(((cpu_t *)0)->intr_line) == 1 && offsetof(cpu_t, intr_line) < 4096, "intr_line for LDRB");
_Static_assert(sizeof(((cpu_t *)0)->exit_request) == 1 && offsetof(cpu_t, exit_request) < 4096, "exit_request for LDRB");
_Static_assert(sizeof(cc_op_t) == 4, "cc_op_t must be 32-bit for STR W");
_Static_assert(sizeof(((cpu_t *)0)->halted) == 1, "halted must be one byte for STRB");
_Static_assert(offsetof(cpu_t, halted) < 4096, "halted offset must fit STRB immediate");
_Static_assert(offsetof(cpu_t, gpr) + 32 < 4096, "gpr offsets must fit LDRB/STRB immediates");
_Static_assert(offsetof(cpu_t, seg) + 6 * sizeof(seg_t) < 4096, "seg offsets must fit LDRH immediates");
_Static_assert(offsetof(cpu_t, cc_src2) < 16380, "cpu_t fields must fit LDR/STR immediates");

// One translated block. Valid entries are linked into a hash bucket (by physical
// address) and into the list of each physical page they were decoded from, so a
// write to code invalidates just that page's blocks.
typedef struct {
    uint32_t phys, mode;
    uint32_t eip, cs_base;    // where it was translated: blocks embed absolute eips (call return
                              // addresses, branch targets), so a page mapped at two linear
                              // addresses (BeOS shares add-on text between teams) needs two blocks
    uint32_t next;            // next entry in the hash bucket (index + 1, 0 = end)
    uint32_t page[2];         // physical page numbers the block was decoded from
    uint32_t page_next[2];    // next entry in each page's list (index + 1)
    uint8_t  n_pages;
    bool     valid;
    uint32_t *body;           // first word after the prologue
    jit_block_info info;
} entry_t;

// Helper records (copies of decoded instructions) referenced by generated code.
// Chunks are never freed before jit_destroy: a flush can happen in the middle of
// a running block (self-modifying code), so records are only reused by later
// translations, which run from the dispatcher.
#define ARENA_CHUNK 512
typedef struct arena_chunk {
    struct arena_chunk *next;
    uint32_t used;
    jit_hrec_t recs[ARENA_CHUNK];
} arena_chunk_t;

struct jit {
    uint32_t   trace[JIT_TRACE_LEN];
    uint32_t   trace_pos;
    bool       single_step;
    uint32_t   trace_lo, trace_hi;
    mem_bus_t *bus;
    arena_chunk_t *arena_head, *arena_cur;
    uint32_t *buf;
    size_t    buf_words;
    uint32_t *top;
    uint32_t *exit_stub;      // permanent epilogue returning JIT_EXIT_OK (start of buf)
    uint32_t  stub_words;
    uint32_t *x87_core[3];    // shared x87 add / mul / sub routines (after the exit stub)
    int       write_depth;    // nested write_begin() calls
    uint32_t *bucket;         // TABLE_SIZE heads (entry index + 1)
    entry_t  *pool;           // MAX_ENTRIES
    uint32_t  n_entries;
    uint32_t *page_head;      // per physical page: first entry (index + 1)
    uint64_t *code_bits;      // per physical page: 64-byte chunks holding translated code
    uint64_t  translated;
    uint64_t  inval_pages, inval_blocks, flush_full, flush_other;   // BEEMU_STATS counters
    bool      fastmem;        // inline fast-TLB memory path (BEEMU_NO_FASTMEM=1 turns it off)
    bool      chaining;       // block chaining (BEEMU_NO_CHAIN=1 turns it off)
    bool      inline_flags;   // inline condition / carry evaluation (BEEMU_NO_INLINE_FLAGS=1 turns it off)
    bool      jump_cache;     // inline jump-cache lookup at dynamic exits (BEEMU_NO_JUMP_CACHE=1 turns it off)
    bool      x87_direct;
    bool      x87_inline;      // x87 data moves as ARM64 code (BEEMU_NO_X87_INLINE)
    bool      regcache;        // guest registers in x25..x28 within a block (BEEMU_REGCACHE=1; off by default)
    bool      inline_alu;      // shifts and 32-bit multiplies as ARM64 code (BEEMU_NO_INLINE_ALU)
    bool      pause_yields;   // PAUSE ends the block (several CPUs share the thread): jit_set_smp()     // common x87 forms call fpu_jit_* directly (BEEMU_NO_X87_JIT=1 turns it off)
    uint64_t  flush_gen;      // bumped by every full flush (old exit slots are gone)
    uint64_t  links;          // BEEMU_STATS: chain slots patched
    bool      stats;          // BEEMU_STATS=1
    uint64_t  dispatches;
    struct { uint32_t lin; uint32_t n; } hot[4096];   // dispatcher entries by guest address (stats only)
    cpu_t    *cpus[8];        // CPUs that ran here (their fast TLBs follow code pages / map changes)
    int       n_cpus;
    bool      executable;
};

/* ------------------------------------------------------------------ */
/* Code buffer                                                         */
/* ------------------------------------------------------------------ */

static uint32_t *alloc_code(size_t bytes, bool *executable)
{
#if JIT_HOST_ARM64
    int flags = MAP_PRIVATE | MAP_ANON;
#if defined(__APPLE__)
    flags |= MAP_JIT;
#endif
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE | PROT_EXEC, flags, -1, 0);
    if (p == MAP_FAILED) return NULL;
    *executable = true;
    return p;
#else
    *executable = false;
    return calloc(1, bytes);
#endif
}

// Code writes can nest (a page invalidation during translation patches old blocks).
static void write_begin(jit_t *j)
{
#if JIT_HOST_ARM64 && defined(__APPLE__)
    if (j->executable && j->write_depth++ == 0) pthread_jit_write_protect_np(0);
#else
    (void)j;
#endif
}

static void write_end(jit_t *j, uint32_t *start, size_t words)
{
#if JIT_HOST_ARM64
#if defined(__APPLE__)
    if (j->executable && --j->write_depth == 0) pthread_jit_write_protect_np(1);
    sys_icache_invalidate(start, words * 4);
#else
    (void)j;
    __builtin___clear_cache((char *)start, (char *)(start + words));
#endif
#else
    (void)j; (void)start; (void)words;
#endif
}

/* ------------------------------------------------------------------ */
/* Block cache                                                         */
/* ------------------------------------------------------------------ */

static uint32_t hash_phys(uint32_t a) { return (a * 2654435761u) >> (32 - TABLE_BITS); }

static entry_t *lookup(jit_t *j, uint32_t phys, uint32_t mode, uint32_t eip, uint32_t cs_base)
{
    for (uint32_t i = j->bucket[hash_phys(phys)]; i; i = j->pool[i - 1].next) {
        entry_t *e = &j->pool[i - 1];
        if (e->phys == phys && e->mode == mode && e->eip == eip && e->cs_base == cs_base) return e;
    }
    return NULL;
}

// Bits for the 64-byte chunks covering [off, off + len) within a page.
static uint64_t chunk_mask(uint32_t off, uint32_t len)
{
    uint32_t a = off >> 6, b = (off + len - 1) >> 6;
    uint64_t hi = b >= 63 ? ~0ull : ((1ull << (b + 1)) - 1);
    return hi & ~((1ull << a) - 1);
}

// Writes to a code page must take the slow path (so they can invalidate): take
// write permission for it out of every CPU's fast TLB.
static void add_code_page(jit_t *j, uint32_t pg)
{
    if (j->bus->page_flags[pg] & PG_CODE) return;
    mem_set_code_page(j->bus, pg << MEM_PAGE_SHIFT, true);
    for (int i = 0; i < j->n_cpus; i++) cpu_ftlb_drop_writes(j->cpus[i], pg);
}

static void on_map_change(void *opaque)
{
    jit_t *j = opaque;
    for (int i = 0; i < j->n_cpus; i++) cpu_ftlb_flush(j->cpus[i]);
}

static void register_cpu(jit_t *j, cpu_t *cpu)
{
    for (int i = 0; i < j->n_cpus; i++) if (j->cpus[i] == cpu) return;
    if (j->n_cpus < (int)(sizeof j->cpus / sizeof j->cpus[0])) j->cpus[j->n_cpus++] = cpu;
    cpu_ftlb_flush(cpu);
}

// Register a translated block: hash bucket, page lists, code chunks.
static entry_t *insert(jit_t *j, uint32_t phys, uint32_t mode, uint32_t eip, uint32_t cs_base,
                       const uint32_t pages[2], int n_pages,
                       const jit_block_info *info)
{
    uint32_t idx = j->n_entries++;
    entry_t *e = &j->pool[idx];
    uint32_t h = hash_phys(phys);
    *e = (entry_t){ .phys = phys, .mode = mode, .eip = eip, .cs_base = cs_base, .next = j->bucket[h],
                    .n_pages = (uint8_t)n_pages,
                    .valid = true, .body = (uint32_t *)info->code + PROLOGUE_WORDS, .info = *info };
    j->bucket[h] = idx + 1;
    uint32_t off = phys & 0xFFFu, len = info->guest_bytes ? info->guest_bytes : 1;
    for (int k = 0; k < n_pages; k++) {
        uint32_t pg = pages[k] >> MEM_PAGE_SHIFT;
        e->page[k] = pg;
        e->page_next[k] = j->page_head[pg];
        j->page_head[pg] = idx + 1;
        if (k == 0) j->code_bits[pg] |= chunk_mask(off, off + len > 0x1000 ? 0x1000 - off : len);
        else        j->code_bits[pg] |= chunk_mask(0, off + len - 0x1000);
        add_code_page(j, pg);
    }
    return e;
}

// Take a block out of the cache. Code that still jumps into its body (chained
// blocks) is sent back to the dispatcher instead; the eip is stored before any jump.
static void remove_entry(jit_t *j, entry_t *e)
{
    uint32_t idx = (uint32_t)(e - j->pool) + 1;
    uint32_t *pp = &j->bucket[hash_phys(e->phys)];
    while (*pp && *pp != idx) pp = &j->pool[*pp - 1].next;
    if (*pp) *pp = e->next;
    e->valid = false;
    j->inval_blocks++;
#if JIT_HOST_ARM64
    if (j->executable) {
        write_begin(j);
        *e->body = a64_enc_b((int32_t)(j->exit_stub - e->body));
        write_end(j, e->body, 1);
    }
#endif
}

static void invalidate_page(jit_t *j, uint32_t pg)
{
    for (uint32_t i = j->page_head[pg]; i; ) {
        entry_t *e = &j->pool[i - 1];
        i = e->page_next[e->page[0] == pg ? 0 : 1];
        if (e->valid) remove_entry(j, e);
    }
    j->page_head[pg] = 0;
    j->code_bits[pg] = 0;
    mem_set_code_page(j->bus, pg << MEM_PAGE_SHIFT, false);
    j->inval_pages++;
}

void jit_flush(jit_t *j)
{
    j->flush_other++;
    j->flush_gen++;
    for (uint32_t i = 0; i < j->n_entries; i++) {
        entry_t *e = &j->pool[i];
        for (int k = 0; k < e->n_pages; k++) {
            j->page_head[e->page[k]] = 0;
            j->code_bits[e->page[k]] = 0;
            mem_set_code_page(j->bus, e->page[k] << MEM_PAGE_SHIFT, false);
        }
    }
    memset(j->bucket, 0, sizeof(uint32_t) * TABLE_SIZE);
    j->n_entries = 0;
    for (int i = 0; i < j->n_cpus; i++) memset(j->cpus[i]->jc, 0, sizeof j->cpus[i]->jc);   // bodies are reused
    j->top = j->buf + j->stub_words;
    j->arena_cur = j->arena_head;
    if (j->arena_head) j->arena_head->used = 0;
}

static jit_hrec_t *arena_alloc(jit_t *j)
{
    if (!j->arena_cur) {
        j->arena_head = j->arena_cur = calloc(1, sizeof(arena_chunk_t));
        if (!j->arena_cur) return NULL;
    }
    if (j->arena_cur->used == ARENA_CHUNK) {
        if (!j->arena_cur->next && !(j->arena_cur->next = calloc(1, sizeof(arena_chunk_t))))
            return NULL;
        j->arena_cur = j->arena_cur->next;
        j->arena_cur->used = 0;
    }
    return &j->arena_cur->recs[j->arena_cur->used++];
}

// The bus calls this before a write to a page holding translated code. Only writes
// that touch a 64-byte chunk with code in it invalidate (the page's blocks).
static bool on_code_write(void *opaque, uint32_t addr, uint32_t len)
{
    jit_t *j = opaque;
    uint32_t pg = addr >> MEM_PAGE_SHIFT;
    if (!(j->code_bits[pg] & chunk_mask(addr & 0xFFFu, len ? len : 1))) return false;
    invalidate_page(j, pg);
    return true;
}

/* ------------------------------------------------------------------ */
/* Translation                                                         */
/* ------------------------------------------------------------------ */

/* Guest register cache. Within a block up to RC_N guest GPRs live in x25..x28 (callee-saved,
 * so they survive C calls). Bindings are made only at the start of an instruction (on the
 * block's main path, never inside a branch), so a bound register holds the current value on
 * every path. Invariant: at block entry and exit all guest state is in cpu_t. Dirty registers
 * are written back before every C call (helpers read cpu_t, and faults longjmp out) and every
 * exit; after every call all bound registers are reloaded (helpers may change them). "Dirty"
 * is conservative: a write on a side path marks it, and it stays set after a write-back. */
#define RC_N 4
typedef struct {
    int8_t   slot_of[8];   // guest register -> slot + 1
    int8_t   guest[RC_N];  // slot -> guest register + 1
    bool     dirty[RC_N];
    uint32_t last[RC_N];   // instruction number of the last use (LRU)
} regcache_t;

typedef struct {
    emit_t     e;
    jit_t     *j;
    uint32_t   eip;        // guest eip of the instruction being translated
    bool       code32;     // 32-bit code segment
    bool       ss32;       // 32-bit stack segment
    bool       user;       // CPL 3 (part of the block key): picks the fast-TLB permission tags
    uint32_t   cs_base;    // CS base the block was translated under
    uint32_t   lin_page;   // linear page of the block (chained targets must be in it)
    int        cc_known;   // cc_op the last inline instruction left (0 = unknown / materialized)
    uint32_t   mode;       // block_mode() the block is translated under
    bool       dyn_exit;   // block ends with a computed eip (ret, indirect jmp/call)
    bool       wrote_mem;  // the current instruction stores to memory
    jit_exit_t exit;
    uint32_t   insns, bytes;
    regcache_t rc;         // guest registers held in x25..x28 (zero = none)
} ctx_t;

typedef enum { ST_NEXT, ST_END, ST_FAIL, ST_INVALID } step_t;

/* Register use in generated code:
 *   x19       cpu_t* (callee-saved, set up by the prologue)
 *   w20..w24  values that must survive helper calls (callee-saved, saved by the prologue)
 *   w9..w12   short-lived scratch, never live across a helper call
 *   x0..x3    helper arguments / return value, x16 helper address
 */
enum { R_CPU = 19, S_EA = 20, S_A = 21, S_B = 22, S_R = 23, S_X = 24,
       T0 = 9, T1 = 10, R_CALL = 16 };

#define FRAME_SIZE 96   // x19..x24, x30, x25..x28 (the register cache), rounded up to 16

/* ---- C helpers called from generated code ---- */
// Slow paths of generated memory accesses; each refills the fast TLB for next time.
static uint32_t h_read(cpu_t *cpu, uint32_t lin, uint32_t size)
{
    uint32_t v = cpu_read_lin(cpu, lin, (int)size);
    cpu_ftlb_fill(cpu, lin, false);
    return v;
}
// Notes in cpu->smc_hit when the write modified translated code: the block then
// stops after the current instruction so the next one is fetched again.
static void h_write(cpu_t *cpu, uint32_t lin, uint32_t val, uint32_t size)
{
    struct mem_bus *bus = cpu->mem;
    bus->code_written = false;
    cpu_write_lin(cpu, lin, val, (int)size);
    if (bus->code_written) cpu->smc_hit = true;
    else cpu_ftlb_fill(cpu, lin, true);
}
static uint32_t h_cond(cpu_t *cpu, uint32_t cond)
{
    return cpu_eval_cond(cpu, (int)cond) ? 1u : 0u;
}

/* ---- emit helpers ---- */
static void emit(ctx_t *c, uint32_t w) { a64_emit(&c->e, w); }
static void mov_imm(ctx_t *c, int rd, uint32_t v) { a64_mov_imm32(&c->e, rd, v); }
static void ld(ctx_t *c, int rt, uint32_t off) { emit(c, a64_enc_ldr_w(rt, R_CPU, off)); }
static void st(ctx_t *c, int rt, uint32_t off) { emit(c, a64_enc_str_w(rt, R_CPU, off)); }

static void prologue(ctx_t *c)
{
    emit(c, a64_enc_stp_pre(19, 20, A64_SP, -FRAME_SIZE));
    emit(c, a64_enc_stp_off(21, 22, A64_SP, 16));
    emit(c, a64_enc_stp_off(23, 24, A64_SP, 32));
    emit(c, a64_enc_str_x(30, A64_SP, 48));
    emit(c, a64_enc_stp_off(25, 26, A64_SP, 64));
    emit(c, a64_enc_stp_off(27, 28, A64_SP, 80));
    emit(c, a64_enc_mov_x(R_CPU, 0));
}

static const int RC_HOST[RC_N] = { 25, 26, 27, 28 };

// Write dirty cached registers back to cpu_t (no change to the cache state).
static void rc_flush(ctx_t *c)
{
    for (int k = 0; k < RC_N; k++)
        if (c->rc.guest[k] && c->rc.dirty[k]) st(c, RC_HOST[k], OFF_GPR(c->rc.guest[k] - 1));
}

static void rc_reload(ctx_t *c)
{
    for (int k = 0; k < RC_N; k++)
        if (c->rc.guest[k]) ld(c, RC_HOST[k], OFF_GPR(c->rc.guest[k] - 1));
}

#define EPILOGUE_W0_WORDS 7

// Restore the frame and return whatever is in w0. No write-back: it follows a helper call,
// before which the cache was written back (and cpu_t is what the helper left).
static void epilogue_w0(ctx_t *c)
{
    emit(c, a64_enc_ldp_off(25, 26, A64_SP, 64));
    emit(c, a64_enc_ldp_off(27, 28, A64_SP, 80));
    emit(c, a64_enc_ldr_x(30, A64_SP, 48));
    emit(c, a64_enc_ldp_off(23, 24, A64_SP, 32));
    emit(c, a64_enc_ldp_off(21, 22, A64_SP, 16));
    emit(c, a64_enc_ldp_post(19, 20, A64_SP, FRAME_SIZE));
    emit(c, A64_RET);
}

static void epilogue(ctx_t *c, jit_exit_t exit)
{
    rc_flush(c);
    emit(c, a64_enc_movz(0, (uint16_t)exit, 0));
    emit(c, a64_enc_ldp_off(25, 26, A64_SP, 64));
    emit(c, a64_enc_ldp_off(27, 28, A64_SP, 80));
    emit(c, a64_enc_ldr_x(30, A64_SP, 48));
    emit(c, a64_enc_ldp_off(23, 24, A64_SP, 32));
    emit(c, a64_enc_ldp_off(21, 22, A64_SP, 16));
    emit(c, a64_enc_ldp_post(19, 20, A64_SP, FRAME_SIZE));
    emit(c, A64_RET);
    c->exit = exit;
}

// End the block at a known eip.
static void finish(ctx_t *c, jit_exit_t exit)
{
    mov_imm(c, T0, c->eip);
    st(c, T0, OFF_EIP);
    epilogue(c, exit);
}

static void finish_at(ctx_t *c, uint32_t eip, jit_exit_t exit)
{
    mov_imm(c, T0, eip);
    st(c, T0, OFF_EIP);
    epilogue(c, exit);
}

static void finish_dyn(ctx_t *c, int r);
static uint32_t block_mode(const cpu_t *cpu);
static void add_const(ctx_t *c, int rd, int rn, uint32_t imm);

// Forward branch whose target is patched later.
typedef struct { uint32_t *site; uint32_t word; } fixup_t;

static fixup_t branch_fwd(ctx_t *c, uint32_t word)
{
    fixup_t f = { c->e.p, word };
    emit(c, word);
    return f;
}

// Point a forward conditional branch (B.cond / CBZ / CBNZ) at the current position.
static void patch_here(ctx_t *c, fixup_t f)
{
    if (c->e.overflow || f.site >= c->e.end) return;   // block is discarded anyway
    int32_t words = (int32_t)(c->e.p - f.site);
    *f.site = f.word | (((uint32_t)words & 0x7FFFFu) << 5);
}

// Same for an unconditional B.
static void patch_b_here(ctx_t *c, fixup_t f)
{
    if (c->e.overflow || f.site >= c->e.end) return;
    *f.site = a64_enc_b((int32_t)(c->e.p - f.site));
}

/* ---- block chaining ----
 * A direct jump (jmp/call rel, both sides of jcc/loop, falling off a full block)
 * to an eip in the block's own linear page ends with a chainable exit:
 *
 *       store eip
 *       if (--chain_budget <= 0) goto plain       (dispatcher bookkeeping, time slices)
 *       if (intr_line && IF) goto plain            (interrupt deliverable)
 *       if (exit_request) goto plain
 *   slot: b unlinked                               <- patched to "b <next block's body>"
 *   unlinked: chain_from = &slot
 *   plain: return JIT_EXIT_OK
 *
 * The dispatcher patches the slot once it has the next block. Same-page targets
 * keep chains valid in every address space: the target's physical address is a
 * fixed offset from the source block's, and invalidating a page drops both.
 * Invalidated targets have their body's first word turned into a jump to the exit
 * stub (remove_entry()), which is fine because the eip is stored first. */
// The checks before leaving a block for another without the dispatcher (uses w10).
static void exit_checks(ctx_t *c, fixup_t plain[3])
{
    ld(c, T1, OFF_CHAIN_BUDGET);
    emit(c, a64_enc_subs_imm(T1, T1, 1));
    st(c, T1, OFF_CHAIN_BUDGET);
    plain[0] = branch_fwd(c, a64_enc_bcond(A64_LE, 0));
    emit(c, a64_enc_ldrb(T1, R_CPU, OFF_INTR_LINE));
    emit(c, a64_enc_cbz_w(T1, 4));                           // no INTR: skip the IF test
    ld(c, T1, OFF_EFLAGS);
    emit(c, a64_enc_ubfx(T1, T1, 9, 1));                     // IF
    plain[1] = branch_fwd(c, a64_enc_cbnz_w(T1, 0));
    emit(c, a64_enc_ldrb(T1, R_CPU, OFF_EXIT_REQ));
    plain[2] = branch_fwd(c, a64_enc_cbnz_w(T1, 0));
}

static void finish_chain(ctx_t *c, uint32_t eip)
{
    if (!c->j->chaining || c->j->single_step) {
        finish_at(c, eip, JIT_EXIT_OK);
        return;
    }
    if (((c->cs_base + eip) & ~0xFFFu) != c->lin_page) {
        // Another page: its physical address depends on the address space, so no
        // permanent link; the jump cache (keyed by linear address) finds it instead.
        bool dyn = c->dyn_exit;
        mov_imm(c, S_B, eip);                                // S registers are dead at a block exit
        finish_dyn(c, S_B);
        c->dyn_exit = dyn;
        return;
    }
    fixup_t plain[3];
    rc_flush(c);                                             // the next block starts from cpu_t
    mov_imm(c, T0, eip);
    st(c, T0, OFF_EIP);
    exit_checks(c, plain);
    emit(c, a64_enc_b(1));                                   // the slot
    emit(c, a64_enc_adr(10, -4));
    emit(c, a64_enc_str_x(10, R_CPU, OFF_CHAIN_FROM));
    for (int i = 0; i < 3; i++) patch_here(c, plain[i]);
    epilogue(c, JIT_EXIT_OK);
}

/* End the block at a computed eip held in register `r` (ret, indirect jmp/call).
 * With the jump cache: after the same checks as a chained exit, look the target's
 * linear address up in cpu->jc (hash: lin + lin >> 12, 12 bits) and, when its lin
 * and mode match this block's mode (near transfers keep the mode), branch straight
 * to the cached body. Otherwise return to the dispatcher, which fills the entry. */
static void finish_dyn(ctx_t *c, int r)
{
    rc_flush(c);
    st(c, r, OFF_EIP);
    c->dyn_exit = true;
    if (!c->j->jump_cache || c->j->single_step) { epilogue(c, JIT_EXIT_OK); return; }
    fixup_t plain[5];
    exit_checks(c, plain);
    add_const(c, T0, r, c->cs_base);                         // w9 = linear target
    emit(c, a64_enc_alu_lsr(A64_OP_ADD, T1, T0, T0, 12));
    emit(c, a64_enc_ubfx(T1, T1, 0, 12));
    emit(c, a64_enc_ldr_x(11, R_CPU, OFF_JC_BASE));
    emit(c, a64_enc_add_x_lsl(11, 11, T1, 4));
    emit(c, a64_enc_ldr_w(12, 11, 0));
    emit(c, a64_enc_cmp_w(12, T0));
    plain[3] = branch_fwd(c, a64_enc_bcond(A64_NE, 0));
    emit(c, a64_enc_ldr_w(12, 11, 4));
    mov_imm(c, T1, c->mode);
    emit(c, a64_enc_cmp_w(12, T1));
    plain[4] = branch_fwd(c, a64_enc_bcond(A64_NE, 0));
    emit(c, a64_enc_ldr_x(12, 11, 8));
    emit(c, a64_enc_br(12));
    for (int i = 0; i < 5; i++) patch_here(c, plain[i]);
    epilogue(c, JIT_EXIT_OK);
}

// Call a C helper. x0 = cpu is set here; other arguments must already be in place.
// What a C callee does with guest registers decides the register cache's work around the call:
// write back first (it reads them, or may fault: delivery and longjmp need cpu_t current),
// reload after (it may change them). Callee-saved x25..x28 survive the call either way.
static bool reads_gprs(const void *fn) { return fn != (const void *)h_cond && fn != (const void *)cpu_get_cf &&
                                                fn != (const void *)cpu_flush_flags && fn != (const void *)fpu_jit_mem; }
static bool writes_gprs(const void *fn) { return fn == (const void *)helper_exec || fn == (const void *)fpu_jit_reg; }

static void call(ctx_t *c, const void *fn)
{
    if (reads_gprs(fn)) rc_flush(c);
    emit(c, a64_enc_mov_x(0, R_CPU));
    a64_mov_imm64(&c->e, R_CALL, (uint64_t)(uintptr_t)fn);
    emit(c, a64_enc_blr(R_CALL));
    if (writes_gprs(fn)) rc_reload(c);
}

// Page faults report the faulting instruction: store its eip before a memory access.
static void store_eip(ctx_t *c)
{
    mov_imm(c, T0, c->eip);
    st(c, T0, OFF_EIP);
}

/* ---- inline memory fast path ----
 * Look the linear address in `addr` up in the fast TLB (cpu.h). On a hit, x11 =
 * host address - linear address and execution falls through; misses (and accesses
 * that cross a page) branch to fixups the caller points at the slow path.
 * Uses w10..w12 only. */
static int ftlb_lookup(ctx_t *c, int addr, int size, bool write, fixup_t miss[2])
{
    int n = 0;
    emit(c, a64_enc_ubfx(T1, addr, 12, 10));                 // index
    emit(c, a64_enc_ldr_x(11, R_CPU, OFF_FTLB_BASE));
    emit(c, a64_enc_add_x_lsl(11, 11, T1, 5));
    emit(c, a64_enc_ldr_w(12, 11, 4u * (uint32_t)((c->user ? 2 : 0) + (write ? 1 : 0))));
    emit(c, a64_enc_lsr_imm(T1, addr, 12));
    emit(c, a64_enc_add_imm(T1, T1, 1));
    emit(c, a64_enc_cmp_w(12, T1));
    miss[n++] = branch_fwd(c, a64_enc_bcond(A64_NE, 0));
    if (size > 1) {
        emit(c, a64_enc_ubfx(T1, addr, 0, 12));
        emit(c, a64_enc_cmp_imm(T1, 0x1000u - (uint32_t)size));
        miss[n++] = branch_fwd(c, a64_enc_bcond(A64_HI, 0));
    }
    emit(c, a64_enc_ldr_x(11, 11, 16));                      // addend
    return n;
}

// w0 = guest memory[addr], size bytes, zero-extended.
static void call_read(ctx_t *c, int addr, int size)
{
    fixup_t miss[2], done = { 0 };
    int n = 0;
    if (c->j->fastmem) {
        n = ftlb_lookup(c, addr, size, false, miss);
        emit(c, a64_enc_ldr_uxtw(size, 0, 11, addr));
        done = branch_fwd(c, a64_enc_b(0));
        for (int i = 0; i < n; i++) patch_here(c, miss[i]);
    }
    store_eip(c);                                            // page faults report this instruction
    emit(c, a64_enc_mov_w(1, addr));
    emit(c, a64_enc_movz(2, (uint16_t)size, 0));
    call(c, (const void *)h_read);
    if (c->j->fastmem) patch_b_here(c, done);
}

static void call_write(ctx_t *c, int addr, int val, int size)
{
    fixup_t miss[2], done = { 0 };
    int n = 0;
    if (c->j->fastmem) {
        n = ftlb_lookup(c, addr, size, true, miss);
        emit(c, a64_enc_str_uxtw(size, val, 11, addr));
        done = branch_fwd(c, a64_enc_b(0));
        for (int i = 0; i < n; i++) patch_here(c, miss[i]);
    }
    store_eip(c);
    emit(c, a64_enc_mov_w(1, addr));
    emit(c, a64_enc_mov_w(2, val));
    emit(c, a64_enc_movz(3, (uint16_t)size, 0));
    call(c, (const void *)h_write);
    if (c->j->fastmem) patch_b_here(c, done);
    c->wrote_mem = true;
}

// rd = carry flag (0/1), via the lazy-flag evaluator.
/* ---- inline flag evaluation ----
 * When the translator knows which lazy-flag op is current (c->cc_known, set by
 * set_cc() earlier in this block and not disturbed since), conditions are computed
 * from the stored fields with one ARM64 compare. Narrow ops are shifted to the top
 * of the register first, so N/Z/C/V describe the 8/16-bit operation exactly:
 *   SUB/CMP/NEG  CMP  src1, src2   (ARM C = !borrow, so x86 B = LO)
 *   ADD          CMN  src1, src2   (ARM C = carry,   so x86 B = HS)
 *   LOGIC        TST  dst, dst     (C = V = 0, as x86 leaves CF = OF = 0)
 *   anything     TST  dst, dst     for ZF/SF conditions only
 * Everything else (parity, ADC/SBB/INC/DEC/shift conditions on CF/OF) calls C. */
enum { K_ADD, K_ADC, K_SUB, K_SBB, K_LOGIC, K_INC, K_DEC, K_SHL, K_SHR, K_SAR, K_MUL };

// T0 = cc field at `off`, shifted so the op's sign bit is bit 31.
static void ld_cc_top(ctx_t *c, int rd, uint32_t off, int sh)
{
    ld(c, rd, off);
    if (sh) emit(c, a64_enc_lsl_imm(rd, rd, sh));
}

// Set the ARM flags for the known op. Returns its kind, or -1 if not handled.
static int cc_compare(ctx_t *c, bool result_only)
{
    if (c->cc_known <= 0 || c->cc_known >= CC_OP__COUNT) return -1;
    int idx = c->cc_known - 1, kind = idx / 3, sh = 32 - (8 << (idx % 3));
    if (!result_only && (kind == K_SUB || kind == K_ADD)) {
        ld_cc_top(c, T0, OFF_CC_SRC1, sh);
        ld_cc_top(c, T1, OFF_CC_SRC2, sh);
        emit(c, kind == K_SUB ? a64_enc_cmp_w(T0, T1) : a64_enc_cmn_w(T0, T1));
        return kind;
    }
    ld_cc_top(c, T0, OFF_CC_DST, sh);
    emit(c, a64_enc_tst_w(T0, T0));
    return kind == K_LOGIC ? K_LOGIC : -2;   // -2: only ZF/SF are meaningful
}

// w0 = x86 condition `cond` inline, if the known op allows it.
static bool inline_cond(ctx_t *c, int cond)
{
    int base = (cond & 15) >> 1;               // O B E BE S P L LE
    bool neg = cond & 1;
    if (base == 5 || c->cc_known <= 0) return false;
    int idx = c->cc_known - 1, kind = idx / 3;
    static const int8_t SUBMAP[8] = { A64_VS, A64_LO, A64_EQ, A64_LS, A64_MI, -1, A64_LT, A64_LE };
    int arm;
    if (kind == K_SUB || kind == K_ADD) {
        if (kind == K_ADD && base == 3) return false;           // BE/A: CF || ZF has no ARM twin after CMN
        cc_compare(c, false);
        arm = (kind == K_ADD && base == 1) ? A64_HS : SUBMAP[base];
    } else if (kind == K_LOGIC) {
        if (base == 1) {                                     // B / AE: CF is 0
            emit(c, a64_enc_movz(0, neg ? 1 : 0, 0));
            return true;
        }
        cc_compare(c, true);
        arm = base == 3 ? A64_EQ : SUBMAP[base];             // BE = ZF when CF = 0
    } else {
        if (base != 2 && base != 4) return false;            // E / S from the result
        cc_compare(c, true);
        arm = SUBMAP[base];
    }
    emit(c, a64_enc_cset(0, neg ? (arm ^ 1) : arm));
    return true;
}

// rd = carry flag (0/1): inline for the common known ops, else via the lazy-flag evaluator.
static void get_cf(ctx_t *c, int rd)
{
    int kind = c->cc_known > 0 && c->cc_known < CC_OP__COUNT ? (c->cc_known - 1) / 3 : -1;
    if (kind == K_SUB || kind == K_ADD) {
        cc_compare(c, false);
        emit(c, a64_enc_cset(rd, kind == K_SUB ? A64_LO : A64_HS));
    } else if (kind == K_LOGIC) {
        emit(c, a64_enc_mov_w(rd, A64_ZR));
    } else if (kind == K_INC || kind == K_DEC || kind == K_MUL) {
        ld(c, rd, OFF_CC_SRC1);                              // the CF INC/DEC preserved; MUL's overflow
    } else {
        call(c, (const void *)cpu_get_cf);
        emit(c, a64_enc_mov_w(rd, 0));
    }
}

// rd = rn + imm (any 32-bit imm, may use T1)
static void add_const(ctx_t *c, int rd, int rn, uint32_t imm)
{
    if (imm == 0)              { if (rd != rn) emit(c, a64_enc_mov_w(rd, rn)); }
    else if (imm < 4096)       emit(c, a64_enc_add_imm(rd, rn, imm));
    else if (-imm < 4096)      emit(c, a64_enc_sub_imm(rd, rn, -imm));
    else { mov_imm(c, T1, imm); emit(c, a64_enc_alu(A64_OP_ADD, rd, rn, T1)); }
}

/* ---- guest register access by size (cpu_t, or the register cache) ---- */
// Byte registers 4..7 are AH CH DH BH: byte 1 of EAX..EBX.
static uint32_t off_reg8(int r) { return r < 4 ? OFF_GPR(r) : OFF_GPR(r - 4) + 1; }

// Guest register (0..7) a size/number pair refers to.
static int gpr_of(int r, int size) { return size == 1 && r >= 4 ? r - 4 : r; }

static int rc_host(ctx_t *c, int g)                         // host register, or -1
{
    int k = c->rc.slot_of[g] - 1;
    if (k < 0) return -1;
    c->rc.last[k] = c->insns + 1;
    return RC_HOST[k];
}

static void ld_reg(ctx_t *c, int rd, int r, int size)
{
    int h = rc_host(c, gpr_of(r, size));
    if (h >= 0) {
        if (size == 4)      emit(c, a64_enc_mov_w(rd, h));
        else if (size == 2) emit(c, a64_enc_uxth(rd, h));
        else if (r < 4)     emit(c, a64_enc_uxtb(rd, h));
        else                emit(c, a64_enc_ubfx(rd, h, 8, 8));
        return;
    }
    if (size == 4)      ld(c, rd, OFF_GPR(r));
    else if (size == 2) emit(c, a64_enc_ldrh(rd, R_CPU, OFF_GPR(r)));
    else                emit(c, a64_enc_ldrb(rd, R_CPU, off_reg8(r)));
}

static void st_reg(ctx_t *c, int rs, int r, int size)
{
    int g = gpr_of(r, size), h = rc_host(c, g);
    if (h >= 0) {
        c->rc.dirty[c->rc.slot_of[g] - 1] = true;
        if (size == 4)      emit(c, a64_enc_mov_w(h, rs));
        else if (size == 2) emit(c, a64_enc_bfi(h, rs, 0, 16));
        else                emit(c, a64_enc_bfi(h, rs, r < 4 ? 0 : 8, 8));
        return;
    }
    if (size == 4)      st(c, rs, OFF_GPR(r));
    else if (size == 2) emit(c, a64_enc_strh(rs, R_CPU, OFF_GPR(r)));
    else                emit(c, a64_enc_strb(rs, R_CPU, off_reg8(r)));
}

// Bind guest register g to a slot (at an instruction start only): a free slot, else the least
// recently used one not used by this instruction (written back if dirty). No-op if bound.
static void rc_bind(ctx_t *c, int g)
{
    uint32_t now = c->insns + 1;
    if (c->rc.slot_of[g]) { c->rc.last[c->rc.slot_of[g] - 1] = now; return; }
    int k = -1;
    for (int i = 0; i < RC_N && k < 0; i++) if (!c->rc.guest[i]) k = i;
    if (k < 0)
        for (int i = 0; i < RC_N; i++)
            if (c->rc.last[i] != now && (k < 0 || c->rc.last[i] < c->rc.last[k])) k = i;
    if (k < 0) return;                                       // all slots in use by this instruction
    if (c->rc.guest[k]) {
        int old = c->rc.guest[k] - 1;
        if (c->rc.dirty[k]) st(c, RC_HOST[k], OFF_GPR(old));
        c->rc.slot_of[old] = 0;
    }
    ld(c, RC_HOST[k], OFF_GPR(g));
    c->rc.guest[k] = (int8_t)(g + 1);
    c->rc.slot_of[g] = (int8_t)(k + 1);
    c->rc.dirty[k] = false;
    c->rc.last[k] = now;
}

// Bind the registers an instruction names (operands, address registers, the stack pointer for
// stack instructions...). Anything not bound is simply read and written in cpu_t.
static void rc_bind_insn(ctx_t *c, const insn_t *in)
{
    if (!c->j->regcache) return;
    const operand_t *ops[3] = { &in->dst, &in->src, &in->src2 };
    bool mem = false;
    for (int i = 0; i < 3; i++) {
        if (ops[i]->kind == OPK_REG) rc_bind(c, gpr_of(ops[i]->reg, ops[i]->size));
        if (ops[i]->kind == OPK_MEM) mem = true;
    }
    if (in->op == OP_FPU && in->mod != 3) mem = true;
    if (mem || in->op == OP_LEA) {
        if (in->mem.base >= 0) rc_bind(c, in->mem.base);
        if (in->mem.index >= 0) rc_bind(c, in->mem.index);
    }
    switch (in->op) {
    case OP_PUSH: case OP_POP: case OP_CALL: case OP_RET: rc_bind(c, R_ESP); break;
    case OP_LEAVE: rc_bind(c, R_ESP); rc_bind(c, R_EBP); break;
    case OP_MUL: case OP_IMUL: if (in->src.kind == OPK_NONE) { rc_bind(c, R_EAX); rc_bind(c, R_EDX); } break;
    case OP_LOOP: case OP_LOOPE: case OP_LOOPNE: case OP_JCXZ: rc_bind(c, R_ECX); break;
    case OP_SHL: case OP_SHR: case OP_SAR: if (in->src.kind != OPK_IMM) rc_bind(c, R_ECX); break;
    default: break;
    }
}

/* ---- effective address ---- */
// rd = offset of the memory operand; with_seg adds the segment base (linear address).
static void emit_ea(ctx_t *c, const insn_t *in, int rd, bool with_seg)
{
    const memref_t *m = &in->mem;
    bool have = false;
    if (m->base >= 0) { ld_reg(c, rd, m->base, 4); have = true; }
    if (m->index >= 0) {
        ld_reg(c, T0, m->index, 4);
        if (have)          emit(c, a64_enc_alu_lsl(A64_OP_ADD, rd, rd, T0, m->shift));
        else if (m->shift) emit(c, a64_enc_lsl_imm(rd, T0, m->shift));
        else               emit(c, a64_enc_mov_w(rd, T0));
        have = true;
    }
    if (!have) mov_imm(c, rd, (uint32_t)m->disp);
    else       add_const(c, rd, rd, (uint32_t)m->disp);
    if (in->addrsize == 2) emit(c, a64_enc_uxth(rd, rd));
    if (with_seg) {
        ld(c, T0, OFF_SEG_BASE(m->seg));
        emit(c, a64_enc_alu(A64_OP_ADD, rd, rd, T0));
    }
}

static bool has_mem(const insn_t *in)
{
    return in->dst.kind == OPK_MEM || in->src.kind == OPK_MEM;
}

/* ---- generic operand load/store (memory operands use the address in S_EA) ---- */
static bool load_op(ctx_t *c, const insn_t *in, const operand_t *o, int rd)
{
    switch (o->kind) {
    case OPK_REG: ld_reg(c, rd, o->reg, o->size); return true;
    case OPK_IMM: mov_imm(c, rd, in->imm); return true;
    case OPK_MEM: call_read(c, S_EA, o->size); emit(c, a64_enc_mov_w(rd, 0)); return true;
    case OPK_SEG: emit(c, a64_enc_ldrh(rd, R_CPU, OFF_SEG_SEL(o->reg))); return true;
    default: return false;
    }
}

static bool store_op(ctx_t *c, const operand_t *o, int rs)
{
    switch (o->kind) {
    case OPK_REG: st_reg(c, rs, o->reg, o->size); return true;
    case OPK_MEM: call_write(c, S_EA, rs, o->size); return true;
    default: return false;
    }
}

/* ---- lazy flags ---- */
static int size_idx(int size) { return size == 1 ? 0 : size == 2 ? 1 : 2; }

// Record cc_op (family base + size) and the operand fields. Pass -1 to skip a field.
static void set_cc(ctx_t *c, cc_op_t family8, int size, int dst, int src1, int src2)
{
    c->cc_known = (int)family8 + size_idx(size);
    mov_imm(c, T0, (uint32_t)family8 + (uint32_t)size_idx(size));
    st(c, T0, OFF_CC_OP);
    if (dst >= 0)  st(c, dst, OFF_CC_DST);
    if (src1 >= 0) st(c, src1, OFF_CC_SRC1);
    if (src2 >= 0) st(c, src2, OFF_CC_SRC2);
}

/* ---- stack ---- */
// Push the value in `val` (n = 2 or 4 bytes). Uses S_EA and S_X.
static void push_reg(ctx_t *c, int val, int n)
{
    ld_reg(c, S_X, R_ESP, 4);
    emit(c, a64_enc_sub_imm(S_X, S_X, (uint32_t)n));
    if (!c->ss32) emit(c, a64_enc_uxth(S_X, S_X));
    ld(c, T0, OFF_SEG_BASE(S_SS));
    emit(c, a64_enc_alu(A64_OP_ADD, S_EA, S_X, T0));
    call_write(c, S_EA, val, n);
    st_reg(c, S_X, R_ESP, c->ss32 ? 4 : 2);   // update ESP only after the write
}

// Pop n bytes into rd (must not be S_EA or S_X).
static void pop_reg(ctx_t *c, int rd, int n)
{
    ld_reg(c, S_X, R_ESP, 4);
    if (!c->ss32) emit(c, a64_enc_uxth(S_X, S_X));
    ld(c, T0, OFF_SEG_BASE(S_SS));
    emit(c, a64_enc_alu(A64_OP_ADD, S_EA, S_X, T0));
    call_read(c, S_EA, n);
    emit(c, a64_enc_mov_w(rd, 0));
    emit(c, a64_enc_add_imm(S_X, S_X, (uint32_t)n));
    st_reg(c, S_X, R_ESP, c->ss32 ? 4 : 2);
}

// ESP += imm (respecting the stack size).
static void adjust_sp(ctx_t *c, uint32_t imm)
{
    ld_reg(c, S_X, R_ESP, 4);
    add_const(c, S_X, S_X, imm);
    st_reg(c, S_X, R_ESP, c->ss32 ? 4 : 2);
}

/* ---- instruction translators ---- */

static step_t tr_mov(ctx_t *c, const insn_t *in)
{
    // Segment / control / debug register writes need system semantics (slice 2b).
    if (in->dst.kind != OPK_REG && in->dst.kind != OPK_MEM) return ST_FAIL;
    if (in->src.kind == OPK_CR || in->src.kind == OPK_DR) return ST_FAIL;
    if (has_mem(in)) emit_ea(c, in, S_EA, true);
    if (!load_op(c, in, &in->src, S_A)) return ST_FAIL;
    return store_op(c, &in->dst, S_A) ? ST_NEXT : ST_FAIL;
}

// ADD ADC SUB SBB CMP AND OR XOR TEST, all sizes, reg/mem/imm.
static step_t tr_alu(ctx_t *c, const insn_t *in)
{
    uint32_t opbase;
    cc_op_t cc;
    bool store = true, arith = true, carry = false;
    switch (in->op) {
    case OP_ADD:  opbase = A64_OP_ADD; cc = CC_OP_ADD8; break;
    case OP_ADC:  opbase = A64_OP_ADD; cc = CC_OP_ADC8; carry = true; break;
    case OP_SUB:  opbase = A64_OP_SUB; cc = CC_OP_SUB8; break;
    case OP_SBB:  opbase = A64_OP_SUB; cc = CC_OP_SBB8; carry = true; break;
    case OP_CMP:  opbase = A64_OP_SUB; cc = CC_OP_SUB8; store = false; break;
    case OP_AND:  opbase = A64_OP_AND; cc = CC_OP_LOGIC8; arith = false; break;
    case OP_OR:   opbase = A64_OP_ORR; cc = CC_OP_LOGIC8; arith = false; break;
    case OP_XOR:  opbase = A64_OP_EOR; cc = CC_OP_LOGIC8; arith = false; break;
    case OP_TEST: opbase = A64_OP_AND; cc = CC_OP_LOGIC8; arith = false; store = false; break;
    default: return ST_FAIL;
    }
    int size = in->dst.size;

    if (has_mem(in)) emit_ea(c, in, S_EA, true);
    if (carry) get_cf(c, S_X);
    if (!load_op(c, in, &in->dst, S_A)) return ST_FAIL;
    if (!load_op(c, in, &in->src, S_B)) return ST_FAIL;
    emit(c, a64_enc_alu(opbase, S_R, S_A, S_B));
    if (carry) emit(c, a64_enc_alu(opbase, S_R, S_R, S_X));
    if (store && !store_op(c, &in->dst, S_R)) return ST_FAIL;
    set_cc(c, cc, size, S_R, arith ? S_A : -1, arith ? S_B : -1);
    return ST_NEXT;
}

static step_t tr_incdec(ctx_t *c, const insn_t *in)
{
    bool inc = in->op == OP_INC;
    if (has_mem(in)) emit_ea(c, in, S_EA, true);
    get_cf(c, S_X);                         // INC/DEC preserve CF: keep the old one in cc_src1
    if (!load_op(c, in, &in->dst, S_A)) return ST_FAIL;
    emit(c, inc ? a64_enc_add_imm(S_R, S_A, 1) : a64_enc_sub_imm(S_R, S_A, 1));
    if (!store_op(c, &in->dst, S_R)) return ST_FAIL;
    set_cc(c, inc ? CC_OP_INC8 : CC_OP_DEC8, in->dst.size, S_R, S_X, -1);
    return ST_NEXT;
}

static step_t tr_notneg(ctx_t *c, const insn_t *in)
{
    if (has_mem(in)) emit_ea(c, in, S_EA, true);
    if (!load_op(c, in, &in->dst, S_B)) return ST_FAIL;
    if (in->op == OP_NOT) {
        emit(c, a64_enc_mvn_w(S_R, S_B));
        return store_op(c, &in->dst, S_R) ? ST_NEXT : ST_FAIL;   // NOT leaves flags alone
    }
    emit(c, a64_enc_alu(A64_OP_SUB, S_R, A64_ZR, S_B));       // NEG = 0 - x, flags as SUB
    if (!store_op(c, &in->dst, S_R)) return ST_FAIL;
    set_cc(c, CC_OP_SUB8, in->dst.size, S_R, -1, S_B);
    st(c, A64_ZR, OFF_CC_SRC1);
    return ST_NEXT;
}

// SHL / SHR / SAR r/m by an immediate or CL (all sizes). A zero count changes nothing,
// not even flags: with CL that is a runtime test, after which the lazy-flag op is unknown.
static step_t tr_shift(ctx_t *c, const insn_t *in)
{
    int size = in->dst.size;
    bool by_cl = in->src.kind == OPK_REG;
    uint32_t n = in->imm & 31u;
    if (!by_cl && n == 0) return ST_NEXT;
    cc_op_t fam = in->op == OP_SHL ? CC_OP_SHL8 : in->op == OP_SHR ? CC_OP_SHR8 : CC_OP_SAR8;
    if (has_mem(in)) emit_ea(c, in, S_EA, true);
    fixup_t zero = { 0 };
    if (by_cl) {
        ld_reg(c, S_B, R_ECX, 1);
        emit(c, a64_enc_ubfx(S_B, S_B, 0, 5));
        zero = branch_fwd(c, a64_enc_cbz_w(S_B, 0));
    }
    if (!load_op(c, in, &in->dst, S_A)) return ST_FAIL;     // zero-extended
    int src = S_A;
    if (in->op == OP_SAR && size < 4) {
        emit(c, size == 1 ? a64_enc_sxtb(S_R, S_A) : a64_enc_sxth(S_R, S_A));
        src = S_R;
    }
    if (by_cl)
        emit(c, in->op == OP_SHL ? a64_enc_lslv(S_R, src, S_B) : in->op == OP_SHR ? a64_enc_lsrv(S_R, src, S_B)
                                                                : a64_enc_asrv(S_R, src, S_B));
    else
        emit(c, in->op == OP_SHL ? a64_enc_lsl_imm(S_R, src, (int)n) : in->op == OP_SHR ? a64_enc_lsr_imm(S_R, src, (int)n)
                                                                    : a64_enc_asr_imm(S_R, src, (int)n));
    if (size < 4 && in->op != OP_SHR) emit(c, size == 1 ? a64_enc_uxtb(S_R, S_R) : a64_enc_uxth(S_R, S_R));
    if (!store_op(c, &in->dst, S_R)) return ST_FAIL;
    if (!by_cl) mov_imm(c, S_B, n);
    set_cc(c, fam, size, S_R, S_A, S_B);
    if (by_cl) { patch_here(c, zero); c->cc_known = 0; }
    return ST_NEXT;
}

// MUL / IMUL r/m32 (EDX:EAX = EAX * r/m) and IMUL r32, r/m32 [, imm]. Flags lazily (CC_OP_MUL32).
static step_t tr_mul(ctx_t *c, const insn_t *in)
{
    bool sgn = in->op == OP_IMUL;
    if (has_mem(in)) emit_ea(c, in, S_EA, true);
    if (in->src.kind == OPK_NONE) {                          // one operand: EDX:EAX
        if (!load_op(c, in, &in->dst, S_A)) return ST_FAIL;
        ld_reg(c, T0, R_EAX, 4);
        emit(c, sgn ? a64_enc_smull(S_R, T0, S_A) : a64_enc_umull(S_R, T0, S_A));
        st_reg(c, S_R, R_EAX, 4);
        emit(c, a64_enc_lsr_x_imm(S_B, S_R, 32));
        st_reg(c, S_B, R_EDX, 4);
        if (sgn) emit(c, a64_enc_cmp_x_sxtw(S_R, S_R));
        else     emit(c, a64_enc_cmp_imm(S_B, 0));
    } else {                                                 // IMUL r32, r/m32 [, imm]: truncated
        if (in->src2.kind == OPK_IMM) {
            if (!load_op(c, in, &in->src, S_A)) return ST_FAIL;
            mov_imm(c, S_B, in->imm);
        } else {
            ld_reg(c, S_A, in->dst.reg, 4);
            if (!load_op(c, in, &in->src, S_B)) return ST_FAIL;
        }
        emit(c, a64_enc_smull(S_R, S_A, S_B));
        st_reg(c, S_R, in->dst.reg, 4);
        emit(c, a64_enc_cmp_x_sxtw(S_R, S_R));
    }
    emit(c, a64_enc_cset(S_X, A64_NE));
    set_cc(c, CC_OP_MUL8, 4, S_R, S_X, -1);
    return ST_NEXT;
}

static step_t tr_xchg(ctx_t *c, const insn_t *in)
{
    if (has_mem(in)) emit_ea(c, in, S_EA, true);
    if (!load_op(c, in, &in->dst, S_A) || !load_op(c, in, &in->src, S_B)) return ST_FAIL;
    if (!store_op(c, &in->dst, S_B) || !store_op(c, &in->src, S_A)) return ST_FAIL;
    return ST_NEXT;
}

static step_t tr_movx(ctx_t *c, const insn_t *in)
{
    if (has_mem(in)) emit_ea(c, in, S_EA, true);
    if (!load_op(c, in, &in->src, S_A)) return ST_FAIL;     // zero-extended by the load
    if (in->op == OP_MOVSX)
        emit(c, in->src.size == 1 ? a64_enc_sxtb(S_A, S_A) : a64_enc_sxth(S_A, S_A));
    return store_op(c, &in->dst, S_A) ? ST_NEXT : ST_FAIL;
}

static step_t tr_lea(ctx_t *c, const insn_t *in)
{
    if (in->dst.kind != OPK_REG) return ST_FAIL;
    emit_ea(c, in, S_A, false);
    st_reg(c, S_A, in->dst.reg, in->dst.size);
    return ST_NEXT;
}

static step_t tr_push(ctx_t *c, const insn_t *in)
{
    if (has_mem(in)) emit_ea(c, in, S_EA, true);     // address uses the old ESP
    if (!load_op(c, in, &in->src, S_A)) return ST_FAIL;
    push_reg(c, S_A, in->opsize);
    return ST_NEXT;
}

static step_t tr_pop(ctx_t *c, const insn_t *in)
{
    if (in->dst.kind != OPK_REG && in->dst.kind != OPK_MEM) return ST_FAIL;   // pop sreg: 2b
    pop_reg(c, S_A, in->opsize);
    if (in->dst.kind == OPK_MEM) emit_ea(c, in, S_EA, true);   // address uses the new ESP
    return store_op(c, &in->dst, S_A) ? ST_NEXT : ST_FAIL;
}

static uint32_t next_eip(const ctx_t *c, const insn_t *in, uint32_t delta)
{
    uint32_t v = c->eip + in->len + delta;
    return in->opsize == 2 ? (v & 0xFFFF) : v;
}

static step_t tr_call(ctx_t *c, const insn_t *in)
{
    uint32_t ret = next_eip(c, in, 0);
    if (in->dst.kind == OPK_REL) {
        mov_imm(c, S_A, ret);
        push_reg(c, S_A, in->opsize);
        c->eip = next_eip(c, in, in->imm);
        finish_chain(c, c->eip);
        return ST_END;
    }
    if (has_mem(in)) emit_ea(c, in, S_EA, true);
    if (!load_op(c, in, &in->dst, S_B)) return ST_FAIL;     // target, read before the push
    mov_imm(c, S_A, ret);
    push_reg(c, S_A, in->opsize);
    finish_dyn(c, S_B);
    return ST_END;
}

static step_t tr_jmp(ctx_t *c, const insn_t *in)
{
    if (in->dst.kind == OPK_REL) {
        c->eip = next_eip(c, in, in->imm);
        finish_chain(c, c->eip);
        return ST_END;
    }
    if (has_mem(in)) emit_ea(c, in, S_EA, true);
    if (!load_op(c, in, &in->dst, S_B)) return ST_FAIL;
    finish_dyn(c, S_B);
    return ST_END;
}

static step_t tr_ret(ctx_t *c, const insn_t *in)
{
    pop_reg(c, S_B, in->opsize);
    if (in->src.kind == OPK_IMM && in->imm) adjust_sp(c, in->imm);
    finish_dyn(c, S_B);
    return ST_END;
}

static step_t tr_leave(ctx_t *c, const insn_t *in)
{
    int ssz = c->ss32 ? 4 : 2;
    ld_reg(c, T0, R_EBP, ssz);
    st_reg(c, T0, R_ESP, ssz);
    pop_reg(c, S_A, in->opsize);
    st_reg(c, S_A, R_EBP, in->opsize);
    return ST_NEXT;
}

// Set or clear a bit in eflags (IF, DF).
static step_t tr_flagbit(ctx_t *c, uint32_t mask, bool set)
{
    ld(c, T0, OFF_EFLAGS);
    mov_imm(c, T1, mask);
    emit(c, a64_enc_alu(set ? A64_OP_ORR : A64_OP_BIC, T0, T0, T1));
    st(c, T0, OFF_EFLAGS);
    return ST_NEXT;
}

// CLC / STC / CMC: materialize the lazy flags, then edit CF.
static step_t tr_carry(ctx_t *c, uint32_t opbase)
{
    c->cc_known = 0;
    call(c, (const void *)cpu_flush_flags);
    ld(c, T0, OFF_EFLAGS);
    mov_imm(c, T1, EFL_CF);
    emit(c, a64_enc_alu(opbase, T0, T0, T1));
    st(c, T0, OFF_EFLAGS);
    return ST_NEXT;
}

/* ---- branches ---- */

// Address of the next instruction (IP wraps at 64 KB in 16-bit code).
static uint32_t seq_eip(const ctx_t *c, const insn_t *in)
{
    uint32_t v = c->eip + in->len;
    return c->code32 ? v : (v & 0xFFFF);
}


// w0 = x86 condition `cond` (0/1), from the lazy flags.
static void emit_cond(ctx_t *c, int cond)
{
    if (c->j->inline_flags && inline_cond(c, cond)) return;
    emit(c, a64_enc_movz(1, (uint16_t)cond, 0));
    call(c, (const void *)h_cond);
}

// Both paths end the block: taken -> target, not taken -> next instruction.
static step_t tr_jcc(ctx_t *c, const insn_t *in)
{
    uint32_t taken = next_eip(c, in, in->imm), fall = seq_eip(c, in);
    emit_cond(c, in->cond);
    fixup_t nt = branch_fwd(c, a64_enc_cbz_w(0, 0));
    finish_chain(c, taken);
    patch_here(c, nt);
    finish_chain(c, fall);
    c->eip = fall;
    c->dyn_exit = true;
    return ST_END;
}

// LOOP / LOOPE / LOOPNE / JCXZ. The counter is CX or ECX by address size.
static step_t tr_loop(ctx_t *c, const insn_t *in)
{
    int asz = in->addrsize;
    uint32_t taken = next_eip(c, in, in->imm), fall = seq_eip(c, in);
    fixup_t nt[2];
    int n = 0;
    ld_reg(c, S_A, R_ECX, asz);
    if (in->op == OP_JCXZ) {
        nt[n++] = branch_fwd(c, a64_enc_cbnz_w(S_A, 0));
    } else {
        emit(c, a64_enc_sub_imm(S_A, S_A, 1));
        st_reg(c, S_A, R_ECX, asz);
        if (asz == 2) emit(c, a64_enc_uxth(S_A, S_A));
        nt[n++] = branch_fwd(c, a64_enc_cbz_w(S_A, 0));
        if (in->op != OP_LOOP) {
            emit_cond(c, in->op == OP_LOOPE ? 4 /*E*/ : 5 /*NE*/);
            nt[n++] = branch_fwd(c, a64_enc_cbz_w(0, 0));
        }
    }
    finish_chain(c, taken);
    for (int i = 0; i < n; i++) patch_here(c, nt[i]);
    finish_chain(c, fall);
    c->eip = fall;
    c->dyn_exit = true;
    return ST_END;
}

static step_t tr_setcc(ctx_t *c, const insn_t *in)
{
    if (has_mem(in)) emit_ea(c, in, S_EA, true);
    emit_cond(c, in->cond);
    emit(c, a64_enc_mov_w(S_A, 0));
    return store_op(c, &in->dst, S_A) ? ST_NEXT : ST_FAIL;
}

// The source is always read (memory operands can fault even when the condition is false).
static step_t tr_cmov(ctx_t *c, const insn_t *in)
{
    if (in->dst.kind != OPK_REG) return ST_FAIL;
    if (has_mem(in)) emit_ea(c, in, S_EA, true);
    if (!load_op(c, in, &in->src, S_B)) return ST_FAIL;
    emit_cond(c, in->cond);
    ld_reg(c, S_A, in->dst.reg, in->dst.size);
    emit(c, a64_enc_cmp_imm(0, 0));
    emit(c, a64_enc_csel(S_R, S_B, S_A, A64_NE));
    st_reg(c, S_R, in->dst.reg, in->dst.size);
    return ST_NEXT;
}

/* ---- generic C helper ---- */

// Call helper_exec with a saved copy of the instruction. A nonzero result ends
// the block with that exit code (helper_exec has set cpu->eip).
static step_t tr_helper(ctx_t *c, const insn_t *in, bool ends)
{
    jit_hrec_t *r = arena_alloc(c->j);
    if (!r) return ST_FAIL;
    r->in = *in;
    r->eip = c->eip;
    r->next_eip = seq_eip(c, in);
    a64_mov_imm64(&c->e, 1, (uint64_t)(uintptr_t)r);
    call(c, (const void *)helper_exec);
    if (ends) {
        epilogue_w0(c);
        c->exit = in->op == OP_HLT ? JIT_EXIT_HALT : JIT_EXIT_OK;   // nominal: the helper decides
        c->dyn_exit = true;
        return ST_END;
    }
    emit(c, a64_enc_cbz_w(0, 1 + EPILOGUE_W0_WORDS));
    epilogue_w0(c);
    return ST_NEXT;
}

// Common x87 forms: call the lean entry point in fpu.c directly with everything decoded (linear
// address, its offset, the FOP and eip as constants). It returns 0 when done; 1 when it declined
// (unusual operands, unmasked exceptions, a fast-TLB miss...), and then the full helper runs.

/* ---- x87 data moves as ARM64 code ----
 * FXCH, FLD ST(i), FST(P) ST(i), FLD m32/m64, FST(P) m32 (round to nearest), FLDCW, FNSTCW.
 * Each first checks everything that could make the instruction do anything but the plain move
 * (pending #MF, CR0.EM/TS, empty or full registers, NaN/infinity/denormal values, fast-TLB miss,
 * results out of range, for FST m32 PE unmasked or RC other than nearest) and branches to the
 * C path (fpu_jit_mem/reg, then helper_exec) on any of them before changing anything. Results,
 * tags (recomputed from the values like set_preg's tag_of), status (TOP, C1, PE) and the FPU
 * pointer registers are those of fpu.c's general code; test_x87 compares the two.
 * Registers: w1 = status word, w2 = tag word, w3 = TOP, w0/w4..w8/w13..w15 and T0/T1 scratch;
 * the memory forms keep the linear address in S_A and x11 = fast-TLB addend after the lookup. */
enum { X_SW = 1, X_TW = 2, X_TOP = 3 };
#define X87_MAX_SLOW 24

typedef struct { fixup_t f[X87_MAX_SLOW]; int n; } slowlist_t;

static void x_slow(ctx_t *c, slowlist_t *sl, uint32_t word)
{
    if (sl->n < X87_MAX_SLOW) sl->f[sl->n++] = branch_fwd(c, word);
    else c->e.overflow = true;                               // cannot happen; discard the block
}

static void x_slow_add(ctx_t *c, slowlist_t *sl, const fixup_t *f, int n)   // already-emitted branches
{
    for (int k = 0; k < n; k++) {
        if (sl->n < X87_MAX_SLOW) sl->f[sl->n++] = f[k];
        else c->e.overflow = true;
    }
}

// Status/CR0 checks; loads w1 (and w2, w3 unless control). pm: PE must be masked (the form can
// be inexact); pc64: precision control must be 64 bits (native arithmetic).
static void x_guard(ctx_t *c, slowlist_t *sl, bool control, bool pm, bool pc64)
{
    emit(c, a64_enc_ldrh(X_SW, R_CPU, OFF_FPU_SW));
    if (!control) {
        emit(c, a64_enc_ubfx(T0, X_SW, 7, 1));               // ES: #MF pending
        x_slow(c, sl, a64_enc_cbnz_w(T0, 0));
    }
    ld(c, T0, OFF_CR0);
    emit(c, a64_enc_ubfx(T0, T0, 2, 2));                     // EM, TS: #NM
    x_slow(c, sl, a64_enc_cbnz_w(T0, 0));
    if (pm || pc64) emit(c, a64_enc_ldrh(T0, R_CPU, OFF_FPU_CW));
    if (pm) {
        emit(c, a64_enc_ubfx(T1, T0, 5, 1));                 // PM masked
        x_slow(c, sl, a64_enc_cbz_w(T1, 0));
    }
    if (pc64) {
        emit(c, a64_enc_ubfx(T1, T0, 8, 2));                 // PC = 3
        emit(c, a64_enc_cmp_imm(T1, 3));
        x_slow(c, sl, a64_enc_bcond(A64_NE, 0));
    }
    if (!control) {
        emit(c, a64_enc_ldrh(X_TW, R_CPU, OFF_FPU_TW));
        emit(c, a64_enc_ubfx(X_TOP, X_SW, 11, 3));
    }
}

static void x_phys(ctx_t *c, int rd, int i)                  // rd = (TOP + i) & 7
{
    emit(c, a64_enc_add_imm(rd, X_TOP, (uint32_t)i & 7));
    emit(c, a64_enc_ubfx(rd, rd, 0, 3));
}

static void x_tag(ctx_t *c, int rd, int p)                   // rd = tag of physical register p
{
    emit(c, a64_enc_alu(A64_OP_ADD, rd, p, p));
    emit(c, a64_enc_lsrv(rd, X_TW, rd));
    emit(c, a64_enc_ubfx(rd, rd, 0, 2));
}

// Branch to the slow path unless physical register p is empty (want_empty) / present.
static void x_need(ctx_t *c, slowlist_t *sl, int p, bool want_empty)
{
    x_tag(c, T0, p);
    emit(c, a64_enc_cmp_imm(T0, 3));
    x_slow(c, sl, a64_enc_bcond(want_empty ? A64_NE : A64_EQ, 0));
}

static void x_off(ctx_t *c, int rd, int p)                   // rd = offsetof(st[p])
{
    emit(c, a64_enc_alu_lsl(A64_OP_ADD, rd, p, p, 2));
    emit(c, a64_enc_lsl_imm(rd, rd, 1));
    emit(c, a64_enc_add_imm(rd, rd, OFF_ST));
}

static void x_load(ctx_t *c, int xs, int we, int off)        // xs = significand, we = sign/exponent
{
    emit(c, a64_enc_ldr_uxtw(8, xs, R_CPU, off));
    emit(c, a64_enc_add_imm(T0, off, 8));
    emit(c, a64_enc_ldr_uxtw(2, we, R_CPU, T0));
}

static void x_store(ctx_t *c, int xs, int we, int off)
{
    emit(c, a64_enc_str_uxtw(8, xs, R_CPU, off));
    emit(c, a64_enc_add_imm(T0, off, 8));
    emit(c, a64_enc_str_uxtw(2, we, R_CPU, T0));
}

// rd = tag_of(value): 0 valid, 1 zero, 2 special. Uses T0.
static void x_tag_of(ctx_t *c, int rd, int we, int xs)
{
    emit(c, a64_enc_movz(rd, 2, 0));
    emit(c, a64_enc_ubfx(T0, we, 0, 15));
    fixup_t zero = branch_fwd(c, a64_enc_cbz_w(T0, 0));
    emit(c, a64_enc_add_imm(T0, T0, 1));
    emit(c, a64_enc_ubfx(T0, T0, 15, 1));
    fixup_t d1 = branch_fwd(c, a64_enc_cbnz_w(T0, 0));       // exponent 0x7FFF
    emit(c, a64_enc_lsr_x_imm(T0, xs, 63));
    fixup_t d2 = branch_fwd(c, a64_enc_cbz_w(T0, 0));        // no integer bit: unsupported
    emit(c, a64_enc_movz(rd, 0, 0));
    fixup_t d3 = branch_fwd(c, a64_enc_b(0));
    patch_here(c, zero);
    fixup_t d4 = branch_fwd(c, a64_enc_cbnz_x(xs, 0));       // denormal
    emit(c, a64_enc_movz(rd, 1, 0));
    patch_here(c, d1); patch_here(c, d2); patch_b_here(c, d3); patch_here(c, d4);
}

static void x_set_tag(ctx_t *c, int p, int tag)              // tag word field p = tag (T0, T1)
{
    emit(c, a64_enc_alu(A64_OP_ADD, T0, p, p));
    emit(c, a64_enc_movz(T1, 3, 0));
    emit(c, a64_enc_lslv(T1, T1, T0));
    emit(c, a64_enc_alu(A64_OP_BIC, X_TW, X_TW, T1));
    emit(c, a64_enc_lslv(T1, tag, T0));
    emit(c, a64_enc_alu(A64_OP_ORR, X_TW, X_TW, T1));
}

static void x_set_top(ctx_t *c, int t)
{
    emit(c, a64_enc_movz(T0, 0x3800, 0));
    emit(c, a64_enc_alu(A64_OP_BIC, X_SW, X_SW, T0));
    emit(c, a64_enc_alu_lsl(A64_OP_ORR, X_SW, X_SW, t, 11));
}

static void x_pop(ctx_t *c)                                  // tag(TOP) = empty, TOP + 1
{
    emit(c, a64_enc_movz(0, 3, 0));
    x_set_tag(c, X_TOP, 0);
    x_phys(c, 4, 1);
    x_set_top(c, 4);
}

// Last-instruction pointers (non-control instructions), then write back status and tags.
static void x_finish(ctx_t *c, const insn_t *in, uint32_t op)
{
    mov_imm(c, T0, c->eip);
    st(c, T0, OFF_FPU_IP);
    emit(c, a64_enc_ldrh(T0, R_CPU, OFF_SEG_SEL(S_CS)));
    emit(c, a64_enc_strh(T0, R_CPU, OFF_FPU_CS));
    emit(c, a64_enc_movz(T0, (uint16_t)op, 0));
    emit(c, a64_enc_strh(T0, R_CPU, OFF_FPU_OP));
    if (in->mod != 3) {
        st(c, S_EA, OFF_FPU_DP);
        emit(c, a64_enc_ldrh(T0, R_CPU, OFF_SEG_SEL(in->mem.seg)));
        emit(c, a64_enc_strh(T0, R_CPU, OFF_FPU_DS));
    }
    emit(c, a64_enc_strh(X_SW, R_CPU, OFF_FPU_SW));
    emit(c, a64_enc_strh(X_TW, R_CPU, OFF_FPU_TW));
}

#define X64(w) ((w) | A64_X)

// Rounding decision (fpu.c's round_up): `rest` (X) is the discarded part with one half = 2^63,
// odd the kept part's low bit, sign the value's sign, rc the RC field. Sets up and inexact (0/1).
// Uses T0, T1; up, inexact must differ from the inputs.
static void x_round_up(ctx_t *c, int up, int inexact, int rest, int odd, int sign, int rc)
{
    emit(c, a64_enc_cmp_x(rest, A64_ZR));
    emit(c, a64_enc_cset(inexact, A64_NE));
    emit(c, a64_enc_movz_x(T1, 0x8000, 3));
    emit(c, a64_enc_cmp_x(rest, T1));
    emit(c, a64_enc_cset(up, A64_HI));                       // nearest: above one half,
    emit(c, a64_enc_csel(up, odd, up, A64_EQ));              // or exactly half and odd
    emit(c, a64_enc_movz(T0, 1, 0));
    emit(c, a64_enc_alu(A64_OP_EOR, T0, T0, sign));
    emit(c, a64_enc_cmp_imm(rc, 1));
    emit(c, a64_enc_csel(T0, sign, T0, A64_EQ));             // down: negative; up: positive
    emit(c, a64_enc_alu(A64_OP_AND, T0, T0, inexact));
    emit(c, a64_enc_cmp_imm(rc, 0));
    emit(c, a64_enc_csel(up, up, T0, A64_EQ));
    emit(c, a64_enc_cmp_imm(rc, 3));
    emit(c, a64_enc_csel(up, A64_ZR, up, A64_EQ));           // chop
}

// Slow path unless (sig X, se W) is ordinary: zero or a normal number (not a denormal, NaN,
// infinity or unsupported encoding).
static void x_need_ordinary(ctx_t *c, slowlist_t *sl, int sig, int se)
{
    emit(c, a64_enc_ubfx(T0, se, 0, 15));
    fixup_t nz = branch_fwd(c, a64_enc_cbnz_w(T0, 0));
    x_slow(c, sl, a64_enc_cbnz_x(sig, 0));                   // denormal
    fixup_t ok = branch_fwd(c, a64_enc_b(0));
    patch_here(c, nz);
    emit(c, a64_enc_add_imm(T0, T0, 1));
    emit(c, a64_enc_ubfx(T0, T0, 15, 1));
    x_slow(c, sl, a64_enc_cbnz_w(T0, 0));
    emit(c, a64_enc_lsr_x_imm(T0, sig, 63));
    x_slow(c, sl, a64_enc_cbz_w(T0, 0));
    patch_b_here(c, ok);
}

// (sig, se) = the float32/float64 at [x11 + S_A], which must be zero or normal. Uses x5, T0, T1.
static void x_load_real_ordinary(ctx_t *c, slowlist_t *sl, int size, int sig, int se)
{
    fixup_t zero;
    if (size == 4) {
        emit(c, a64_enc_ldr_uxtw(4, 5, 11, S_A));
        emit(c, a64_enc_ubfx(T0, 5, 23, 8));
        emit(c, a64_enc_lsr_imm(T1, 5, 31));
        zero = branch_fwd(c, a64_enc_cbz_w(T0, 0));
        emit(c, a64_enc_cmp_imm(T0, 0xFF));
        x_slow(c, sl, a64_enc_bcond(A64_EQ, 0));
        emit(c, a64_enc_lsl_x_imm(sig, 5, 40));
        emit(c, a64_enc_movz(5, 16383 - 127, 0));
    } else {
        emit(c, a64_enc_ldr_uxtw(8, 5, 11, S_A));
        emit(c, a64_enc_ubfx_x(T0, 5, 52, 11));
        emit(c, a64_enc_lsr_x_imm(T1, 5, 63));
        zero = branch_fwd(c, a64_enc_cbz_w(T0, 0));
        emit(c, a64_enc_cmp_imm(T0, 0x7FF));
        x_slow(c, sl, a64_enc_bcond(A64_EQ, 0));
        emit(c, a64_enc_lsl_x_imm(sig, 5, 11));
        emit(c, a64_enc_movz(5, 16383 - 1023, 0));
    }
    emit(c, a64_enc_alu(A64_OP_ADD, T0, T0, 5));
    emit(c, a64_enc_alu_lsl(A64_OP_ORR, se, T0, T1, 15));
    emit(c, a64_enc_movz_x(T0, 0x8000, 3));
    emit(c, a64_enc_orr_x(sig, sig, T0));
    fixup_t done = branch_fwd(c, a64_enc_b(0));
    patch_here(c, zero);                                     // exponent 0: zero, or a denormal (slow)
    if (size == 4) emit(c, a64_enc_lsl_imm(T0, 5, 9)); else emit(c, a64_enc_lsl_x_imm(T0, 5, 12));
    x_slow(c, sl, size == 4 ? a64_enc_cbnz_w(T0, 0) : a64_enc_cbnz_x(T0, 0));
    emit(c, a64_enc_movz_x(sig, 0, 0));
    emit(c, a64_enc_lsl_imm(se, T1, 15));
    patch_b_here(c, done);
}

/* Exact FADD/FSUB/FSUBR/FMUL of two normal operands at PC = 64 bits, any RC: fpu.c's native_arith
 * in ARM64. Operand a = (x6, w7), b = (x8, w13); aop 0 add, 1 mul, 4 a - b (SUBR is loaded
 * swapped). Result: sign w15, exponent w14, significand x16, discarded part x17 (half = 2^63),
 * then rounded, range-checked (slow path outside the normal range) and written to physical
 * register dst; status C1/PE. Uses w0, w5, w22..w24, T0, T1. */
static void x_arith_compute(ctx_t *c, slowlist_t *sl, int aop)
{
    // Zero operands (the callers let only zeros and normal numbers through): exact results.
    if (aop == 4) {                                          // a - b = a + (-b)
        emit(c, a64_enc_movz(T0, 0x8000, 0));
        emit(c, a64_enc_alu(A64_OP_EOR, 13, 13, T0));
    }
    emit(c, a64_enc_movz(23, 0, 0));                         // not rounded,
    emit(c, a64_enc_movz(24, 0, 0));                         // exact
    emit(c, a64_enc_ubfx(T0, 7, 0, 15));
    fixup_t az = branch_fwd(c, a64_enc_cbz_w(T0, 0));
    emit(c, a64_enc_ubfx(T0, 13, 0, 15));
    fixup_t bz = branch_fwd(c, a64_enc_cbz_w(T0, 0));
    fixup_t go = branch_fwd(c, a64_enc_b(0));
    patch_here(c, az);
    patch_here(c, bz);
    emit(c, a64_enc_movz(0, 0, 0));
    emit(c, a64_enc_movz_x(16, 0, 0));
    if (aop == 1) {                                          // x * 0 = 0 with the sign of the product
        emit(c, a64_enc_alu(A64_OP_EOR, 7, 7, 13));
        emit(c, a64_enc_movz(T0, 0x8000, 0));
        emit(c, a64_enc_alu(A64_OP_AND, 7, 7, T0));
        emit(c, A64_RET);
    } else {
        // x + 0 = x; 0 + y = y; 0 + 0: equal signs keep it, else -0 when rounding down, +0 otherwise
        emit(c, a64_enc_ubfx(T0, 13, 0, 15));
        fixup_t b_nz = branch_fwd(c, a64_enc_cbnz_w(T0, 0));  // a is zero: the result is b
        emit(c, a64_enc_ubfx(T0, 7, 0, 15));
        fixup_t a_nz = branch_fwd(c, a64_enc_cbnz_w(T0, 0));  // b is zero: the result is a
        emit(c, a64_enc_alu(A64_OP_EOR, T0, 7, 13));
        emit(c, a64_enc_ubfx(T0, T0, 15, 1));
        fixup_t same = branch_fwd(c, a64_enc_cbz_w(T0, 0));
        emit(c, a64_enc_ldrh(T0, R_CPU, OFF_FPU_CW));
        emit(c, a64_enc_ubfx(T0, T0, 10, 2));
        emit(c, a64_enc_cmp_imm(T0, 1));
        emit(c, a64_enc_movz(7, 0x8000, 0));
        emit(c, a64_enc_csel(7, 7, A64_ZR, A64_EQ));
        patch_here(c, same);
        emit(c, A64_RET);
        patch_here(c, b_nz);
        emit(c, a64_enc_mov_w(7, 13));
        emit(c, a64_enc_mov_x(16, 8));
        emit(c, A64_RET);
        patch_here(c, a_nz);
        emit(c, a64_enc_mov_x(16, 6));
        emit(c, A64_RET);
    }
    patch_b_here(c, go);
    if (aop == 1) {
        emit(c, a64_enc_alu(A64_OP_EOR, 15, 7, 13));
        emit(c, a64_enc_ubfx(15, 15, 15, 1));
        emit(c, a64_enc_ubfx(14, 7, 0, 15));
        emit(c, a64_enc_ubfx(T0, 13, 0, 15));
        emit(c, a64_enc_alu(A64_OP_ADD, 14, 14, T0));
        mov_imm(c, T0, 16382);
        emit(c, a64_enc_alu(A64_OP_SUB, 14, 14, T0));        // exponent if the product has bit 127
        emit(c, a64_enc_mul_x(17, 6, 8));
        emit(c, a64_enc_umulh(16, 6, 8));
        emit(c, a64_enc_lsr_x_imm(T0, 16, 63));
        fixup_t top = branch_fwd(c, a64_enc_cbnz_w(T0, 0));
        emit(c, a64_enc_lsl_x_imm(16, 16, 1));
        emit(c, X64(a64_enc_alu_lsr(A64_OP_ORR, 16, 16, 17, 63)));
        emit(c, a64_enc_lsl_x_imm(17, 17, 1));
        emit(c, a64_enc_sub_imm(14, 14, 1));
        patch_here(c, top);
    } else {
        emit(c, a64_enc_ubfx(14, 7, 0, 15));
        emit(c, a64_enc_ubfx(5, 13, 0, 15));
        // a = the larger magnitude
        emit(c, a64_enc_cmp_w(14, 5));
        fixup_t keep1 = branch_fwd(c, a64_enc_bcond(A64_HI, 0));
        fixup_t swap1 = branch_fwd(c, a64_enc_bcond(A64_LO, 0));
        emit(c, a64_enc_cmp_x(6, 8));
        fixup_t keep2 = branch_fwd(c, a64_enc_bcond(A64_HS, 0));
        patch_here(c, swap1);
        emit(c, a64_enc_mov_x(T0, 6)); emit(c, a64_enc_mov_x(6, 8)); emit(c, a64_enc_mov_x(8, T0));
        emit(c, a64_enc_mov_w(T0, 7)); emit(c, a64_enc_mov_w(7, 13)); emit(c, a64_enc_mov_w(13, T0));
        emit(c, a64_enc_mov_w(T0, 14)); emit(c, a64_enc_mov_w(14, 5)); emit(c, a64_enc_mov_w(5, T0));
        patch_here(c, keep1); patch_here(c, keep2);
        emit(c, a64_enc_lsr_imm(15, 7, 15));                 // sign of the result
        // b's significand aligned to a's: hi x16 : lo x17, sticky w0
        emit(c, a64_enc_alu(A64_OP_SUB, 22, 14, 5));         // d
        emit(c, a64_enc_movz_x(17, 0, 0));
        emit(c, a64_enc_movz(0, 0, 0));
        emit(c, a64_enc_mov_x(16, 8));
        fixup_t al0 = branch_fwd(c, a64_enc_cbz_w(22, 0));
        emit(c, a64_enc_cmp_imm(22, 64));
        fixup_t big = branch_fwd(c, a64_enc_bcond(A64_HS, 0));
        emit(c, a64_enc_movz(T0, 64, 0));
        emit(c, a64_enc_alu(A64_OP_SUB, T0, T0, 22));
        emit(c, X64(a64_enc_lslv(17, 8, T0)));
        emit(c, X64(a64_enc_lsrv(16, 8, 22)));
        fixup_t al1 = branch_fwd(c, a64_enc_b(0));
        patch_here(c, big);
        emit(c, a64_enc_cmp_imm(22, 128));
        fixup_t huge = branch_fwd(c, a64_enc_bcond(A64_HS, 0));
        emit(c, X64(a64_enc_lsrv(17, 8, 22)));               // shift d - 64 (count mod 64)
        emit(c, a64_enc_alu(A64_OP_SUB, T0, A64_ZR, 22));
        emit(c, X64(a64_enc_lslv(T0, 8, T0)));               // bits shifted out (128 - d mod 64)
        emit(c, a64_enc_cmp_imm(22, 64));
        emit(c, X64(a64_enc_csel(T0, A64_ZR, T0, A64_EQ)));  // none when d = 64
        emit(c, a64_enc_cmp_x(T0, A64_ZR));
        emit(c, a64_enc_cset(0, A64_NE));
        emit(c, a64_enc_movz_x(16, 0, 0));
        fixup_t al2 = branch_fwd(c, a64_enc_b(0));
        patch_here(c, huge);
        emit(c, a64_enc_movz_x(16, 0, 0));
        emit(c, a64_enc_movz_x(17, 0, 0));
        emit(c, a64_enc_movz(0, 1, 0));
        patch_here(c, al0); patch_b_here(c, al1); patch_b_here(c, al2);
        emit(c, a64_enc_alu(A64_OP_EOR, T0, 7, 13));
        emit(c, a64_enc_ubfx(T0, T0, 15, 1));
        fixup_t diff = branch_fwd(c, a64_enc_cbnz_w(T0, 0));
        emit(c, X64(a64_enc_alu(A64_OP_ADDS, 16, 6, 16)));   // same signs: add
        fixup_t done1 = branch_fwd(c, a64_enc_bcond(A64_LO, 0));
        emit(c, a64_enc_movz(T1, 1, 0));
        emit(c, a64_enc_alu(A64_OP_AND, T0, 17, T1));
        emit(c, a64_enc_alu(A64_OP_ORR, 0, 0, T0));
        emit(c, a64_enc_lsr_x_imm(17, 17, 1));
        emit(c, X64(a64_enc_alu_lsl(A64_OP_ORR, 17, 17, 16, 63)));
        emit(c, a64_enc_lsr_x_imm(16, 16, 1));
        emit(c, a64_enc_movz_x(T0, 0x8000, 3));
        emit(c, a64_enc_orr_x(16, 16, T0));
        emit(c, a64_enc_add_imm(14, 14, 1));
        fixup_t done2 = branch_fwd(c, a64_enc_b(0));
        patch_here(c, diff);                                 // different signs: a - b - sticky
        emit(c, X64(a64_enc_alu(A64_OP_ADD, T0, 17, 0)));
        emit(c, X64(a64_enc_alu(A64_OP_SUBS, 17, A64_ZR, T0)));
        emit(c, X64(a64_enc_alu(A64_OP_SBC, 16, 6, 16)));
        emit(c, a64_enc_orr_x(T0, 16, 17));
        x_slow(c, sl, a64_enc_cbz_x(T0, 0));                 // exactly zero: SoftFloat's sign rules
        fixup_t nz = branch_fwd(c, a64_enc_cbnz_x(16, 0));
        emit(c, a64_enc_mov_x(16, 17));
        emit(c, a64_enc_movz_x(17, 0, 0));
        emit(c, a64_enc_sub_imm(14, 14, 64));
        patch_here(c, nz);
        emit(c, a64_enc_clz_x(T0, 16));
        fixup_t done3 = branch_fwd(c, a64_enc_cbz_w(T0, 0));
        emit(c, X64(a64_enc_lslv(16, 16, T0)));
        emit(c, a64_enc_movz(T1, 64, 0));
        emit(c, a64_enc_alu(A64_OP_SUB, T1, T1, T0));
        emit(c, X64(a64_enc_lsrv(T1, 17, T1)));
        emit(c, a64_enc_orr_x(16, 16, T1));
        emit(c, X64(a64_enc_lslv(17, 17, T0)));
        emit(c, a64_enc_alu(A64_OP_SUB, 14, 14, T0));
        patch_here(c, done1); patch_b_here(c, done2); patch_here(c, done3);
        emit(c, a64_enc_orr_x(17, 17, 0));                   // the sticky bit joins the rest
    }
    emit(c, a64_enc_movz(T1, 1, 0));
    emit(c, a64_enc_alu(A64_OP_AND, 5, 16, T1));             // odd
    emit(c, a64_enc_ldrh(22, R_CPU, OFF_FPU_CW));
    emit(c, a64_enc_ubfx(22, 22, 10, 2));
    x_round_up(c, 23, 24, 17, 5, 15, 22);
    emit(c, X64(a64_enc_alu(A64_OP_ADD, 16, 16, 23)));
    fixup_t nc = branch_fwd(c, a64_enc_cbnz_x(16, 0));
    emit(c, a64_enc_movz_x(16, 0x8000, 3));
    emit(c, a64_enc_add_imm(14, 14, 1));
    patch_here(c, nc);
    emit(c, a64_enc_sub_imm(T0, 14, 1));
    mov_imm(c, T1, 0x7FFD);
    emit(c, a64_enc_cmp_w(T0, T1));
    x_slow(c, sl, a64_enc_bcond(A64_HI, 0));                 // overflow / underflow: general code
    emit(c, a64_enc_alu_lsl(A64_OP_ORR, 7, 14, 15, 15));
}

// The computation lives in one shared routine per operation (emitted next to the exit stub,
// kept across flushes): inline, it made x87-heavy blocks several times larger and the teapot
// slower than the C call (instruction cache). It returns w0 = 0 with the result in x16/w7,
// up w23, inexact w24, or w0 = 1 (slow path, nothing changed).
static void x_arith(ctx_t *c, slowlist_t *sl, int aop, int dst)
{
    uint32_t *core = c->j->x87_core[aop == 0 ? 0 : aop == 1 ? 1 : 2];
    emit(c, a64_enc_bl((int32_t)(core - c->e.p)));
    x_slow(c, sl, a64_enc_cbnz_w(0, 0));
    x_off(c, 5, dst);
    x_store(c, 16, 7, 5);
    emit(c, a64_enc_ubfx(6, 7, 0, 15));
    emit(c, a64_enc_cmp_imm(6, 0));
    emit(c, a64_enc_cset(6, A64_EQ));                        // zero or valid
    x_set_tag(c, dst, 6);
    emit(c, a64_enc_movz(T0, 0x200, 0));
    emit(c, a64_enc_alu(A64_OP_BIC, X_SW, X_SW, T0));
    emit(c, a64_enc_alu_lsl(A64_OP_ORR, X_SW, X_SW, 24, 5));  // PE
    emit(c, a64_enc_alu_lsl(A64_OP_ORR, X_SW, X_SW, 23, 9));  // C1 = rounded up
}

// Emit the inline form if there is one (true); its slow-path branches go into sl.
static bool x87_inline(ctx_t *c, const insn_t *in, uint32_t op, slowlist_t *sl)
{
    int esc = in->opcode & 7, reg = in->reg, i = in->rm;
    if (in->mod == 3) {
        if (esc == 1 && reg == 1) {                          // FXCH ST(i)
            x_guard(c, sl, false, false, false);
            x_phys(c, 4, i);
            x_need(c, sl, X_TOP, false);
            x_need(c, sl, 4, false);
            x_off(c, 5, X_TOP); x_off(c, 6, 4);
            x_load(c, 7, 8, 5); x_load(c, 13, 14, 6);
            x_store(c, 13, 14, 5); x_store(c, 7, 8, 6);
            x_tag_of(c, 0, 14, 13); x_set_tag(c, X_TOP, 0);
            x_tag_of(c, 0, 8, 7);   x_set_tag(c, 4, 0);
            emit(c, a64_enc_movz(T0, 0x200, 0));             // C1 = 0
            emit(c, a64_enc_alu(A64_OP_BIC, X_SW, X_SW, T0));
            x_finish(c, in, op);
            return true;
        }
        if (esc == 1 && reg == 0) {                          // FLD ST(i)
            x_guard(c, sl, false, false, false);
            x_phys(c, 4, i);
            x_phys(c, 6, 7);                                 // the new TOP
            x_need(c, sl, 4, false);
            x_need(c, sl, 6, true);
            x_off(c, 5, 4); x_load(c, 7, 8, 5);
            x_off(c, 5, 6); x_store(c, 7, 8, 5);
            x_tag_of(c, 0, 8, 7); x_set_tag(c, 6, 0);
            x_set_top(c, 6);
            x_finish(c, in, op);
            return true;
        }
        if (((esc == 5 || esc == 7) && (reg == 2 || reg == 3)) || (esc == 1 && reg == 3)) {   // FST(P) ST(i)
            x_guard(c, sl, false, false, false);
            x_need(c, sl, X_TOP, false);
            x_phys(c, 4, i);
            x_off(c, 5, X_TOP); x_load(c, 7, 8, 5);
            x_off(c, 6, 4); x_store(c, 7, 8, 6);
            x_tag_of(c, 0, 8, 7); x_set_tag(c, 4, 0);
            if (!(esc == 5 && reg == 2)) x_pop(c);
            x_finish(c, in, op);
            return true;
        }
        if ((esc == 0 || esc == 4 || esc == 6) && (reg == 0 || reg == 1 || reg == 4 || reg == 5)) {
            // D8: ST0 = ST0 op ST(i). DC/DE: ST(i) = ST(i) op ST0 with SUB/SUBR exchanged
            // (fast_reg's REV table), DE pops.
            int aop = reg;
            bool to_i = esc != 0;
            if (to_i && reg >= 4) aop = reg ^ 1;
            x_guard(c, sl, false, true, true);
            x_phys(c, 4, i);
            x_need(c, sl, X_TOP, false);
            x_need(c, sl, 4, false);
            int pa = to_i ? 4 : X_TOP, pb = to_i ? X_TOP : 4;
            if (aop == 5) { int t = pa; pa = pb; pb = t; aop = 4; }        // b - a
            x_off(c, 5, pa); x_load(c, 6, 7, 5);
            x_off(c, 5, pb); x_load(c, 8, 13, 5);
            x_need_ordinary(c, sl, 6, 7);
            x_need_ordinary(c, sl, 8, 13);
            x_arith(c, sl, aop, to_i ? 4 : X_TOP);
            if (esc == 6) x_pop(c);
            x_finish(c, in, op);
            return true;
        }
        return false;
    }
    bool fld = (esc == 1 || esc == 5) && reg == 0, fst = (esc == 1 || esc == 5) && (reg == 2 || reg == 3);
    bool ldcw = esc == 1 && reg == 5, stcw = esc == 1 && reg == 7;
    bool fist = (esc == 3 || esc == 7) && (reg == 2 || reg == 3);           // FIST(P) m32 / m16
    bool arith = (esc == 0 || esc == 4) && (reg == 0 || reg == 1 || reg == 4 || reg == 5);
    if (!fld && !fst && !ldcw && !stcw && !fist && !arith) return false;
    emit_ea(c, in, S_EA, false);                             // offset (FDP)
    ld(c, T0, OFF_SEG_BASE(in->mem.seg));
    emit(c, a64_enc_alu(A64_OP_ADD, S_A, S_EA, T0));         // linear address
    fixup_t miss[2];
    if (ldcw || stcw) {                                      // control: no #MF check, no pointers
        x_guard(c, sl, true, false, false);
        int n = ftlb_lookup(c, S_A, 2, stcw, miss);
        x_slow_add(c, sl, miss, n);
        if (stcw) {
            emit(c, a64_enc_ldrh(4, R_CPU, OFF_FPU_CW));
            emit(c, a64_enc_str_uxtw(2, 4, 11, S_A));
            return true;
        }
        emit(c, a64_enc_ldr_uxtw(2, 4, 11, S_A));
        emit(c, a64_enc_strh(4, R_CPU, OFF_FPU_CW));
        emit(c, a64_enc_alu(A64_OP_BIC, 5, X_SW, 4));       // unmasked exceptions pending?
        emit(c, a64_enc_ubfx(5, 5, 0, 6));
        mov_imm(c, T0, 0x8080);                              // ES | B
        emit(c, a64_enc_alu(A64_OP_BIC, X_SW, X_SW, T0));
        fixup_t none = branch_fwd(c, a64_enc_cbz_w(5, 0));
        emit(c, a64_enc_alu(A64_OP_ORR, X_SW, X_SW, T0));
        patch_here(c, none);
        emit(c, a64_enc_strh(X_SW, R_CPU, OFF_FPU_SW));
        return true;
    }
    if (arith) {                                             // ST0 = ST0 op m32/m64 real
        int size = esc == 0 ? 4 : 8, aop = reg;
        x_guard(c, sl, false, true, true);
        x_need(c, sl, X_TOP, false);
        int n = ftlb_lookup(c, S_A, size, false, miss);
        x_slow_add(c, sl, miss, n);
        bool rev = aop == 5;                                 // m - ST0
        if (rev) aop = 4;
        x_load_real_ordinary(c, sl, size, rev ? 6 : 8, rev ? 7 : 13);
        x_off(c, 5, X_TOP);
        x_load(c, rev ? 8 : 6, rev ? 13 : 7, 5);
        x_need_ordinary(c, sl, rev ? 8 : 6, rev ? 13 : 7);
        x_arith(c, sl, aop, X_TOP);
        x_finish(c, in, op);
        return true;
    }
    if (fist) {
        int size = esc == 3 ? 4 : 2;
        x_guard(c, sl, false, true, false);
        x_need(c, sl, X_TOP, false);
        int n = ftlb_lookup(c, S_A, size, true, miss);
        x_slow_add(c, sl, miss, n);
        x_off(c, 5, X_TOP);
        x_load(c, 7, 8, 5);
        emit(c, a64_enc_ubfx(13, 8, 0, 15));                 // exponent
        emit(c, a64_enc_lsr_imm(14, 8, 15));                 // sign
        fixup_t zero = branch_fwd(c, a64_enc_cbz_w(13, 0));
        emit(c, a64_enc_add_imm(T0, 13, 1));
        emit(c, a64_enc_ubfx(T0, T0, 15, 1));
        x_slow(c, sl, a64_enc_cbnz_w(T0, 0));                // NaN / infinity
        emit(c, a64_enc_lsr_x_imm(T0, 7, 63));
        x_slow(c, sl, a64_enc_cbz_w(T0, 0));                 // unsupported
        mov_imm(c, T0, 16383);
        emit(c, a64_enc_alu(A64_OP_SUB, 13, 13, T0));        // unbiased exponent (signed)
        emit(c, a64_enc_cmp_imm(13, size == 4 ? 30 : 14));
        x_slow(c, sl, a64_enc_bcond(A64_GT, 0));             // too big: maybe out of range
        emit(c, a64_enc_ldrh(0, R_CPU, OFF_FPU_CW));
        emit(c, a64_enc_ubfx(0, 0, 10, 2));                  // RC
        emit(c, a64_enc_cmp_imm(13, 0));
        fixup_t below1 = branch_fwd(c, a64_enc_bcond(A64_LT, 0));
        emit(c, a64_enc_movz(T0, 63, 0));                    // integer part, fraction at the top
        emit(c, a64_enc_alu(A64_OP_SUB, T0, T0, 13));
        emit(c, X64(a64_enc_lsrv(4, 7, T0)));
        emit(c, a64_enc_add_imm(T0, 13, 1));
        emit(c, X64(a64_enc_lslv(5, 7, T0)));
        fixup_t split = branch_fwd(c, a64_enc_b(0));
        patch_here(c, below1);
        emit(c, a64_enc_movz(4, 0, 0));
        emit(c, a64_enc_mov_x(5, 7));                        // [1/2, 1): the whole significand
        emit(c, a64_enc_add_imm(T0, 13, 1));
        fixup_t half = branch_fwd(c, a64_enc_cbz_w(T0, 0));
        emit(c, a64_enc_movz_x(5, 1, 0));                    // below one half: only "nonzero" matters
        patch_b_here(c, split); patch_here(c, half);
        emit(c, a64_enc_movz(T1, 1, 0));
        emit(c, a64_enc_alu(A64_OP_AND, 6, 4, T1));
        x_round_up(c, 16, 15, 5, 6, 14, 0);
        emit(c, a64_enc_alu(A64_OP_ADD, 4, 4, 16));
        mov_imm(c, T0, size == 4 ? 0x7FFFFFFFu : 0x7FFFu);
        emit(c, a64_enc_cmp_w(4, T0));
        x_slow(c, sl, a64_enc_bcond(A64_HI, 0));             // out of range (or -2^31): general code
        emit(c, a64_enc_alu(A64_OP_SUB, 0, A64_ZR, 4));
        emit(c, a64_enc_cmp_imm(14, 0));
        emit(c, a64_enc_csel(0, 0, 4, A64_NE));
        emit(c, a64_enc_lsl_imm(15, 15, 5));                 // PE
        emit(c, a64_enc_alu_lsl(A64_OP_ORR, 15, 15, 16, 9)); // C1 = rounded up
        fixup_t write = branch_fwd(c, a64_enc_b(0));
        patch_here(c, zero);                                 // exponent 0: zero, or a denormal (slow)
        x_slow(c, sl, a64_enc_cbnz_x(7, 0));
        emit(c, a64_enc_movz(0, 0, 0));
        emit(c, a64_enc_movz(15, 0, 0));
        patch_b_here(c, write);
        emit(c, a64_enc_str_uxtw(size, 0, 11, S_A));
        emit(c, a64_enc_movz(T0, 0x200, 0));
        emit(c, a64_enc_alu(A64_OP_BIC, X_SW, X_SW, T0));
        emit(c, a64_enc_alu(A64_OP_ORR, X_SW, X_SW, 15));
        if (reg == 3) x_pop(c);
        x_finish(c, in, op);
        return true;
    }
    int size = esc == 1 ? 4 : 8;
    x_guard(c, sl, false, fst, false);
    if (fld) {
        x_phys(c, 6, 7);
        x_need(c, sl, 6, true);
    } else {
        x_need(c, sl, X_TOP, false);
    }
    int n = ftlb_lookup(c, S_A, size, fst, miss);
    x_slow_add(c, sl, miss, n);
    if (fld) {
        fixup_t zero, store;
        if (size == 4) {
            emit(c, a64_enc_ldr_uxtw(4, 7, 11, S_A));
            emit(c, a64_enc_ubfx(8, 7, 23, 8));              // exponent
            emit(c, a64_enc_lsr_imm(13, 7, 31));             // sign
            zero = branch_fwd(c, a64_enc_cbz_w(8, 0));
            emit(c, a64_enc_cmp_imm(8, 0xFF));
            x_slow(c, sl, a64_enc_bcond(A64_EQ, 0));         // NaN / infinity
            emit(c, a64_enc_lsl_x_imm(14, 7, 40));
            mov_imm(c, 15, 16383 - 127);
        } else {
            emit(c, a64_enc_ldr_uxtw(8, 7, 11, S_A));
            emit(c, a64_enc_ubfx_x(8, 7, 52, 11));
            emit(c, a64_enc_lsr_x_imm(13, 7, 63));
            zero = branch_fwd(c, a64_enc_cbz_w(8, 0));
            emit(c, a64_enc_cmp_imm(8, 0x7FF));
            x_slow(c, sl, a64_enc_bcond(A64_EQ, 0));
            emit(c, a64_enc_lsl_x_imm(14, 7, 11));
            mov_imm(c, 15, 16383 - 1023);
        }
        emit(c, a64_enc_movz_x(7, 0x8000, 3));               // integer bit
        emit(c, a64_enc_orr_x(7, 7, 14));
        emit(c, a64_enc_alu(A64_OP_ADD, 8, 8, 15));
        emit(c, a64_enc_alu_lsl(A64_OP_ORR, 8, 8, 13, 15));
        emit(c, a64_enc_movz(0, 0, 0));                      // tag: valid
        store = branch_fwd(c, a64_enc_b(0));
        patch_here(c, zero);                                 // exponent 0: zero, or a denormal (slow)
        if (size == 4) emit(c, a64_enc_ubfx(14, 7, 0, 23)); else emit(c, a64_enc_lsl_x_imm(14, 7, 12));
        x_slow(c, sl, size == 4 ? a64_enc_cbnz_w(14, 0) : a64_enc_cbnz_x(14, 0));
        emit(c, a64_enc_movz_x(7, 0, 0));
        emit(c, a64_enc_lsl_imm(8, 13, 15));
        emit(c, a64_enc_movz(0, 1, 0));                      // tag: zero
        patch_b_here(c, store);
        x_off(c, 5, 6);
        x_store(c, 7, 8, 5);
        x_set_tag(c, 6, 0);
        x_set_top(c, 6);
        x_finish(c, in, op);
        return true;
    }
    // FST(P) m32/m64 with RC.
    int shift = size == 4 ? 40 : 11;                         // discarded significand bits
    x_off(c, 5, X_TOP);
    x_load(c, 7, 8, 5);
    emit(c, a64_enc_ubfx(13, 8, 0, 15));                     // exponent
    emit(c, a64_enc_lsr_imm(14, 8, 15));                     // sign
    emit(c, a64_enc_movz(15, 0, 0));                         // status bits to set (PE, C1)
    fixup_t zero = branch_fwd(c, a64_enc_cbz_w(13, 0));
    emit(c, a64_enc_add_imm(T0, 13, 1));
    emit(c, a64_enc_ubfx(T0, T0, 15, 1));
    x_slow(c, sl, a64_enc_cbnz_w(T0, 0));                    // NaN / infinity
    emit(c, a64_enc_lsr_x_imm(T0, 7, 63));
    x_slow(c, sl, a64_enc_cbz_w(T0, 0));                     // unsupported
    mov_imm(c, T0, size == 4 ? 16383 - 127 : 16383 - 1023);
    emit(c, a64_enc_alu(A64_OP_SUB, 13, 13, T0));            // float exponent (may wrap)
    emit(c, a64_enc_lsr_x_imm(4, 7, shift));                 // kept bits
    emit(c, a64_enc_lsl_x_imm(5, 7, 64 - shift));            // discarded bits, at the top
    emit(c, a64_enc_movz(T1, 1, 0));
    emit(c, a64_enc_alu(A64_OP_AND, 6, 4, T1));
    emit(c, a64_enc_ldrh(0, R_CPU, OFF_FPU_CW));
    emit(c, a64_enc_ubfx(0, 0, 10, 2));                      // RC
    x_round_up(c, 16, 15, 5, 6, 14, 0);
    emit(c, X64(a64_enc_alu(A64_OP_ADD, 4, 4, 16)));
    emit(c, a64_enc_lsr_x_imm(T0, 4, 64 - shift));
    fixup_t nocarry = branch_fwd(c, a64_enc_cbz_x(T0, 0));
    emit(c, a64_enc_lsr_x_imm(4, 4, 1));
    emit(c, a64_enc_add_imm(13, 13, 1));
    patch_here(c, nocarry);
    emit(c, a64_enc_sub_imm(T0, 13, 1));
    emit(c, a64_enc_cmp_imm(T0, size == 4 ? 253 : 2045));
    x_slow(c, sl, a64_enc_bcond(A64_HI, 0));                 // overflow / underflow: general code
    if (size == 4) {
        emit(c, a64_enc_ubfx(4, 4, 0, 23));
        emit(c, a64_enc_alu_lsl(A64_OP_ORR, 0, 4, 13, 23));
        emit(c, a64_enc_alu_lsl(A64_OP_ORR, 0, 0, 14, 31));
    } else {
        emit(c, a64_enc_ubfx_x(4, 4, 0, 52));
        emit(c, X64(a64_enc_alu_lsl(A64_OP_ORR, 0, 4, 13, 52)));
        emit(c, X64(a64_enc_alu_lsl(A64_OP_ORR, 0, 0, 14, 63)));
    }
    emit(c, a64_enc_lsl_imm(15, 15, 5));                     // PE
    emit(c, a64_enc_alu_lsl(A64_OP_ORR, 15, 15, 16, 9));     // C1 = rounded up
    fixup_t write = branch_fwd(c, a64_enc_b(0));
    patch_here(c, zero);                                     // exponent 0: zero, or a denormal (slow)
    x_slow(c, sl, a64_enc_cbnz_x(7, 0));
    if (size == 4) emit(c, a64_enc_lsl_imm(0, 14, 31));
    else emit(c, a64_enc_lsl_x_imm(0, 14, 63));
    patch_b_here(c, write);
    emit(c, a64_enc_str_uxtw(size, 0, 11, S_A));
    emit(c, a64_enc_movz(T0, 0x200, 0));
    emit(c, a64_enc_alu(A64_OP_BIC, X_SW, X_SW, T0));
    emit(c, a64_enc_alu(A64_OP_ORR, X_SW, X_SW, 15));
    if (reg == 3) x_pop(c);
    x_finish(c, in, op);
    return true;
}

static step_t tr_fpu(ctx_t *c, const insn_t *in)
{
    uint32_t op = (uint32_t)(in->opcode & 7) << 8 | (uint32_t)in->mod << 6 | (uint32_t)in->reg << 3 | (uint32_t)in->rm;
    slowlist_t sl = { .n = 0 };
    fixup_t fast_done = { 0 };
    bool inl = c->j->x87_inline && x87_inline(c, in, op, &sl);
    if (inl) {
        fast_done = branch_fwd(c, a64_enc_b(0));
        for (int k = 0; k < sl.n; k++) patch_here(c, sl.f[k]);
    }
    if (in->mod != 3) {
        emit_ea(c, in, S_EA, false);                         // offset within the segment
        ld(c, T0, OFF_SEG_BASE(in->mem.seg));
        emit(c, a64_enc_alu(A64_OP_ADD, 1, S_EA, T0));       // w1 = linear address
        emit(c, a64_enc_mov_w(2, S_EA));                     // w2 = offset
        mov_imm(c, 3, c->eip);
        mov_imm(c, 4, op | (uint32_t)in->mem.seg << 16);
        call(c, (const void *)fpu_jit_mem);
    } else {
        mov_imm(c, 1, c->eip);
        mov_imm(c, 2, op);
        call(c, (const void *)fpu_jit_reg);
    }
    fixup_t done = branch_fwd(c, a64_enc_cbz_w(0, 0));
    step_t r = tr_helper(c, in, false);
    patch_here(c, done);
    if (inl) patch_b_here(c, fast_done);
    return r;
}

static bool lockable(op_t op)
{
    switch (op) {
    case OP_ADD: case OP_ADC: case OP_AND: case OP_BTC: case OP_BTR: case OP_BTS:
    case OP_CMPXCHG: case OP_CMPXCHG8B: case OP_DEC: case OP_INC: case OP_NEG: case OP_NOT:
    case OP_OR: case OP_SBB: case OP_SUB: case OP_XOR: case OP_XADD: case OP_XCHG:
        return true;
    default:
        return false;
    }
}

static step_t translate_insn(ctx_t *c, const insn_t *in)
{
    // LOCK: guest CPUs run round-robin on one host thread and switch only between
    // blocks, so a plain read-modify-write is already atomic. (Per-CPU host threads
    // will need LSE atomics here.) REP is ignored on non-string instructions.
    if (in->lock && !(lockable(in->op) && in->dst.kind == OPK_MEM)) return ST_INVALID;

    // c->cc_known stays valid across inline instructions: those that change flags all
    // go through set_cc(), which updates it (after reading the old CF if they need it).
    // C helpers and CLC/STC/CMC change flags behind its back.
    if (!c->j->inline_flags) c->cc_known = 0;

    bool ends;
    if (c->j->inline_alu) {
        if (in->op == OP_SHL || in->op == OP_SHR || in->op == OP_SAR) return tr_shift(c, in);
        if ((in->op == OP_MUL || in->op == OP_IMUL) && in->dst.size == 4 && in->dst.kind != OPK_NONE &&
            (in->src.kind == OPK_NONE || in->dst.kind == OPK_REG))
            return tr_mul(c, in);
    }
    if (in->op == OP_FPU && c->j->x87_direct && fpu_jit_handles(in)) {
        c->cc_known = 0;
        return tr_fpu(c, in);
    }
    if (helper_supports(in, &ends)) {
        c->cc_known = 0;
        return tr_helper(c, in, ends);
    }

    switch (in->op) {
    case OP_NOP:
        // PAUSE (F3 90) in a spin loop: let the other CPUs run (the lock holder among them).
        if (in->rep == 1 && c->j->pause_yields) { finish_at(c, seq_eip(c, in), JIT_EXIT_REQUEST); return ST_END; }
        return ST_NEXT;
    case OP_MOV:  return tr_mov(c, in);
    case OP_ADD: case OP_ADC: case OP_SUB: case OP_SBB: case OP_CMP:
    case OP_AND: case OP_OR:  case OP_XOR: case OP_TEST:
        return tr_alu(c, in);
    case OP_INC: case OP_DEC: return tr_incdec(c, in);
    case OP_NOT: case OP_NEG: return tr_notneg(c, in);
    case OP_XCHG: return tr_xchg(c, in);
    case OP_MOVZX: case OP_MOVSX: return tr_movx(c, in);
    case OP_LEA:  return tr_lea(c, in);
    case OP_PUSH: return tr_push(c, in);
    case OP_POP:  return tr_pop(c, in);
    case OP_CALL: return tr_call(c, in);
    case OP_JMP:  return tr_jmp(c, in);
    case OP_RET:  return tr_ret(c, in);
    case OP_LEAVE: return tr_leave(c, in);
    case OP_CLI:  return tr_flagbit(c, EFL_IF, false);
    case OP_STI:  return tr_flagbit(c, EFL_IF, true);
    case OP_CLD:  return tr_flagbit(c, EFL_DF, false);
    case OP_STD:  return tr_flagbit(c, EFL_DF, true);
    case OP_CLC:  return tr_carry(c, A64_OP_BIC);
    case OP_STC:  return tr_carry(c, A64_OP_ORR);
    case OP_CMC:  return tr_carry(c, A64_OP_EOR);
    case OP_JCC:  return tr_jcc(c, in);
    case OP_LOOP: case OP_LOOPE: case OP_LOOPNE: case OP_JCXZ: return tr_loop(c, in);
    case OP_SETCC: return tr_setcc(c, in);
    case OP_CMOVCC: return tr_cmov(c, in);

    default:
        return ST_FAIL;
    }
}

// Translate the block at linear address lin (physical phys). Blocks never cross a
// 4 KB page (the last instruction may straddle into the next one). A block embeds
// absolute eips, so it is only valid at the CS:EIP it was translated for (the cache
// key includes them); other address spaces mapping the same page there can share it.
static bool translate(jit_t *j, cpu_t *cpu, uint32_t lin, uint32_t phys, jit_block_info *info,
                      uint32_t pages_out[2], int *n_pages_out)
{
    size_t free_words = (size_t)((j->buf + j->buf_words) - j->top);
    if (free_words < MIN_FREE_WORDS || j->n_entries >= MAX_ENTRIES) {
        j->flush_full++;
        j->flush_other--;
        jit_flush(j);
    }

    bool code32 = cpu_code32(cpu);
    // The first instruction may fault (#PF) while being fetched: do that before the
    // code buffer is made writable (a fault longjmps out of here).
    {
        uint8_t first[X86_MAX_INSN_LEN];
        insn_t tmp;
        size_t got = cpu_fetch(cpu, lin, first, sizeof first, false);
        if (got < sizeof first && decode_insn(first, got, code32, &tmp) == 0)
            cpu_fetch(cpu, lin + (uint32_t)got, first + got, 1, true);   // faults on the missing byte
    }

    uint32_t *start = j->top;
    ctx_t c = {
        .e = { start, j->buf + j->buf_words, false },
        .j = j,
        .eip = cpu->eip,
        .code32 = code32,
        .ss32 = cpu_stack32(cpu),
        .user = cpu->cpl == 3,
        .cs_base = cpu->seg[S_CS].base,
        .lin_page = lin & ~0xFFFu,
        .mode = block_mode(cpu),
    };
    uint32_t cs_base = cpu->seg[S_CS].base;
    uint32_t pages[2] = { phys & ~0xFFFu, 0 };
    int n_pages = 1;

    write_begin(j);
    prologue(&c);
    assert(c.e.p - start == PROLOGUE_WORDS);
    bool sti_window = false;
    for (;;) {
        uint32_t ilin = cs_base + c.eip;
        if (c.insns && ((ilin ^ lin) & ~0xFFFu)) { finish_chain(&c, c.eip); break; }   // next page
        uint8_t bytes[X86_MAX_INSN_LEN];
        size_t got = cpu_fetch(cpu, ilin, bytes, sizeof bytes, false);
        insn_t in;
        int len = decode_insn(bytes, got, c.code32, &in);

        if (len == 0 && got < sizeof bytes && c.insns) { finish(&c, JIT_EXIT_OK); break; }  // not fetchable yet
        if (len == 0 || in.op == OP_INVALID) {
            finish(&c, JIT_EXIT_INVALID);
            break;
        }
        if (n_pages == 1 && ((ilin + (uint32_t)len - 1) ^ ilin) & ~0xFFFu) {   // straddles into the next page
            uint32_t p2;
            if (cpu_fetch_phys(cpu, ilin + (uint32_t)len - 1, false, &p2)) pages[n_pages++] = p2 & ~0xFFFu;
        }
        uint32_t *insn_start = c.e.p;
        regcache_t rc_before = c.rc;
        c.wrote_mem = false;
        rc_bind_insn(&c, &in);
        step_t s = translate_insn(&c, &in);
        if (s == ST_NEXT && c.wrote_mem) {
            // Self-modifying code: if this instruction's stores hit translated code,
            // leave the block now; the rest of it may be stale.
            uint32_t next = c.code32 ? c.eip + (uint32_t)len : ((c.eip + (uint32_t)len) & 0xFFFF);
            emit(&c, a64_enc_ldrb(T0, R_CPU, OFF_SMC));
            uint32_t *site = c.e.p;
            emit(&c, 0);
            emit(&c, a64_enc_strb(A64_ZR, R_CPU, OFF_SMC));
            mov_imm(&c, T0, next);
            st(&c, T0, OFF_EIP);
            epilogue(&c, JIT_EXIT_OK);
            if (!c.e.overflow && site < c.e.end) *site = a64_enc_cbz_w(T0, (int32_t)(c.e.p - site));
            c.exit = JIT_EXIT_OK;
        }
        if (s == ST_FAIL || s == ST_INVALID) {
            c.e.p = insn_start;          // drop any partial code for this instruction
            c.rc = rc_before;            // and its register bindings
            c.e.overflow = false;
            finish(&c, s == ST_INVALID ? JIT_EXIT_INVALID : JIT_EXIT_UNSUPPORTED);
            break;
        }
        c.insns++;
        c.bytes += (uint32_t)len;
        if (s == ST_END) break;

        c.eip += (uint32_t)len;
        if (!c.code32) c.eip &= 0xFFFF;
        // STI enables interrupts after the next instruction: end the block right there,
        // so a pending interrupt is taken in the one-instruction window (sti; nop; cli).
        if (sti_window) { finish_chain(&c, c.eip); break; }
        sti_window = in.op == OP_STI;
        if (c.insns >= (j->single_step ? 1u : MAX_BLOCK_INSNS)) { finish_chain(&c, c.eip); break; }
        if (c.e.overflow) break;
    }
    size_t nwords = (size_t)(c.e.p - start);
    write_end(j, start, nwords);
    if (c.e.overflow) return false;
    j->top = c.e.p;
    j->translated++;

    // The caller registers the pages the block was decoded from (insert()).
    pages_out[0] = pages[0];
    pages_out[1] = pages[1];
    *n_pages_out = n_pages;

    info->code = start;
    info->nwords = (uint32_t)nwords;
    info->guest_insns = c.insns;
    info->guest_bytes = c.bytes;
    info->end_eip = c.eip;
    info->dyn_exit = c.dyn_exit;
    info->exit = c.exit;
    return true;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

jit_t *jit_create(mem_bus_t *bus, size_t code_bytes)
{
    jit_t *j = calloc(1, sizeof(*j));
    if (!j) return NULL;
    j->bus = bus;
    j->buf_words = code_bytes / 4;
    j->buf = alloc_code(code_bytes, &j->executable);
    j->bucket = calloc(TABLE_SIZE, sizeof(uint32_t));
    j->pool = calloc(MAX_ENTRIES, sizeof(entry_t));
    j->page_head = calloc(MEM_NUM_PAGES, sizeof(uint32_t));
    j->code_bits = calloc(MEM_NUM_PAGES, sizeof(uint64_t));
    if (!j->buf || !j->bucket || !j->pool || !j->page_head || !j->code_bits) { jit_destroy(j); return NULL; }
    // A shared exit at the start of the buffer (kept across flushes): invalidated
    // blocks branch here, returning JIT_EXIT_OK with the eip their caller stored.
    j->exit_stub = j->buf;
    write_begin(j);
    {
        ctx_t c = { .e = { j->buf, j->buf + j->buf_words, false }, .j = j };
        epilogue(&c, JIT_EXIT_OK);
        static const int AOP[3] = { 0, 1, 4 };
        for (int k = 0; k < 3; k++) {
            j->x87_core[k] = c.e.p;
            slowlist_t sl = { .n = 0 };
            x_arith_compute(&c, &sl, AOP[k]);
            emit(&c, a64_enc_movz(0, 0, 0));
            emit(&c, A64_RET);
            for (int f = 0; f < sl.n; f++) patch_here(&c, sl.f[f]);
            emit(&c, a64_enc_movz(0, 1, 0));
            emit(&c, A64_RET);
        }
        j->stub_words = (uint32_t)(c.e.p - j->buf);
    }
    write_end(j, j->buf, j->stub_words);
    j->top = j->buf + j->stub_words;
    j->trace_lo = 1; j->trace_hi = 0;           // tracing off
    mem_set_code_write_cb(bus, on_code_write, j);
    mem_set_map_change_cb(bus, on_map_change, j);
    // Off by default: measured 3-7% slower on R5 (bash loop, gcc; see CLAUDE.md). BEEMU_REGCACHE=1.
    const char *rc = getenv("BEEMU_REGCACHE");
    j->regcache = rc && *rc == '1';
    const char *nf = getenv("BEEMU_NO_FASTMEM");
    j->fastmem = !(nf && *nf == '1');
    const char *sv = getenv("BEEMU_STATS");
    j->stats = sv && *sv == '1';
    const char *nj = getenv("BEEMU_NO_JUMP_CACHE");
    j->jump_cache = !(nj && *nj == '1');
    const char *ni = getenv("BEEMU_NO_INLINE_FLAGS");
    j->inline_flags = !(ni && *ni == '1');
    const char *nx = getenv("BEEMU_NO_X87_JIT");
    j->x87_direct = !(nx && *nx == '1');
    const char *nxi = getenv("BEEMU_NO_X87_INLINE");
    j->x87_inline = !(nxi && *nxi == '1');
    const char *na = getenv("BEEMU_NO_INLINE_ALU");
    j->inline_alu = !(na && *na == '1');
    const char *nc = getenv("BEEMU_NO_CHAIN");
    j->chaining = !(nc && *nc == '1');
    return j;
}

void jit_destroy(jit_t *j)
{
    if (!j) return;
    const char *st = getenv("BEEMU_STATS");
    if (st && *st == '1')
        fprintf(stderr, "[jit] %llu blocks translated; code writes invalidated %llu pages (%llu blocks); "
                "flushes: %llu full, %llu other; %llu chain links\n",
                (unsigned long long)j->translated, (unsigned long long)j->inval_pages,
                (unsigned long long)j->inval_blocks, (unsigned long long)j->flush_full,
                (unsigned long long)j->flush_other, (unsigned long long)j->links);
    if (st && *st == '1') {
        fprintf(stderr, "[jit] %llu dispatcher entries; hottest guest addresses:\n", (unsigned long long)j->dispatches);
        for (int r = 0; r < 12; r++) {
            int best = -1;
            for (int k = 0; k < 4096; k++) if (j->hot[k].n && (best < 0 || j->hot[k].n > j->hot[best].n)) best = k;
            if (best < 0) break;
            fprintf(stderr, "    %08X  %u\n", j->hot[best].lin, j->hot[best].n);
            j->hot[best].n = 0;
        }
    }
#if JIT_HOST_ARM64
    if (j->buf) munmap(j->buf, j->buf_words * 4);
#else
    free(j->buf);
#endif
    free(j->bucket);
    free(j->pool);
    free(j->page_head);
    free(j->code_bits);
    for (arena_chunk_t *a = j->arena_head; a; ) {
        arena_chunk_t *next = a->next;
        free(a);
        a = next;
    }
    free(j);
}

// Translation-time assumptions a cached block depends on.
static uint32_t block_mode(const cpu_t *cpu)
{
    return (cpu_code32(cpu) ? 1u : 0u) | (cpu_stack32(cpu) ? 2u : 0u) | (cpu->cpl == 3 ? 4u : 0u);
}

bool jit_get_block(jit_t *j, cpu_t *cpu, jit_block_info *out)
{
    // Blocks are cached by physical address, mode, and the CS:EIP they were translated at:
    // a CR3 switch keeps them valid; a page also mapped elsewhere gets its own blocks there.
    if (!cpu->mem) cpu->mem = j->bus;                // code is fetched through the CPU (paging)
    if (!j->n_cpus || j->cpus[0] != cpu) register_cpu(j, cpu);
    uint32_t lin = cpu->seg[S_CS].base + cpu->eip, phys;
    cpu_fetch_phys(cpu, lin, true, &phys);           // #PF if the code page is not mapped
    uint32_t mode = block_mode(cpu);
    entry_t *e = lookup(j, phys, mode, cpu->eip, cpu->seg[S_CS].base);
    if (e) { *out = e->info; return true; }

    jit_block_info info;
    uint32_t pages[2];
    int n_pages;
    if (!translate(j, cpu, lin, phys, &info, pages, &n_pages)) return false;
    // translate() may have flushed the cache, so insert after it returns.
    insert(j, phys, mode, cpu->eip, cpu->seg[S_CS].base, pages, n_pages, &info);
    *out = info;
    return true;
}

// Point a chainable exit's slot at a block body (see finish_chain()).
static void link_slot(jit_t *j, uint32_t *slot, const uint32_t *body)
{
    write_begin(j);
    *slot = a64_enc_b((int32_t)(body - slot));
    write_end(j, slot, 1);
    j->links++;
}

jit_exit_t jit_run(jit_t *j, cpu_t *cpu, uint64_t max_blocks)
{
#if JIT_HOST_ARM64
    typedef uint32_t (*block_fn)(cpu_t *);
    if (!cpu->mem) cpu->mem = j->bus;   // helpers reach memory through the CPU
    if (!j->n_cpus || j->cpus[0] != cpu) register_cpu(j, cpu);
    volatile uint64_t n = 0;
    // The unlinked chain slot the previous block left through, and the state it
    // expects: the next block is linked to it only if nothing else intervened.
    uint32_t *volatile chain_slot = NULL;
    uint32_t chain_lin = 0, chain_mode = 0;
    uint64_t chain_gen = 0;
    cpu->stop = 0;
    cpu->delivering = 0;
    // Faults raised by helpers are delivered and then longjmp here, abandoning
    // the block that was running (see system.h).
    if (setjmp(cpu->fault_jmp)) {
        chain_slot = NULL;
        if (cpu->stop) {
            jit_exit_t x = (jit_exit_t)cpu->stop;
            cpu->stop = 0;
            return x;
        }
        n = n + 1;
    }
    while (n < max_blocks) {
        // Hardware interrupts are taken between blocks.
        if (cpu->intr_line && (cpu->eflags & EFL_IF) && cpu->intr_ack && !cpu->intr_shadow) {
            cpu->halted = false;
            int vec = cpu->intr_ack(cpu->intr_opaque);
            static int trace = -1;
            if (trace < 0) { const char *e = getenv("BEEMU_TRACE_VEC"); trace = e && *e == '1'; }
            if (trace && vec != 0x40 && vec != 0x08)    // debugging aid (timer vectors left out)
                fprintf(stderr, "[vec %02X] at %04X:%08X\n", vec, cpu->seg[S_CS].selector, cpu->eip);
            cpu_interrupt(cpu, vec, cpu->eip, false, false, 0);
        }
        cpu->intr_shadow = false;
        if (cpu->exit_request) { cpu->exit_request = false; return JIT_EXIT_REQUEST; }
        if (cpu->halted) return JIT_EXIT_HALT;
        jit_block_info b;
        if (!jit_get_block(j, cpu, &b)) return JIT_EXIT_INVALID;
        bool tracing = j->trace_lo <= j->trace_hi;
        if (!tracing) {                                       // jump cache for ret / indirect jumps
            uint32_t jl = cpu->seg[S_CS].base + cpu->eip;
            jc_entry_t *je = &cpu->jc[(jl + (jl >> 12)) & (JC_SIZE - 1)];
            je->lin = jl;
            je->mode = block_mode(cpu);
            je->body = b.code + PROLOGUE_WORDS;
        }
        if (chain_slot && chain_gen == j->flush_gen && !tracing &&
            chain_lin == cpu->seg[S_CS].base + cpu->eip && chain_mode == block_mode(cpu))
            link_slot(j, chain_slot, b.code + PROLOGUE_WORDS);
        chain_slot = NULL;
        uint32_t lin = cpu->seg[S_CS].base + cpu->eip;
        j->trace[j->trace_pos++ % JIT_TRACE_LEN] = lin;
        j->dispatches++;
        if (j->stats) {
            uint32_t h = (lin * 2654435761u) >> 20;
            for (int k = 0; k < 16; k++, h = (h + 1) & 4095)   // short probe: a full table must not stall dispatch
                if (!j->hot[h].n || j->hot[h].lin == lin) { j->hot[h].lin = lin; j->hot[h].n++; break; }
        }
        if (lin >= j->trace_lo && lin <= j->trace_hi) {
            uint8_t ib[X86_MAX_INSN_LEN];
            insn_t in;
            char txt[96] = "?";
            uint32_t pa;
            if (cpu_peek_phys(cpu, lin, &pa)) mem_read_block(j->bus, pa, ib, sizeof ib);
            else memset(ib, 0xFF, sizeof ib);
            if (decode_insn(ib, sizeof ib, cpu_code32(cpu), &in)) insn_format(&in, cpu->eip, txt, sizeof txt);
            fprintf(stderr, "%08X  %-34s eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X esp=%08X fl=%03X\n",
                    lin, txt, cpu->gpr[0], cpu->gpr[3], cpu->gpr[1], cpu->gpr[2], cpu->gpr[6], cpu->gpr[7],
                    cpu->gpr[4], cpu_calc_flags(cpu) & 0xFFF);
        }
        // Chained exits count against the block budget, so time slices stay the same size.
        uint64_t left = max_blocks - n;
        int32_t budget = tracing ? 1 : left > (1u << 30) ? (1 << 30) : (int32_t)left;
        cpu->chain_budget = budget;
        cpu->chain_from = NULL;
        block_fn fn;
        memcpy(&fn, &b.code, sizeof fn);
        jit_exit_t x = (jit_exit_t)fn(cpu);
        int32_t used = budget - cpu->chain_budget;
        n = n + (uint64_t)(used > 0 ? used : 1);
        if (cpu->chain_from) {
            chain_slot = cpu->chain_from;
            chain_lin = cpu->seg[S_CS].base + cpu->eip;
            chain_mode = block_mode(cpu);
            chain_gen = j->flush_gen;
        }
        if (x != JIT_EXIT_OK) return x;
    }
    return JIT_EXIT_LIMIT;
#else
    (void)j; (void)cpu; (void)max_blocks;
    return JIT_EXIT_NO_HOST;
#endif
}

uint64_t jit_blocks_translated(const jit_t *j) { return j->translated; }
uint64_t jit_chain_links(const jit_t *j) { return j->links; }
uint64_t jit_dispatches(const jit_t *j) { return j->dispatches; }

void jit_set_single_step(jit_t *j, bool on) { j->single_step = on; jit_flush(j); }
void jit_set_trace(jit_t *j, uint32_t lo, uint32_t hi)
{
    j->trace_lo = lo; j->trace_hi = hi;
    jit_flush(j);       // drop chain links and jump-cache entries so traced blocks go through the dispatcher
}

int jit_recent_blocks(const jit_t *j, uint32_t *out, int max)
{
    int n = j->trace_pos < JIT_TRACE_LEN ? (int)j->trace_pos : JIT_TRACE_LEN;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = j->trace[(j->trace_pos - (uint32_t)n + (uint32_t)i) % JIT_TRACE_LEN];
    return n;
}

void jit_set_x87_inline(jit_t *j, bool on) { j->x87_inline = on; jit_flush(j); }
void jit_set_regcache(jit_t *j, bool on) { j->regcache = on; jit_flush(j); }

void jit_set_smp(jit_t *j, bool on) { j->pause_yields = on; }
