#include "machine.h"
#include "cpu.h"
#include "decode.h"
#include "io.h"
#include "jit.h"
#include "mem.h"
#include "system.h"
#include "dev/cmos.h"
#include "dev/disk.h"
#include "dev/fwcfg.h"
#include "dev/g400.h"
#include "dev/es1370.h"
#include "dev/dma.h"
#include "dev/sb16.h"
#include "dev/apic.h"
#include "dev/ide.h"
#include "dev/pci.h"
#include "dev/pic.h"
#include "dev/pit.h"
#include "dev/ps2.h"
#include "dev/ne2k.h"
#include "dev/rtl8139.h"
#include "dev/vga.h"
#include "net.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JIT_CODE_BYTES (64u << 20)
#define BLOCKS_PER_SLICE 4000

struct machine {
    int        frame_src;            // card shown by the last machine_frame: 0 VGA, 1 G400
    uint64_t   idle_ns;              // host time spent sleeping while the guest CPU was halted
    machine_config_t cfg;
    mem_bus_t *bus;
    io_bus_t  *io;
    jit_t     *jit;
    uint8_t   *ram;
    uint32_t   ram_size;
    uint8_t   *bios;            // ROM image, at least 128 KB (data at the end)
    uint32_t   bios_size;
    cpu_t      cpus[MAX_CPUS];
    int        n_cpus;
    int        stop_cpu;              // CPU whose instruction stopped the machine
    apic_sys_t apic;                  // with n_cpus > 1
    bool       have_apic;
    bool       wait_sipi[MAX_CPUS];   // AP after power-on or INIT: waits for a start-up IPI

    pic_t      pic;
    pit_t      pit;
    cmos_t     cmos;
    ps2_t      ps2;
    pci_bus_t  pci;
    fwcfg_t    fw;
    vga_t      vga;
    pci_dev_t  i440fx, piix3;
    ide_t      ide;
    disk_t    *disks[4];
    rtl8139_t  nic;
    ne2k_t     ne2k;
    bool       have_nic;        // at least one of the two
    bool       have_rtl, have_ne2k;
    int        rtl_slot, ne2k_slot;
    bool       rtl_level, ne2k_level;   // each NIC's INTA# before the command register's disable bit
    g400_t     g400;
    bool       have_g400, g400_level;
    es1370_t   es1370;
    dma_t      dma;
    sb16_t     sb16;
    bool       have_sb16;
    bool       have_es1370, es1370_level;
    uint16_t   pci_irqs;        // ISA IRQs currently driven high by PCI INTx (via PIIX3 PIRQ routing)
    uint64_t   net_polled_ns;
    // Frames from the network the NIC could not take yet (receiver busy, ring full).
    struct { uint16_t len; uint8_t data[1518]; } netq[128];
    int        netq_head, netq_count;
    uint64_t   netq_dropped;

    uint8_t    port92, post_code;
    bool       reset_pending;
    bool       stopped;
    char       stop_reason[256];
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static uint8_t *read_file(const char *path, uint32_t *len, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errlen, "cannot open %s", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (16 << 20)) { fclose(f); snprintf(err, errlen, "%s: bad size", path); return NULL; }
    uint8_t *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf); fclose(f); snprintf(err, errlen, "cannot read %s", path); return NULL;
    }
    fclose(f);
    *len = (uint32_t)n;
    return buf;
}

static void cpu_init_ipi(void *opaque, int i);
static void cpu_sipi(void *opaque, int i, uint8_t vector);

static void set_irq(void *opaque, int irq, int level)
{
    machine_t *m = opaque;
    pic_set_irq(&m->pic, irq, level);
    if (m->have_apic) {                          // ISA IRQ n -> I/O APIC pin n; the PIT also on pin 2
        apic_set_irq(&m->apic, irq, level != 0);
        if (irq == 0) apic_set_irq(&m->apic, 2, level != 0);
    }
    static int trace = -1;
    if (trace < 0) { const char *e = getenv("BEEMU_TRACE_IRQ"); trace = e ? (int)strtol(e, NULL, 10) : 0; }
    if (trace && irq == trace)                     // debugging aid: BEEMU_TRACE_IRQ=<irq>
        fprintf(stderr, "[irq %d=%d] master irr %02X isr %02X imr %02X | slave irr %02X isr %02X imr %02X | IF %d intr %d\n",
                irq, level, m->pic.c[0].irr, m->pic.c[0].isr, m->pic.c[0].imr, m->pic.c[1].irr, m->pic.c[1].isr,
                m->pic.c[1].imr, (m->cpus[0].eflags & EFL_IF) != 0, m->cpus[0].intr_line);
}

/* ---- PCI INTx: device INTA# -> PIIX3 PIRQ A-D ((slot - 1) & 3) -> ISA IRQ (cfg 0x60-0x63) ---- */

#define G400_SLOT 5
#define ES1370_SLOT 6

static void pirq_update(machine_t *m)
{
    bool level[4] = { false };
    // INTA# of slot s goes to PIRQ (s - 1) & 3, as on the i440FX/PIIX3 (QEMU and SeaBIOS agree;
    // SeaBIOS writes the resulting IRQ to INTERRUPT_LINE and drivers trust it). Command bit 10
    // disables a device's INTx.
    if (m->have_rtl && m->rtl_level && !(m->nic.pci.cfg[0x05] & 0x04)) level[(m->rtl_slot - 1) & 3] = true;
    if (m->have_ne2k && m->ne2k_level && !(m->ne2k.pci.cfg[0x05] & 0x04)) level[(m->ne2k_slot - 1) & 3] = true;
    if (m->have_g400 && m->g400_level && !(m->g400.pci.cfg[0x05] & 0x04)) level[(G400_SLOT - 1) & 3] = true;
    if (m->have_es1370 && m->es1370_level && !(m->es1370.pci.cfg[0x05] & 0x04)) level[(ES1370_SLOT - 1) & 3] = true;
    uint16_t want = 0;
    for (int p = 0; p < 4; p++) {
        uint8_t route = m->piix3.cfg[0x60 + p];
        if (level[p] && !(route & 0x80) && (route & 0x0F) >= 3) want |= (uint16_t)(1u << (route & 0x0F));
    }
    uint16_t changed = want ^ m->pci_irqs;
    m->pci_irqs = want;
    for (int irq = 0; irq < 16; irq++)
        if (changed & (1u << irq)) set_irq(m, irq, (want >> irq) & 1);
}

static void rtl_intx(void *opaque, int level)
{
    machine_t *m = opaque;
    m->rtl_level = level != 0;
    pirq_update(m);
}

static void g400_intx(void *opaque, int level)
{
    machine_t *m = opaque;
    m->g400_level = level != 0;
    pirq_update(m);
}

static void ne2k_intx(void *opaque, int level)
{
    machine_t *m = opaque;
    m->ne2k_level = level != 0;
    pirq_update(m);
}

// PIIX3 config writes (PIRQ routing registers 0x60-0x63). The ISA bridge's opaque is the machine.
static void pci_routing_written(pci_dev_t *d, int off, int size)
{
    (void)off; (void)size;
    assert(d == &((machine_t *)d->opaque)->piix3);   // only the ISA bridge's opaque is the machine
    pirq_update(d->opaque);
}

// A NIC's config writes (command bit 10 = INTx disable). Its pci_dev's opaque is the NIC, not
// the machine (taking it for the machine once scribbled over the PIC from the stray pointer).
static void rtl_cfg_written(pci_dev_t *d, int off, int size)
{
    (void)off; (void)size;
    pirq_update(((rtl8139_t *)d->opaque)->intx_opaque);
}

static void es1370_intx(void *opaque, int level)
{
    machine_t *m = opaque;
    m->es1370_level = level != 0;
    pirq_update(m);
}

static void es1370_cfg_written(pci_dev_t *d, int off, int size)
{
    (void)off; (void)size;
    pirq_update(((es1370_t *)d->opaque)->intx_opaque);
}

static void g400_cfg_written(pci_dev_t *d, int off, int size)
{
    (void)off; (void)size;
    pirq_update(((g400_t *)d->opaque)->intx_opaque);
}

static void ne2k_cfg_written(pci_dev_t *d, int off, int size)
{
    (void)off; (void)size;
    pirq_update(((ne2k_t *)d->opaque)->intx_opaque);
}

/* ---- network frames ---- */

static void trace_frame(const char *dir, const uint8_t *f, size_t len)
{
    static int trace = -1;
    if (trace < 0) { const char *e = getenv("BEEMU_TRACE_NET"); trace = e && *e == '1'; }
    if (!trace || len < 14) return;
    int type = f[12] << 8 | f[13];
    char what[96] = "";
    if (type == 0x0800 && len >= 34) {                      // IPv4: addresses, protocol, ports
        const uint8_t *ip = f + 14;
        int ihl = (ip[0] & 15) * 4, proto = ip[9];
        const uint8_t *l4 = ip + ihl;
        int sp = 0, dp = 0;
        if ((proto == 6 || proto == 17) && len >= (size_t)(14 + ihl + 4)) { sp = l4[0] << 8 | l4[1]; dp = l4[2] << 8 | l4[3]; }
        snprintf(what, sizeof what, "%d.%d.%d.%d:%d -> %d.%d.%d.%d:%d %s", ip[12], ip[13], ip[14], ip[15], sp,
                 ip[16], ip[17], ip[18], ip[19], dp, proto == 6 ? "TCP" : proto == 17 ? "UDP" : proto == 1 ? "ICMP" : "IP");
    } else if (type == 0x0806) {
        snprintf(what, sizeof what, "ARP");
    }
    fprintf(stderr, "[net %-3s] %4zu bytes  type %04X  %s\n", dir, len, type, what);
}

static void nic_send(void *opaque, const uint8_t *frame, size_t len)
{
    machine_t *m = opaque;
    trace_frame("out", frame, len);
    if (m->cfg.net) m->cfg.net->send(m->cfg.net, frame, len);
}

static void net_deliver(void *opaque, const uint8_t *frame, size_t len)
{
    machine_t *m = opaque;
    int cap = (int)(sizeof m->netq / sizeof m->netq[0]);
    if (len > sizeof m->netq[0].data || m->netq_count == cap) { m->netq_dropped++; return; }
    int i = (m->netq_head + m->netq_count++) % cap;
    m->netq[i].len = (uint16_t)len;
    memcpy(m->netq[i].data, frame, len);
}

static void net_service(machine_t *m)
{
    uint64_t now = clock_host_ns();
    if (m->cfg.net && now - m->net_polled_ns >= 500000) {       // host sockets every 0.5 ms
        m->net_polled_ns = now;
        m->cfg.net->poll(m->cfg.net);
    }
    int cap = (int)(sizeof m->netq / sizeof m->netq[0]);
    while (m->netq_count && m->have_nic) {
        const uint8_t *f = m->netq[m->netq_head].data;
        size_t len = m->netq[m->netq_head].len;
        // Every card sees every frame (each filters by its own MAC). A frame waits while a card
        // whose receiver is on has no room; a stopped card takes and drops it.
        if (m->have_rtl && !rtl8139_can_receive(&m->nic, len)) break;
        if (m->have_ne2k && !ne2k_can_receive(&m->ne2k, len)) break;
        trace_frame("in", f, len);
        if (m->have_rtl) rtl8139_receive(&m->nic, f, len);
        if (m->have_ne2k) ne2k_receive(&m->ne2k, f, len);
        m->netq_head = (m->netq_head + 1) % cap;
        m->netq_count--;
    }
}

static void request_reset(void *opaque)
{
    machine_t *m = opaque;
    m->reset_pending = true;
    m->cpus[0].exit_request = true;
}

static void memory_changed(void *opaque) { jit_flush(((machine_t *)opaque)->jit); }

/* ------------------------------------------------------------------ */
/* i440FX PAM: what 0xC0000-0xFFFFF decodes to                          */
/* ------------------------------------------------------------------ */

// attr: bit 0 = reads from DRAM, bit 1 = writes to DRAM.
static void map_pam_range(machine_t *m, uint32_t base, uint32_t size, uint8_t attr)
{
    uint8_t *ram = m->ram + base;
    if (attr & 1) {
        if (attr & 2) mem_map_ram(m->bus, base, size, ram);
        else          mem_map_rom(m->bus, base, size, ram);           // read-only shadow
    } else if (attr & 2) {
        mem_map_ram(m->bus, base, size, ram);   // write-only shadow: approximated as read/write
    } else if (base >= 0xE0000) {
        mem_map_rom(m->bus, base, size, m->bios + m->bios_size - 0x20000 + (base - 0xE0000));
    } else {
        mem_unmap(m->bus, base, size);          // to the PCI bus: nothing there
    }
}

static void apply_pam(machine_t *m)
{
    const uint8_t *pam = &m->i440fx.cfg[0x59];
    map_pam_range(m, 0xF0000, 0x10000, (pam[0] >> 4) & 3);
    for (int i = 1; i <= 6; i++) {
        uint32_t base = 0xC0000 + (uint32_t)(i - 1) * 0x8000;
        map_pam_range(m, base, 0x4000, pam[i] & 3);
        map_pam_range(m, base + 0x4000, 0x4000, (pam[i] >> 4) & 3);
    }
    jit_flush(m->jit);
}

static void i440fx_written(pci_dev_t *d, int off, int size)
{
    if (off + size > 0x59 && off < 0x60) apply_pam(d->opaque);
}

static void chipset_setup(machine_t *m)
{
    pci_dev_t *h = &m->i440fx, *isa = &m->piix3;
    memset(h, 0, sizeof *h);
    memset(isa, 0, sizeof *isa);

    // 00:00.0 host bridge. Subsystem 1af4:1100 is what SeaBIOS's QEMU build looks for.
    pci_set_ids(h, 0x8086, 0x1237, 0x02, 0x060000, 0x1AF4, 0x1100);
    pci_set_wmask(h, 0x59, 1, 0xF0);
    pci_set_wmask(h, 0x5A, 6, 0xFF);
    pci_set_wmask(h, 0x72, 1, 0x78);          // SMRAM (stored, not decoded)
    h->written = i440fx_written;
    h->opaque = m;

    // 00:01.0 PIIX3 ISA bridge (multi-function: header type bit 7).
    pci_set_ids(isa, 0x8086, 0x7000, 0x00, 0x060100, 0x1AF4, 0x1100);
    isa->cfg[0x0E] = 0x80;
    for (int i = 0; i < 4; i++) isa->cfg[0x60 + i] = 0x80;   // PIRQ routing: disabled
    isa->written = pci_routing_written;
    isa->opaque = m;
    pci_set_wmask(isa, 0x60, 4, 0x8F);
    pci_set_wmask(isa, 0x69, 1, 0xFE);        // top of memory
    pci_set_wmask(isa, 0x70, 1, 0xEF);        // MBIRQ
    pci_set_wmask(isa, 0x76, 2, 0x8F);
    pci_set_wmask(isa, 0x80, 1, 0x7F);
    pci_set_wmask(isa, 0xA0, 0x20, 0xFF);     // power management / misc

    pci_add(&m->pci, 0, 0, h);
    pci_add(&m->pci, 1, 0, isa);
    pci_add(&m->pci, 1, 1, &m->ide.pci);       // 00:01.1 PIIX3 IDE (core/dev/ide.c)
    pci_add(&m->pci, 2, 0, &m->vga.pci);
    if (m->have_rtl) pci_add(&m->pci, m->rtl_slot, 0, &m->nic.pci);     // RTL8139
    if (m->have_ne2k) pci_add(&m->pci, m->ne2k_slot, 0, &m->ne2k.pci);  // RTL8029 (NE2000)
    if (m->have_g400) pci_add(&m->pci, G400_SLOT, 0, &m->g400.pci);     // Matrox G400 MAX
    if (m->have_es1370) pci_add(&m->pci, ES1370_SLOT, 0, &m->es1370.pci); // Ensoniq ES1370
}

/* ------------------------------------------------------------------ */
/* Small ports                                                         */
/* ------------------------------------------------------------------ */

static void debug_write(void *opaque, uint16_t port, uint32_t v, int size)
{
    (void)port; (void)size;
    machine_t *m = opaque;
    char ch = (char)v;
    if (m->cfg.debug_out) m->cfg.debug_out(m->cfg.debug_opaque, &ch, 1);
    else { fputc(ch, stdout); if (ch == '\n') fflush(stdout); }
}

static uint32_t debug_read(void *opaque, uint16_t port, int size)
{
    (void)opaque; (void)port; (void)size;
    return 0xE9;                                // QEMU's debugcon readback value
}

static uint32_t misc_read(void *opaque, uint16_t port, int size)
{
    (void)size;
    machine_t *m = opaque;
    if (port == 0x92) return m->port92;
    if (port == 0x80) return m->post_code;
    return 0xFF;
}

static void misc_write(void *opaque, uint16_t port, uint32_t v, int size)
{
    (void)size;
    machine_t *m = opaque;
    if (port == 0x80) { m->post_code = (uint8_t)v; return; }
    if (port == 0x92) {                         // fast A20 (A20 itself is not gated) and reset
        if ((v & 1) && !(m->port92 & 1)) request_reset(m);
        m->port92 = (uint8_t)(v & ~1u);
    }
}

/* ------------------------------------------------------------------ */
/* fw_cfg contents                                                     */
/* ------------------------------------------------------------------ */

static void fwcfg_setup(machine_t *m)
{
    fwcfg_t *f = &m->fw;
    fwcfg_add_u64(f, FW_CFG_RAM_SIZE, m->ram_size);
    fwcfg_add_u16(f, FW_CFG_NB_CPUS, (uint16_t)m->n_cpus);
    fwcfg_add_u16(f, FW_CFG_MAX_CPUS, (uint16_t)m->n_cpus);
    fwcfg_add_u16(f, FW_CFG_NOGRAPHIC, 0);
    fwcfg_add_u16(f, FW_CFG_BOOT_MENU, m->cfg.boot_menu ? 1 : 0);
    fwcfg_add_u64(f, FW_CFG_NUMA, 0);
    uint8_t uuid[16] = { 0 };
    fwcfg_add_bytes(f, FW_CFG_UUID, uuid, 16);

    // etc/e820: {u64 address, u64 length, u32 type} little-endian. All RAM in one
    // entry; SeaBIOS carves out the legacy holes itself.
    uint8_t e[20] = { 0 };
    uint64_t len = m->ram_size;
    for (int i = 0; i < 8; i++) e[8 + i] = (uint8_t)(len >> (8 * i));
    e[16] = 1;
    fwcfg_add_file(f, "etc/e820", e, sizeof e);
}

static void cmos_setup(machine_t *m)
{
    cmos_t *c = &m->cmos;
    uint32_t kb = m->ram_size / 1024;
    uint32_t ext = kb > 1024 ? kb - 1024 : 0;
    if (ext > 0xFFFF) ext = 0xFFFF;
    cmos_set(c, 0x10, 0x00);                    // no floppies
    cmos_set(c, 0x12, 0x00);                    // no legacy hard disk types
    cmos_set(c, 0x14, 0x06);                    // equipment: VGA, math coprocessor bit
    cmos_set(c, 0x15, 0x80); cmos_set(c, 0x16, 0x02);           // 640 KB base memory
    cmos_set(c, 0x17, (uint8_t)ext); cmos_set(c, 0x18, (uint8_t)(ext >> 8));
    cmos_set(c, 0x30, (uint8_t)ext); cmos_set(c, 0x31, (uint8_t)(ext >> 8));
    uint32_t above16 = m->ram_size > (16u << 20) ? (m->ram_size - (16u << 20)) >> 16 : 0;
    if (above16 > 0xFFFF) above16 = 0xFFFF;
    cmos_set(c, 0x34, (uint8_t)above16); cmos_set(c, 0x35, (uint8_t)(above16 >> 8));
    // QEMU's boot-order encoding: 0x3D = 1st | 2nd << 4, 0x38 = 3rd << 4 | 1 (skip
    // the floppy signature check). Devices: 1 floppy, 2 hard disk, 3 CD-ROM.
    if (m->cfg.boot == 'd') { cmos_set(c, 0x3D, 0x23); cmos_set(c, 0x38, 0x11); }   // CD-ROM, hard disk, floppy
    else                    { cmos_set(c, 0x3D, 0x32); cmos_set(c, 0x38, 0x11); }   // hard disk, CD-ROM, floppy
    cmos_set(c, 0x5F, (uint8_t)(m->n_cpus - 1));
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

machine_t *machine_create(const machine_config_t *cfg, char *err, size_t errlen)
{
    machine_t *m = calloc(1, sizeof *m);
    if (!m) { snprintf(err, errlen, "out of memory"); return NULL; }
    m->cfg = *cfg;
    if (!m->cfg.ram_mb) m->cfg.ram_mb = 64;
    if (m->cfg.ram_mb < 2 || m->cfg.ram_mb > 3072) { snprintf(err, errlen, "RAM must be 2..3072 MB"); goto fail; }
    m->n_cpus = m->cfg.n_cpus > 0 ? m->cfg.n_cpus : 1;
    if (m->n_cpus > MAX_CPUS) m->n_cpus = MAX_CPUS;

    uint32_t bios_len;
    uint8_t *bios = read_file(cfg->bios_path, &bios_len, err, errlen);
    if (!bios) goto fail;
    if (bios_len & 0xFFF) { free(bios); snprintf(err, errlen, "BIOS size must be a multiple of 4 KB"); goto fail; }
    m->bios_size = bios_len < 0x20000 ? 0x20000 : bios_len;
    m->bios = mem_alloc_host(m->bios_size);
    if (!m->bios) { free(bios); snprintf(err, errlen, "out of memory"); goto fail; }
    memset(m->bios, 0xFF, m->bios_size);
    memcpy(m->bios + m->bios_size - bios_len, bios, bios_len);
    free(bios);

    uint8_t *vrom = NULL;
    uint32_t vrom_len = 0;
    if (cfg->vgabios_path && !(vrom = read_file(cfg->vgabios_path, &vrom_len, err, errlen))) goto fail;

    m->ram_size = m->cfg.ram_mb << 20;
    m->ram = mem_alloc_host(m->ram_size);
    m->bus = mem_create();
    m->io = io_create();
    if (!m->ram || !m->bus || !m->io) { free(vrom); snprintf(err, errlen, "out of memory"); goto fail; }
    m->jit = jit_create(m->bus, JIT_CODE_BYTES);
    if (m->jit && m->n_cpus > 1) jit_set_smp(m->jit, true);
    if (!m->jit) { free(vrom); snprintf(err, errlen, "cannot allocate JIT code buffer"); goto fail; }

    mem_map_ram(m->bus, 0, 0xA0000, m->ram);
    mem_map_ram(m->bus, 0x100000, m->ram_size - 0x100000, m->ram + 0x100000);
    mem_map_rom(m->bus, (uint32_t)(0x100000000ull - m->bios_size), m->bios_size, m->bios);

    bool vga_ok = vga_init(&m->vga, m->bus, vrom, vrom_len, clock_host, NULL, memory_changed, m);
    free(vrom);
    if (!vga_ok) { snprintf(err, errlen, "cannot allocate VRAM"); goto fail; }

    if (!ide_init(&m->ide, m->bus, m->io, set_irq, m)) { snprintf(err, errlen, "out of memory"); goto fail; }
    for (int i = 0; i < 4; i++) {
        const char *path = m->cfg.drives[i].path;
        if (!path) continue;
        bool cd = m->cfg.drives[i].cdrom;
        m->disks[i] = disk_open(path, m->cfg.drives_writable && !cd, err, errlen);
        if (!m->disks[i]) goto fail;
        if (cd && disk_size(m->disks[i]) % 2048) { snprintf(err, errlen, "%s: not a multiple of 2048 bytes", path); goto fail; }
        if (!cd && disk_size(m->disks[i]) % 512) { snprintf(err, errlen, "%s: not a multiple of 512 bytes", path); goto fail; }
        if (!ide_attach(&m->ide, i, cd ? IDE_CD : IDE_HD, m->disks[i])) { snprintf(err, errlen, "out of memory"); goto fail; }
    }

    if (!m->cfg.no_nic) {
        static const uint8_t DEFAULT_MAC[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
        static const uint8_t ZERO[6];
        const uint8_t *mac = memcmp(m->cfg.mac, ZERO, 6) ? m->cfg.mac : DEFAULT_MAC;
        m->have_rtl = m->cfg.nic_model != MACHINE_NIC_NE2K;
        m->have_ne2k = m->cfg.nic_model != MACHINE_NIC_RTL8139;
        m->rtl_slot = 3;
        m->ne2k_slot = m->have_rtl ? 4 : 3;
        if (m->have_rtl) {
            rtl8139_init(&m->nic, m->bus, m->io, mac, clock_host, NULL);
            m->nic.set_intx = rtl_intx;
            m->nic.intx_opaque = m;
            m->nic.send = nic_send;
            m->nic.send_opaque = m;
            m->nic.pci.written = rtl_cfg_written;       // the INTx disable bit
        }
        if (m->have_ne2k) {
            uint8_t mac2[6];
            memcpy(mac2, mac, 6);
            if (m->have_rtl) mac2[5]++;                  // two cards, two addresses (two DHCP leases)
            ne2k_init(&m->ne2k, m->io, mac2);
            m->ne2k.set_intx = ne2k_intx;
            m->ne2k.intx_opaque = m;
            m->ne2k.send = nic_send;
            m->ne2k.send_opaque = m;
            m->ne2k.pci.written = ne2k_cfg_written;
        }
        m->have_nic = true;
    }
    if (m->cfg.video == MACHINE_VIDEO_G400) {
        if (!g400_init(&m->g400, m->bus, clock_host, NULL)) { snprintf(err, errlen, "cannot allocate G400 VRAM"); goto fail; }
        m->g400.set_intx = g400_intx;
        m->g400.intx_opaque = m;
        m->g400.pci.written = g400_cfg_written;
        m->have_g400 = true;
    }
    if (m->cfg.sound == MACHINE_SOUND_ES1370) {
        es1370_init(&m->es1370, m->bus, m->io, clock_host, NULL);
        m->es1370.set_intx = es1370_intx;
        m->es1370.intx_opaque = m;
        m->es1370.pci.written = es1370_cfg_written;
        m->es1370.out = m->cfg.sound_out;
        m->es1370.out_opaque = m->cfg.sound_opaque;
        m->have_es1370 = true;
    }
    if (m->cfg.sound == MACHINE_SOUND_SB16) {
        sb16_init(&m->sb16, m->io, &m->dma, clock_host, NULL);
        m->sb16.set_irq = set_irq;
        m->sb16.irq_opaque = m;
        m->sb16.out_fn = m->cfg.sound_out;
        m->sb16.out_opaque = m->cfg.sound_opaque;
        m->have_sb16 = true;
    }
    if (m->cfg.net) {
        m->cfg.net->deliver = net_deliver;
        m->cfg.net->deliver_opaque = m;
    }

    for (int i = 0; i < m->n_cpus; i++) {
        cpu_init(&m->cpus[i], i);
        m->cpus[i].mem = m->bus;
        m->cpus[i].io = m->io;
    }
    m->cpus[0].intr_ack = pic_ack;
    m->cpus[0].intr_opaque = &m->pic;
    if (m->n_cpus > 1) {
        for (int i = 0; i < m->n_cpus; i++) m->cpus[i].has_apic = true;
        apic_init(&m->apic, m->n_cpus, m->cpus, m->bus, clock_host, NULL);
        m->apic.pic_ack = pic_ack;
        m->apic.pic_opaque = &m->pic;
        m->apic.init_cpu = cpu_init_ipi;
        m->apic.start_cpu = cpu_sipi;
        m->apic.machine = m;
        m->have_apic = true;
    }

    pci_bus_init(&m->pci);
    m->pci.reset = request_reset;
    m->pci.reset_opaque = m;
    fwcfg_init(&m->fw);
    fwcfg_setup(m);
    cmos_init(&m->cmos, clock_host, NULL, set_irq, m);
    cmos_setup(m);

    pic_register(&m->pic, m->io);
    pit_register(&m->pit, m->io);
    dma_init(&m->dma, m->bus);
    dma_register(&m->dma, m->io);
    cmos_register(&m->cmos, m->io);
    ps2_register(&m->ps2, m->io);
    pci_bus_register(&m->pci, m->io);
    fwcfg_register(&m->fw, m->io);
    vga_register(&m->vga, m->io);
    ide_register(&m->ide);
    io_register(m->io, 0x402, 1, debug_read, debug_write, m);
    io_register(m->io, 0x80, 1, misc_read, misc_write, m);
    io_register(m->io, 0x92, 1, misc_read, misc_write, m);

    // Debugging aids (see CLAUDE.md): BEEMU_STEP=1, BEEMU_TRACE=lo-hi (hex linear addresses).
    const char *e;
    if ((e = getenv("BEEMU_STEP")) && *e == '1') jit_set_single_step(m->jit, true);
    if ((e = getenv("BEEMU_TRACE"))) {
        char *dash;
        uint32_t lo = (uint32_t)strtoul(e, &dash, 16);
        uint32_t hi = *dash == '-' ? (uint32_t)strtoul(dash + 1, NULL, 16) : lo;
        jit_set_trace(m->jit, lo, hi);
    }

    machine_reset(m);
    return m;

fail:
    machine_destroy(m);
    return NULL;
}

void machine_reset(machine_t *m)
{
    for (int i = 0; i < m->n_cpus; i++) cpu_reset(&m->cpus[i]);
    pic_init(&m->pic, &m->cpus[0]);
    if (m->have_apic) {
        m->pic.out = (void (*)(void *, bool))apic_set_pic;
        m->pic.out_opaque = &m->apic;
        apic_reset(&m->apic);
        for (int i = 0; i < m->n_cpus; i++) m->wait_sipi[i] = i > 0;
    }
    pit_init(&m->pit, clock_host, NULL, set_irq, m);
    ps2_init(&m->ps2, set_irq, m, request_reset, m);
    // PCI functions come back with BARs unassigned and the PAM in its reset state.
    for (int bar = 0; bar < 7; bar++)
        if (m->vga.pci.bar_on[bar]) { m->vga.pci.map(&m->vga.pci, bar, m->vga.pci.bar_addr[bar], false); m->vga.pci.bar_on[bar] = false; }
    pci_cfg_write(&m->vga.pci, 0x04, 0, 2);
    pci_cfg_write(&m->vga.pci, 0x10, 0, 4);
    pci_cfg_write(&m->vga.pci, 0x18, 0, 4);
    pci_cfg_write(&m->vga.pci, 0x30, 0, 4);
    ide_reset(&m->ide);
    if (m->have_rtl) rtl8139_reset(&m->nic);
    if (m->have_ne2k) ne2k_reset(&m->ne2k);
    if (m->have_g400) g400_reset(&m->g400);
    if (m->have_es1370) es1370_reset(&m->es1370);
    dma_reset(&m->dma);
    if (m->have_sb16) sb16_reset(&m->sb16);
    m->rtl_level = m->ne2k_level = m->g400_level = m->es1370_level = false;
    m->pci_irqs = 0;
    m->netq_count = 0;
    chipset_setup(m);
    apply_pam(m);
    m->port92 = 0;
    m->reset_pending = false;
    jit_flush(m->jit);
}

void machine_destroy(machine_t *m)
{
    if (!m) return;
    if (m->jit) jit_destroy(m->jit);
    if (m->vga.vram) vga_free(&m->vga);
    if (m->have_g400) g400_free(&m->g400);
    if (m->ide.dma_tmp) ide_free(&m->ide);
    for (int i = 0; i < 4; i++) disk_close(m->disks[i]);
    fwcfg_free(&m->fw);
    if (m->io) io_destroy(m->io);
    if (m->bus) mem_destroy(m->bus);
    mem_free_host(m->ram);
    mem_free_host(m->bios);
    free(m);
}

/* ------------------------------------------------------------------ */
/* Running                                                             */
/* ------------------------------------------------------------------ */

static void stop(machine_t *m, const char *why)
{
    cpu_t *c = &m->cpus[m->stop_cpu];
    uint32_t lin = c->seg[S_CS].base + c->eip, pa;
    uint8_t bytes[X86_MAX_INSN_LEN];
    if (cpu_peek_phys(c, lin, &pa)) mem_read_block(m->bus, pa, bytes, sizeof bytes);
    else memset(bytes, 0xFF, sizeof bytes);
    insn_t in;
    char text[96] = "?";
    if (decode_insn(bytes, sizeof bytes, cpu_code32(c), &in)) insn_format(&in, c->eip, text, sizeof text);
    snprintf(m->stop_reason, sizeof m->stop_reason,
             "%s at %04X:%08X (linear %08X, %s, CPL %d): %s [%02X %02X %02X %02X]",
             why, c->seg[S_CS].selector, c->eip, lin, cpu_pmode(c) ? "protected" : "real",
             c->cpl, text, bytes[0], bytes[1], bytes[2], bytes[3]);
    m->stopped = true;
}

// Advance timers; returns ns until the next device event.
// INIT: the AP goes back to its reset state and waits for a start-up IPI.
static void cpu_init_ipi(void *opaque, int i)
{
    machine_t *m = opaque;
    cpu_reset(&m->cpus[i]);
    apic_reset_cpu(&m->apic, i);
    m->wait_sipi[i] = true;
}

// SIPI: a waiting AP starts in real mode at vector * 4 KB (CS = vector << 8, IP = 0).
static void cpu_sipi(void *opaque, int i, uint8_t vector)
{
    machine_t *m = opaque;
    if (!m->wait_sipi[i]) return;
    cpu_t *c = &m->cpus[i];
    c->seg[S_CS].selector = (uint16_t)(vector << 8);
    c->seg[S_CS].base = (uint32_t)vector << 12;
    c->eip = 0;
    c->halted = false;
    m->wait_sipi[i] = false;
}

static uint64_t update_devices(machine_t *m)
{
    uint64_t a = pit_update(&m->pit), b = cmos_update(&m->cmos);
    if (m->have_g400) g400_update(&m->g400);
    if (m->have_es1370) { uint64_t e = es1370_update(&m->es1370); if (e < b) b = e; }
    if (m->have_sb16) { uint64_t e = sb16_update(&m->sb16); if (e < b) b = e; }
    if (m->have_apic) { uint64_t e = apic_update(&m->apic); if (e < b) b = e; }
    if (m->have_nic) {
        if (m->have_rtl) rtl8139_update(&m->nic);
        net_service(m);
        if (m->cfg.net && a > 1000000) a = 1000000;    // a halted guest still hears the network within 1 ms
    }
    return a < b ? a : b;
}

// The CPUs take turns on this thread, one time slice each, switching only between blocks (so
// LOCKed instructions stay atomic). PAUSE ends a slice when there is more than one CPU, so a
// spinning CPU hands over to the one holding the lock. The thread sleeps only when every CPU
// is halted (or waiting for a start-up IPI) with nothing to deliver.
bool machine_run(machine_t *m, uint64_t ns)
{
    uint64_t end = clock_host_ns() + ns;
    while (!m->stopped) {
        uint64_t wait = update_devices(m);
        if (m->reset_pending) machine_reset(m);
        uint64_t now = clock_host_ns();
        if (now >= end) break;
        bool ran = false;
        for (int i = 0; i < m->n_cpus && !m->stopped; i++) {
            cpu_t *c = &m->cpus[i];
            if (m->wait_sipi[i]) continue;
            if (c->halted && !(c->intr_line && (c->eflags & EFL_IF))) continue;
            ran = true;
            if (m->have_apic) apic_set_current(&m->apic, i);
            jit_exit_t x = jit_run(m->jit, c, m->n_cpus > 1 ? BLOCKS_PER_SLICE / 4 : BLOCKS_PER_SLICE);
            switch (x) {
            case JIT_EXIT_LIMIT: case JIT_EXIT_HALT: case JIT_EXIT_REQUEST: case JIT_EXIT_OK:
                break;
            case JIT_EXIT_SHUTDOWN:
                fprintf(stderr, "beemu: triple fault on CPU %d, resetting\n", i);
                machine_reset(m);
                break;
            case JIT_EXIT_UNSUPPORTED: m->stop_cpu = i; stop(m, "unsupported instruction"); break;
            case JIT_EXIT_INVALID:     m->stop_cpu = i; stop(m, "invalid instruction"); break;
            default:                   m->stop_cpu = i; stop(m, "CPU stopped"); break;
            }
            if (m->n_cpus > 1 && i + 1 < m->n_cpus) update_devices(m);
        }
        if (!ran) {
            uint64_t sl = wait;
            if (sl > end - now) sl = end - now;
            if (sl > 2000000) sl = 2000000;
            struct timespec ts = { 0, (long)sl };
            nanosleep(&ts, NULL);
            m->idle_ns += clock_host_ns() - now;
        }
    }
    return !m->stopped;
}

void machine_read(machine_t *m, uint32_t addr, void *buf, size_t len) { mem_read_block(m->bus, addr, buf, len); }

size_t machine_read_lin(machine_t *m, uint32_t lin, void *buf, size_t len)
{
    uint8_t *out = buf;
    size_t done = 0;
    while (done < len) {
        uint32_t a = lin + (uint32_t)done, pa;
        size_t chunk = 0x1000u - (a & 0xFFFu);
        if (chunk > len - done) chunk = len - done;
        if (!cpu_peek_phys(&m->cpus[0], a, &pa)) break;      // not mapped in the current address space
        mem_read_block(m->bus, pa, out + done, chunk);
        done += chunk;
    }
    return done;
}

void machine_dump(machine_t *m, char *out, size_t n)
{
    cpu_t *c = &m->cpus[0];
    uint32_t lin = c->seg[S_CS].base + c->eip, pa;
    uint8_t bytes[X86_MAX_INSN_LEN];
    if (cpu_peek_phys(c, lin, &pa)) mem_read_block(m->bus, pa, bytes, sizeof bytes);
    else memset(bytes, 0xFF, sizeof bytes);
    insn_t in;
    char text[96] = "?";
    if (decode_insn(bytes, sizeof bytes, cpu_code32(c), &in)) insn_format(&in, c->eip, text, sizeof text);
    uint32_t f = c->eflags;
    snprintf(out, n,
             "CS:EIP %04X:%08X (lin %08X) %s%s CPL %d halted %d IF %d intr %d  %s\n"
             "EAX %08X EBX %08X ECX %08X EDX %08X ESI %08X EDI %08X EBP %08X ESP %08X\n"
             "DS %04X ES %04X SS %04X(base %08X) FS %04X GS %04X CR0 %08X  post %02X\n",
             c->seg[S_CS].selector, c->eip, lin, cpu_pmode(c) ? "pm" : "real", cpu_code32(c) ? "32" : "16",
             c->cpl, c->halted, (f & EFL_IF) != 0, c->intr_line, text,
             c->gpr[0], c->gpr[3], c->gpr[1], c->gpr[2], c->gpr[6], c->gpr[7], c->gpr[5], c->gpr[4],
             c->seg[S_DS].selector, c->seg[S_ES].selector, c->seg[S_SS].selector, c->seg[S_SS].base,
             c->seg[S_FS].selector, c->seg[S_GS].selector, c->cr0, m->post_code);
    size_t used = strlen(out);
    for (int i = 1; i < m->n_cpus && used < n; i++) {             // the other CPUs, one line each
        cpu_t *a = &m->cpus[i];
        uint32_t al = a->seg[S_CS].base + a->eip, apa;
        uint8_t ab[X86_MAX_INSN_LEN];
        if (cpu_peek_phys(a, al, &apa)) mem_read_block(m->bus, apa, ab, sizeof ab); else memset(ab, 0xFF, sizeof ab);
        insn_t ai;
        char at[96] = "?";
        if (decode_insn(ab, sizeof ab, cpu_code32(a), &ai)) insn_format(&ai, a->eip, at, sizeof at);
        snprintf(out + used, n - used, "CPU %d: %s%04X:%08X CPL %d halted %d IF %d intr %d  %s\n", i,
                 m->wait_sipi[i] ? "(waiting for SIPI) " : "", a->seg[S_CS].selector, a->eip, a->cpl, a->halted,
                 (a->eflags & EFL_IF) != 0, a->intr_line, at);
        used = strlen(out);
    }
    uint32_t sp = c->seg[S_SS].base + (cpu_stack32(c) ? c->gpr[R_ESP] : (c->gpr[R_ESP] & 0xFFFF));
    used += (size_t)snprintf(out + used, used < n ? n - used : 0, "stack:");
    for (int i = 0; i < 12 && used < n; i++) {
        uint32_t pa;
        if (cpu_peek_phys(c, sp + 4u * (uint32_t)i, &pa))
            used += (size_t)snprintf(out + used, n - used, " %08X", mem_read(m->bus, pa, 4));
        else
            used += (size_t)snprintf(out + used, n - used, " --------");
    }
    if (used < n)
        used += (size_t)snprintf(out + used, n - used,
            "\nPIC master irr %02X isr %02X imr %02X base %02X | slave irr %02X isr %02X imr %02X base %02X"
            "\nPIT ch0 mode %d count %u armed %d | BDA ticks %08X",
            m->pic.c[0].irr, m->pic.c[0].isr, m->pic.c[0].imr, m->pic.c[0].base,
            m->pic.c[1].irr, m->pic.c[1].isr, m->pic.c[1].imr, m->pic.c[1].base,
            m->pit.ch[0].mode, m->pit.ch[0].count, m->pit.ch[0].armed, mem_read(m->bus, 0x46C, 4));
    if (used < n) used += (size_t)snprintf(out + used, n - used, "\nrecent blocks (distinct, newest last):");
    uint32_t rec[JIT_TRACE_LEN];
    int k = jit_recent_blocks(m->jit, rec, JIT_TRACE_LEN), shown = 0;
    for (int i = 0; i < k && used < n; i++) {
        bool seen = false;
        for (int q = i + 1; q < k && !seen; q++) seen = rec[q] == rec[i];
        if (seen) continue;
        used += (size_t)snprintf(out + used, n - used, "%s%08X", (shown++ % 8) ? " " : "\n  ", rec[i]);
    }
    if (used < n) snprintf(out + used, n - used, "\n");
}

const char *machine_stop_reason(const machine_t *m) { return m->stopped ? m->stop_reason : NULL; }

uint64_t machine_idle_ns(const machine_t *m) { return m->idle_ns; }

void machine_frame_invalidate(machine_t *m)
{
    fbrows_invalidate(&m->vga.rows);
    if (m->have_g400) { fbrows_invalidate(&m->g400.rows); m->g400.cur_key = 0; }
}

const uint32_t *machine_frame_rows(machine_t *m, int *w, int *h, int *y0, int *y1)
{
    const uint32_t *fb = NULL;
    int src = 0;
    if (m->have_g400 && g400_active(&m->g400) && (fb = g400_render(&m->g400, w, h))) {
        src = 1;
        *y0 = m->g400.dirty_y0; *y1 = m->g400.dirty_y1;
    } else {
        fb = vga_render(&m->vga, w, h);
        *y0 = m->vga.dirty_y0; *y1 = m->vga.dirty_y1;
    }
    if (src != m->frame_src) { *y0 = 0; *y1 = *h; m->frame_src = src; }   // the other card's picture was up
    return fb;
}

const uint32_t *machine_frame(machine_t *m, int *w, int *h)
{
    int y0, y1;
    return machine_frame_rows(m, w, h, &y0, &y1);
}
int  machine_text(machine_t *m, char *out, int out_len) { return vga_text(&m->vga, out, out_len); }
void machine_key(machine_t *m, int hid, bool down) { ps2_key(&m->ps2, hid, down); }
void machine_mouse(machine_t *m, int dx, int dy, uint8_t buttons) { ps2_mouse(&m->ps2, dx, dy, buttons); }
