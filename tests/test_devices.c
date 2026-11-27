// Device unit tests with a fake clock: PIC, PIT, CMOS, fw_cfg, PCI config space, PS/2, RTL8139, NE2000 PCI,
// Matrox G400 2D engine, ES1370 sound, ISA DMA, Sound Blaster 16 PnP,
// local APICs and the I/O APIC.
#include "clock.h"
#include "cpu.h"
#include "io.h"
#include "dev/cmos.h"
#include "dev/fwcfg.h"
#include "dev/g400.h"
#include "dev/es1370.h"
#include "dev/dma.h"
#include "dev/sb16.h"
#include "dev/apic.h"
#include "dev/vga.h"
#include "dev/pci.h"
#include "dev/pic.h"
#include "dev/pit.h"
#include "dev/ps2.h"
#include "dev/ne2k.h"
#include "dev/rtl8139.h"
#include "mem.h"
#include <stdio.h>
#include <string.h>

static int g_fail, g_run;
static uint32_t g_rng = 12345;
static uint32_t rnd(uint32_t n) { g_rng = g_rng * 1103515245u + 12345u; return (g_rng >> 8) % n; }
#define CHECK(cond, msg) do { g_run++; if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); g_fail++; } } while (0)

static uint64_t g_now;
static uint64_t fake_clock(void *o) { (void)o; return g_now; }

static pic_t g_pic;
static int g_irq_level[16], g_irq_edges[16];
static void irq_to_pic(void *o, int irq, int level)
{
    (void)o;
    if (level && !g_irq_level[irq]) g_irq_edges[irq]++;
    g_irq_level[irq] = level;
    pic_set_irq(&g_pic, irq, level);
}

static void out8(io_bus_t *io, uint16_t p, uint8_t v) { io_write(io, p, v, 1); }
static uint8_t in8(io_bus_t *io, uint16_t p) { return (uint8_t)io_read(io, p, 1); }

/* ---- RTL8139 helpers ---- */
static uint8_t g_sent[2048];
static size_t g_sent_len;
static int g_sent_count, g_nic_irq;
static void nic_send_cb(void *o, const uint8_t *f, size_t len) { (void)o; memcpy(g_sent, f, len); g_sent_len = len; g_sent_count++; }
static void nic_intx_cb(void *o, int level) { (void)o; g_nic_irq = level; }

#define NB 0xC000   // I/O BAR
static uint32_t nic_rd(io_bus_t *io, int off, int size) { return io_read(io, (uint16_t)(NB + off), size); }
static void nic_wr(io_bus_t *io, int off, uint32_t v, int size) { io_write(io, (uint16_t)(NB + off), v, size); }

// Read one EEPROM word the way drivers do: 9346CR in programming mode, bit-banged.
static uint16_t ee_read(io_bus_t *io, int addr)
{
    const uint8_t PROG = 0x80, CS = 0x08, SK = 0x04, DI = 0x02;
    uint32_t cmd = (1u << 8) | (2u << 6) | (uint32_t)addr;            // start bit, READ, address
    nic_wr(io, 0x50, PROG, 1);
    nic_wr(io, 0x50, PROG | CS, 1);
    for (int b = 8; b >= 0; b--) {
        uint8_t d = (cmd >> b) & 1 ? DI : 0;
        nic_wr(io, 0x50, PROG | CS | d, 1);
        nic_wr(io, 0x50, PROG | CS | d | SK, 1);
        nic_wr(io, 0x50, PROG | CS | d, 1);
    }
    uint16_t v = 0;
    for (int b = 0; b < 16; b++) {
        nic_wr(io, 0x50, PROG | CS | SK, 1);
        v = (uint16_t)(v << 1 | (nic_rd(io, 0x50, 1) & 1));
        nic_wr(io, 0x50, PROG | CS, 1);
    }
    nic_wr(io, 0x50, 0, 1);
    return v;
}

static uint32_t crc32_ref(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
    return ~c;
}

/* ---- NE2000 PCI helpers: the DP8390 as a driver sees it ---- */
#define EB 0xD000   // its 32-byte I/O BAR
static uint8_t ne_rd(io_bus_t *io, int off) { return in8(io, (uint16_t)(EB + off)); }
static void ne_wr(io_bus_t *io, int off, uint8_t v) { out8(io, (uint16_t)(EB + off), v); }

// Remote DMA: set address and count, start a read (CR = STA | RD read) or write.
static void ne_dma_setup(io_bus_t *io, uint16_t addr, uint16_t count, bool write)
{
    ne_wr(io, 0x00, 0x22);                                          // page 0, STA, abort DMA
    ne_wr(io, 0x0A, (uint8_t)count); ne_wr(io, 0x0B, (uint8_t)(count >> 8));
    ne_wr(io, 0x08, (uint8_t)addr);  ne_wr(io, 0x09, (uint8_t)(addr >> 8));
    ne_wr(io, 0x00, write ? 0x12 : 0x0A);
}

// Read `len` bytes (even) from card memory in word mode.
static void ne_read_mem(io_bus_t *io, uint16_t addr, uint8_t *buf, uint16_t len)
{
    ne_dma_setup(io, addr, len, false);
    for (int i = 0; i < len; i += 2) {
        uint16_t w = (uint16_t)io_read(io, EB + 0x10, 2);
        buf[i] = (uint8_t)w; buf[i + 1] = (uint8_t)(w >> 8);
    }
}

static void pic_program(io_bus_t *io)
{
    out8(io, 0x20, 0x11); out8(io, 0x21, 0x08); out8(io, 0x21, 0x04); out8(io, 0x21, 0x01);   // master: vectors 8-15
    out8(io, 0xA0, 0x11); out8(io, 0xA1, 0x70); out8(io, 0xA1, 0x02); out8(io, 0xA1, 0x01);   // slave: 0x70-0x77
    out8(io, 0x21, 0x00); out8(io, 0xA1, 0x00);
}

// Row-cached rendering must look exactly like a full render, and every row that changed
// must be inside the reported dirty range. render(full) renders; full = drop the cache first.
typedef const uint32_t *(*render_fn)(void *dev, bool full, int *w, int *h, int *y0, int *y1);
static void check_render_cache(const char *name, void *dev, render_fn render, void (*mutate)(void *dev, int kind))
{
    static uint32_t prev[1280 * 1024], fresh[1280 * 1024];
    int w, h, y0, y1, bad_match = 0, bad_dirty = 0, quiet_ok = 0, quiet = 0;
    const uint32_t *fb = render(dev, true, &w, &h, &y0, &y1);
    memcpy(prev, fb, (size_t)w * h * 4);
    for (int it = 0; it < 400; it++) {
        int kind = (int)rnd(8);
        mutate(dev, kind);
        fb = render(dev, false, &w, &h, &y0, &y1);
        for (int y = 0; y < h; y++)
            if ((y < y0 || y >= y1) && memcmp(prev + (size_t)y * w, fb + (size_t)y * w, (size_t)w * 4)) { bad_dirty++; break; }
        if (kind == 7) { quiet++; quiet_ok += y0 >= y1; }
        memcpy(prev, fb, (size_t)w * h * 4);
        fb = render(dev, true, &w, &h, &y0, &y1);
        memcpy(fresh, fb, (size_t)w * h * 4);
        if (memcmp(prev, fresh, (size_t)w * h * 4)) bad_match++;
        memcpy(prev, fresh, (size_t)w * h * 4);
    }
    char msg[128];
    snprintf(msg, sizeof msg, "%s: cached frames equal full renders (%d differ)", name, bad_match);
    CHECK(bad_match == 0, msg);
    snprintf(msg, sizeof msg, "%s: changed rows are inside the dirty range (%d frames not)", name, bad_dirty);
    CHECK(bad_dirty == 0, msg);
    snprintf(msg, sizeof msg, "%s: an unchanged screen reports no dirty rows (%d of %d)", name, quiet_ok, quiet);
    CHECK(quiet > 0 && quiet_ok == quiet, msg);
}

static const uint32_t *vga_rnd_render(void *dev, bool full, int *w, int *h, int *y0, int *y1)
{
    vga_t *v = dev;
    if (full) fbrows_invalidate(&v->rows);
    const uint32_t *fb = vga_render(v, w, h);
    *y0 = v->dirty_y0; *y1 = v->dirty_y1;
    return fb;
}

static void vga_rnd_mutate(void *dev, int kind)
{
    vga_t *v = dev;
    uint32_t span = 640u * 480u * 2u + 640u * 2u * 8u;
    switch (kind) {
    case 0: case 1: case 2: { int n = 1 + (int)rnd(30); uint32_t a = rnd(span); for (int i = 0; i < n; i++) v->vram[(a + i) % span] = (uint8_t)rnd(256); break; }
    case 3: v->dispi[9] = (uint16_t)rnd(8); break;                   // Y_OFF: scroll
    case 4: v->dispi[3] = rnd(2) ? 16 : 8; break;                    // BPP 16 <-> 8
    case 5: v->dac[rnd(256)][rnd(3)] = (uint8_t)rnd(64); break;      // palette (shows in 8 bpp)
    case 6: memset(v->vram + rnd(480) * 1280, (int)rnd(256), 1280); break;   // a whole row
    default: break;                                                  // 7: nothing
    }
}

static const uint32_t *g4_rnd_render(void *dev, bool full, int *w, int *h, int *y0, int *y1)
{
    g400_t *g = dev;
    if (full) { fbrows_invalidate(&g->rows); g->cur_key = 0; }
    const uint32_t *fb = g400_render(g, w, h);
    *y0 = g->dirty_y0; *y1 = g->dirty_y1;
    return fb;
}

static void g4_rnd_mutate(void *dev, int kind)
{
    g400_t *g = dev;
    switch (kind) {
    case 0: case 1: { int n = 1 + (int)rnd(30); uint32_t a = rnd(640 * 490); for (int i = 0; i < n; i++) g->vram[a + i] = (uint8_t)rnd(256); break; }
    case 2: { int x = (int)rnd(720), y = (int)rnd(560);              // move the cursor (partly off screen too)
              g->curpos[0] = (uint8_t)x; g->curpos[1] = (uint8_t)(x >> 8); g->curpos[2] = (uint8_t)y; g->curpos[3] = (uint8_t)(y >> 8); break; }
    case 3: g->vram[0x100000 + rnd(1024)] = (uint8_t)rnd(256); break; // cursor image
    case 4: g->pal[rnd(256)][rnd(3)] = (uint8_t)rnd(256); break;
    case 5: g->crtc[0x0D] = (uint8_t)(rnd(4) * 80); break;            // start address (8-byte units)
    case 6: g->x[0x06] = (uint8_t)(rnd(3) + 1); break;                // cursor mode 1..3
    default: break;
    }
}

// ES1370: captured output and interrupt line.
static int16_t g_snd[200000 * 2];
static int g_snd_n, g_es_irq;
static void snd_capture(void *o, const int16_t *f, int n)
{
    (void)o;
    for (int i = 0; i < n && g_snd_n < 200000; i++, g_snd_n++) { g_snd[2 * g_snd_n] = f[2 * i]; g_snd[2 * g_snd_n + 1] = f[2 * i + 1]; }
}
static void es_intx(void *o, int level) { (void)o; g_es_irq = level; }

static int g_sb_irq_line = -1, g_sb_irq_level;
static void sb_irq(void *o, int irq, int level) { (void)o; g_sb_irq_line = irq; g_sb_irq_level = level; }
static uint8_t pnp_rd(io_bus_t *io, uint16_t rp, uint8_t reg) { io_write(io, 0x279, reg, 1); return (uint8_t)io_read(io, rp, 1); }
static void pnp_wr(io_bus_t *io, uint8_t reg, uint8_t v) { io_write(io, 0x279, reg, 1); io_write(io, 0xA79, v, 1); }

static int g_init_cpu = -1, g_sipi_cpu = -1, g_sipi_vec = -1, g_pic_acks;
static void t_init(void *o, int i) { (void)o; g_init_cpu = i; }
static void t_sipi(void *o, int i, uint8_t v) { (void)o; g_sipi_cpu = i; g_sipi_vec = v; }
static int t_pic_ack(void *o) { (void)o; g_pic_acks++; return 0x20; }

int main(void)
{
    io_bus_t *io = io_create();
    cpu_t cpu;
    cpu_init(&cpu, 0);

    /* ---- PIC ---- */
    pic_init(&g_pic, &cpu);
    pic_register(&g_pic, io);
    pic_program(io);
    CHECK(!cpu.intr_line, "pic: idle");
    pic_set_irq(&g_pic, 3, 1);
    CHECK(cpu.intr_line, "pic: IRQ 3 raises INTR");
    pic_set_irq(&g_pic, 1, 1);
    CHECK(pic_ack(&g_pic) == 9, "pic: IRQ 1 wins over IRQ 3 (vector 9)");
    CHECK(!cpu.intr_line, "pic: IRQ 3 blocked while IRQ 1 is in service");
    out8(io, 0x20, 0x20);                                   // EOI
    CHECK(cpu.intr_line && pic_ack(&g_pic) == 11, "pic: then IRQ 3 (vector 11)");
    out8(io, 0x20, 0x20);
    pic_set_irq(&g_pic, 1, 0);
    pic_set_irq(&g_pic, 3, 0);
    pic_set_irq(&g_pic, 1, 1);                              // still high from before? no: new edge
    pic_ack(&g_pic); out8(io, 0x20, 0x20);
    pic_set_irq(&g_pic, 1, 1);                              // no new edge: no new request
    CHECK(!cpu.intr_line, "pic: edge triggered, level held = one request");
    pic_set_irq(&g_pic, 1, 0);
    out8(io, 0x21, 0x02);                                   // mask IRQ 1
    pic_set_irq(&g_pic, 1, 1);
    CHECK(!cpu.intr_line, "pic: masked");
    out8(io, 0x21, 0x00);
    CHECK(cpu.intr_line && pic_ack(&g_pic) == 9, "pic: unmasking delivers the latched request");
    out8(io, 0x20, 0x20);
    pic_set_irq(&g_pic, 1, 0);
    pic_set_irq(&g_pic, 12, 1);                             // slave IRQ 4
    CHECK(cpu.intr_line && pic_ack(&g_pic) == 0x74, "pic: cascaded IRQ 12 -> vector 0x74");
    CHECK((in8(io, 0x20), 1), "pic: read");
    out8(io, 0xA0, 0x0B);                                   // read ISR
    CHECK(in8(io, 0xA0) == 0x10, "pic: slave ISR bit 4");
    out8(io, 0xA0, 0x20); out8(io, 0x20, 0x20);
    pic_set_irq(&g_pic, 12, 0);
    io_write(io, 0x4D0, 0x0C00, 2);                         // IRQ 10, 11 level triggered (as SeaBIOS sets)
    CHECK(in8(io, 0x4D1) == 0x0C, "pic: ELCR");
    pic_set_irq(&g_pic, 11, 1);
    pic_ack(&g_pic); out8(io, 0xA0, 0x20); out8(io, 0x20, 0x20);
    CHECK(cpu.intr_line, "pic: level-triggered IRQ re-requests while the line is high");
    pic_set_irq(&g_pic, 11, 0);
    CHECK(!cpu.intr_line, "pic: and stops when it drops");

    /* ---- PIT ---- */
    pit_t pit;
    g_now = 1000000000ull;
    pit_init(&pit, fake_clock, NULL, irq_to_pic, NULL);
    pit_register(&pit, io);
    out8(io, 0x43, 0x34);                                   // ch0, lo/hi, mode 2
    out8(io, 0x40, 0x00); out8(io, 0x40, 0x00);             // 65536: 18.2 Hz
    g_now += 1000000;                                       // 1 ms = 1193 ticks
    out8(io, 0x43, 0x00);                                   // latch
    uint32_t cnt = in8(io, 0x40); cnt |= (uint32_t)in8(io, 0x40) << 8;
    CHECK(cnt == 65536 - 1193, "pit: count after 1 ms");
    int edges = g_irq_edges[0];
    pit_update(&pit);
    CHECK(g_irq_edges[0] == edges, "pit: no IRQ 0 before the period ends");
    g_now += 55000000;
    pit_update(&pit);
    CHECK(g_irq_edges[0] == edges + 1, "pit: IRQ 0 after one period");
    pic_ack(&g_pic); out8(io, 0x20, 0x20);
    // channel 2 one-shot through port 0x61, the way SeaBIOS calibrates the TSC
    out8(io, 0x61, 0x01);                                   // gate on
    out8(io, 0x43, 0xB0);                                   // ch2, lo/hi, mode 0
    out8(io, 0x42, 0xE8); out8(io, 0x42, 0x03);             // 1000 ticks
    CHECK(!(in8(io, 0x61) & 0x20), "pit: OUT2 low while counting");
    g_now += 900000;                                        // 1073 ticks
    CHECK(in8(io, 0x61) & 0x20, "pit: OUT2 high at terminal count");
    uint8_t r1 = in8(io, 0x61) & 0x10;
    g_now += 15085;
    CHECK((in8(io, 0x61) & 0x10) != r1, "pit: refresh bit toggles");

    /* ---- CMOS ---- */
    cmos_t cmos;
    cmos_init(&cmos, fake_clock, NULL, irq_to_pic, NULL);
    cmos_register(&cmos, io);
    cmos_set(&cmos, 0x34, 0x77);
    out8(io, 0x70, 0x80 | 0x34);                            // NMI-disable bit is ignored
    CHECK(in8(io, 0x71) == 0x77, "cmos: RAM byte");
    out8(io, 0x70, 0x0D);
    CHECK(in8(io, 0x71) == 0x80, "cmos: register D valid");
    out8(io, 0x70, 0x0B); out8(io, 0x71, 0x82);             // SET, 24h, BCD
    out8(io, 0x70, 0x04); out8(io, 0x71, 0x23);             // 23 h
    out8(io, 0x70, 0x02); out8(io, 0x71, 0x59);             // 59 min
    out8(io, 0x70, 0x0B); out8(io, 0x71, 0x02);             // run
    out8(io, 0x70, 0x04);
    CHECK(in8(io, 0x71) == 0x23, "cmos: guest-set hour sticks");
    out8(io, 0x70, 0x0B); out8(io, 0x71, 0x06);             // binary mode
    out8(io, 0x70, 0x04);
    CHECK(in8(io, 0x71) == 23, "cmos: binary mode");
    out8(io, 0x70, 0x0A); out8(io, 0x71, 0x26);             // 1024 Hz
    out8(io, 0x70, 0x0B); out8(io, 0x71, 0x46);             // periodic interrupt on
    edges = g_irq_edges[8];
    cmos_update(&cmos);
    g_now += 2000000;
    cmos_update(&cmos);
    CHECK(g_irq_edges[8] == edges + 1, "cmos: periodic IRQ 8");
    out8(io, 0x70, 0x0C);
    CHECK((in8(io, 0x71) & 0xC0) == 0xC0, "cmos: register C reports it");
    CHECK(!g_irq_level[8], "cmos: reading C lowers IRQ 8");

    /* ---- fw_cfg ---- */
    fwcfg_t fw;
    fwcfg_init(&fw);
    fwcfg_register(&fw, io);
    fwcfg_add_u16(&fw, FW_CFG_NB_CPUS, 2);
    fwcfg_add_file(&fw, "etc/test", "hello", 5);
    io_write(io, 0x510, FW_CFG_SIGNATURE, 2);
    char sig[5] = { 0 };
    for (int i = 0; i < 4; i++) sig[i] = (char)in8(io, 0x511);
    CHECK(!strcmp(sig, "QEMU"), "fw_cfg: signature");
    io_write(io, 0x510, FW_CFG_NB_CPUS, 2);
    CHECK(in8(io, 0x511) == 2 && in8(io, 0x511) == 0, "fw_cfg: CPU count (little-endian)");
    io_write(io, 0x510, FW_CFG_FILE_DIR, 2);
    uint8_t dir[68];
    for (int i = 0; i < 68; i++) dir[i] = in8(io, 0x511);
    CHECK(dir[3] == 1 && dir[7] == 5 && !strcmp((char *)dir + 12, "etc/test"), "fw_cfg: file directory (big-endian)");
    io_write(io, 0x510, (uint16_t)(dir[8] << 8 | dir[9]), 2);
    CHECK(in8(io, 0x511) == 'h', "fw_cfg: file contents");
    io_write(io, 0x510, 0x8000, 2);                         // arch-local item, not present
    CHECK(in8(io, 0x511) == 0, "fw_cfg: 0x8000 is not item 0");
    fwcfg_free(&fw);

    /* ---- PCI ---- */
    pci_bus_t pci;
    pci_dev_t dev = { 0 };
    pci_bus_init(&pci);
    pci_bus_register(&pci, io);
    pci_set_ids(&dev, 0x1234, 0x1111, 2, 0x030000, 0, 0);
    pci_set_bar(&dev, 0, 16u << 20, false, true);
    pci_add(&pci, 2, 0, &dev);
    io_write(io, 0xCF8, 0x80001000, 4);                     // 00:02.0 reg 0
    CHECK(io_read(io, 0xCFC, 4) == 0x11111234, "pci: vendor/device");
    CHECK(io_read(io, 0xCFE, 2) == 0x1111, "pci: 16-bit read at offset 2");
    io_write(io, 0xCF8, 0x80001010, 4);
    io_write(io, 0xCFC, 0xFFFFFFFF, 4);
    CHECK(io_read(io, 0xCFC, 4) == 0xFF000008, "pci: BAR sizing (16 MB prefetchable)");
    io_write(io, 0xCF8, 0x80000800, 4);                     // empty slot
    CHECK(io_read(io, 0xCFC, 4) == 0xFFFFFFFF, "pci: empty slot reads all ones");

    /* ---- PS/2: every byte is a new IRQ edge, the way BeOS's driver needs it ---- */
    ps2_t ps2;
    ps2_init(&ps2, irq_to_pic, NULL, NULL, NULL);
    ps2_register(&ps2, io);
    out8(io, 0x64, 0x60); out8(io, 0x60, 0x47);             // cmd byte: KINT, AINT, SYS, XLAT (as BeOS sets it)
    out8(io, 0x64, 0xA8);                                   // enable the aux port
    out8(io, 0x64, 0xD4); out8(io, 0x60, 0xF4);             // mouse: enable reporting
    CHECK((in8(io, 0x64) & 0x21) == 0x21 && in8(io, 0x60) == 0xFA, "ps2: mouse ACK on the aux port");
    int e12 = g_irq_edges[12];
    ps2_mouse(&ps2, 5, -3, 1);                              // right 5, up 3, left button
    uint8_t pkt[3];
    for (int i = 0; i < 3; i++) {
        CHECK(g_irq_level[12], "ps2: IRQ 12 up while a mouse byte waits");
        pkt[i] = in8(io, 0x60);
    }
    CHECK(pkt[0] == 0x09 && pkt[1] == 5 && pkt[2] == 3, "ps2: mouse packet (PS/2 y points up)");
    CHECK(g_irq_edges[12] == e12 + 3, "ps2: one IRQ 12 edge per byte");
    CHECK(!g_irq_level[12] && !(in8(io, 0x64) & 1), "ps2: IRQ 12 drops once the queue is empty");
    int e1 = g_irq_edges[1];
    ps2_key(&ps2, 0xE3, true);                              // left GUI: E0 5B
    CHECK(in8(io, 0x60) == 0xE0 && in8(io, 0x60) == 0x5B, "ps2: extended key in set 1");
    CHECK(g_irq_edges[1] == e1 + 2 && !g_irq_level[1], "ps2: one IRQ 1 edge per byte");

    /* ---- RTL8139 ---- */
    {
        mem_bus_t *bus = mem_create();
        uint8_t *ram = mem_alloc_host(1 << 20);
        mem_map_ram(bus, 0, 1 << 20, ram);
        rtl8139_t nic;
        static const uint8_t MAC[6] = { 0x52, 0x54, 0x00, 0xAB, 0xCD, 0xEF };
        rtl8139_init(&nic, bus, io, MAC, fake_clock, NULL);
        nic.send = nic_send_cb;
        nic.set_intx = nic_intx_cb;
        CHECK(pci_cfg_read(&nic.pci, 0, 4) == 0x813910ECu && pci_cfg_read(&nic.pci, 0x0B, 1) == 0x02, "nic: PCI 10ec:8139, network class");
        pci_cfg_write(&nic.pci, 0x10, NB, 4);
        pci_cfg_write(&nic.pci, 0x04, 0x05, 2);                     // I/O + bus master
        CHECK(nic_rd(io, 0x00, 4) == 0xAB005452u && nic_rd(io, 0x04, 2) == 0xEFCD, "nic: MAC in IDR0-5");
        CHECK(ee_read(io, 0) == 0x8129 && ee_read(io, 2) == 0x8139, "nic: EEPROM signature and device id");
        CHECK(ee_read(io, 7) == 0x5452 && ee_read(io, 8) == 0xAB00 && ee_read(io, 9) == 0xEFCD, "nic: EEPROM MAC words");
        CHECK((nic_rd(io, 0x40, 4) & 0x7CC00000u) == 0x74000000u, "nic: TCR hardware version RTL8139C");
        CHECK(nic_rd(io, 0x64, 2) & 0x0004, "nic: PHY link up");

        nic_wr(io, 0x37, 0x10, 1);                                  // soft reset
        CHECK(nic_rd(io, 0x37, 1) == 0x01 && nic_rd(io, 0x38, 2) == 0xFFF0, "nic: reset: ring empty, CAPR = -16");
        nic_wr(io, 0x37, 0x0C, 1);                                  // RE | TE
        nic_wr(io, 0x30, 0x10000, 4);                               // RBSTART, 8 KB ring
        nic_wr(io, 0x44, 0x0000000A, 4);                            // APM | AB
        nic_wr(io, 0x3C, 0xC07F, 2);

        // transmit
        uint8_t frame[100];
        for (int i = 0; i < 100; i++) frame[i] = (uint8_t)(i * 7);
        mem_write_block(bus, 0x20000, frame, sizeof frame);
        nic_wr(io, 0x24, 0x20000, 4);                               // TSAD1
        nic_wr(io, 0x14, 100, 4);                                   // TSD1: size, OWN = 0 -> send
        CHECK(g_sent_count == 1 && g_sent_len == 100 && !memcmp(g_sent, frame, 100), "nic: transmit sends the buffer");
        CHECK((nic_rd(io, 0x14, 4) & 0xA000) == 0xA000, "nic: TSD gets OWN and TOK");
        CHECK((nic_rd(io, 0x3E, 2) & 0x0004) && g_nic_irq, "nic: TOK interrupt");
        nic_wr(io, 0x3E, 0x0004, 2);
        CHECK(nic_rd(io, 0x3E, 2) == 0 && !g_nic_irq, "nic: ISR write-1-to-clear drops INTA");

        // receive: a 42-byte broadcast (padded to 60), then a unicast for us, then one for someone else
        uint8_t arp[42];
        memset(arp, 0xFF, 6); memcpy(arp + 6, "\x52\x55\x0A\x00\x02\x02", 6); arp[12] = 0x08; arp[13] = 0x06;
        for (int i = 14; i < 42; i++) arp[i] = (uint8_t)i;
        CHECK(rtl8139_receive(&nic, arp, sizeof arp), "nic: broadcast accepted");
        uint8_t hdr[4];
        mem_read_block(bus, 0x10000, hdr, 4);
        CHECK(hdr[0] == 0x01 && hdr[1] == 0x20 && (hdr[2] | hdr[3] << 8) == 64, "nic: header ROK|BAR, length 60 + CRC");
        uint8_t got[64];
        mem_read_block(bus, 0x10004, got, 64);
        uint8_t padded[60] = { 0 };
        memcpy(padded, arp, 42);
        uint32_t crc = crc32_ref(padded, 60);
        CHECK(!memcmp(got, padded, 60) && !memcmp(got + 60, &crc, 4), "nic: frame padded to 60 bytes, CRC appended");
        CHECK(nic_rd(io, 0x3A, 2) == 68 && !(nic_rd(io, 0x37, 1) & 1), "nic: CBR advanced, ring not empty");
        CHECK((nic_rd(io, 0x3E, 2) & 1) && g_nic_irq, "nic: ROK interrupt");
        uint8_t uni[80];
        memcpy(uni, MAC, 6); memset(uni + 6, 0x11, 74);
        CHECK(rtl8139_receive(&nic, uni, sizeof uni), "nic: unicast to us accepted");
        mem_read_block(bus, 0x10000 + 68, hdr, 4);
        CHECK(hdr[0] == 0x01 && hdr[1] == 0x40, "nic: header ROK|PAM");
        uint16_t cbr = (uint16_t)nic_rd(io, 0x3A, 2);
        uni[5] ^= 1;
        rtl8139_receive(&nic, uni, sizeof uni);
        CHECK(nic_rd(io, 0x3A, 2) == cbr, "nic: unicast to another MAC filtered");
        nic_wr(io, 0x38, (uint32_t)(cbr - 16), 2);                  // driver consumed everything
        CHECK(nic_rd(io, 0x37, 1) & 1, "nic: CAPR = CBR - 16 empties the ring");

        // fill the 8 KB ring with 1000-byte frames: wrap-around, then overflow
        uint8_t big[1000];
        for (int i = 0; i < 1000; i++) big[i] = (uint8_t)(i ^ 0x5A);
        memset(big, 0xFF, 6);
        int accepted = 0;
        while (rtl8139_can_receive(&nic, sizeof big) && accepted < 20) { rtl8139_receive(&nic, big, sizeof big); accepted++; }
        CHECK(accepted >= 7 && accepted <= 8, "nic: an 8 KB ring holds 7-8 KB frames");
        CHECK(!rtl8139_receive(&nic, big, sizeof big) && (nic_rd(io, 0x3E, 2) & 0x10) && nic_rd(io, 0x4C, 4) == 1,
              "nic: overflow sets RXOVW and counts a missed packet");
        // walk the ring like a driver: every record intact, across the wrap
        uint32_t rd = cbr, ok = 0;
        for (int k = 0; k < accepted; k++) {
            uint8_t rec[1008];
            for (int i = 0; i < 1008; i++) rec[i] = ram[0x10000 + ((rd + (uint32_t)i) % 8192)];
            uint32_t c2 = crc32_ref(big, 1000);
            if (rec[0] == 0x01 && (rec[2] | rec[3] << 8) == 1004 && !memcmp(rec + 4, big, 1000) && !memcmp(rec + 1004, &c2, 4)) ok++;
            rd = ((rd + 1008 + 3) & ~3u) % 8192;
        }
        CHECK(ok == (uint32_t)accepted && rd == nic_rd(io, 0x3A, 2), "nic: records intact across the ring wrap");
        mem_free_host(ram);
        mem_destroy(bus);
    }

    /* ---- NE2000 PCI (RTL8029) ---- */
    {
        ne2k_t ne;
        static const uint8_t MAC[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
        ne2k_init(&ne, io, MAC);
        ne.send = nic_send_cb;
        ne.set_intx = nic_intx_cb;
        g_nic_irq = 0;
        g_sent_count = 0;
        CHECK(pci_cfg_read(&ne.pci, 0, 4) == 0x802910ECu && pci_cfg_read(&ne.pci, 0x08, 4) >> 8 == 0x020000,
              "ne2k: PCI 10ec:8029, Ethernet class");
        pci_cfg_write(&ne.pci, 0x10, 0xFFFFFFFFu, 4);
        CHECK(pci_cfg_read(&ne.pci, 0x10, 4) == 0xFFFFFFE1u && pci_cfg_read(&ne.pci, 0x3D, 1) == 1, "ne2k: 32-byte I/O BAR, INTA");
        pci_cfg_write(&ne.pci, 0x10, EB, 4);
        pci_cfg_write(&ne.pci, 0x04, 0x01, 2);
        CHECK(ne_rd(io, 0x00) == 0x21 && (ne_rd(io, 0x07) & 0x80), "ne2k: power-on: stopped, ISR RST");
        ne_wr(io, 0x07, 0xFF);
        CHECK(ne_rd(io, 0x07) == 0x80, "ne2k: writing ISR does not clear RST");
        ne_wr(io, 0x00, 0x22);
        CHECK(ne_rd(io, 0x07) == 0x00, "ne2k: START clears RST");
        (void)ne_rd(io, 0x1F);                                      // reset port
        ne_wr(io, 0x1F, 0);
        CHECK(ne_rd(io, 0x00) == 0x21 && ne_rd(io, 0x07) == 0x80, "ne2k: reset port read resets the chip");
        CHECK(ne_rd(io, 0x0A) == 0x50 && ne_rd(io, 0x0B) == 0x43, "ne2k: RTL8029 ID 'PC'");

        // PROM, byte mode (DCR = 0x48): 32 bytes, every PROM byte twice
        ne_wr(io, 0x0E, 0x48);
        ne_dma_setup(io, 0, 32, false);
        uint8_t prom[32];
        for (int i = 0; i < 32; i++) prom[i] = ne_rd(io, 0x10);
        bool ok = prom[28] == 0x57 && prom[29] == 0x57 && prom[30] == 0x57;
        for (int i = 0; i < 6; i++) ok = ok && prom[2 * i] == MAC[i] && prom[2 * i + 1] == MAC[i];
        CHECK(ok, "ne2k: PROM in byte mode: MAC bytes doubled, 0x57 signature");
        CHECK((ne_rd(io, 0x07) & 0x40) && ne_rd(io, 0x08) == 32 && ne_rd(io, 0x09) == 0, "ne2k: RDC after the count, CRDA advanced");
        ne_wr(io, 0x07, 0x40);
        // PROM, word mode (DCR = 0x49): 16 words, low byte = the PROM byte
        ne_wr(io, 0x0E, 0x49);
        ne_dma_setup(io, 0, 32, false);
        uint16_t pw[16];
        for (int i = 0; i < 16; i++) pw[i] = (uint16_t)io_read(io, EB + 0x10, 2);
        ok = pw[14] == 0x5757 && pw[15] == 0x5757;
        for (int i = 0; i < 6; i++) ok = ok && pw[i] == (MAC[i] | MAC[i] << 8);
        CHECK(ok, "ne2k: PROM in word mode: each word = MAC byte twice");
        CHECK(ne_rd(io, 0x07) & 0x40, "ne2k: RDC in word mode");
        ne_wr(io, 0x07, 0xFF);
        ne_dma_setup(io, 0x4000, 0, false);
        CHECK(ne_rd(io, 0x07) & 0x40, "ne2k: zero-length remote DMA completes at once");
        ne_wr(io, 0x07, 0xFF);

        // driver init: ring 0x46-0x60 (26 pages), TX buffer at 0x40, PAR, broadcast
        ne_wr(io, 0x00, 0x21);
        ne_wr(io, 0x01, 0x46); ne_wr(io, 0x02, 0x60); ne_wr(io, 0x03, 0x46);
        ne_wr(io, 0x0C, 0x04);                                      // RCR: AB
        ne_wr(io, 0x0F, 0x7F);                                      // IMR
        ne_wr(io, 0x00, 0x61);                                      // page 1
        for (int i = 0; i < 6; i++) ne_wr(io, 1 + i, MAC[i]);
        ne_wr(io, 0x07, 0x47);                                      // CURR
        CHECK(ne_rd(io, 0x01) == MAC[0] && ne_rd(io, 0x06) == MAC[5] && ne_rd(io, 0x07) == 0x47, "ne2k: page 1 PAR and CURR");
        ne_wr(io, 0x00, 0xA1);                                      // page 2 reads back page 0 setup
        CHECK(ne_rd(io, 0x01) == 0x46 && ne_rd(io, 0x02) == 0x60 && ne_rd(io, 0x0C) == 0x04 && ne_rd(io, 0x0F) == 0x7F,
              "ne2k: page 2 PSTART/PSTOP/RCR/IMR");
        ne_wr(io, 0x00, 0x22);                                      // page 0, start

        // transmit: copy 100 bytes in by remote write (word mode), then TXP
        uint8_t frame[100];
        for (int i = 0; i < 100; i++) frame[i] = (uint8_t)(i * 11 + 3);
        ne_dma_setup(io, 0x4000, 100, true);
        for (int i = 0; i < 100; i += 2) io_write(io, EB + 0x10, frame[i] | frame[i + 1] << 8, 2);
        CHECK((ne_rd(io, 0x07) & 0x40) && g_nic_irq, "ne2k: remote write completes with RDC (interrupt)");
        ne_wr(io, 0x07, 0x40);
        CHECK(!g_nic_irq, "ne2k: ISR write-1-to-clear drops INTA");
        ne_wr(io, 0x04, 0x40); ne_wr(io, 0x05, 100); ne_wr(io, 0x06, 0);
        ne_wr(io, 0x00, 0x26);                                      // STA | TXP
        CHECK(g_sent_count == 1 && g_sent_len == 100 && !memcmp(g_sent, frame, 100), "ne2k: transmit sends TBCR bytes from TPSR");
        CHECK((ne_rd(io, 0x07) & 0x02) && (ne_rd(io, 0x04) & 0x01) && !(ne_rd(io, 0x00) & 0x04) && g_nic_irq,
              "ne2k: PTX in ISR and TSR, TXP clears, interrupt");
        ne_wr(io, 0x07, 0xFF);

        // receive: a 42-byte broadcast (padded to 60)
        uint8_t arp[42];
        memset(arp, 0xFF, 6); memcpy(arp + 6, "\x52\x55\x0A\x00\x02\x02", 6); arp[12] = 0x08; arp[13] = 0x06;
        for (int i = 14; i < 42; i++) arp[i] = (uint8_t)i;
        CHECK(ne2k_receive(&ne, arp, sizeof arp), "ne2k: broadcast accepted");
        uint8_t rec[1600];
        ne_read_mem(io, 0x4700, rec, 64);
        uint8_t padded[60] = { 0 };
        memcpy(padded, arp, 42);
        CHECK(rec[0] == 0x21 && rec[1] == 0x48 && (rec[2] | rec[3] << 8) == 64, "ne2k: header PRX|PHY, next page, length 4 + 60");
        CHECK(!memcmp(rec + 4, padded, 60), "ne2k: frame padded to 60 bytes");
        ne_wr(io, 0x00, 0x62);
        CHECK(ne_rd(io, 0x07) == 0x48, "ne2k: CURR advanced one page");
        ne_wr(io, 0x00, 0x22);
        CHECK((ne_rd(io, 0x07) & 0x01) && g_nic_irq, "ne2k: PRX interrupt");
        ne_wr(io, 0x07, 0xFF);
        uint8_t uni[80];
        memcpy(uni, MAC, 6); memset(uni + 6, 0x22, 74);
        uni[5] ^= 1;
        ne2k_receive(&ne, uni, sizeof uni);
        CHECK(!(ne_rd(io, 0x07) & 0x01), "ne2k: unicast to another MAC filtered");
        uni[5] ^= 1;
        CHECK(ne2k_receive(&ne, uni, sizeof uni) && (ne_rd(io, 0x07) & 0x01), "ne2k: unicast to us accepted");
        ne_read_mem(io, 0x4800, rec, 4);
        CHECK(rec[0] == 0x01 && rec[1] == 0x49 && (rec[2] | rec[3] << 8) == 84, "ne2k: unicast header PRX, length 4 + 80");
        ne_wr(io, 0x03, 0x48);                                      // driver consumed both (BNRY = CURR - 1)
        ne_wr(io, 0x07, 0xFF);

        // fill the ring with 1000-byte frames (4 pages each), then overflow; drain it like a
        // driver reading through remote DMA, which wraps at PSTOP
        uint8_t big[1000];
        int accepted = 0, round_ok = 0, drained = 0;
        for (int round = 0; round < 3; round++) {
            int got = 0;
            while (ne2k_can_receive(&ne, sizeof big) && got < 20) {
                for (int i = 0; i < 1000; i++) big[i] = (uint8_t)(i ^ (accepted + got));
                memcpy(big, MAC, 6);
                ne2k_receive(&ne, big, sizeof big);
                got++;
            }
            if (round == 0) {
                CHECK(got == 6, "ne2k: 26-page ring holds 6 four-page frames");
                CHECK(!ne2k_receive(&ne, big, sizeof big) && (ne_rd(io, 0x07) & 0x10), "ne2k: full ring: frame refused, OVW");
            }
            int seen = 0;
            for (;;) {
                uint8_t bnry = ne_rd(io, 0x03);
                ne_wr(io, 0x00, 0x62); uint8_t curr = ne_rd(io, 0x07); ne_wr(io, 0x00, 0x22);
                uint8_t page = bnry + 1 == 0x60 ? 0x46 : bnry + 1;
                if (page == curr) break;
                ne_read_mem(io, (uint16_t)(page << 8), rec, 4);
                uint16_t len = (uint16_t)(rec[2] | rec[3] << 8);
                uint8_t next = rec[1];
                ne_read_mem(io, (uint16_t)((page << 8) + 4), rec + 4, (uint16_t)((len - 4 + 1) & ~1));
                for (int i = 0; i < 1000; i++) big[i] = (uint8_t)(i ^ (accepted + seen));
                memcpy(big, MAC, 6);
                if (len == 1004 && !memcmp(rec + 4, big, 1000)) drained++;
                seen++;
                ne_wr(io, 0x03, next == 0x46 ? 0x5F : next - 1);
            }
            if (seen == got) round_ok++;
            accepted += got;
        }
        CHECK(round_ok == 3 && drained == accepted && accepted >= 18, "ne2k: records intact across ring wraps");
        ne_wr(io, 0x07, 0x7F);
        ne_wr(io, 0x00, 0x21);                                      // stopped: frames are dropped
        CHECK(ne2k_can_receive(&ne, 1000) && ne2k_receive(&ne, big, sizeof big) && !(ne_rd(io, 0x07) & 0x01),
              "ne2k: stopped chip drops frames");
        pci_cfg_write(&ne.pci, 0x04, 0, 2);                         // unmap the BAR
    }

    // ---- Matrox G400: 2D engine, status, BAR overlap ----
    {
        mem_bus_t *bus = mem_create();
        static g400_t g;
        CHECK(g400_init(&g, bus, fake_clock, NULL), "g400: init");
        CHECK(pci_cfg_read(&g.pci, 0, 4) == 0x0525102Bu, "g400: PCI id 102B:0525");
        uint32_t st = g400_reg_read(&g, 0x1E14, 4);
        CHECK((st & 0x20000) && !(st & 0x10000), "g400: STATUS reports DMA idle and engine idle");

        // BAR 0 at 0xFA000000; the driver turns the ROM on over the framebuffer and off again.
        pci_cfg_write(&g.pci, 0x10, 0xFA000000u, 4);
        pci_cfg_write(&g.pci, 0x04, 2, 2);
        pci_cfg_write(&g.pci, 0x30, 0xFA000001u, 4);
        CHECK(mem_read(bus, 0xFA000000u, 1) == 0, "g400: ROM visible while enabled");
        pci_cfg_write(&g.pci, 0x30, 0xFA000000u, 4);
        mem_write(bus, 0xFA000010u, 0x5A, 1);
        CHECK(g.vram[0x10] == 0x5A, "g400: framebuffer start still mapped after the ROM BAR is turned off");

        #define DW(reg, v) g400_reg_write(&g, 0x1C00 + 4 * (reg), (v), 4)
        DW(0x01, 0);                        // MACCESS: 8 bpp
        DW(0x07, 0xFFFFFFFFu);              // PLNWT
        DW(0x23, 0x8500);                   // PITCH 1280, ylin
        DW(0x25, 0);                        // YDSTORG
        DW(0x20, 0x04FF0000u);              // CXBNDRY 0..1279
        DW(0x26, 0); DW(0x27, 1023 * 1280); // YTOP, YBOT (line addresses)
        DW(0x00, 0x000C7844u);              // DWGCTL: TRAP, replace
        DW(0x09, 0xB4B4B4B4u);              // FCOL
        DW(0x21, 20u << 16 | 10);           // FXBNDRY: x 10..19
        DW(0x24, 56 * 1280 / 32);           // YDST: line address / 32
        g400_reg_write(&g, 0x1D5C, 3, 4);   // LEN = 3, go
        const uint8_t *v = g.vram;
        CHECK(v[56 * 1280 + 10] == 0xB4 && v[58 * 1280 + 19] == 0xB4, "g400: ylin fill covers the rectangle");
        CHECK(v[56 * 1280 + 9] == 0 && v[56 * 1280 + 20] == 0 && v[59 * 1280 + 10] == 0 && v[55 * 1280 + 10] == 0,
              "g400: ylin fill stays inside it");
        CHECK(g400_reg_read(&g, 0x1C90, 4) == 59 * 1280 / 32, "g400: YDST advances by pitch/32 per line");

        DW(0x23, 0x0500);                   // XY mode: YDST is a row
        DW(0x27, 1023 * 1280);
        DW(0x09, 0x11111111u);
        DW(0x21, 4u << 16 | 0);
        DW(0x24, 10);
        g400_reg_write(&g, 0x1D5C, 2, 4);
        CHECK(v[10 * 1280] == 0x11 && v[11 * 1280 + 3] == 0x11 && v[12 * 1280] == 0 && v[10 * 1280 + 4] == 0,
              "g400: xy fill");
        DW(0x26, 11 * 1280);                // clip: YTOP at row 11
        DW(0x09, 0x22222222u);
        DW(0x24, 10);
        g400_reg_write(&g, 0x1D5C, 2, 4);
        CHECK(v[10 * 1280] == 0x11 && v[11 * 1280] == 0x22, "g400: YTOP clips lines above it");
        DW(0x26, 0);

        // Screen to screen copy (ylin): row 100 x 0..7 -> row 200 x 200..207.
        for (int i = 0; i < 8; i++) g.vram[100 * 1280 + i] = (uint8_t)(0x40 + i);
        DW(0x23, 0x8500);
        DW(0x00, 0x040C4008u);              // BITBLT, replace
        DW(0x16, 0);                        // SGN: left to right, down
        DW(0x1B, 100 * 1280);               // AR3: source start
        DW(0x18, 100 * 1280 + 7);           // AR0: source end
        DW(0x1D, 1280);                     // AR5: source pitch
        DW(0x21, 207u << 16 | 200);         // FXBNDRY: inclusive right edge for blits
        DW(0x24, 200 * 1280 / 32);
        g400_reg_write(&g, 0x1D5C, 1, 4);
        bool same = true;
        for (int i = 0; i < 8; i++) same &= v[200 * 1280 + 200 + i] == 0x40 + i;
        CHECK(same && v[200 * 1280 + 208] == 0 && v[200 * 1280 + 199] == 0, "g400: blit copies the span");

        // Right to left (SGN.scanleft): AR3 is the source's right end; also clipped at x 203.
        DW(0x16, 1);
        DW(0x1B, 100 * 1280 + 7);
        DW(0x18, 100 * 1280);
        DW(0x20, 203u << 16 | 0);           // CXBNDRY: x 0..203
        DW(0x21, 307u << 16 | 300);         // dest x 300..307: all clipped away
        DW(0x24, 210 * 1280 / 32);
        g400_reg_write(&g, 0x1D5C, 1, 4);
        DW(0x1B, 100 * 1280 + 7);           // AR3/AR0 moved on by AR5: load them again
        DW(0x18, 100 * 1280);
        DW(0x21, 207u << 16 | 200);         // dest x 200..207, columns 204..207 clipped
        DW(0x24, 220 * 1280 / 32);
        g400_reg_write(&g, 0x1D5C, 1, 4);
        same = v[210 * 1280 + 300] == 0;
        for (int i = 0; i < 4; i++) same &= v[220 * 1280 + 200 + i] == 0x40 + i;
        CHECK(same && v[220 * 1280 + 204] == 0, "g400: right-to-left blit maps source columns and clips");
        DW(0x20, 0x04FF0000u);
        #undef DW
        g400_free(&g);
        mem_destroy(bus);
    }

    // ---- renderers: row cache ----
    {
        mem_bus_t *bus = mem_create();
        static vga_t v;
        CHECK(vga_init(&v, bus, NULL, 0, fake_clock, NULL, NULL, NULL), "vga: init");
        v.dispi[1] = 640; v.dispi[2] = 480; v.dispi[3] = 16; v.dispi[4] = 0x01 | 0x40;   // 640x480x16, LFB
        for (uint32_t i = 0; i < 640u * 490u * 2u; i++) v.vram[i] = (uint8_t)rnd(256);
        check_render_cache("vga 16/8 bpp", &v, vga_rnd_render, vga_rnd_mutate);
        vga_free(&v);

        static g400_t g;
        CHECK(g400_init(&g, bus, fake_clock, NULL), "g400: init (render)");
        g.crtc[1] = 79; g.crtc[0x12] = 0xDF; g.crtc[7] = 0x02; g.crtc[0x13] = 40;      // 640x480, pitch 640
        g.crtcext[3] = 0x80;                                                           // MGA mode
        g.x[0x19] = 0;                                                                 // 8 bpp
        g.x[0x04] = 0x00; g.x[0x05] = 0x04; g.x[0x06] = 2;                             // cursor at 1 MB, XGA
        for (int i = 0; i < 12; i++) g.x[0x08 + i] = (uint8_t)rnd(256);
        for (uint32_t i = 0; i < 640u * 490u; i++) g.vram[i] = (uint8_t)rnd(256);
        for (int i = 0; i < 1024; i++) g.vram[0x100000 + i] = (uint8_t)rnd(256);
        g.curpos[0] = 100; g.curpos[2] = 120;
        CHECK(g400_active(&g), "g400: test mode active");
        check_render_cache("g400 8 bpp + cursor", &g, g4_rnd_render, g4_rnd_mutate);
        g400_free(&g);
        mem_destroy(bus);
    }

    // ---- Ensoniq ES1370: DAC2 as R5's es137x drives it ----
    {
        mem_bus_t *bus = mem_create();
        uint8_t *ram = mem_alloc_host(1 << 20);
        mem_map_ram(bus, 0, 1 << 20, ram);
        io_bus_t *eio = io_create();
        static es1370_t es;
        g_now = 1000000000ull;
        es1370_init(&es, bus, eio, fake_clock, NULL);
        es.out = snd_capture; es.set_intx = es_intx;
        CHECK(pci_cfg_read(&es.pci, 0, 4) == 0x50001274u && (pci_cfg_read(&es.pci, 8, 4) >> 8) == 0x040100u, "es1370: PCI id, audio class");
        // 4096-byte buffer at 0x7000, 16-bit stereo, 44100 Hz, interrupt every 512 samples.
        for (int i = 0; i < 1024; i++) {
            int16_t l = (int16_t)(i * 31 - 15000), r = (int16_t)(12000 - i * 17);
            memcpy(ram + 0x7000 + 4 * i, &l, 2); memcpy(ram + 0x7000 + 4 * i + 2, &r, 2);
        }
        es1370_write(&es, 0x0C, 0x0C, 4);                   // memory page C
        es1370_write(&es, 0x38, 0x7000, 4);                 // DAC2 frame address
        es1370_write(&es, 0x3C, 1024 - 1, 4);               // DAC2 frame size (dwords - 1)
        es1370_write(&es, 0x28, 512 - 1, 4);                // DAC2 samples per interrupt - 1
        es1370_write(&es, 0x20, 0x0000020C, 4);             // SCTRL: P2 16-bit stereo, interrupt on
        es1370_write(&es, 0x00, 0x401E0000u | 0x20 | 2, 4); // CTRL: PCLKDIV 30 (44100 Hz), DAC2 on
        g_snd_n = 0;
        g_now += 511ull * 1000000000ull / 44100;           // 511 samples: no interrupt yet
        es1370_update(&es);
        CHECK(!g_es_irq && !(es1370_read(&es, 0x04, 4) & 2), "es1370: no interrupt before 512 samples");
        g_now += 2ull * 1000000000ull / 44100 + 1000;
        es1370_update(&es);
        CHECK(g_es_irq && (es1370_read(&es, 0x04, 4) & 0x80000002u) == 0x80000002u, "es1370: DAC2 interrupt after 512 samples");
        CHECK(es1370_read(&es, 0x3E, 2) >= 512 && es1370_read(&es, 0x3E, 2) <= 514, "es1370: play position (high half of 0x3C) in dwords");
        es1370_write(&es, 0x21, 0x00, 1);                   // ack the way es137x does: interrupt enable off ...
        CHECK(!g_es_irq && !(es1370_read(&es, 0x04, 4) & 2), "es1370: clearing the interrupt enable acknowledges");
        es1370_write(&es, 0x21, 0x02, 1);                   // ... and on again
        // Output: one sample behind (interpolation start), exactly the buffer's samples, looping.
        g_now += 3000ull * 1000000000ull / 44100;
        es1370_update(&es);
        int bad = 0;
        for (int i = 1; i < g_snd_n; i++) {
            int k = (i - 1) % 1024;
            if (g_snd[2 * i] != (int16_t)(k * 31 - 15000) || g_snd[2 * i + 1] != (int16_t)(12000 - k * 17)) bad++;
        }
        CHECK(g_snd_n > 3000 && bad == 0, "es1370: 16-bit stereo at 44100 Hz comes out sample-exact and loops");
        // Pause stops it; 8-bit mono at 22050 Hz is upsampled by linear interpolation.
        es1370_write(&es, 0x20, 0x00001200, 4);             // P2 pause
        g_snd_n = 0;
        g_now += 100ull * 1000000000ull / 44100;
        es1370_update(&es);
        CHECK(g_snd_n == 0, "es1370: paused DAC produces nothing");
        for (int i = 0; i < 64; i++) ram[0x8000 + i] = (uint8_t)(i & 1 ? 0xC0 : 0x40);
        es1370_write(&es, 0x00, 0x2, 4);                    // DAC2 off
        es1370_write(&es, 0x0C, 0x0C, 4);
        es1370_write(&es, 0x38, 0x8000, 4);
        es1370_write(&es, 0x3C, 16 - 1, 4);
        es1370_write(&es, 0x20, 0x00000000, 4);             // P2 8-bit mono, no interrupt
        es1370_write(&es, 0x00, (62u << 16) | 0x20 | 2, 4); // PCLKDIV 62: 1411200 / 64 = 22050 Hz
        g_snd_n = 0;
        g_now += 200ull * 1000000000ull / 44100;
        es1370_update(&es);
        // Input alternates -16384 / +16384 at half the output rate: output frames that land on
        // a sample are +-16384 (alternating), the ones in between are their midpoint, 0.
        bool lin = g_snd_n >= 199;
        for (int i = 3; i < 150 && lin; i++) {
            int v = g_snd[2 * i];
            if (i & 1) lin &= (v == 16384 || v == -16384) && v == -g_snd[2 * (i + 2)] && g_snd[2 * i + 1] == v;
            else       lin &= v == 0;
        }
        CHECK(lin, "es1370: 22050 Hz 8-bit mono is upsampled by linear interpolation");
        // Codec: master mute silences the output.
        es1370_write(&es, 0x10, 0x0080, 2); es1370_write(&es, 0x10, 0x0180, 2);
        g_snd_n = 0;
        g_now += 100ull * 1000000000ull / 44100;
        es1370_update(&es);
        bool silent = g_snd_n > 0;
        for (int i = 0; i < g_snd_n; i++) silent &= g_snd[2 * i] == 0 && g_snd[2 * i + 1] == 0;
        CHECK(silent, "es1370: AK4531 master mute");
        // ADC records silence into its buffer (so capture doesn't hang) and interrupts.
        memset(ram + 0x9000, 0x55, 256);
        es1370_write(&es, 0x0C, 0x0D, 4);
        es1370_write(&es, 0x30, 0x9000, 4);
        es1370_write(&es, 0x34, 64 - 1, 4);
        es1370_write(&es, 0x2C, 32 - 1, 4);
        es1370_write(&es, 0x20, 0x00000430, 4);             // R1 16-bit stereo, interrupt on
        es1370_write(&es, 0x00, (30u << 16) | 0x10 | 2, 4); // ADC on at 44100 Hz
        g_now += 40ull * 1000000000ull / 44100;
        es1370_update(&es);
        bool zero = true;
        for (int i = 0; i < 32 * 4; i++) zero &= ram[0x9000 + i] == 0;
        CHECK(zero && (es1370_read(&es, 0x04, 4) & 1), "es1370: ADC writes silence and interrupts");
        io_destroy(eio);
        mem_destroy(bus);
        mem_free_host(ram);
    }

    // ---- ISA DMA + Sound Blaster 16 PnP, driven the way BeOS's isa module and awe64 do ----
    {
        mem_bus_t *bus = mem_create();
        uint8_t *ram = mem_alloc_host(1 << 20);
        mem_map_ram(bus, 0, 1 << 20, ram);
        io_bus_t *sio = io_create();
        static dma_t dma;
        dma_init(&dma, bus);
        dma_register(&dma, sio);
        // 8237: channel 1, 16 bytes at 0x12340 (page 1), auto-init; channel 5, 8 words at 0x20000.
        for (int i = 0; i < 64; i++) ram[0x12340 + i] = (uint8_t)i;
        out8(sio, 0x0C, 0); out8(sio, 0x02, 0x40); out8(sio, 0x02, 0x23); out8(sio, 0x83, 0x01);
        out8(sio, 0x03, 15); out8(sio, 0x03, 0); out8(sio, 0x0B, 0x59); out8(sio, 0x0A, 0x01);
        uint8_t got[40];
        int n = dma_read(&dma, 1, got, 20);
        bool seq = n == 20;
        for (int i = 0; i < 20; i++) seq &= got[i] == (uint8_t)(i % 16);
        CHECK(seq && (in8(sio, 0x08) & 0x02), "dma: channel 1 reads 16 bytes, terminal count, auto-init wraps");
        out8(sio, 0x0B, 0x49);                              // single cycle
        out8(sio, 0x0C, 0); out8(sio, 0x02, 0x40); out8(sio, 0x02, 0x23); out8(sio, 0x03, 3); out8(sio, 0x03, 0);
        out8(sio, 0x0A, 0x01);
        CHECK(dma_read(&dma, 1, got, 20) == 4 && dma_masked(&dma, 1), "dma: single cycle stops (and masks) at terminal count");
        for (int i = 0; i < 16; i++) ram[0x20000 + i] = (uint8_t)(0xA0 + i);
        io_write(sio, 0xD8, 0, 1); io_write(sio, 0xC4, 0x00, 1); io_write(sio, 0xC4, 0x00, 1);   // word address 0x10000
        io_write(sio, 0x8B, 0x00, 1);                       // page (bit 0 ignored on 16-bit channels)
        io_write(sio, 0xC6, 7, 1); io_write(sio, 0xC6, 0, 1); io_write(sio, 0xD6, 0x59, 1); io_write(sio, 0xD4, 0x01, 1);
        io_write(sio, 0xC4, 0x00, 1); io_write(sio, 0xC4, 0x00, 1);
        io_write(sio, 0xD8, 0, 1); io_write(sio, 0xC4, 0x00, 1); io_write(sio, 0xC4, 0x00, 1);
        // 0x10000 words = byte 0x20000 with page 0? A 16-bit channel's address counts words within a
        // 128 KB page: byte address = (page & 0xFE) << 16 | word << 1. Use page 2, word 0.
        io_write(sio, 0x8B, 0x02, 1);
        n = dma_read(&dma, 5, got, 16);
        seq = n == 16;
        for (int i = 0; i < 16; i++) seq &= got[i] == (uint8_t)(0xA0 + i);
        CHECK(seq, "dma: channel 5 moves words from (page & 0xFE) << 16 | word << 1");

        static sb16_t sb;
        g_now = 5000000000ull;
        sb16_init(&sb, sio, &dma, fake_clock, NULL);
        sb.set_irq = sb_irq;
        sb.out_fn = snd_capture;
        CHECK(io_read(sio, 0x22C, 1) == 0xFFu, "sb16: silent before Plug and Play activation");
        // Initiation key, then isolation through read port 0x203.
        io_write(sio, 0x279, 0, 1); io_write(sio, 0x279, 0, 1);
        uint8_t k = 0x6A;
        for (int i = 0; i < 32; i++) { io_write(sio, 0x279, k, 1); k = (uint8_t)(k >> 1 | ((k ^ (k >> 1)) & 1) << 7); }
        pnp_wr(sio, 0x02, 0x04);                            // reset CSNs
        pnp_wr(sio, 0x03, 0x00);                            // wake[0]: isolation
        pnp_wr(sio, 0x00, 0x80);                            // read port 0x203
        uint8_t id[9] = { 0 };
        io_write(sio, 0x279, 0x01, 1);
        for (int b = 0; b < 72; b++) {
            uint8_t x = (uint8_t)io_read(sio, 0x203, 1), y = (uint8_t)io_read(sio, 0x203, 1);
            if (x == 0x55 && y == 0xAA) id[b >> 3] |= (uint8_t)(1u << (b & 7));
        }
        uint8_t lfsr = 0x6A;
        for (int b = 0; b < 64; b++) lfsr = (uint8_t)(lfsr >> 1 | (((lfsr ^ (lfsr >> 1) ^ (id[b >> 3] >> (b & 7))) & 1) << 7));
        CHECK(id[0] == 0x0E && id[1] == 0x8C && id[2] == 0x00 && id[3] == 0x28 && id[8] == lfsr,
              "sb16: isolation reads vendor id CTL0028 with a valid checksum");
        pnp_wr(sio, 0x06, 0x01);                            // CSN 1: config state, resource data next
        uint8_t res[128];
        for (int i = 0; i < 128; i++) res[i] = pnp_rd(sio, 0x203, 0x04);
        bool ld = false, endtag = false;
        uint8_t sum = 0;
        for (int i = 0; i < 128 && !endtag; ) {             // walk the tags
            uint8_t t = res[i];
            if (t & 0x80) { int len = res[i + 1] | res[i + 2] << 8; for (int j = 0; j < 3 + len; j++) sum = (uint8_t)(sum + res[i + j]); i += 3 + len; continue; }
            int len = t & 7, type = t >> 3;
            if (type == 0x2 && res[i + 1] == 0x0E && res[i + 2] == 0x8C && res[i + 4] == 0x31) ld = true;
            for (int j = 0; j < 1 + len; j++) sum = (uint8_t)(sum + res[i + j]);
            if (type == 0xF) endtag = true;
            i += 1 + len;
        }
        CHECK(ld && endtag && sum == 0, "sb16: resource data starts after the serial id, has logical device CTL0031, sums to 0");
        // Configure like BeOS did: 0x220, MPU 0x300, IRQ 5, DMA 1/5, activate, back to wait-for-key.
        pnp_wr(sio, 0x07, 0); pnp_wr(sio, 0x60, 0x02); pnp_wr(sio, 0x61, 0x20); pnp_wr(sio, 0x62, 0x03); pnp_wr(sio, 0x63, 0x00);
        pnp_wr(sio, 0x70, 5); pnp_wr(sio, 0x74, 1); pnp_wr(sio, 0x75, 5); pnp_wr(sio, 0x30, 1); pnp_wr(sio, 0x02, 0x02);
        // DSP reset and version.
        out8(sio, 0x226, 1); out8(sio, 0x226, 0);
        bool ready = (in8(sio, 0x22E) & 0x80) && in8(sio, 0x22A) == 0xAA;
        out8(sio, 0x22C, 0xE1);
        uint8_t maj = in8(sio, 0x22A), min = in8(sio, 0x22A);
        CHECK(ready && maj == 4 && min == 13, "sb16: DSP resets (0xAA) and reports version 4.13");
        out8(sio, 0x224, 0x80); uint8_t irqsel = in8(sio, 0x225);
        out8(sio, 0x224, 0x81); uint8_t dmasel = in8(sio, 0x225);
        CHECK(irqsel == 0x02 && dmasel == 0x22, "sb16: mixer 0x80/0x81 report IRQ 5 and DMA 1/5");
        // 16-bit stereo auto-init playback at 44100 Hz from a 4 KB DMA buffer at 0x30000, 1024 units per block.
        for (int i = 0; i < 1024; i++) {
            int16_t l = (int16_t)(i * 29 - 14000), r = (int16_t)(9000 - i * 13);
            memcpy(ram + 0x30000 + 4 * i, &l, 2); memcpy(ram + 0x30000 + 4 * i + 2, &r, 2);
        }
        io_write(sio, 0xD4, 0x05, 1); io_write(sio, 0xD8, 0, 1);
        io_write(sio, 0xC4, 0x00, 1); io_write(sio, 0xC4, 0x80, 1);      // word 0x8000 -> byte 0x10000 in page 2
        io_write(sio, 0x8B, 0x02, 1);
        io_write(sio, 0xC6, 0xFF, 1); io_write(sio, 0xC6, 0x07, 1);      // 2048 words
        io_write(sio, 0xD6, 0x59, 1); io_write(sio, 0xD4, 0x01, 1);      // auto-init read, unmask ch 5
        out8(sio, 0x224, 0x41); out8(sio, 0x225, 0x40);                  // output gain x2 (unity here)
        out8(sio, 0x224, 0x42); out8(sio, 0x225, 0x40);
        out8(sio, 0x22C, 0x41); out8(sio, 0x22C, 0xAC); out8(sio, 0x22C, 0x44);   // 44100 Hz
        g_snd_n = 0;
        out8(sio, 0x22C, 0xB6); out8(sio, 0x22C, 0x30); out8(sio, 0x22C, 0xFF); out8(sio, 0x22C, 0x03);   // 1024 units
        g_now += 511ull * 1000000000ull / 44100;
        sb16_update(&sb);
        CHECK(!g_sb_irq_level, "sb16: no interrupt before the block (512 stereo frames) ends");
        g_now += 2ull * 1000000000ull / 44100 + 1000;
        sb16_update(&sb);
        out8(sio, 0x224, 0x82);
        CHECK(g_sb_irq_line == 5 && g_sb_irq_level && (in8(sio, 0x225) & 2), "sb16: 16-bit block end raises IRQ 5, mixer 0x82 bit 1");
        in8(sio, 0x22F);
        CHECK(!g_sb_irq_level, "sb16: reading base+0xF acknowledges the 16-bit interrupt");
        // Master and voice at 0 dB.
        out8(sio, 0x224, 0x30); out8(sio, 0x225, 0xF8); out8(sio, 0x224, 0x31); out8(sio, 0x225, 0xF8);
        out8(sio, 0x224, 0x32); out8(sio, 0x225, 0xF8); out8(sio, 0x224, 0x33); out8(sio, 0x225, 0xF8);
        int start_n = g_snd_n;
        g_now += 3000ull * 1000000000ull / 44100;
        sb16_update(&sb);
        // The buffer is 1024 frames; frame f of the output (after the first) is sample (f - 1) mod 1024.
        int bad = 0;
        for (int i = start_n; i < g_snd_n; i++) {
            int kk = (i - 1) % 1024;
            if (g_snd[2 * i] != (int16_t)(kk * 29 - 14000) || g_snd[2 * i + 1] != (int16_t)(9000 - kk * 13)) bad++;
        }
        CHECK(g_snd_n - start_n > 2900 && bad == 0, "sb16: 16-bit stereo through DMA 5 comes out sample-exact at 0 dB");
        out8(sio, 0x22C, 0xD5);                             // pause: silence, real time kept
        start_n = g_snd_n;
        g_now += 100ull * 1000000000ull / 44100;
        sb16_update(&sb);
        bool quiet = g_snd_n - start_n >= 99;
        for (int i = start_n; i < g_snd_n; i++) quiet &= g_snd[2 * i] == 0 && g_snd[2 * i + 1] == 0;
        CHECK(quiet, "sb16: paused DMA plays silence");
        io_destroy(sio);
        mem_destroy(bus);
        mem_free_host(ram);
    }

    // ---- local APICs and I/O APIC ----
    {
        mem_bus_t *bus = mem_create();
        static cpu_t cpus[4];
        static apic_sys_t ap;
        for (int i = 0; i < 4; i++) { cpu_init(&cpus[i], i); cpus[i].mem = bus; }
        g_now = 1000000000ull;
        apic_init(&ap, 4, cpus, bus, fake_clock, NULL);
        ap.init_cpu = t_init; ap.start_cpu = t_sipi; ap.pic_ack = t_pic_ack;
        for (int i = 0; i < 4; i++) {
            lapic_write(&ap, i, 0xF0, 0x1FF);                       // software enable, spurious 0xFF
            lapic_write(&ap, i, 0xE0, 0xFFFFFFFF);                  // flat model
            lapic_write(&ap, i, 0xD0, (1u << i) << 24);             // logical ID bit i
        }
        CHECK(lapic_read(&ap, 2, 0x20) == 2u << 24 && (lapic_read(&ap, 0, 0x30) & 0xFF) == 0x11, "apic: IDs, version");
        // Fixed IPI, physical destination 2.
        lapic_write(&ap, 0, 0x310, 2u << 24);
        lapic_write(&ap, 0, 0x300, 0x00000041);
        CHECK(cpus[2].intr_line && !cpus[1].intr_line && !cpus[3].intr_line, "apic: fixed IPI reaches only its physical target");
        CHECK(apic_ack(cpus[2].intr_opaque) == 0x41 && !cpus[2].intr_line && (lapic_read(&ap, 2, 0x100 + 0x20) & 2),
              "apic: INTA returns the vector and moves it to ISR");
        lapic_write(&ap, 2, 0xB0, 0);
        CHECK(lapic_read(&ap, 2, 0x120) == 0, "apic: EOI clears ISR");
        // Logical (flat) destination 0b1010: CPUs 1 and 3.
        lapic_write(&ap, 0, 0x310, 0x0Au << 24);
        lapic_write(&ap, 0, 0x300, 0x00000850);
        CHECK(!cpus[0].intr_line && cpus[1].intr_line && !cpus[2].intr_line && cpus[3].intr_line, "apic: flat logical destination");
        apic_ack(cpus[1].intr_opaque); lapic_write(&ap, 1, 0xB0, 0);
        apic_ack(cpus[3].intr_opaque); lapic_write(&ap, 3, 0xB0, 0);
        // TPR masks a lower priority class until it drops.
        lapic_write(&ap, 1, 0x80, 0x60);
        lapic_write(&ap, 0, 0x310, 1u << 24);
        lapic_write(&ap, 0, 0x300, 0x00000055);
        CHECK(!cpus[1].intr_line, "apic: TPR 0x60 holds back vector 0x55");
        lapic_write(&ap, 1, 0x80, 0x40);
        CHECK(cpus[1].intr_line && apic_ack(cpus[1].intr_opaque) == 0x55, "apic: lowering TPR lets it through");
        lapic_write(&ap, 1, 0xB0, 0); lapic_write(&ap, 1, 0x80, 0);
        // Shorthand "all excluding self".
        lapic_write(&ap, 2, 0x300, 0x000C0060);
        CHECK(cpus[0].intr_line && cpus[1].intr_line && !cpus[2].intr_line && cpus[3].intr_line, "apic: all-excluding-self shorthand");
        for (int i = 0; i < 4; i++) if (i != 2) { apic_ack(cpus[i].intr_opaque); lapic_write(&ap, i, 0xB0, 0); }
        // INIT (assert) and SIPI go to the machine; INIT level de-assert does nothing.
        lapic_write(&ap, 0, 0x310, 3u << 24);
        lapic_write(&ap, 0, 0x300, 0x00008500);                    // INIT de-assert (level 0, trigger level)
        CHECK(g_init_cpu == -1, "apic: INIT de-assert is ignored");
        lapic_write(&ap, 0, 0x300, 0x00004500);
        lapic_write(&ap, 0, 0x300, 0x00004610);
        CHECK(g_init_cpu == 3 && g_sipi_cpu == 3 && g_sipi_vec == 0x10, "apic: INIT and SIPI reach the target CPU");
        // Timer: one-shot, divide by 4 (bus 1 GHz): 1000 counts = 4 us.
        lapic_write(&ap, 1, 0x3E0, 0x1);
        lapic_write(&ap, 1, 0x320, 0x00000070);
        lapic_write(&ap, 1, 0x380, 1000);
        g_now += 3000;
        apic_update(&ap);
        CHECK(!cpus[1].intr_line && lapic_read(&ap, 1, 0x390) == 250, "apic: timer counts down at bus clock / divider");
        g_now += 1500;
        apic_update(&ap);
        CHECK(cpus[1].intr_line && apic_ack(cpus[1].intr_opaque) == 0x70 && lapic_read(&ap, 1, 0x390) == 0, "apic: one-shot timer fires once");
        lapic_write(&ap, 1, 0xB0, 0);
        g_now += 10000; apic_update(&ap);
        CHECK(!cpus[1].intr_line, "apic: one-shot does not repeat");
        lapic_write(&ap, 1, 0x320, 0x00020071);                    // periodic
        lapic_write(&ap, 1, 0x380, 1000);
        int fired = 0;
        for (int k = 0; k < 10; k++) {
            g_now += 4000; apic_update(&ap);
            if (cpus[1].intr_line) { fired++; apic_ack(cpus[1].intr_opaque); lapic_write(&ap, 1, 0xB0, 0); }
        }
        CHECK(fired == 10, "apic: periodic timer fires every period");
        lapic_write(&ap, 1, 0x320, 0x00030071);                    // masked
        g_now += 8000; apic_update(&ap);
        CHECK(!cpus[1].intr_line, "apic: masked timer stays quiet");
        lapic_write(&ap, 1, 0x380, 0);
        // I/O APIC: pin 4 edge -> vector 0x34 on CPU 2 (physical); pin 10 level -> 0x3A, logical CPU 1.
        ioapic_write(&ap, 0x00, 0x18); ioapic_write(&ap, 0x10, 0x00000034);
        ioapic_write(&ap, 0x00, 0x19); ioapic_write(&ap, 0x10, 2u << 24);
        apic_set_irq(&ap, 4, true);
        CHECK(cpus[2].intr_line && apic_ack(cpus[2].intr_opaque) == 0x34, "ioapic: edge pin delivers its vector to the destination");
        lapic_write(&ap, 2, 0xB0, 0);
        apic_set_irq(&ap, 4, true);
        CHECK(!cpus[2].intr_line, "ioapic: no new edge, no new interrupt");
        apic_set_irq(&ap, 4, false);
        ioapic_write(&ap, 0x00, 0x24); ioapic_write(&ap, 0x10, 0x0000A83A);   // level, active low, logical
        ioapic_write(&ap, 0x00, 0x25); ioapic_write(&ap, 0x10, 0x02u << 24);
        apic_set_irq(&ap, 10, true);
        ioapic_write(&ap, 0x00, 0x24);
        CHECK(cpus[1].intr_line && (ioapic_read(&ap, 0x10) & (1u << 14)), "ioapic: level pin delivers and sets remote IRR");
        CHECK(apic_ack(cpus[1].intr_opaque) == 0x3A && (lapic_read(&ap, 1, 0x180 + 0x10) & (1u << 26)), "apic: level interrupt marked in TMR");
        CHECK(!cpus[1].intr_line, "ioapic: still asserted, but remote IRR holds it");
        lapic_write(&ap, 1, 0xB0, 0);                              // EOI -> remote IRR cleared -> delivered again
        CHECK(cpus[1].intr_line, "ioapic: EOI with the line still asserted delivers again");
        apic_ack(cpus[1].intr_opaque);
        apic_set_irq(&ap, 10, false);
        lapic_write(&ap, 1, 0xB0, 0);
        ioapic_write(&ap, 0x00, 0x24);
        CHECK(!cpus[1].intr_line && !(ioapic_read(&ap, 0x10) & (1u << 14)), "ioapic: deasserted, EOI leaves it idle");
        // 8259 through LINT0: masked -> not delivered; ExtINT -> CPU 0 sees it, INTA asks the PIC.
        apic_set_pic(&ap, true);
        CHECK(!cpus[0].intr_line, "apic: masked LINT0 blocks the 8259");
        lapic_write(&ap, 0, 0x350, 0x00008700);
        CHECK(cpus[0].intr_line && apic_ack(cpus[0].intr_opaque) == 0x20 && g_pic_acks == 1, "apic: LINT0 ExtINT passes the 8259 to CPU 0");
        apic_set_pic(&ap, false);
        CHECK(!cpus[0].intr_line && !cpus[1].intr_line, "apic: only CPU 0 sees the 8259");
        mem_destroy(bus);
    }

    io_destroy(io);
    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
