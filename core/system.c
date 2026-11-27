#include "system.h"
#include "flags.h"
#include "jit.h"
#include "mem.h"
#include <setjmp.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Linear memory and the MMU                                           */
/* ------------------------------------------------------------------ */

enum { ACC_READ, ACC_WRITE, ACC_FETCH };

static bool user_mode(const cpu_t *c) { return c->cpl == 3 && !c->sys_access; }

void cpu_tlb_flush(cpu_t *c, bool keep_global)
{
    for (int i = 0; i < TLB_SIZE; i++)
        if (!keep_global || !(c->tlb[i].flags & TLB_G)) c->tlb[i].vpn1 = 0;
    cpu_ftlb_flush(c);    // the fast TLB keeps no global entries
}

void cpu_tlb_invlpg(cpu_t *c, uint32_t lin)
{
    tlb_entry_t *e = &c->tlb[(lin >> 12) & (TLB_SIZE - 1)];
    if (e->vpn1 == (lin >> 12) + 1) e->vpn1 = 0;
    ftlb_entry_t *f = &c->ftlb[(lin >> 12) & (FTLB_SIZE - 1)];
    if (f->vpn1 == (lin >> 12) + 1) memset(f, 0, sizeof *f);
    for (int i = 0; i < JC_SIZE; i++)
        if (c->jc[i].body && (c->jc[i].lin >> 12) == (lin >> 12)) c->jc[i].body = NULL, c->jc[i].lin = 0;
}

/* ---- fast TLB (generated code's inline memory path) ---- */

void cpu_ftlb_flush(cpu_t *c)
{
    memset(c->ftlb, 0, sizeof c->ftlb);
    memset(c->jc, 0, sizeof c->jc);      // jump-cache targets were found through old mappings
}

void cpu_ftlb_drop_writes(cpu_t *c, uint32_t phys_page)
{
    for (int i = 0; i < FTLB_SIZE; i++)
        if (c->ftlb[i].vpn1 && c->ftlb[i].phys == phys_page) c->ftlb[i].tag[1] = c->ftlb[i].tag[3] = 0;
}

void cpu_ftlb_fill(cpu_t *c, uint32_t lin, bool write)
{
    mem_bus_t *bus = c->mem;
    uint32_t vpn = lin >> 12, frame;
    bool user = user_mode(c);
    if (c->cr0 & CR0_PG) {
        const tlb_entry_t *e = &c->tlb[vpn & (TLB_SIZE - 1)];
        if (e->vpn1 != vpn + 1) return;                      // e.g. evicted by a page-crossing access
        if (user && !(e->flags & TLB_U)) return;
        if (write && !((e->flags & TLB_D) && ((e->flags & TLB_W) || (!user && !(c->cr0 & CR0_WP))))) return;
        frame = e->pfn;
    } else {
        frame = lin & ~0xFFFu;
    }
    uint32_t pg = frame >> 12;
    uint8_t *host = bus->page_ptr[pg];
    if (!host) return;                                       // MMIO or open bus
    if (write && (bus->page_flags[pg] & (PG_ROM | PG_CODE))) return;
    ftlb_entry_t *f = &c->ftlb[vpn & (FTLB_SIZE - 1)];
    if (f->vpn1 != vpn + 1 || f->phys != pg) {
        memset(f, 0, sizeof *f);
        f->vpn1 = vpn + 1;
        f->phys = pg;
        f->addend = (uint64_t)(uintptr_t)host - ((uint64_t)vpn << 12);
    }
    int u = user ? 2 : 0;
    f->tag[u] = vpn + 1;                                     // a write permission implies read
    if (write) f->tag[u + 1] = vpn + 1;
}

static _Noreturn void page_fault(cpu_t *c, uint32_t lin, bool present, bool write, bool user)
{
    c->cr2 = lin;
    cpu_fault(c, EXC_PF, (present ? 1u : 0) | (write ? 2u : 0) | (user ? 4u : 0));
}

// Walk the two-level page tables (4 MB pages with CR4.PSE). On success returns true
// with the physical page address and fills the TLB. commit: set accessed/dirty bits.
// fault: raise #PF on failure instead of returning false.
static bool walk(cpu_t *c, uint32_t lin, int acc, bool user, bool commit, bool fault, uint32_t *pfn)
{
    mem_bus_t *bus = c->mem;
    bool write = acc == ACC_WRITE;
    uint32_t pde_a = (c->cr3 & 0xFFFFF000u) | ((lin >> 20) & 0xFFCu);
    uint32_t pde = mem_read(bus, pde_a, 4);
    if (!(pde & 1)) { if (fault) page_fault(c, lin, false, write, user); return false; }
    bool big = (pde & 0x80) && (c->cr4 & CR4_PSE);
    uint32_t pte_a = 0, pte = 0, frame, leaf;
    bool u_ok, w_ok;
    if (big) {
        frame = (pde & 0xFFC00000u) | (lin & 0x003FF000u);
        leaf = pde;
        u_ok = pde & 4;
        w_ok = pde & 2;
    } else {
        pte_a = (pde & 0xFFFFF000u) | ((lin >> 10) & 0xFFCu);
        pte = mem_read(bus, pte_a, 4);
        if (!(pte & 1)) { if (fault) page_fault(c, lin, false, write, user); return false; }
        frame = pte & 0xFFFFF000u;
        leaf = pte;
        u_ok = pde & pte & 4;
        w_ok = pde & pte & 2;
    }
    if ((user && !u_ok) || (write && !w_ok && (user || (c->cr0 & CR0_WP)))) {
        if (fault) page_fault(c, lin, true, write, user);
        return false;
    }
    if (commit) {
        if (big) {
            uint32_t n = pde | 0x20 | (write ? 0x40 : 0);
            if (n != pde) mem_write(bus, pde_a, n, 4);
            leaf = n;
        } else {
            if (!(pde & 0x20)) mem_write(bus, pde_a, pde | 0x20, 4);
            uint32_t n = pte | 0x20 | (write ? 0x40 : 0);
            if (n != pte) mem_write(bus, pte_a, n, 4);
            leaf = n;
        }
        tlb_entry_t *e = &c->tlb[(lin >> 12) & (TLB_SIZE - 1)];
        e->vpn1 = (lin >> 12) + 1;
        e->pfn = frame;
        e->flags = (uint8_t)((u_ok ? TLB_U : 0) | (w_ok ? TLB_W : 0) | ((leaf & 0x40) ? TLB_D : 0) |
                             (((leaf & 0x100) && (c->cr4 & CR4_PGE)) ? TLB_G : 0));
    }
    *pfn = frame;
    return true;
}

// Linear -> physical for one access; faults on failure.
static uint32_t translate(cpu_t *c, uint32_t lin, int acc)
{
    if (!(c->cr0 & CR0_PG)) return lin;
    bool user = user_mode(c);
    const tlb_entry_t *e = &c->tlb[(lin >> 12) & (TLB_SIZE - 1)];
    if (e->vpn1 == (lin >> 12) + 1) {
        bool ok = !user || (e->flags & TLB_U);
        if (acc == ACC_WRITE)
            ok = ok && (e->flags & TLB_D) && ((e->flags & TLB_W) || (!user && !(c->cr0 & CR0_WP)));
        if (ok) return e->pfn | (lin & 0xFFFu);
    }
    uint32_t pfn;
    walk(c, lin, acc, user, true, true, &pfn);
    return pfn | (lin & 0xFFFu);
}

// The fast TLB (the one generated code reads inline) also serves C helpers: a hit is a plain
// host load or store (cpu_fast_host in system.h).
uint32_t cpu_read_lin(cpu_t *c, uint32_t lin, int size)
{
    uint8_t *h = cpu_fast_host(c, lin, size, 0);
    if (h) {                                          // fixed-size copies: single loads, no library call
        if (size == 4) { uint32_t v; memcpy(&v, h, 4); return v; }
        if (size == 2) { uint16_t v; memcpy(&v, h, 2); return v; }
        return *h;
    }
    if (!(c->cr0 & CR0_PG)) { uint32_t v = mem_read(c->mem, lin, size); cpu_ftlb_fill(c, lin, false); return v; }
    if ((lin & 0xFFFu) + (uint32_t)size <= 0x1000u) {
        uint32_t v = mem_read(c->mem, translate(c, lin, ACC_READ), size);
        cpu_ftlb_fill(c, lin, false);
        return v;
    }
    uint32_t v = 0;                                   // crosses a page: byte by byte
    for (int i = 0; i < size; i++)
        v |= mem_read(c->mem, translate(c, lin + (uint32_t)i, ACC_READ), 1) << (8 * i);
    return v;
}

void cpu_write_lin(cpu_t *c, uint32_t lin, uint32_t value, int size)
{
    uint8_t *h = cpu_fast_host(c, lin, size, 1);
    if (h) {
        if (size == 4) memcpy(h, &value, 4);
        else if (size == 2) { uint16_t v = (uint16_t)value; memcpy(h, &v, 2); }
        else *h = (uint8_t)value;
        return;
    }
    if ((lin & 0xFFFu) + (uint32_t)size <= 0x1000u) {
        uint32_t pa = (c->cr0 & CR0_PG) ? translate(c, lin, ACC_WRITE) : lin;
        bool was = c->mem->code_written;
        c->mem->code_written = false;
        mem_write(c->mem, pa, value, size);
        if (!c->mem->code_written) cpu_ftlb_fill(c, lin, true);   // not on a code page (it just got invalidated)
        c->mem->code_written |= was;
        return;
    }
    if (!(c->cr0 & CR0_PG)) { mem_write(c->mem, lin, value, size); return; }
    // Crosses a page: both pages must be writable before any byte is written.
    uint32_t second = (lin | 0xFFFu) + 1;
    uint32_t p1 = translate(c, lin, ACC_WRITE), p2 = translate(c, second, ACC_WRITE);
    for (int i = 0; i < size; i++) {
        uint32_t a = lin + (uint32_t)i;
        uint32_t pa = a < second ? p1 + (a - lin) : p2 + (a - second);
        mem_write(c->mem, pa, (value >> (8 * i)) & 0xFF, 1);
    }
}

void cpu_probe_write(cpu_t *c, uint32_t lin, uint32_t len)
{
    if (!(c->cr0 & CR0_PG) || !len) return;
    uint32_t last = lin + len - 1;
    for (uint32_t pg = lin & ~0xFFFu;; pg += 0x1000) {
        translate(c, pg < lin ? lin : pg, ACC_WRITE);
        if ((pg >> 12) == (last >> 12)) break;
    }
}

bool cpu_fetch_phys(cpu_t *c, uint32_t lin, bool fault, uint32_t *phys)
{
    if (!(c->cr0 & CR0_PG)) { *phys = lin; return true; }
    bool user = user_mode(c);
    const tlb_entry_t *e = &c->tlb[(lin >> 12) & (TLB_SIZE - 1)];
    if (e->vpn1 == (lin >> 12) + 1 && (!user || (e->flags & TLB_U))) { *phys = e->pfn | (lin & 0xFFFu); return true; }
    uint32_t pfn;
    if (!walk(c, lin, ACC_FETCH, user, true, fault, &pfn)) return false;
    *phys = pfn | (lin & 0xFFFu);
    return true;
}

size_t cpu_fetch(cpu_t *c, uint32_t lin, uint8_t *buf, size_t len, bool fault)
{
    size_t got = 0;
    while (got < len) {
        uint32_t a = lin + (uint32_t)got, phys;
        if (!cpu_fetch_phys(c, a, fault, &phys)) break;
        size_t chunk = 0x1000u - (a & 0xFFFu);
        if (chunk > len - got) chunk = len - got;
        mem_read_block(c->mem, phys, buf + got, chunk);
        got += chunk;
    }
    return got;
}

bool cpu_peek_phys(cpu_t *c, uint32_t lin, uint32_t *phys)
{
    if (!(c->cr0 & CR0_PG)) { *phys = lin; return true; }
    uint32_t pfn;
    uint8_t saved = c->sys_access;
    c->sys_access = 1;
    bool ok = walk(c, lin, ACC_READ, false, false, false, &pfn);
    c->sys_access = saved;
    if (ok) *phys = pfn | (lin & 0xFFFu);
    return ok;
}

// Implicit supervisor accesses: descriptor tables, TSS, interrupt-delivery stacks.
static uint32_t sys_read(cpu_t *c, uint32_t lin, int size)
{
    uint8_t saved = c->sys_access;
    c->sys_access = 1;
    uint32_t v = cpu_read_lin(c, lin, size);
    c->sys_access = saved;
    return v;
}

static void sys_write(cpu_t *c, uint32_t lin, uint32_t v, int size)
{
    uint8_t saved = c->sys_access;
    c->sys_access = 1;
    cpu_write_lin(c, lin, v, size);
    c->sys_access = saved;
}

/* ------------------------------------------------------------------ */
/* Stack                                                               */
/* ------------------------------------------------------------------ */

// A stack described by its segment and pointer, so frames can be built on a
// new stack (interrupt privilege change) before any CPU state is committed.
typedef struct {
    const seg_t *ss;
    bool         big;     // 32-bit stack (ESP) or 16-bit (SP)
    uint32_t     esp;
} stk_t;

static stk_t cur_stack(cpu_t *c)
{
    return (stk_t){ &c->seg[S_SS], cpu_stack32(c), c->gpr[R_ESP] };
}

static void stk_push(cpu_t *c, stk_t *s, uint32_t v, int size)
{
    uint32_t sp = s->esp - (uint32_t)size;
    if (!s->big) sp = (s->esp & 0xFFFF0000u) | (sp & 0xFFFFu);
    s->esp = sp;
    cpu_write_lin(c, s->ss->base + (s->big ? sp : (sp & 0xFFFFu)), v, size);
}

static uint32_t stk_peek(cpu_t *c, const stk_t *s, uint32_t off, int size)
{
    uint32_t sp = s->esp + off;
    if (!s->big) sp &= 0xFFFFu;
    return cpu_read_lin(c, s->ss->base + sp, size);
}

static void stk_add(stk_t *s, uint32_t n)
{
    if (s->big) s->esp += n;
    else        s->esp = (s->esp & 0xFFFF0000u) | ((s->esp + n) & 0xFFFFu);
}

void cpu_push(cpu_t *c, uint32_t value, int size)
{
    stk_t s = cur_stack(c);
    stk_push(c, &s, value, size);
    c->gpr[R_ESP] = s.esp;
}

uint32_t cpu_pop(cpu_t *c, int size)
{
    stk_t s = cur_stack(c);
    uint32_t v = stk_peek(c, &s, 0, size);
    stk_add(&s, (uint32_t)size);
    c->gpr[R_ESP] = s.esp;
    return v;
}

/* ------------------------------------------------------------------ */
/* Faults                                                              */
/* ------------------------------------------------------------------ */

static bool has_error_code(int vec)
{
    return vec == EXC_DF || (vec >= EXC_TS && vec <= EXC_PF) || vec == EXC_AC;
}

_Noreturn void cpu_stop(cpu_t *c, uint32_t exit_code)
{
    c->sys_access = 0;
    c->stop = exit_code;
    c->delivering = 0;
    longjmp(c->fault_jmp, 1);
}

_Noreturn void cpu_fault(cpu_t *c, int vec, uint32_t err)
{
    c->sys_access = 0;
    // Simplified escalation: any fault while delivering one is a double fault.
    if (c->delivering == 2) cpu_stop(c, JIT_EXIT_SHUTDOWN);       // triple fault
    if (c->delivering == 1) { vec = EXC_DF; err = 0; c->delivering = 2; }
    else c->delivering = 1;
    cpu_interrupt(c, vec, c->eip, false, cpu_pmode(c) && has_error_code(vec), err);
    c->delivering = 0;
    longjmp(c->fault_jmp, 1);
}

/* ------------------------------------------------------------------ */
/* Descriptors                                                         */
/* ------------------------------------------------------------------ */

typedef struct { uint32_t lo, hi; } desc_t;

#define D_CODE(hi)       (((hi) & SEG_S) && ((hi) & 0x800u))
#define D_CONFORMING(hi) ((hi) & 0x400u)    // code segments
#define D_RW(hi)         ((hi) & 0x200u)    // code: readable, data: writable
#define SEL_IDX(sel)     ((uint32_t)(sel) & ~3u)

static uint32_t desc_addr(const cpu_t *c, uint16_t sel)
{
    return ((sel & 4) ? c->ldtr.base : c->gdtr.base) + (sel & ~7u);
}

static bool fetch_desc(cpu_t *c, uint16_t sel, desc_t *d)
{
    uint32_t limit;
    if (sel & 4) {
        if (SEL_IDX(c->ldtr.selector) == 0) return false;
        limit = c->ldtr.limit;
    } else {
        limit = c->gdtr.limit;
    }
    if ((sel & ~7u) + 7u > limit) return false;
    uint32_t a = desc_addr(c, sel);
    d->lo = sys_read(c, a, 4);
    d->hi = sys_read(c, a + 4, 4);
    return true;
}

static void set_accessed(cpu_t *c, uint16_t sel, desc_t *d)
{
    if (!(d->hi & 0x100u)) {
        d->hi |= 0x100u;
        sys_write(c, desc_addr(c, sel) + 4, d->hi, 4);
    }
}

static void desc_to_seg(const desc_t *d, uint16_t sel, seg_t *s)
{
    s->selector = sel;
    s->base  = (d->lo >> 16) | ((d->hi & 0xFFu) << 16) | (d->hi & 0xFF000000u);
    uint32_t lim = (d->lo & 0xFFFFu) | (d->hi & 0xF0000u);
    if (d->hi & SEG_G) lim = (lim << 12) | 0xFFFu;
    s->limit = lim;
    s->flags = d->hi & 0x00F0FF00u;
}

static void load_real(seg_t *s, uint16_t sel)
{
    s->selector = sel;
    s->base = (uint32_t)sel << 4;
}

// Load an SS descriptor that must match privilege `cpl`; faults use vector `vec`.
static void load_ss(cpu_t *c, uint16_t sel, int cpl, int vec, seg_t *out)
{
    if (SEL_IDX(sel) == 0) cpu_fault(c, vec, 0);
    desc_t d;
    if ((sel & 3) != cpl || !fetch_desc(c, sel, &d)) cpu_fault(c, vec, SEL_IDX(sel));
    if (!(d.hi & SEG_S) || D_CODE(d.hi) || !D_RW(d.hi) || (int)SEG_DPL(d.hi) != cpl)
        cpu_fault(c, vec, SEL_IDX(sel));
    if (!(d.hi & SEG_P)) cpu_fault(c, EXC_SS, SEL_IDX(sel));
    set_accessed(c, sel, &d);
    desc_to_seg(&d, sel, out);
}

void cpu_load_seg(cpu_t *c, int sreg, uint16_t sel)
{
    seg_t *s = &c->seg[sreg];
    if (!cpu_pmode(c) || (c->eflags & EFL_VM)) { load_real(s, sel); return; }
    if (sreg == S_SS) { load_ss(c, sel, c->cpl, EXC_GP, s); return; }

    if (SEL_IDX(sel) == 0) {          // null selector: allowed, segment becomes unusable
        s->selector = sel; s->base = 0; s->limit = 0; s->flags = 0;
        return;
    }
    desc_t d;
    if (!fetch_desc(c, sel, &d)) cpu_fault(c, EXC_GP, SEL_IDX(sel));
    if (!(d.hi & SEG_S) || (D_CODE(d.hi) && !D_RW(d.hi))) cpu_fault(c, EXC_GP, SEL_IDX(sel));
    if (!D_CODE(d.hi) || !D_CONFORMING(d.hi)) {
        uint32_t dpl = SEG_DPL(d.hi);
        if ((sel & 3u) > dpl || c->cpl > dpl) cpu_fault(c, EXC_GP, SEL_IDX(sel));
    }
    if (!(d.hi & SEG_P)) cpu_fault(c, EXC_NP, SEL_IDX(sel));
    set_accessed(c, sel, &d);
    desc_to_seg(&d, sel, s);
}

// Target of a far jmp/call at the current privilege level.
static void check_code_jmp(cpu_t *c, uint16_t sel, desc_t *d)
{
    if (SEL_IDX(sel) == 0) cpu_fault(c, EXC_GP, 0);
    if (!fetch_desc(c, sel, d)) cpu_fault(c, EXC_GP, SEL_IDX(sel));
    if (!(d->hi & SEG_S)) cpu_stop(c, JIT_EXIT_UNSUPPORTED);   // call/task gate, TSS: not yet
    if (!D_CODE(d->hi)) cpu_fault(c, EXC_GP, SEL_IDX(sel));
    if (D_CONFORMING(d->hi)) {
        if (SEG_DPL(d->hi) > c->cpl) cpu_fault(c, EXC_GP, SEL_IDX(sel));
    } else if ((sel & 3u) > c->cpl || SEG_DPL(d->hi) != c->cpl) {
        cpu_fault(c, EXC_GP, SEL_IDX(sel));
    }
    if (!(d->hi & SEG_P)) cpu_fault(c, EXC_NP, SEL_IDX(sel));
}

// Target of a far ret / iret: same or outer (numerically higher) privilege.
static void check_code_ret(cpu_t *c, uint16_t sel, desc_t *d)
{
    if (SEL_IDX(sel) == 0) cpu_fault(c, EXC_GP, 0);
    uint32_t rpl = sel & 3u;
    if (!fetch_desc(c, sel, d) || rpl < c->cpl || !D_CODE(d->hi)) cpu_fault(c, EXC_GP, SEL_IDX(sel));
    if (D_CONFORMING(d->hi) ? SEG_DPL(d->hi) > rpl : SEG_DPL(d->hi) != rpl)
        cpu_fault(c, EXC_GP, SEL_IDX(sel));
    if (!(d->hi & SEG_P)) cpu_fault(c, EXC_NP, SEL_IDX(sel));
}

static void commit_cs(cpu_t *c, uint16_t sel, desc_t *d, int cpl)
{
    set_accessed(c, sel, d);
    desc_to_seg(d, (uint16_t)(SEL_IDX(sel) | (uint32_t)cpl), &c->seg[S_CS]);
    c->cpl = (uint8_t)cpl;
}

// After a return to an outer level, data segments the new CPL may not use become null.
static void null_outer_segs(cpu_t *c)
{
    static const int regs[4] = { S_ES, S_DS, S_FS, S_GS };
    for (int i = 0; i < 4; i++) {
        seg_t *s = &c->seg[regs[i]];
        uint32_t f = s->flags;
        if (!(f & SEG_S)) continue;
        bool conforming_code = D_CODE(f) && D_CONFORMING(f);
        if (!conforming_code && SEG_DPL(f) < c->cpl) {
            s->selector = 0; s->base = 0; s->limit = 0; s->flags = 0;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Flags                                                               */
/* ------------------------------------------------------------------ */

void cpu_set_eflags(cpu_t *c, uint32_t value, uint32_t mask)
{
    uint32_t cur = cpu_calc_flags(c);
    uint32_t can = EFL_CF | EFL_PF | EFL_AF | EFL_ZF | EFL_SF | EFL_TF | EFL_DF | EFL_OF |
                   EFL_NT | EFL_AC | EFL_ID;
    uint32_t iopl = (cur >> 12) & 3u;
    if (!cpu_pmode(c) || c->cpl == 0)    can |= EFL_IOPL;
    if (!cpu_pmode(c) || c->cpl <= iopl) can |= EFL_IF;
    can &= mask;
    uint32_t f = (cur & ~can) | (value & can);
    f = (f | 2u) & ~(EFL_RF | (1u << 3) | (1u << 5) | (1u << 15) | 0xFFC00000u);
    c->eflags = f;
    c->cc_op = CC_OP_EFLAGS;
}

/* ------------------------------------------------------------------ */
/* Interrupts and exceptions                                           */
/* ------------------------------------------------------------------ */

void cpu_interrupt(cpu_t *c, int vec, uint32_t ret_eip, bool sw, bool has_err, uint32_t err)
{
    uint32_t fl = cpu_calc_flags(c);
    c->eflags = fl;
    c->cc_op = CC_OP_EFLAGS;
    uint32_t v = (uint32_t)vec;

    if (!cpu_pmode(c)) {
        if (v * 4 + 3 > c->idtr.limit) cpu_fault(c, EXC_GP, 0);
        uint32_t ent = sys_read(c, c->idtr.base + v * 4, 4);
        cpu_push(c, fl & 0xFFFFu, 2);
        cpu_push(c, c->seg[S_CS].selector, 2);
        cpu_push(c, ret_eip & 0xFFFFu, 2);
        c->eflags &= ~(EFL_IF | EFL_TF | EFL_AC | EFL_RF);
        load_real(&c->seg[S_CS], (uint16_t)(ent >> 16));
        c->eip = ent & 0xFFFFu;
        return;
    }
    if (c->eflags & EFL_VM) cpu_stop(c, JIT_EXIT_UNSUPPORTED);     // virtual-8086 mode: not yet

    uint32_t ext = sw ? 0 : 1;
    uint32_t gerr = v * 8 + 2 + ext;
    if (v * 8 + 7 > c->idtr.limit) cpu_fault(c, EXC_GP, gerr);
    uint32_t lo = sys_read(c, c->idtr.base + v * 8, 4);
    uint32_t hi = sys_read(c, c->idtr.base + v * 8 + 4, 4);
    uint32_t type = (hi >> 8) & 0x1Fu;
    if (type == 0x05) cpu_stop(c, JIT_EXIT_UNSUPPORTED);           // task gate: not yet
    if (type != 0x06 && type != 0x07 && type != 0x0E && type != 0x0F) cpu_fault(c, EXC_GP, gerr);
    if (sw && SEG_DPL(hi) < c->cpl) cpu_fault(c, EXC_GP, v * 8 + 2);
    if (!(hi & SEG_P)) cpu_fault(c, EXC_NP, gerr);

    bool g32 = (type & 8) != 0, trap = (type & 1) != 0;
    int size = g32 ? 4 : 2;
    uint16_t sel = (uint16_t)(lo >> 16);
    uint32_t off = (lo & 0xFFFFu) | (g32 ? (hi & 0xFFFF0000u) : 0);

    if (SEL_IDX(sel) == 0) cpu_fault(c, EXC_GP, ext);
    desc_t d;
    if (!fetch_desc(c, sel, &d) || !D_CODE(d.hi) || SEG_DPL(d.hi) > c->cpl)
        cpu_fault(c, EXC_GP, SEL_IDX(sel) + ext);
    if (!(d.hi & SEG_P)) cpu_fault(c, EXC_NP, SEL_IDX(sel) + ext);

    int new_cpl = c->cpl;
    uint16_t old_cs = c->seg[S_CS].selector;
    if (!D_CONFORMING(d.hi) && (int)SEG_DPL(d.hi) < c->cpl) {
        // Inter-privilege: switch to the inner stack from the TSS, push the old SS:ESP.
        new_cpl = (int)SEG_DPL(d.hi);
        uint32_t ttype = SEG_TYPE(c->tr.flags);
        if (ttype != 9 && ttype != 11) cpu_stop(c, JIT_EXIT_UNSUPPORTED);   // 16-bit TSS
        uint32_t o = 4 + 8 * (uint32_t)new_cpl;
        if (o + 5 > c->tr.limit) cpu_fault(c, EXC_TS, SEL_IDX(c->tr.selector) + ext);
        uint32_t nesp = sys_read(c, c->tr.base + o, 4);
        uint16_t nss  = (uint16_t)sys_read(c, c->tr.base + o + 4, 2);
        seg_t ss;
        load_ss(c, nss, new_cpl, EXC_TS, &ss);
        stk_t s = { &ss, (ss.flags & SEG_DB) != 0, nesp };
        c->sys_access = 1;                          // the inner stack is written at the new privilege
        stk_push(c, &s, c->seg[S_SS].selector, size);
        stk_push(c, &s, c->gpr[R_ESP], size);
        stk_push(c, &s, fl, size);
        stk_push(c, &s, old_cs, size);
        stk_push(c, &s, ret_eip, size);
        if (has_err) stk_push(c, &s, err, size);
        c->sys_access = 0;
        c->seg[S_SS] = ss;
        c->gpr[R_ESP] = s.esp;
    } else {
        stk_t s = cur_stack(c);
        stk_push(c, &s, fl, size);
        stk_push(c, &s, old_cs, size);
        stk_push(c, &s, ret_eip, size);
        if (has_err) stk_push(c, &s, err, size);
        c->gpr[R_ESP] = s.esp;
    }
    commit_cs(c, sel, &d, new_cpl);
    c->eip = g32 ? off : (off & 0xFFFFu);
    c->eflags &= ~(EFL_TF | EFL_NT | EFL_RF | EFL_VM);
    if (!trap) c->eflags &= ~EFL_IF;
}

/* ------------------------------------------------------------------ */
/* Far transfers                                                       */
/* ------------------------------------------------------------------ */

void cpu_far_jmp(cpu_t *c, uint16_t sel, uint32_t off, int opsize)
{
    if (opsize == 2) off &= 0xFFFFu;
    if (!cpu_pmode(c)) { load_real(&c->seg[S_CS], sel); c->eip = off; return; }
    desc_t d;
    check_code_jmp(c, sel, &d);
    commit_cs(c, sel, &d, c->cpl);
    c->eip = off;
}

void cpu_far_call(cpu_t *c, uint16_t sel, uint32_t off, int opsize, uint32_t ret_eip)
{
    if (opsize == 2) off &= 0xFFFFu;
    uint16_t old_cs = c->seg[S_CS].selector;
    if (!cpu_pmode(c)) {
        cpu_push(c, old_cs, opsize);
        cpu_push(c, ret_eip, opsize);
        load_real(&c->seg[S_CS], sel);
        c->eip = off;
        return;
    }
    desc_t d;
    check_code_jmp(c, sel, &d);          // all checks before anything is pushed
    if (cpu_stack32(c)) cpu_probe_write(c, c->seg[S_SS].base + c->gpr[R_ESP] - 2u * (uint32_t)opsize, 2u * (uint32_t)opsize);
    cpu_push(c, old_cs, opsize);
    cpu_push(c, ret_eip, opsize);
    commit_cs(c, sel, &d, c->cpl);
    c->eip = off;
}

static void set_esp_for(cpu_t *c, const seg_t *ss, uint32_t v)
{
    if (ss->flags & SEG_DB) c->gpr[R_ESP] = v;
    else c->gpr[R_ESP] = (c->gpr[R_ESP] & 0xFFFF0000u) | (v & 0xFFFFu);
}

void cpu_far_ret(cpu_t *c, int opsize, uint16_t imm)
{
    uint32_t n = (uint32_t)opsize;
    stk_t s = cur_stack(c);
    uint32_t off = stk_peek(c, &s, 0, opsize);
    uint16_t sel = (uint16_t)stk_peek(c, &s, n, opsize);
    if (opsize == 2) off &= 0xFFFFu;

    if (!cpu_pmode(c)) {
        stk_add(&s, 2 * n + imm);
        c->gpr[R_ESP] = s.esp;
        load_real(&c->seg[S_CS], sel);
        c->eip = off;
        return;
    }
    desc_t d;
    check_code_ret(c, sel, &d);
    int rpl = sel & 3;
    if (rpl == c->cpl) {
        stk_add(&s, 2 * n + imm);
        c->gpr[R_ESP] = s.esp;
        commit_cs(c, sel, &d, rpl);
        c->eip = off;
        return;
    }
    // Return to an outer level: the outer SS:ESP sits above the parameters.
    uint32_t nesp = stk_peek(c, &s, 2 * n + imm, opsize);
    uint16_t nss  = (uint16_t)stk_peek(c, &s, 3 * n + imm, opsize);
    seg_t ss;
    load_ss(c, nss, rpl, EXC_GP, &ss);
    commit_cs(c, sel, &d, rpl);
    c->seg[S_SS] = ss;
    set_esp_for(c, &ss, nesp + imm);
    null_outer_segs(c);
    c->eip = off;
}

void cpu_iret(cpu_t *c, int opsize)
{
    uint32_t n = (uint32_t)opsize;
    stk_t s = cur_stack(c);
    uint32_t off = stk_peek(c, &s, 0, opsize);
    uint16_t sel = (uint16_t)stk_peek(c, &s, n, opsize);
    uint32_t fl  = stk_peek(c, &s, 2 * n, opsize);
    if (opsize == 2) off &= 0xFFFFu;

    if (!cpu_pmode(c)) {
        stk_add(&s, 3 * n);
        c->gpr[R_ESP] = s.esp;
        load_real(&c->seg[S_CS], sel);
        c->eip = off;
        cpu_set_eflags(c, fl, opsize == 4 ? 0x00257FD5u : 0xFFFFu);
        return;
    }
    if (c->eflags & EFL_NT) cpu_stop(c, JIT_EXIT_UNSUPPORTED);              // task return
    if (opsize == 4 && (fl & EFL_VM) && c->cpl == 0) cpu_stop(c, JIT_EXIT_UNSUPPORTED);   // to VM86

    desc_t d;
    check_code_ret(c, sel, &d);
    int rpl = sel & 3;
    uint32_t mask = opsize == 4 ? 0xFFFFFFFFu : 0xFFFFu;
    if (rpl == c->cpl) {
        stk_add(&s, 3 * n);
        c->gpr[R_ESP] = s.esp;
        commit_cs(c, sel, &d, rpl);
        c->eip = off;
        cpu_set_eflags(c, fl, mask);
        return;
    }
    uint32_t nesp = stk_peek(c, &s, 3 * n, opsize);
    uint16_t nss  = (uint16_t)stk_peek(c, &s, 4 * n, opsize);
    seg_t ss;
    load_ss(c, nss, rpl, EXC_GP, &ss);
    cpu_set_eflags(c, fl, mask);         // with the old (inner) CPL's privileges
    commit_cs(c, sel, &d, rpl);
    c->seg[S_SS] = ss;
    set_esp_for(c, &ss, nesp);
    null_outer_segs(c);
    c->eip = off;
}

/* ------------------------------------------------------------------ */
/* LDT / task register                                                 */
/* ------------------------------------------------------------------ */

void cpu_lldt(cpu_t *c, uint16_t sel)
{
    if (SEL_IDX(sel) == 0) {
        c->ldtr.selector = 0; c->ldtr.base = 0; c->ldtr.limit = 0; c->ldtr.flags = 0;
        return;
    }
    desc_t d;
    if ((sel & 4) || !fetch_desc(c, sel, &d) || (d.hi & SEG_S) || SEG_TYPE(d.hi) != 2)
        cpu_fault(c, EXC_GP, SEL_IDX(sel));
    if (!(d.hi & SEG_P)) cpu_fault(c, EXC_NP, SEL_IDX(sel));
    desc_to_seg(&d, sel, &c->ldtr);
}

void cpu_ltr(cpu_t *c, uint16_t sel)
{
    if (SEL_IDX(sel) == 0) cpu_fault(c, EXC_GP, 0);
    desc_t d;
    if ((sel & 4) || !fetch_desc(c, sel, &d) || (d.hi & SEG_S) ||
        (SEG_TYPE(d.hi) != 1 && SEG_TYPE(d.hi) != 9))
        cpu_fault(c, EXC_GP, SEL_IDX(sel));
    if (!(d.hi & SEG_P)) cpu_fault(c, EXC_NP, SEL_IDX(sel));
    d.hi |= 0x200u;                                   // mark busy
    sys_write(c, desc_addr(c, sel) + 4, d.hi, 4);
    desc_to_seg(&d, sel, &c->tr);
}

/* ------------------------------------------------------------------ */
/* Control registers, MSRs, CPUID, TSC, I/O permission                 */
/* ------------------------------------------------------------------ */

uint32_t cpu_read_cr(cpu_t *c, int n)
{
    switch (n) {
    case 0: return c->cr0;
    case 2: return c->cr2;
    case 3: return c->cr3;
    case 4: return c->cr4;
    default: return 0;
    }
}

bool cpu_write_cr(cpu_t *c, int n, uint32_t v)
{
    switch (n) {
    case 0:
        if ((v & CR0_PG) && !(v & CR0_PE)) cpu_fault(c, EXC_GP, 0);
        if ((v & CR0_NW) && !(v & CR0_CD)) cpu_fault(c, EXC_GP, 0);
    {
        uint32_t old_cr0 = c->cr0;
        c->cr0 = (v | CR0_ET) & 0xE005003Fu;
        if (!(c->cr0 & CR0_PE)) c->cpl = 0;
        if ((old_cr0 ^ c->cr0) & (CR0_PG | CR0_WP | CR0_PE)) cpu_tlb_flush(c, false);
        return true;
    }
    case 2: c->cr2 = v; return true;
    case 3:
        c->cr3 = v;
        cpu_tlb_flush(c, (c->cr4 & CR4_PGE) != 0);   // global pages survive
        return true;
    case 4:
        if (v & ~0x7FFu) cpu_fault(c, EXC_GP, 0);     // Pentium III defines bits 0..10
        if (v & CR4_PAE) return false;                // PAE paging: not emulated (not in CPUID)
        if ((c->cr4 ^ v) & (CR4_PSE | CR4_PGE)) cpu_tlb_flush(c, false);
        c->cr4 = v;
        return true;
    default:
        return true;
    }
}

static uint64_t host_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// The TSC ticks at a nominal 1 GHz. Guests calibrate it against the PIT anyway.
uint64_t cpu_read_tsc(const cpu_t *c)
{
    return host_ns() + (uint64_t)c->tsc_offset;
}

// P6 performance counters: 40 bits wide. A counter enabled (bit 22) for event 0x79
// (CPU_CLK_UNHALTED) advances with the TSC; other events are not modeled and keep
// the value last written.
static uint64_t perf_read(const cpu_t *c, int i)
{
    uint32_t sel = c->perf_sel[i];
    uint64_t v = c->perf_ctr[i];
    if ((sel & (1u << 22)) && (sel & 0xFF) == 0x79) v += cpu_read_tsc(c) - c->perf_base[i];
    return v & 0xFFFFFFFFFFull;
}

static void perf_write(cpu_t *c, int i, uint64_t v)
{
    c->perf_ctr[i] = v & 0xFFFFFFFFFFull;
    c->perf_base[i] = cpu_read_tsc(c);
}

uint64_t cpu_rdpmc(cpu_t *c, uint32_t idx) { return perf_read(c, idx & 1); }

enum {
    MSR_TSC = 0x10, MSR_PLATFORM_ID = 0x17, MSR_APIC_BASE = 0x1B, MSR_BIOS_UPDT_TRIG = 0x79,
    MSR_BIOS_SIGN = 0x8B, MSR_SYSENTER_CS = 0x174, MSR_SYSENTER_ESP = 0x175, MSR_SYSENTER_EIP = 0x176,
};

void cpu_rdmsr(cpu_t *c)
{
    uint64_t v;
    switch (c->gpr[R_ECX]) {
    case MSR_TSC:          v = cpu_read_tsc(c); break;
    case MSR_PLATFORM_ID:  v = 0; break;
    case MSR_APIC_BASE:    v = c->apic_base; break;
    case MSR_BIOS_SIGN:    v = 0; break;
    case MSR_SYSENTER_CS:  v = c->sysenter_cs; break;
    case MSR_SYSENTER_ESP: v = c->sysenter_esp; break;
    case MSR_SYSENTER_EIP: v = c->sysenter_eip; break;
    case 0xC1: case 0xC2:  v = perf_read(c, (int)(c->gpr[R_ECX] - 0xC1)); break;
    case 0x186: case 0x187: v = c->perf_sel[c->gpr[R_ECX] - 0x186]; break;
    default: cpu_fault(c, EXC_GP, 0);
    }
    c->gpr[R_EAX] = (uint32_t)v;
    c->gpr[R_EDX] = (uint32_t)(v >> 32);
}

void cpu_wrmsr(cpu_t *c)
{
    uint64_t v = ((uint64_t)c->gpr[R_EDX] << 32) | c->gpr[R_EAX];
    switch (c->gpr[R_ECX]) {
    case MSR_TSC:
        c->tsc_offset = (int64_t)(v - host_ns());
        break;
    case MSR_APIC_BASE:      c->apic_base = (uint32_t)v; break;
    case MSR_BIOS_UPDT_TRIG: case MSR_BIOS_SIGN: break;      // microcode update: ignored
    case MSR_SYSENTER_CS:    c->sysenter_cs  = (uint32_t)v; break;
    case MSR_SYSENTER_ESP:   c->sysenter_esp = (uint32_t)v; break;
    case MSR_SYSENTER_EIP:   c->sysenter_eip = (uint32_t)v; break;
    case 0xC1: case 0xC2:    perf_write(c, (int)(c->gpr[R_ECX] - 0xC1), v); break;
    case 0x186: case 0x187: {
        int i = (int)(c->gpr[R_ECX] - 0x186);
        c->perf_ctr[i] = perf_read(c, i);           // freeze the running value, then restart
        c->perf_base[i] = cpu_read_tsc(c);
        c->perf_sel[i] = (uint32_t)v;
        break;
    }
    default: cpu_fault(c, EXC_GP, 0);
    }
}

// Pentium III (Katmai, family 6 model 7). Only features that are actually
// emulated are advertised; MMX, FXSR, SSE, APIC, MTRR, SEP get
// switched on as they are implemented.
#define CPUID_FEATURES ((1u << 0) /*FPU*/ | (1u << 3) /*PSE*/ | (1u << 4) /*TSC*/ | (1u << 5) /*MSR*/ | \
                        (1u << 8) /*CX8*/ | (1u << 13) /*PGE*/ | (1u << 15) /*CMOV*/)

void cpu_cpuid(cpu_t *c)
{
    uint32_t leaf = c->gpr[R_EAX], a = 0, b = 0, cc = 0, d = 0;
    if (leaf > 2) leaf = 2;                     // Intel: out-of-range leaves return the highest one
    switch (leaf) {
    case 0:
        a = 2;
        b = 0x756E6547u; d = 0x49656E69u; cc = 0x6C65746Eu;   // "GenuineIntel"
        break;
    case 1:
        a = 0x0673u;                                         // family 6, model 7, stepping 3
        b = (uint32_t)c->apic_id << 24;
        d = CPUID_FEATURES | (c->has_apic && (c->apic_base & 0x800u) ? 1u << 9 : 0);   // APIC (multiprocessor machines)
        break;
    default:
        a = 0x03020101u; d = 0x0C040843u;                    // cache descriptors (512 KB L2)
        break;
    }
    c->gpr[R_EAX] = a; c->gpr[R_EBX] = b; c->gpr[R_ECX] = cc; c->gpr[R_EDX] = d;
}

bool cpu_io_allowed(cpu_t *c, uint16_t port, int size)
{
    if (!cpu_pmode(c)) return true;
    uint32_t iopl = (c->eflags >> 12) & 3u;
    if (!(c->eflags & EFL_VM) && c->cpl <= iopl) return true;
    uint32_t t = SEG_TYPE(c->tr.flags);
    if ((t != 9 && t != 11) || c->tr.limit < 0x67) return false;
    uint32_t map = sys_read(c, c->tr.base + 0x66, 2);
    uint32_t byte = map + (port >> 3);
    if (byte + 1 > c->tr.limit) return false;
    uint32_t bits = sys_read(c, c->tr.base + byte, 2);
    uint32_t need = ((1u << size) - 1u) << (port & 7);
    return (bits & need) == 0;
}
