// The emulated PC: i440FX + PIIX3 chipset, RAM, SeaBIOS ROM, devices, CPUs.
// No SDL here: the frontend feeds input and presents machine_frame().
//
// Memory map:
//   0x00000000-0x0009FFFF  RAM
//   0x000A0000-0x000BFFFF  VGA window
//   0x000C0000-0x000FFFFF  shadow RAM / ROM, under i440FX PAM control
//                          (E0000-FFFFF alias the last 128 KB of the BIOS when not shadowed)
//   0x00100000-RAM top     RAM
//   PCI BARs               assigned by SeaBIOS (VGA LFB, VGA ROM)
//   4 GB - BIOS size       BIOS ROM
#ifndef MACHINE_H
#define MACHINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct machine machine_t;

enum { MACHINE_NIC_BOTH = 0, MACHINE_NIC_RTL8139 = 1, MACHINE_NIC_NE2K = 2 };
enum { MACHINE_VIDEO_STD = 0, MACHINE_VIDEO_G400 = 1 };
enum { MACHINE_SOUND_ES1370 = 0, MACHINE_SOUND_NONE = 1, MACHINE_SOUND_SB16 = 2 };

typedef struct {
    uint32_t    ram_mb;          // default 64
    int         n_cpus;          // 1 (default) .. 8. With 2 or more: local APICs, an I/O APIC, and the
                                 // APs waiting for INIT/SIPI (SeaBIOS counts them and builds the MP table)
    const char *bios_path;       // SeaBIOS image (QEMU's bios.bin)
    const char *vgabios_path;    // SeaVGABIOS for the VGA ROM BAR (vgabios-stdvga.bin); NULL = none
    bool        boot_menu;       // SeaBIOS shows "Press ESC for boot menu"
    // IDE drives: 0 primary master, 1 primary slave, 2 secondary master, 3 secondary slave.
    // Raw images only. Unless drives_writable, guest writes go to a RAM overlay and are lost on exit.
    struct { const char *path; bool cdrom; } drives[4];
    bool        drives_writable;
    char        boot;            // 'c' hard disk first (default), 'd' CD-ROM first
    // Network, unless no_nic. MACHINE_NIC_BOTH (the default): an RTL8139 at 00:03.0 and an
    // NE2000-compatible RTL8029 (QEMU's ne2k_pci, BeOS's etherpci driver) at 00:04.0, so a BeOS
    // install configured for either card finds it (the other one sits idle; its MAC is one higher).
    // MACHINE_NIC_RTL8139 / MACHINE_NIC_NE2K: just that card, at 00:03.0. Frames go to `net` (a backend the
    // caller creates and destroys, see core/net.h); NULL = no cable: the link still looks up
    // to the guest and sent frames are dropped. mac all zero = 52:54:00:12:34:56.
    bool        no_nic;
    int         nic_model;
    struct net_backend *net;
    uint8_t     mac[6];
    // MACHINE_VIDEO_G400 adds a Matrox G400 MAX at 00:05.0 next to the standard VGA card, which
    // stays the boot display; the G400 is shown once its driver switches it on (core/dev/g400.h).
    int         video;
    // Sound card: MACHINE_SOUND_ES1370 (the default) is an Ensoniq AudioPCI at 00:06.0 (BeOS's
    // es137x driver, R5 and Dano); MACHINE_SOUND_SB16 is an ISA Sound Blaster 16 PnP (R5's awe64
    // driver; BeOS finds it by ISA Plug and Play and assigns its resources). The output, 16-bit stereo at SOUND_RATE (core/sound.h), goes
    // to sound_out as host time passes; NULL = computed and dropped.
    int         sound;
    void      (*sound_out)(void *opaque, const int16_t *frames, int n);
    void       *sound_opaque;
    // Port 0x402 debug console output. NULL = stdout.
    void      (*debug_out)(void *opaque, const char *s, size_t n);
    void       *debug_opaque;
} machine_config_t;

machine_t *machine_create(const machine_config_t *cfg, char *err, size_t errlen);
void       machine_destroy(machine_t *m);
void       machine_reset(machine_t *m);

// Run the guest for about ns of host time. Returns false once the guest has
// stopped (unsupported instruction, ...); machine_stop_reason() says why.
bool        machine_run(machine_t *m, uint64_t ns);
const char *machine_stop_reason(const machine_t *m);

const uint32_t *machine_frame(machine_t *m, int *w, int *h);   // XRGB8888
// The same, plus the rows [*y0, *y1) that changed since the previous call (empty when y0 >= y1).
const uint32_t *machine_frame_rows(machine_t *m, int *w, int *h, int *y0, int *y1);
void machine_frame_invalidate(machine_t *m);   // the next frame converts every row again
uint64_t machine_idle_ns(const machine_t *m);   // host time spent waiting while the guest CPU was halted
int  machine_text(machine_t *m, char *out, int out_len);       // text-mode screen as ASCII
// Read guest physical memory (diagnostics).
void machine_read(machine_t *m, uint32_t addr, void *buf, size_t len);
// Same through CPU 0's current page tables; returns how many bytes were mapped (stops at a hole).
size_t machine_read_lin(machine_t *m, uint32_t lin, void *buf, size_t len);

// One-line-per-item dump of CPU 0 (registers, mode, current instruction) for diagnostics.
void machine_dump(machine_t *m, char *out, size_t out_len);
void machine_key(machine_t *m, int hid_usage, bool down);
void machine_mouse(machine_t *m, int dx, int dy, uint8_t buttons);

#endif
