// Saved virtual machines: one small text file per machine (key = value lines) in a directory,
// by default ~/Library/Application Support/beemu/VMs (BEEMU_VM_DIR overrides). Plain C, no UI: the launcher window
// (launcher_mac.m) and the command line (--vm, --save-vm, --list-vms) both use it.
#ifndef VMSTORE_H
#define VMSTORE_H

#include <stdbool.h>
#include <stddef.h>

enum { VM_VIDEO_VESA = 0, VM_VIDEO_G400 = 1 };
enum { VM_SOUND_ES1370 = 0, VM_SOUND_SB16 = 1, VM_SOUND_NONE = 2 };
enum { VM_NIC_BOTH = 0, VM_NIC_RTL8139 = 1, VM_NIC_NE2K = 2, VM_NIC_NONE = 3 };

typedef struct {
    char name[128];
    char hda[1024];          // raw hard disk image ("" = none)
    char cdrom[1024];        // CD image ("" = none)
    bool boot_cd;            // boot from the CD first
    int  ram_mb;             // 16 .. 3072
    int  cpus;               // 1 .. 8
    int  video, sound, nic;  // VM_VIDEO_*, VM_SOUND_*, VM_NIC_*
    bool writable;           // keep guest writes in the disk image
} vm_config_t;

#define VM_MAX 64

void vm_defaults(vm_config_t *vm);                 // 256 MB, 1 CPU, VESA, ES1370, both NICs, writes discarded
const char *vm_dir(void);                          // the default directory (created on first use)

// All machines in dir (NULL = default), sorted by name. Returns the count (at most max).
int  vm_list(const char *dir, vm_config_t *out, int max);
bool vm_load(const char *dir, const char *name, vm_config_t *out);
// Saves under vm->name, replacing a machine of that name. err gets a message on failure.
bool vm_save(const char *dir, const vm_config_t *vm, char *err, size_t errlen);
bool vm_delete(const char *dir, const char *name);
bool vm_rename(const char *dir, const char *old_name, const vm_config_t *vm, char *err, size_t errlen);

// File format (exposed for tests).
size_t vm_format(const vm_config_t *vm, char *buf, size_t len);
bool   vm_parse(const char *text, vm_config_t *out);

#endif
