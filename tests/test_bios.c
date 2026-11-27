// End-to-end: boot SeaBIOS + SeaVGABIOS (QEMU's prebuilt images) on the
// emulated machine until POST finishes, then check the boot menu responds to
// the keyboard (8042 -> IRQ 1 -> PIC -> CPU -> SeaBIOS's INT 9 handler).
// Exits 77 (skipped) when the firmware images aren't installed.
#include "machine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int g_fail, g_run;
#define CHECK(cond, msg) do { g_run++; if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); g_fail++; } } while (0)

static char g_log[1 << 16];
static size_t g_log_len;

static void capture(void *opaque, const char *s, size_t n)
{
    (void)opaque;
    if (g_log_len + n < sizeof g_log - 1) { memcpy(g_log + g_log_len, s, n); g_log_len += n; g_log[g_log_len] = 0; }
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

// Run until `needle` shows up in the debug log (or on screen if screen=true), or timeout.
static bool run_until(machine_t *m, const char *needle, bool screen, double timeout)
{
    static char text[132 * 61 + 64];
    double t0 = now_s();
    while (now_s() - t0 < timeout) {
        if (!machine_run(m, 20000000)) return false;
        if (!screen && strstr(g_log, needle)) return true;
        if (screen) { machine_text(m, text, sizeof text); if (strstr(text, needle)) return true; }
    }
    return false;
}

int main(void)
{
    if (access(BIOS_PATH, R_OK) || access(VGABIOS_PATH, R_OK)) {
        printf("(skipped: %s or %s not found)\n", BIOS_PATH, VGABIOS_PATH);
        return 77;
    }
    machine_config_t cfg = { .ram_mb = 64, .n_cpus = 1, .bios_path = BIOS_PATH,
                             .vgabios_path = VGABIOS_PATH, .debug_out = capture };
    char err[256];

    /* ---- POST ---- */
    machine_t *m = machine_create(&cfg, err, sizeof err);
    CHECK(m != NULL, err);
    if (!m) return 1;
    double t0 = now_s();
    bool posted = run_until(m, "No bootable device", false, 20.0);
    double secs = now_s() - t0;
    // The debug log gets the message first; give the screen output (same printf) time to follow.
    machine_run(m, 50000000ull);
    if (machine_stop_reason(m)) printf("guest stopped: %s\n", machine_stop_reason(m));
    CHECK(posted, "SeaBIOS reaches 'No bootable device'");
    printf("POST to boot attempt: %.2f s of host time\n", secs);
    static const char *milestones[] = {
        "Running on QEMU (i440fx)", "Found QEMU fw_cfg", "Found 7 PCI devices",
        "PCI: init bdf=00:03.0 id=10ec:8139", "PCI: map device bdf=00:03.0  bar 0",
        "PCI: init bdf=00:04.0 id=10ec:8029", "PCI: map device bdf=00:04.0  bar 0",
        "PCI: init bdf=00:06.0 id=1274:5000", "PCI: map device bdf=00:06.0  bar 0",
        "PCI: map device bdf=00:02.0  bar 6, addr febe0000", "CPU Mhz=", "Start SeaVGABIOS",
        "VBE DISPI: lfb_addr=fd000000, size 16 MB", "set VGA mode 3", "PS2 keyboard initialized",
        "All threads complete.", "e820 map has", "Booting from Hard Disk...",
    };
    for (unsigned i = 0; i < sizeof milestones / sizeof milestones[0]; i++) {
        bool found = strstr(g_log, milestones[i]) != NULL;
        if (!found) printf("missing from debug log: %s\n", milestones[i]);
        CHECK(found, "POST milestone");
    }
    static char text[132 * 61 + 64];
    machine_text(m, text, sizeof text);
    CHECK(!strncmp(text, "SeaBIOS (version", 16), "banner is on the VGA text screen");
    CHECK(strstr(text, "No bootable device.") != NULL, "boot failure message on screen");
    int w = 0, h = 0;
    CHECK(machine_frame(m, &w, &h) && w == 640 && h == 400, "80x25 text mode renders at 640x400");
    machine_destroy(m);

    /* ---- keyboard: the boot menu ---- */
    g_log_len = 0; g_log[0] = 0;
    cfg.boot_menu = true;
    m = machine_create(&cfg, err, sizeof err);
    if (!m) { CHECK(0, err); return 1; }
    CHECK(run_until(m, "Press ESC for boot menu", true, 20.0), "boot menu prompt on screen");
    machine_key(m, 0x29, true);                              // ESC (HID usage)
    machine_run(m, 20000000);
    machine_key(m, 0x29, false);
    CHECK(run_until(m, "Select boot device", true, 10.0), "ESC opens the boot menu");
    machine_destroy(m);

    /* ---- multiprocessor: SeaBIOS starts every AP (INIT/SIPI) and builds the MP table ---- */
    for (int n = 2; n <= 8; n *= 2) {
        g_log_len = 0; g_log[0] = 0;
        cfg.boot_menu = false;
        cfg.n_cpus = n;
        m = machine_create(&cfg, err, sizeof err);
        if (!m) { CHECK(0, err); return 1; }
        char want[64];
        snprintf(want, sizeof want, "Found %d cpu(s) max supported %d cpu(s)", n, n);
        CHECK(run_until(m, "No bootable device", false, 20.0) && strstr(g_log, want) && strstr(g_log, "Copying MPTABLE"),
              "SeaBIOS finds every CPU and copies an MP table");
        if (!strstr(g_log, want)) printf("missing from debug log: %s\n", want);
        machine_destroy(m);
    }
    cfg.n_cpus = 1;

    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
