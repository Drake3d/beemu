// System-level tests: self-checking guest programs (tests/asm/*.asm, built
// with nasm) run through the JIT with a test device on the I/O bus. See
// tests/asm/check.inc for the device's ports. ARM64 hosts only.
#include "cpu.h"
#include "io.h"
#include "dev/disk.h"
#include "dev/ide.h"
#include "jit.h"
#include "mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail, g_run;
#define CHECK(cond, msg) do { g_run++; if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); g_fail++; } } while (0)

typedef struct {
    int done, checks, fails;
    uint32_t echo;
} tdev_t;

static uint32_t tdev_read(void *opaque, uint16_t port, int size)
{
    (void)size;
    tdev_t *t = opaque;
    return port == 0xE3 ? t->echo : 0xFFFFFFFFu;
}

static void tdev_write(void *opaque, uint16_t port, uint32_t v, int size)
{
    (void)size;
    tdev_t *t = opaque;
    switch (port) {
    case 0xE0: t->done = 1; break;
    case 0xE1: t->fails++; printf("  guest check failed at line %u\n", v); break;
    case 0xE2: t->checks++; break;
    case 0xE3: t->echo = v; break;
    }
}

typedef struct {
    mem_bus_t *bus;
    io_bus_t  *io;
    uint8_t   *ram;
    jit_t     *jit;
    tdev_t     dev;
    cpu_t      cpu;
    ide_t      ide;
    int        irq_raised[16];
} machine_t_;

static void count_irq(void *opaque, int irq, int level)
{
    machine_t_ *m = opaque;
    if (level) m->irq_raised[irq]++;
}

#define RAM_SIZE (4u << 20)

static void machine_init(machine_t_ *m)
{
    memset(m, 0, sizeof *m);
    m->bus = mem_create();
    m->ram = mem_alloc_host(RAM_SIZE);
    mem_map_ram(m->bus, 0, RAM_SIZE, m->ram);
    m->io = io_create();
    io_register(m->io, 0xE0, 4, tdev_read, tdev_write, &m->dev);
    m->jit = jit_create(m->bus, 4u << 20);
    cpu_init(&m->cpu, 0);
    m->cpu.mem = m->bus;
    m->cpu.io = m->io;
}

static void machine_free(machine_t_ *m)
{
    jit_destroy(m->jit);
    io_destroy(m->io);
    mem_free_host(m->ram);
    mem_destroy(m->bus);
}

// Flat 32-bit protected mode, CPL 0, as if a loader had set it up.
static void flat32(cpu_t *c, uint32_t eip)
{
    c->cr0 |= CR0_PE;
    for (int s = 0; s < 6; s++) {
        c->seg[s].base = 0;
        c->seg[s].limit = 0xFFFFFFFFu;
        c->seg[s].flags = SEG_P | SEG_S | SEG_DB | SEG_G | (s == S_CS ? 0xBu << 8 : 0x3u << 8);
    }
    c->seg[S_CS].selector = 0x08;
    for (int s = 0; s < 6; s++) if (s != S_CS) c->seg[s].selector = 0x10;
    c->gpr[R_ESP] = 0x80000;
    c->eip = eip;
}

static size_t load_bin(machine_t_ *m, const char *name, uint32_t at)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.bin", ASM_DIR, name);
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); return 0; }
    static uint8_t buf[1 << 20];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    mem_write_block(m->bus, at, buf, n);
    return n;
}

// Run a guest program to completion and report its own checks.
// setup (optional) runs after machine_init, e.g. to attach disks.
static void run_guest_with(const char *name, uint32_t org, bool flat, int n_checks,
                           void (*setup)(machine_t_ *), void (*after)(machine_t_ *))
{
    static machine_t_ m;
    machine_init(&m);
    if (setup) setup(&m);
    if (!load_bin(&m, name, org)) { CHECK(0, name); machine_free(&m); return; }
    if (flat) flat32(&m.cpu, org);
    else {   // real mode at 0000:org
        m.cpu.seg[S_CS].selector = 0;
        m.cpu.seg[S_CS].base = 0;
        m.cpu.eip = org;
    }
    jit_exit_t x = jit_run(m.jit, &m.cpu, 10u * 1000 * 1000);
    printf("%-8s exit %d, eip %08X, %d guest checks, %d failed\n",
           name, x, m.cpu.eip, m.dev.checks, m.dev.fails);
    CHECK(x == JIT_EXIT_HALT, "guest halts");
    CHECK(m.dev.done, "guest reached DONE");
    CHECK(m.dev.fails == 0, "guest checks pass");
    CHECK(m.dev.checks == n_checks, "guest ran exactly its checks");
    if (after) after(&m);
    machine_free(&m);
}

static void run_guest(const char *name, uint32_t org, bool flat, int n_checks)
{
    run_guest_with(name, org, flat, n_checks, NULL, NULL);
}

/* ---- IDE test images ---- */
static char g_hd_path[512], g_cd_path[512];
static disk_t *g_hd, *g_cd;

static bool make_image(char *path, size_t pathlen, const char *tag, uint32_t blocks, uint32_t bsize, uint32_t marker)
{
    const char *tmp = getenv("TMPDIR");
    snprintf(path, pathlen, "%s/beemu-%s-XXXXXX", tmp && *tmp ? tmp : "/tmp", tag);
    int fd = mkstemp(path);
    if (fd < 0) return false;
    uint32_t *blk = malloc(bsize);
    bool ok = blk != NULL;
    for (uint32_t b = 0; ok && b < blocks; b++) {
        for (uint32_t i = 0; i < bsize / 4; i++) blk[i] = marker | b;
        ok = write(fd, blk, bsize) == (ssize_t)bsize;
    }
    free(blk);
    close(fd);
    return ok;
}

static void ide_setup(machine_t_ *m)
{
    char err[256];
    bool ok = make_image(g_hd_path, sizeof g_hd_path, "hd", 2048, 512, 0x5A000000u) &&
              make_image(g_cd_path, sizeof g_cd_path, "cd", 16, 2048, 0xCD000000u);
    CHECK(ok, "create test images");
    g_hd = disk_open(g_hd_path, false, err, sizeof err);
    g_cd = disk_open(g_cd_path, false, err, sizeof err);
    CHECK(g_hd && g_cd, "open test images");
    ide_init(&m->ide, m->bus, m->io, count_irq, m);
    ide_attach(&m->ide, 0, IDE_HD, g_hd);
    ide_attach(&m->ide, 2, IDE_CD, g_cd);
    ide_register(&m->ide);
    pci_cfg_write(&m->ide.pci, 0x20, 0xC001, 4);   // bus master at 0xC000
    pci_cfg_write(&m->ide.pci, 0x04, 0x0005, 2);   // I/O + bus master enable
}

static void ide_after(machine_t_ *m)
{
    CHECK(m->irq_raised[14] > 0 && m->irq_raised[15] > 0, "IDE raised IRQ 14 and 15");
    // The guest wrote sector 7, but only to the overlay: the file is unchanged.
    FILE *f = fopen(g_hd_path, "rb");
    uint32_t v = 0;
    if (f) { fseek(f, 7 * 512 + 100, SEEK_SET); if (fread(&v, 4, 1, f) != 1) v = 0; fclose(f); }
    CHECK(v == 0x5A000007u, "image file untouched by guest writes");
    ide_free(&m->ide);
    disk_close(g_hd);
    disk_close(g_cd);
    unlink(g_hd_path);
    unlink(g_cd_path);
}

int main(void)
{
#if defined(__aarch64__)
    run_guest("basic32", 0x10000, true, 104);   // = CHECK lines in the .asm
    run_guest("modes", 0x7C00, false, 25);      // 26 lines, the last is unreachable
    run_guest_with("ide", 0x10000, true, 45, ide_setup, ide_after);   // = CHECK lines
    run_guest("x87", 0x10000, true, 33);      // = CHECK lines
    run_guest("paging", 0x10000, true, 27);   // = CHECK lines

    // Triple fault: no usable IDT, so INT3 -> #GP -> #DF -> shutdown.
    {
        machine_t_ m;
        machine_init(&m);
        static const uint8_t prog[] = { 0xCC };
        mem_write_block(m.bus, 0x1000, prog, sizeof prog);
        flat32(&m.cpu, 0x1000);
        m.cpu.idtr.limit = 0;
        CHECK(jit_run(m.jit, &m.cpu, 100) == JIT_EXIT_SHUTDOWN, "triple fault shuts down");
        machine_free(&m);
    }

    // A long REP yields to the dispatcher and resumes where it stopped.
    {
        machine_t_ m;
        machine_init(&m);
        static const uint8_t prog[] = {
            0xBF, 0x00, 0x00, 0x10, 0x00,        // mov edi,0x100000
            0xB9, 0x00, 0x00, 0x03, 0x00,        // mov ecx,0x30000
            0xB0, 0x5A,                          // mov al,0x5a
            0xF3, 0xAA,                          // rep stosb
            0xF4,
        };
        mem_write_block(m.bus, 0x1000, prog, sizeof prog);
        flat32(&m.cpu, 0x1000);
        jit_exit_t x = jit_run(m.jit, &m.cpu, 100);
        CHECK(x == JIT_EXIT_HALT && m.cpu.gpr[R_ECX] == 0 && m.cpu.gpr[R_EDI] == 0x130000, "long rep stosb");
        CHECK(m.ram[0x100000] == 0x5A && m.ram[0x12FFFF] == 0x5A && m.ram[0x130000] == 0, "rep stosb filled exactly");
        machine_free(&m);
    }
#else
    printf("(system tests skipped: host is not ARM64)\n");
#endif
    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
