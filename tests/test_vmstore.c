// Saved machine store (platform/vmstore.c): file format round trip, defaults and limits,
// list/load/save/rename/delete in a temporary directory.
#include "vmstore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

static int g_fail, g_run;
#define CHECK(cond, msg) do { g_run++; if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); g_fail++; } } while (0)

static int count_files(const char *dir)
{
    DIR *d = opendir(dir);
    int n = 0;
    struct dirent *de;
    while (d && (de = readdir(d))) if (de->d_name[0] != '.') n++;
    if (d) closedir(d);
    return n;
}

int main(void)
{
    vm_config_t a, b;
    vm_defaults(&a);
    CHECK(a.ram_mb == 256 && a.cpus == 1 && a.video == VM_VIDEO_VESA && a.sound == VM_SOUND_ES1370 &&
          a.nic == VM_NIC_BOTH && !a.writable && !a.boot_cd, "defaults");

    // Round trip of every field, with spaces and '=' in strings.
    snprintf(a.name, sizeof a.name, "R5 = test / SMP");
    snprintf(a.hda, sizeof a.hda, "/Users/x/My Disks/beos 5.raw");
    snprintf(a.cdrom, sizeof a.cdrom, "/Volumes/CD/dano.iso");
    a.boot_cd = true; a.ram_mb = 512; a.cpus = 8; a.video = VM_VIDEO_G400; a.sound = VM_SOUND_SB16;
    a.nic = VM_NIC_NE2K; a.writable = true;
    char text[4096];
    vm_format(&a, text, sizeof text);
    CHECK(vm_parse(text, &b), "parse");
    CHECK(!memcmp(&a, &b, sizeof a) || (!strcmp(a.name, b.name) && !strcmp(a.hda, b.hda) && !strcmp(a.cdrom, b.cdrom) &&
          a.boot_cd == b.boot_cd && a.ram_mb == b.ram_mb && a.cpus == b.cpus && a.video == b.video &&
          a.sound == b.sound && a.nic == b.nic && a.writable == b.writable), "round trip");
    for (int s = VM_SOUND_ES1370; s <= VM_SOUND_NONE; s++)
        for (int n = VM_NIC_BOTH; n <= VM_NIC_NONE; n++) {
            a.sound = s; a.nic = n;
            vm_format(&a, text, sizeof text);
            vm_parse(text, &b);
            CHECK(b.sound == s && b.nic == n, "sound/nic values");
        }

    // Limits, unknown keys and values, comments, no name.
    CHECK(vm_parse("name=x\ncpus = 99\nram = 1\nvideo = tnt2\nfoo = bar\n# cpus = 2\n", &b), "parse loose");
    CHECK(b.cpus == 8 && b.ram_mb == 16 && b.video == VM_VIDEO_VESA, "clamped");
    CHECK(vm_parse("name = y\ncpus = 0\r\n", &b) && b.cpus == 1, "cpus min, CRLF");
    CHECK(!vm_parse("cpus = 2\n", &b), "no name");

    // Directory operations.
    char dir[] = "/tmp/beemu_vmstore_XXXXXX";
    CHECK(mkdtemp(dir) != NULL, "mkdtemp");
    char err[512];
    vm_config_t list[VM_MAX];
    CHECK(vm_list(dir, list, VM_MAX) == 0, "empty");
    vm_defaults(&a); snprintf(a.name, sizeof a.name, "beta"); a.cpus = 2;
    CHECK(vm_save(dir, &a, err, sizeof err), "save beta");
    vm_defaults(&a); snprintf(a.name, sizeof a.name, "Alpha/1"); a.cpus = 4;
    CHECK(vm_save(dir, &a, err, sizeof err), "save Alpha/1 (slash in name)");
    int n = vm_list(dir, list, VM_MAX);
    CHECK(n == 2 && !strcmp(list[0].name, "Alpha/1") && !strcmp(list[1].name, "beta"), "list sorted");
    CHECK(vm_load(dir, "beta", &b) && b.cpus == 2, "load");
    CHECK(!vm_load(dir, "gamma", &b), "load missing");
    a.cpus = 3;                                         // replaces the machine of the same name
    CHECK(vm_save(dir, &a, err, sizeof err) && count_files(dir) == 2, "save replaces");
    CHECK(vm_load(dir, "Alpha/1", &b) && b.cpus == 3, "replaced");
    vm_config_t r = a; snprintf(r.name, sizeof r.name, "beta");
    CHECK(!vm_rename(dir, "Alpha/1", &r, err, sizeof err) && strstr(err, "already"), "rename onto existing");
    snprintf(r.name, sizeof r.name, "Gamma");
    CHECK(vm_rename(dir, "Alpha/1", &r, err, sizeof err), "rename");
    CHECK(!vm_load(dir, "Alpha/1", &b) && vm_load(dir, "Gamma", &b) && b.cpus == 3 && count_files(dir) == 2, "renamed");
    CHECK(vm_delete(dir, "beta") && !vm_delete(dir, "beta") && vm_list(dir, list, VM_MAX) == 1, "delete");
    vm_delete(dir, "Gamma");
    CHECK(count_files(dir) == 0, "no leftovers");
    rmdir(dir);

    printf("%d/%d vmstore checks passed\n", g_run - g_fail, g_run);
    return g_fail != 0;
}
