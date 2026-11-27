#include "vmstore.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define EXT ".beemuvm"

void vm_defaults(vm_config_t *vm)
{
    memset(vm, 0, sizeof *vm);
    snprintf(vm->name, sizeof vm->name, "BeOS");
    vm->ram_mb = 256;
    vm->cpus = 1;
    vm->video = VM_VIDEO_VESA;
    vm->sound = VM_SOUND_ES1370;
    vm->nic = VM_NIC_BOTH;
}

const char *vm_dir(void)
{
    static char dir[PATH_MAX];
    if (!dir[0] && getenv("BEEMU_VM_DIR")) {
        snprintf(dir, sizeof dir, "%s", getenv("BEEMU_VM_DIR"));
        mkdir(dir, 0755);
    }
    if (!dir[0]) {
        const char *home = getenv("HOME");
        char base[PATH_MAX];
        snprintf(base, sizeof base, "%s/Library/Application Support/beemu", home ? home : "/tmp");
        mkdir(base, 0755);
        snprintf(dir, sizeof dir, "%s/VMs", base);
        mkdir(dir, 0755);
    }
    return dir;
}

static const char *dir_or_default(const char *dir) { return dir ? dir : vm_dir(); }

/* ---- format ---- */
static const char *VIDEO[] = { "vesa", "g400" };
static const char *SOUND[] = { "es1370", "sb16", "none" };
static const char *NIC[]   = { "both", "rtl8139", "ne2k", "none" };

static int lookup(const char *v, const char *const *names, int n, int dflt)
{
    for (int i = 0; i < n; i++) if (!strcmp(v, names[i])) return i;
    return dflt;
}

size_t vm_format(const vm_config_t *vm, char *buf, size_t len)
{
    int n = snprintf(buf, len,
                     "# beemu virtual machine\n"
                     "name = %s\nhda = %s\ncdrom = %s\nboot = %s\nram = %d\ncpus = %d\n"
                     "video = %s\nsound = %s\nnic = %s\nkeep_disk_changes = %s\n",
                     vm->name, vm->hda, vm->cdrom, vm->boot_cd ? "cd" : "disk", vm->ram_mb, vm->cpus,
                     VIDEO[vm->video & 1], SOUND[vm->sound >= 0 && vm->sound < 3 ? vm->sound : 0],
                     NIC[vm->nic >= 0 && vm->nic < 4 ? vm->nic : 0], vm->writable ? "yes" : "no");
    return n < 0 ? 0 : (size_t)n;
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    size_t i = 0;
    while (s[i] && isspace((unsigned char)s[i])) i++;
    if (i) memmove(s, s + i, n - i + 1);
}

bool vm_parse(const char *text, vm_config_t *out)
{
    vm_defaults(out);
    out->name[0] = 0;
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char line[1200];
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, p, n);
        line[n] = 0;
        p = e ? e + 1 : p + n;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        char *key = line, *val = eq + 1;
        trim(key);
        trim(val);
        if (!strcmp(key, "name")) snprintf(out->name, sizeof out->name, "%s", val);
        else if (!strcmp(key, "hda")) snprintf(out->hda, sizeof out->hda, "%s", val);
        else if (!strcmp(key, "cdrom")) snprintf(out->cdrom, sizeof out->cdrom, "%s", val);
        else if (!strcmp(key, "boot")) out->boot_cd = !strcmp(val, "cd");
        else if (!strcmp(key, "ram")) out->ram_mb = atoi(val);
        else if (!strcmp(key, "cpus")) out->cpus = atoi(val);
        else if (!strcmp(key, "video")) out->video = lookup(val, VIDEO, 2, VM_VIDEO_VESA);
        else if (!strcmp(key, "sound")) out->sound = lookup(val, SOUND, 3, VM_SOUND_ES1370);
        else if (!strcmp(key, "nic")) out->nic = lookup(val, NIC, 4, VM_NIC_BOTH);
        else if (!strcmp(key, "keep_disk_changes")) out->writable = !strcmp(val, "yes");
    }
    if (out->ram_mb < 16) out->ram_mb = 16;
    if (out->ram_mb > 3072) out->ram_mb = 3072;
    if (out->cpus < 1) out->cpus = 1;
    if (out->cpus > 8) out->cpus = 8;
    return out->name[0] != 0;
}

/* ---- files ---- */
static bool read_file(const char *path, char *buf, size_t len)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    size_t n = fread(buf, 1, len - 1, f);
    buf[n] = 0;
    fclose(f);
    return true;
}

static bool has_ext(const char *fn)
{
    size_t n = strlen(fn), k = strlen(EXT);
    return n > k && !strcmp(fn + n - k, EXT);
}

// Path of the file holding machine `name`, or "" if there is none.
static void find_file(const char *dir, const char *name, char *path, size_t len)
{
    path[0] = 0;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    char buf[8192];
    while ((de = readdir(d))) {
        if (!has_ext(de->d_name)) continue;
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        vm_config_t vm;
        if (read_file(p, buf, sizeof buf) && vm_parse(buf, &vm) && !strcmp(vm.name, name)) {
            snprintf(path, len, "%s", p);
            break;
        }
    }
    closedir(d);
}

static int by_name(const void *a, const void *b)
{
    return strcasecmp(((const vm_config_t *)a)->name, ((const vm_config_t *)b)->name);
}

int vm_list(const char *dir, vm_config_t *out, int max)
{
    dir = dir_or_default(dir);
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *de;
    char buf[8192];
    while (n < max && (de = readdir(d))) {
        if (!has_ext(de->d_name)) continue;
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        if (read_file(p, buf, sizeof buf) && vm_parse(buf, &out[n])) n++;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof *out, by_name);
    return n;
}

bool vm_load(const char *dir, const char *name, vm_config_t *out)
{
    char p[PATH_MAX], buf[8192];
    find_file(dir_or_default(dir), name, p, sizeof p);
    return p[0] && read_file(p, buf, sizeof buf) && vm_parse(buf, out);
}

bool vm_save(const char *dir, const vm_config_t *vm, char *err, size_t errlen)
{
    dir = dir_or_default(dir);
    if (!vm->name[0]) { snprintf(err, errlen, "The machine needs a name."); return false; }
    char p[PATH_MAX];
    find_file(dir, vm->name, p, sizeof p);
    if (!p[0]) {                                     // a new file named after the machine
        char base[160];
        size_t k = 0;
        for (const char *s = vm->name; *s && k + 1 < sizeof base; s++, k++)
            base[k] = (*s == '/' || *s == ':' || (k == 0 && *s == '.')) ? '_' : *s;
        base[k] = 0;
        snprintf(p, sizeof p, "%s/%s%s", dir, base, EXT);
        for (int i = 2; access(p, F_OK) == 0 && i < 1000; i++) snprintf(p, sizeof p, "%s/%s %d%s", dir, base, i, EXT);
    }
    char text[4096], tmp[PATH_MAX + 8];
    size_t n = vm_format(vm, text, sizeof text);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "w");
    if (!f || fwrite(text, 1, n, f) != n) {
        snprintf(err, errlen, "Cannot write %s: %s", tmp, strerror(errno));
        if (f) fclose(f);
        return false;
    }
    fclose(f);
    if (rename(tmp, p) != 0) { snprintf(err, errlen, "Cannot save %s: %s", p, strerror(errno)); unlink(tmp); return false; }
    return true;
}

bool vm_delete(const char *dir, const char *name)
{
    char p[PATH_MAX];
    find_file(dir_or_default(dir), name, p, sizeof p);
    return p[0] && unlink(p) == 0;
}

bool vm_rename(const char *dir, const char *old_name, const vm_config_t *vm, char *err, size_t errlen)
{
    dir = dir_or_default(dir);
    if (strcmp(old_name, vm->name)) {
        vm_config_t other;
        if (vm_load(dir, vm->name, &other)) { snprintf(err, errlen, "There is already a machine called \"%s\".", vm->name); return false; }
        if (!vm_save(dir, vm, err, errlen)) return false;
        vm_delete(dir, old_name);
        return true;
    }
    return vm_save(dir, vm, err, errlen);
}
