// beemu: BeOS emulator for Apple Silicon.
//
//   beemu [options]
//     --hda FILE / --hdb / --hdc / --hdd   raw hard disk image on IDE primary/secondary master/slave
//     --cdrom FILE                         ISO image on the secondary master (like QEMU)
//     --boot c|d                           boot from hard disk (default) or CD-ROM first
//     --writable                           let the guest write to the disk images (default: writes
//                                          go to a RAM overlay and are discarded on exit)
//     --bios FILE --vgabios FILE --ram MB
//     --headless SECONDS [--screenshot FILE.png] [--script FILE]   (see script_event below)
//     --vm NAME          start a saved machine (other options given too override its settings)
//     --save-vm NAME     save the machine the other options describe under NAME, then exit
//     --list-vms         list the saved machines;  --menu  show the machine launcher window
//
// Saved machines live in ~/Library/Application Support/beemu/VMs (BEEMU_VM_DIR overrides).
// The app (beemu.app) opens the launcher when it is started without disk options.
//
// Firmware defaults: bios/bios.bin and bios/vgabios-stdvga.bin in the current
// directory, then QEMU's copies from Homebrew (/opt/homebrew/share/qemu).
// --headless runs without a window for the given time, then prints the text
// screen (handy for tests and quick checks).
#include "machine.h"
#include "platform.h"
#include "png.h"
#include "clock.h"
#include "net.h"
#include "sound.h"
#include "vmstore.h"
#include "launcher.h"
#ifdef BEEMU_HAVE_SLIRP
#include "net_slirp.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <mach-o/dyld.h>

// Directory holding the running executable (beemu.app/Contents/MacOS inside the app).
static const char *exe_dir(void)
{
    static char dir[PATH_MAX];
    if (!dir[0]) {
        char path[PATH_MAX], real[PATH_MAX];
        uint32_t n = sizeof path;
        if (_NSGetExecutablePath(path, &n) == 0 && realpath(path, real)) {
            char *slash = strrchr(real, '/');
            if (slash) *slash = 0;
            snprintf(dir, sizeof dir, "%s", real);
        }
    }
    return dir;
}

static bool in_app_bundle(void) { return strstr(exe_dir(), ".app/Contents/MacOS") != NULL; }

static const char *find_firmware(const char *name)
{
    static char buf[2][PATH_MAX];
    static int which;
    char bundled[PATH_MAX];
    snprintf(bundled, sizeof bundled, "%s/../Resources/firmware", exe_dir());
    const char *dirs[] = { bundled, "bios", "/opt/homebrew/share/qemu", "/usr/local/share/qemu" };
    for (unsigned i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        char *p = buf[which];
        snprintf(p, sizeof buf[0], "%s/%s", dirs[i], name);
        if (access(p, R_OK) == 0) { which ^= 1; return p; }
    }
    return NULL;
}

/* ---- headless input scripts ----
 * One event per line: "<seconds> <what> ...", applied once that much host time has passed:
 *   key <hid usage hex> [down|up|tap]   type <text>   mouse <dx> <dy> [buttons]
 *   click [left|right|middle]           shot <file.png>
 *   mark <label>   prints how busy the guest CPU was since the previous mark (not halted)
 *   quit           ends the run (useful in window mode, where a script also works)
 */
static bool g_script_quit;

/* ---- sound: to the audio device (window) and/or a WAV file (--wav) ---- */
static FILE    *g_wav;
static uint32_t g_wav_frames;
static bool     g_audio_live;

static void wav_header(FILE *f, uint32_t frames)
{
    uint32_t data = frames * 4;
    uint8_t h[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0, 2,0,
                      0,0,0,0, 0,0,0,0, 4,0, 16,0, 'd','a','t','a', 0,0,0,0 };
    uint32_t v[4] = { 36 + data, SOUND_RATE, SOUND_RATE * 4, data };
    int at[4] = { 4, 24, 28, 40 };
    for (int k = 0; k < 4; k++) for (int b = 0; b < 4; b++) h[at[k] + b] = (uint8_t)(v[k] >> (8 * b));
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, f);
    fseek(f, 0, SEEK_END);
}

static void sound_out(void *opaque, const int16_t *frames, int n)
{
    (void)opaque;
    if (g_audio_live) platform_audio_write(frames, n);
    if (g_wav) { fwrite(frames, 4, (size_t)n, g_wav); g_wav_frames += (uint32_t)n; }   // host is little endian
}
typedef struct { double t; char what[16]; char arg[1024]; } script_ev_t;

static int hid_for_char(char ch, bool *shift)
{
    static const char *row1 = "1234567890";
    static const char *sh1 = "!@#$%^&*()";
    *shift = false;
    if (ch >= 'a' && ch <= 'z') return 0x04 + (ch - 'a');
    if (ch >= 'A' && ch <= 'Z') { *shift = true; return 0x04 + (ch - 'A'); }
    for (int i = 0; i < 10; i++) {
        if (ch == row1[i]) return 0x1E + i;
        if (ch == sh1[i]) { *shift = true; return 0x1E + i; }
    }
    static const struct { char c, s; int hid; } P[] = {
        { ' ', 0, 0x2C }, { '\n', 0, 0x28 }, { '-', '_', 0x2D }, { '=', '+', 0x2E }, { '[', '{', 0x2F },
        { ']', '}', 0x30 }, { '\\', '|', 0x31 }, { ';', ':', 0x33 }, { '\'', '"', 0x34 }, { '`', '~', 0x35 },
        { ',', '<', 0x36 }, { '.', '>', 0x37 }, { '/', '?', 0x38 },
    };
    for (unsigned i = 0; i < sizeof P / sizeof P[0]; i++) {
        if (ch == P[i].c) return P[i].hid;
        if (P[i].s && ch == P[i].s) { *shift = true; return P[i].hid; }
    }
    return -1;
}

// Run the machine for `secs`, pressing and releasing so keys see real time pass.
static bool run_for(machine_t *m, double secs)
{
    return machine_run(m, (uint64_t)(secs * 1e9));
}

static void tap(machine_t *m, int hid, bool shift)
{
    if (shift) { machine_key(m, 0xE1, true); run_for(m, 0.02); }
    machine_key(m, hid, true);
    run_for(m, 0.05);
    machine_key(m, hid, false);
    if (shift) { run_for(m, 0.02); machine_key(m, 0xE1, false); }
    run_for(m, 0.05);
}

static void script_event(machine_t *m, const script_ev_t *e, uint8_t *buttons)
{
    if (!strcmp(e->what, "key")) {
        char mode[8] = "tap";
        unsigned hid = 0;
        sscanf(e->arg, "%x %7s", &hid, mode);
        if (!strcmp(mode, "down")) machine_key(m, (int)hid, true);
        else if (!strcmp(mode, "up")) machine_key(m, (int)hid, false);
        else tap(m, (int)hid, false);
    } else if (!strcmp(e->what, "type")) {
        for (const char *p = e->arg; *p; p++) {
            bool sh = false;
            char c = *p;
            if (c == '\\' && p[1]) {                        // \n Enter, \t Tab, \\ backslash
                c = p[1] == 'n' ? '\n' : p[1] == 't' ? '\t' : p[1];
                p++;
            }
            int hid = c == '\t' ? 0x2B : hid_for_char(c, &sh);
            if (hid >= 0) tap(m, hid, sh);
        }
    } else if (!strcmp(e->what, "mouse")) {
        int dx = 0, dy = 0, b = -1;
        sscanf(e->arg, "%d %d %d", &dx, &dy, &b);
        if (b >= 0) *buttons = (uint8_t)b;
        // Large moves go out as several packets, like a real mouse.
        while (dx || dy) {
            int sx = dx > 100 ? 100 : dx < -100 ? -100 : dx, sy = dy > 100 ? 100 : dy < -100 ? -100 : dy;
            machine_mouse(m, sx, sy, *buttons);
            dx -= sx; dy -= sy;
            run_for(m, 0.01);
        }
        if (b >= 0) machine_mouse(m, 0, 0, *buttons);
    } else if (!strcmp(e->what, "moveto")) {
        // Absolute screen position: slam into the top-left corner, then step one axis at a
        // time in 1-count packets. BeOS's default acceleration turns each such packet into
        // about 1.92 pixels (measured: speed x (1 + |d| x accel) with speed ~1.03, accel ~0.86).
        double x = 0, y = 0, k = 1.92;
        sscanf(e->arg, "%lf %lf %lf", &x, &y, &k);
        for (int i = 0; i < 40; i++) { machine_mouse(m, -100, -100, *buttons); run_for(m, 0.005); }
        for (int n = (int)(x / k + 0.5); n > 0; n--) { machine_mouse(m, 1, 0, *buttons); run_for(m, 0.004); }
        for (int n = (int)(y / k + 0.5); n > 0; n--) { machine_mouse(m, 0, 1, *buttons); run_for(m, 0.004); }
        run_for(m, 0.05);
    } else if (!strcmp(e->what, "click")) {
        uint8_t bit = strstr(e->arg, "right") ? 2 : strstr(e->arg, "middle") ? 4 : 1;
        machine_mouse(m, 0, 0, (uint8_t)(*buttons | bit));
        run_for(m, 0.08);
        machine_mouse(m, 0, 0, *buttons);
        run_for(m, 0.05);
    } else if (!strcmp(e->what, "quit")) {
        g_script_quit = true;
    } else if (!strcmp(e->what, "mark")) {
        static uint64_t t0, idle0;
        uint64_t t = clock_host_ns(), idle = machine_idle_ns(m);
        if (t0) {
            double span = (double)(t - t0) / 1e9, busy = span - (double)(idle - idle0) / 1e9;
            fprintf(stderr, "beemu: mark %s: CPU busy %.2f s of %.2f s (%.0f%%)\n", e->arg, busy, span, 100.0 * busy / span);
        }
        t0 = t; idle0 = idle;
    } else if (!strcmp(e->what, "shot")) {
        int w, h;
        const uint32_t *fb = machine_frame(m, &w, &h);
        if (!fb || !png_write_xrgb(e->arg, fb, w, h)) fprintf(stderr, "beemu: cannot write %s\n", e->arg);
    } else {
        fprintf(stderr, "beemu: unknown script event '%s'\n", e->what);
    }
}

static int load_script(const char *path, script_ev_t *ev, int max)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "beemu: cannot open %s\n", path); return -1; }
    char line[1100];
    int n = 0;
    while (n < max && fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        if (!strchr(line, '\n') && !feof(f)) {
            fprintf(stderr, "beemu: %s: line too long\n", path);
            fclose(f);
            return -1;
        }
        line[strcspn(line, "\n")] = 0;
        int used = 0;
        if (sscanf(line, "%lf %15s %n", &ev[n].t, ev[n].what, &used) < 2) continue;
        snprintf(ev[n].arg, sizeof ev[n].arg, "%s", line + used);
        n++;
    }
    fclose(f);
    return n;
}

/* ---- saved machines ---- */
static void apply_vm(const vm_config_t *vm, machine_config_t *cfg)
{
    for (int i = 0; i < 4; i++) cfg->drives[i].path = NULL, cfg->drives[i].cdrom = false;
    if (vm->hda[0]) cfg->drives[0].path = vm->hda;
    if (vm->cdrom[0]) { cfg->drives[2].path = vm->cdrom; cfg->drives[2].cdrom = true; }
    cfg->boot = vm->boot_cd ? 'd' : 'c';
    cfg->ram_mb = (uint32_t)vm->ram_mb;
    cfg->n_cpus = vm->cpus;
    cfg->video = vm->video == VM_VIDEO_G400 ? MACHINE_VIDEO_G400 : MACHINE_VIDEO_STD;
    cfg->sound = vm->sound == VM_SOUND_SB16 ? MACHINE_SOUND_SB16 : vm->sound == VM_SOUND_NONE ? MACHINE_SOUND_NONE
                                                                                              : MACHINE_SOUND_ES1370;
    cfg->no_nic = vm->nic == VM_NIC_NONE;
    cfg->nic_model = vm->nic == VM_NIC_RTL8139 ? MACHINE_NIC_RTL8139 : vm->nic == VM_NIC_NE2K ? MACHINE_NIC_NE2K
                                                                                             : MACHINE_NIC_BOTH;
    cfg->drives_writable = vm->writable;
}

static void vm_from_cfg(const machine_config_t *cfg, const char *name, vm_config_t *vm)
{
    vm_defaults(vm);
    snprintf(vm->name, sizeof vm->name, "%s", name);
    if (cfg->drives[0].path && !cfg->drives[0].cdrom) snprintf(vm->hda, sizeof vm->hda, "%s", cfg->drives[0].path);
    if (cfg->drives[2].path && cfg->drives[2].cdrom) snprintf(vm->cdrom, sizeof vm->cdrom, "%s", cfg->drives[2].path);
    vm->boot_cd = cfg->boot == 'd';
    vm->ram_mb = (int)cfg->ram_mb;
    vm->cpus = cfg->n_cpus;
    vm->video = cfg->video == MACHINE_VIDEO_G400 ? VM_VIDEO_G400 : VM_VIDEO_VESA;
    vm->sound = cfg->sound == MACHINE_SOUND_SB16 ? VM_SOUND_SB16 : cfg->sound == MACHINE_SOUND_NONE ? VM_SOUND_NONE
                                                                                                    : VM_SOUND_ES1370;
    vm->nic = cfg->no_nic ? VM_NIC_NONE : cfg->nic_model == MACHINE_NIC_RTL8139 ? VM_NIC_RTL8139
            : cfg->nic_model == MACHINE_NIC_NE2K ? VM_NIC_NE2K : VM_NIC_BOTH;
    vm->writable = cfg->drives_writable;
}

static void read_line(const char *path, char *buf, size_t len)
{
    buf[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fgets(buf, (int)len, f)) buf[strcspn(buf, "\n")] = 0;
    fclose(f);
}

static void write_line(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "%s\n", text); fclose(f); }
}

// Before saved machines, the app remembered one disk (last-disk) and a video card (video):
// turn them into the first machine so nothing is lost.
static void migrate_last_disk(const char *support)
{
    vm_config_t list[1];
    if (vm_list(NULL, list, 1) > 0) return;
    char path[PATH_MAX + 16], last[PATH_MAX], video[16];
    snprintf(path, sizeof path, "%s/last-disk", support);
    read_line(path, last, sizeof last);
    if (!last[0]) return;
    snprintf(path, sizeof path, "%s/video", support);
    read_line(path, video, sizeof video);
    vm_config_t vm;
    vm_defaults(&vm);
    size_t n = strlen(last);
    if (n > 4 && !strcasecmp(last + n - 4, ".iso")) { snprintf(vm.cdrom, sizeof vm.cdrom, "%s", last); vm.boot_cd = true; }
    else snprintf(vm.hda, sizeof vm.hda, "%s", last);
    vm.video = !strcmp(video, "g400") ? VM_VIDEO_G400 : VM_VIDEO_VESA;
    char err[256];
    if (vm_save(NULL, &vm, err, sizeof err)) fprintf(stderr, "beemu: saved %s as machine \"%s\"\n", last, vm.name);
}

static void usage(void)
{
    fprintf(stderr, "usage: beemu [--hda|--hdb|--hdc|--hdd FILE] [--cdrom FILE] [--boot c|d] [--writable]\n"
                    "             [--bios FILE] [--vgabios FILE] [--ram MB] [--cpus 1-8] [--headless SECONDS [--screenshot FILE.png]]\n"
                    "             [--nic both|rtl8139|ne2k|none] [--video std|g400] [--sound es1370|sb16|none] [--wav FILE.wav]\n"
                    "             [--net user|none] [--mac 52:54:00:12:34:56]\n"
                    "             [--hostfwd [tcp|udp:]HOSTPORT:GUESTPORT]...\n"
                    "             [--vm NAME] [--save-vm NAME] [--list-vms] [--menu]\n");
}

int main(int argc, char **argv)
{
    machine_config_t cfg = { .ram_mb = 64, .n_cpus = 1 };
    double headless = 0;
    const char *screenshot = NULL, *script = NULL;
#ifdef BEEMU_HAVE_SLIRP
    const char *net_mode = "user";
#else
    const char *net_mode = "none";
#endif
    struct { bool udp; int host_port, guest_port; } fwd[16];
    int n_fwd = 0;
    const char *wav_path = NULL;
    const char *save_vm = NULL;
    bool list_vms = false, menu = false;
    static vm_config_t vm;              // drive paths point into it
    // --vm first, so the other options override what it says.
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--vm")) {
            if (!vm_load(NULL, argv[i + 1], &vm)) {
                fprintf(stderr, "beemu: no saved machine called \"%s\" (in %s; --list-vms lists them)\n", argv[i + 1], vm_dir());
                return 2;
            }
            apply_vm(&vm, &cfg);
        }
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strncmp(a, "-psn_", 5)) continue;          // Finder's process serial number (older macOS)
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--bios") && v)          { cfg.bios_path = v; i++; }
        else if (!strcmp(a, "--vgabios") && v)  { cfg.vgabios_path = v; i++; }
        else if (!strcmp(a, "--ram") && v)      { cfg.ram_mb = (uint32_t)atoi(v); i++; }
        else if (!strcmp(a, "--headless") && v) { headless = atof(v); i++; }
        else if (!strcmp(a, "--screenshot") && v) { screenshot = v; i++; }
        else if (!strcmp(a, "--script") && v)   { script = v; i++; }
        else if (!strncmp(a, "--hd", 4) && a[4] >= 'a' && a[4] <= 'd' && !a[5] && v) {
            cfg.drives[a[4] - 'a'].path = v; cfg.drives[a[4] - 'a'].cdrom = false; i++;
        }
        else if (!strcmp(a, "--cdrom") && v)    { cfg.drives[2].path = v; cfg.drives[2].cdrom = true; i++; }
        else if (!strcmp(a, "--boot") && v && (*v == 'c' || *v == 'd')) { cfg.boot = *v; i++; }
        else if (!strcmp(a, "--writable"))      { cfg.drives_writable = true; }
        else if (!strcmp(a, "--nic") && v && (!strcmp(v, "none") || !strcmp(v, "rtl8139") || !strcmp(v, "ne2k") ||
                                              !strcmp(v, "both"))) {
            cfg.no_nic = !strcmp(v, "none");
            cfg.nic_model = !strcmp(v, "ne2k") ? MACHINE_NIC_NE2K : !strcmp(v, "rtl8139") ? MACHINE_NIC_RTL8139
                                                                                          : MACHINE_NIC_BOTH;
            i++;
        }
        else if (!strcmp(a, "--net") && v && (!strcmp(v, "none") || !strcmp(v, "user"))) { net_mode = v; i++; }
        else if (!strcmp(a, "--cpus") && v) {
            int n = atoi(v);
            if (n < 1 || n > 8) { fprintf(stderr, "beemu: --cpus takes 1 to 8\n"); return 2; }
            cfg.n_cpus = n;
            i++;
        }
        else if (!strcmp(a, "--sound") && v && (!strcmp(v, "es1370") || !strcmp(v, "sb16") || !strcmp(v, "none"))) {
            cfg.sound = !strcmp(v, "none") ? MACHINE_SOUND_NONE : !strcmp(v, "sb16") ? MACHINE_SOUND_SB16 : MACHINE_SOUND_ES1370;
            i++;
        }
        else if (!strcmp(a, "--wav") && v) { wav_path = v; i++; }
        else if (!strcmp(a, "--vm") && v) { i++; }
        else if (!strcmp(a, "--save-vm") && v) { save_vm = v; i++; }
        else if (!strcmp(a, "--list-vms")) { list_vms = true; }
        else if (!strcmp(a, "--menu")) { menu = true; }
        else if (!strcmp(a, "--video") && v && (!strcmp(v, "std") || !strcmp(v, "g400"))) {
            cfg.video = !strcmp(v, "g400") ? MACHINE_VIDEO_G400 : MACHINE_VIDEO_STD;
            i++;
        }
        else if (!strcmp(a, "--mac") && v) {
            unsigned b[6];
            if (sscanf(v, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) { usage(); return 2; }
            for (int k = 0; k < 6; k++) cfg.mac[k] = (uint8_t)b[k];
            i++;
        }
        else if (!strcmp(a, "--hostfwd") && v && n_fwd < 16) {
            const char *p = v;
            bool udp = false;
            if (!strncmp(p, "udp:", 4)) { udp = true; p += 4; } else if (!strncmp(p, "tcp:", 4)) p += 4;
            int hp, gp;
            if (sscanf(p, "%d:%d", &hp, &gp) != 2) { usage(); return 2; }
            fwd[n_fwd].udp = udp; fwd[n_fwd].host_port = hp; fwd[n_fwd].guest_port = gp;
            n_fwd++;
            i++;
        }
        else { usage(); return 2; }
    }
    if (list_vms) {
        static vm_config_t all[VM_MAX];
        int n = vm_list(NULL, all, VM_MAX);
        printf("%d saved machine%s in %s\n", n, n == 1 ? "" : "s", vm_dir());
        for (int k = 0; k < n; k++) {
            const vm_config_t *x = &all[k];
            static const char *const vid[] = { "vesa", "g400" }, *const snd[] = { "es1370", "sb16", "none" },
                                     *const nic[] = { "both", "rtl8139", "ne2k", "none" };
            printf("  %s: %s%s%s%s, boot %s, %d MB, %d CPU%s, %s, %s, nic %s%s\n", x->name, x->hda[0] ? x->hda : "",
                   x->hda[0] && x->cdrom[0] ? " + " : "", x->cdrom[0] ? "CD " : "", x->cdrom, x->boot_cd ? "cd" : "disk",
                   x->ram_mb, x->cpus, x->cpus == 1 ? "" : "s", vid[x->video], snd[x->sound], nic[x->nic],
                   x->writable ? ", writable" : "");
        }
        return 0;
    }
    if (save_vm) {
        if (!(cfg.drives[0].path && *cfg.drives[0].path) && !(cfg.drives[2].path && *cfg.drives[2].path)) { fprintf(stderr, "beemu: --save-vm needs --hda or --cdrom\n"); return 2; }
        vm_config_t nv;
        char e[512];
        vm_from_cfg(&cfg, save_vm, &nv);
        // Store absolute paths: the launcher starts machines from any directory.
        char abs_path[PATH_MAX];
        if (nv.hda[0] && realpath(nv.hda, abs_path)) snprintf(nv.hda, sizeof nv.hda, "%s", abs_path);
        else if (nv.hda[0]) { fprintf(stderr, "beemu: cannot find %s\n", nv.hda); return 1; }
        if (nv.cdrom[0] && realpath(nv.cdrom, abs_path)) snprintf(nv.cdrom, sizeof nv.cdrom, "%s", abs_path);
        else if (nv.cdrom[0]) { fprintf(stderr, "beemu: cannot find %s\n", nv.cdrom); return 1; }
        if (cfg.ram_mb == 64 && !vm.name[0]) nv.ram_mb = 256;     // 64 is only the command line's default
        if (!vm_save(NULL, &nv, e, sizeof e)) { fprintf(stderr, "beemu: %s\n", e); return 1; }
        char text[4096];
        vm_format(&nv, text, sizeof text);
        printf("beemu: saved in %s:\n%s", vm_dir(), text);
        return 0;
    }

    // Started from Finder (beemu.app) without a disk, or with --menu: the machine launcher.
    bool app = in_app_bundle();
    bool no_disks = !cfg.drives[0].path && !cfg.drives[1].path && !cfg.drives[2].path && !cfg.drives[3].path;
    if (menu || (app && headless <= 0 && no_disks)) {
        char support[PATH_MAX], last_vm_file[PATH_MAX + 16], last_vm[sizeof vm.name];
        const char *home = getenv("HOME");
        snprintf(support, sizeof support, "%s/Library/Application Support/beemu", home ? home : "/tmp");
        mkdir(support, 0755);
        snprintf(last_vm_file, sizeof last_vm_file, "%s/last-vm", support);
        migrate_last_disk(support);
        read_line(last_vm_file, last_vm, sizeof last_vm);
        if (!platform_ui_init()) { fprintf(stderr, "beemu: cannot open the launcher window\n"); return 1; }
        if (!platform_launcher(NULL, last_vm[0] ? last_vm : NULL, &vm)) return 0;
        write_line(last_vm_file, vm.name);
        apply_vm(&vm, &cfg);
        fprintf(stderr, "beemu: starting machine \"%s\"\n", vm.name);
    }

    if (!cfg.bios_path) cfg.bios_path = find_firmware("bios.bin");
    if (!cfg.vgabios_path) cfg.vgabios_path = find_firmware("vgabios-stdvga.bin");
    if (!cfg.bios_path) {
        fprintf(stderr, "beemu: no SeaBIOS image found; pass --bios FILE (QEMU's bios.bin works)\n");
        if (app) platform_error("beemu", "No SeaBIOS firmware found (bios.bin).");
        return 1;
    }
    fprintf(stderr, "beemu: BIOS %s, VGA BIOS %s, %u MB RAM, %d CPU%s\n",
            cfg.bios_path, cfg.vgabios_path ? cfg.vgabios_path : "(none)", cfg.ram_mb, cfg.n_cpus, cfg.n_cpus == 1 ? "" : "s");
    for (int i = 0; i < 4; i++)
        if (cfg.drives[i].path)
            fprintf(stderr, "beemu: IDE %d: %s %s%s\n", i, cfg.drives[i].cdrom ? "CD-ROM" : "disk", cfg.drives[i].path,
                    cfg.drives[i].cdrom ? "" : cfg.drives_writable ? " (writable)" : " (writes discarded on exit)");

    char err[256];
    net_backend_t *net = NULL;
    const char *nic_name = cfg.nic_model == MACHINE_NIC_NE2K ? "NE2000 PCI (RTL8029)"
                         : cfg.nic_model == MACHINE_NIC_RTL8139 ? "RTL8139" : "RTL8139 + NE2000 PCI";
    if (!cfg.no_nic && !strcmp(net_mode, "user")) {
#ifdef BEEMU_HAVE_SLIRP
        net_fwd_t nf[16];
        for (int k = 0; k < n_fwd; k++) nf[k] = (net_fwd_t){ fwd[k].udp, fwd[k].host_port, fwd[k].guest_port };
        net = net_slirp_create(nf, n_fwd, err, sizeof err);
        if (!net) {
            fprintf(stderr, "beemu: network: %s\n", err);
            if (app) platform_error("beemu: network", err);
            return 1;
        }
        fprintf(stderr, "beemu: %s, user-mode network 10.0.2.0/24 (gateway/host 10.0.2.2, DNS 10.0.2.3)\n", nic_name);
        for (int k = 0; k < n_fwd; k++)
            fprintf(stderr, "beemu: forwarding 127.0.0.1:%d -> guest %s port %d\n", fwd[k].host_port,
                    fwd[k].udp ? "UDP" : "TCP", fwd[k].guest_port);
#endif
    } else if (!cfg.no_nic) {
        fprintf(stderr, "beemu: %s with no network attached\n", nic_name);
    }
    cfg.net = net;
    if (cfg.sound != MACHINE_SOUND_NONE) {
        if (wav_path) {
            if (!(g_wav = fopen(wav_path, "wb"))) { fprintf(stderr, "beemu: cannot write %s\n", wav_path); return 1; }
            wav_header(g_wav, 0);
        }
        if (headless <= 0) g_audio_live = platform_audio_open(SOUND_RATE);
        cfg.sound_out = sound_out;
        fprintf(stderr, "beemu: sound: %s%s%s%s\n", cfg.sound == MACHINE_SOUND_SB16 ? "Sound Blaster 16 PnP" : "Ensoniq ES1370", g_audio_live ? ", playing" : "",
                g_wav ? ", recording to " : "", g_wav ? wav_path : "");
    }
    machine_t *m = machine_create(&cfg, err, sizeof err);
    if (!m) {
        fprintf(stderr, "beemu: %s\n", err);
        if (app) platform_error("beemu could not start", err);
        if (net) net->destroy(net);
        return 1;
    }

    int rc = 0;
    platform_boost_thread();
    if (headless > 0) {
        static script_ev_t ev[512];
        int n_ev = script ? load_script(script, ev, 512) : 0;
        if (n_ev < 0) { machine_destroy(m); if (net) net->destroy(net); return 1; }
        // Events run in order at their times; the guest keeps running in between.
        uint64_t t0 = clock_host_ns();
        uint8_t buttons = 0;
        int next = 0;
        // BEEMU_HEADLESS_FRAMES=1: render a frame every 1/60 s as the window would (no SDL), to
        // measure what frame conversion costs the guest; with BEEMU_FULL_FRAMES, whole frames.
        bool hframes = getenv("BEEMU_HEADLESS_FRAMES") != NULL, hfull = getenv("BEEMU_FULL_FRAMES") != NULL;
        uint64_t next_hframe = t0;
        for (;;) {
            double now = (double)(clock_host_ns() - t0) / 1e9;
            if (now >= headless) break;
            while (next < n_ev && ev[next].t <= now) script_event(m, &ev[next++], &buttons);
            if (hframes && clock_host_ns() >= next_hframe) {
                int fw, fh, fy0, fy1;
                if (hfull) machine_frame_invalidate(m);
                machine_frame_rows(m, &fw, &fh, &fy0, &fy1);
                next_hframe += 16666667;
                if (clock_host_ns() > next_hframe) next_hframe = clock_host_ns() + 16666667;
                continue;
            }
            double until = next < n_ev && ev[next].t < headless ? ev[next].t : headless;
            double slice = until - now > 0.02 ? 0.02 : (until - now > 0 ? until - now : 0.001);
            if (hframes) {
                double to_frame = (double)(int64_t)(next_hframe - clock_host_ns()) / 1e9;
                if (to_frame < slice) slice = to_frame > 0.0005 ? to_frame : 0.0005;
            }
            if (!run_for(m, slice)) break;
        }
        static char text[132 * 61 + 64];
        machine_text(m, text, sizeof text);
        printf("\n---- screen ----\n%s----------------\n", text);
        const char *dump_spec = getenv("BEEMU_DUMP");          // hex addr:len -> raw bytes to stdout file
        if (dump_spec) {
            char *colon;
            uint32_t a = (uint32_t)strtoul(dump_spec, &colon, 16);
            size_t len = *colon == ':' ? strtoul(colon + 1, NULL, 16) : 256;
            const char *out = getenv("BEEMU_DUMP_FILE");
            uint8_t *buf = malloc(len);
            FILE *f = out ? fopen(out, "wb") : NULL;
            if (buf && f) { machine_read(m, a, buf, len); fwrite(buf, 1, len, f); }
            if (f) fclose(f);
            free(buf);
        }
        // BEEMU_DUMP_LIN=addr:len[,addr:len...] (hex, linear, current address space) ->
        // $BEEMU_DUMP_FILE_<addr>.bin, for disassembling guest code where it stopped.
        const char *lin_spec = getenv("BEEMU_DUMP_LIN"), *lin_out = getenv("BEEMU_DUMP_FILE");
        for (const char *p = lin_spec; p && *p && lin_out; ) {
            char *end;
            uint32_t a = (uint32_t)strtoul(p, &end, 16);
            size_t len = *end == ':' ? strtoul(end + 1, &end, 16) : 256;
            uint8_t *buf = calloc(1, len);
            char path[PATH_MAX];
            snprintf(path, sizeof path, "%s_%08X.bin", lin_out, a);
            size_t got = buf ? machine_read_lin(m, a, buf, len) : 0;
            FILE *f = got ? fopen(path, "wb") : NULL;
            if (f) { fwrite(buf, 1, got, f); fclose(f); }
            fprintf(stderr, "beemu: dump %08X: %zu of %zu bytes mapped%s%s\n", a, got, len, f ? " -> " : "", f ? path : "");
            free(buf);
            p = *end == ',' ? end + 1 : end + strlen(end);
        }
        if (screenshot) {
            int w, h;
            const uint32_t *fb = machine_frame(m, &w, &h);
            if (!fb || !png_write_xrgb(screenshot, fb, w, h)) fprintf(stderr, "beemu: cannot write %s\n", screenshot);
        }
        char dump[8192];
        machine_dump(m, dump, sizeof dump);
        printf("%s", dump);
    } else {
        int w, h;
        machine_frame(m, &w, &h);
        char title[160];
        snprintf(title, sizeof title, vm.name[0] ? "beemu: %s" : "beemu", vm.name);
        if (!platform_init(title, 1280, 800)) { machine_destroy(m); return 1; }
        bool running = true;
        uint8_t buttons = 0;
        uint64_t next_frame = clock_host_ns() + 16666667;
        static script_ev_t ev[512];                       // --script works in the window too
        int n_ev = script ? load_script(script, ev, 512) : 0, next_ev = 0;
        if (n_ev < 0) n_ev = 0;
        uint64_t script_t0 = clock_host_ns();
        bool stats = getenv("BEEMU_STATS") != NULL;           // frame cost, printed at exit
        bool full_frames = getenv("BEEMU_FULL_FRAMES") != NULL; // convert and present whole frames (old way)
        uint64_t render_ns = 0, present_ns = 0, frames = 0, presents = 0, rows = 0, slow_presents = 0, slow_present_max = 0;
        while (running) {
            plat_event e;
            while (platform_poll_event(&e)) {
                switch (e.type) {
                case PLAT_EVENT_QUIT: running = false; break;
                case PLAT_EVENT_KEY: machine_key(m, e.scancode, e.down); break;   // SDL scancodes are HID usages
                case PLAT_EVENT_MOUSE_MOVE: machine_mouse(m, e.dx, e.dy, buttons); break;
                case PLAT_EVENT_MOUSE_BUTTON: {
                    uint8_t bit = e.button == 1 ? 1 : e.button == 3 ? 2 : e.button == 2 ? 4 : 0;
                    buttons = e.down ? (uint8_t)(buttons | bit) : (uint8_t)(buttons & ~bit);
                    machine_mouse(m, 0, 0, buttons);
                    break;
                }
                default: break;
                }
            }
            // Frames are paced by the clock, not by vsync: the guest runs until the next 60 Hz
            // frame is due, then the frame is presented without waiting for the display.
            // (A blocking vsync present left the emulated CPU idle ~30% of the time, more on
            // 120 Hz displays.)
            while (next_ev < n_ev && ev[next_ev].t <= (double)(clock_host_ns() - script_t0) / 1e9)
                script_event(m, &ev[next_ev++], &buttons);
            if (g_script_quit) running = false;
            uint64_t now = clock_host_ns();
            if (now + 1000000 > next_frame) next_frame = now + 1000000;   // fell behind: run at least 1 ms
            if (!machine_run(m, next_frame - now)) running = false;
            next_frame += 16666667;
            uint64_t t0 = stats ? clock_host_ns() : 0;
            int y0, y1;
            if (full_frames) machine_frame_invalidate(m);
            const uint32_t *fb = machine_frame_rows(m, &w, &h, &y0, &y1);
            uint64_t t1 = stats ? clock_host_ns() : 0;
            if (fb && full_frames) { platform_present_frame(fb, w, h, w * 4); presents++; }
            else if (fb) presents += platform_present_rows(fb, w, h, w * 4, y0, y1);
            if (stats) {
                uint64_t pns = clock_host_ns() - t1;
                if (pns > slow_present_max) slow_present_max = pns;
                if (pns > 5000000) slow_presents++;
                render_ns += t1 - t0; present_ns += pns; frames++;
                if (full_frames) y0 = 0, y1 = h;
                if (y0 < y1) rows += (uint64_t)(y1 - y0);
            }
        }
        if (stats && frames)
            fprintf(stderr, "beemu: %llu frames: render %.3f ms, upload + present %.3f ms per frame; %llu presented, "
                            "%.0f rows changed per frame\n",
                    (unsigned long long)frames, (double)render_ns / (double)frames / 1e6,
                    (double)present_ns / (double)frames / 1e6, (unsigned long long)presents, (double)rows / (double)frames);
        if (stats && frames)
            fprintf(stderr, "beemu: %llu presents took over 5 ms (longest %.1f ms)\n", (unsigned long long)slow_presents,
                    (double)slow_present_max / 1e6);
        platform_shutdown();
    }
    const char *why = machine_stop_reason(m);
    if (why) {
        fprintf(stderr, "beemu: guest stopped: %s\n", why);
        if (app) platform_error("The guest stopped", why);
        rc = 1;
    }
    machine_destroy(m);
    if (g_wav) { wav_header(g_wav, g_wav_frames); fclose(g_wav); g_wav = NULL; }
    if (net) net->destroy(net);
    return rc;
}
