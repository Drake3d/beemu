// QEMU firmware configuration interface (fw_cfg), port I/O flavor: selector at
// 0x510 (16-bit write), data at 0x511 (byte reads). SeaBIOS's QEMU build reads
// the RAM map, CPU count and named files from it. No DMA interface.
#ifndef FWCFG_H
#define FWCFG_H

#include <stddef.h>
#include <stdint.h>
#include "io.h"

#define FWCFG_MAX_ITEMS 32
#define FWCFG_FILE_FIRST 0x20

enum {
    FW_CFG_SIGNATURE = 0x00, FW_CFG_ID = 0x01, FW_CFG_UUID = 0x02, FW_CFG_RAM_SIZE = 0x03,
    FW_CFG_NOGRAPHIC = 0x04, FW_CFG_NB_CPUS = 0x05, FW_CFG_MACHINE_ID = 0x06,
    FW_CFG_NUMA = 0x0D, FW_CFG_BOOT_MENU = 0x0E, FW_CFG_MAX_CPUS = 0x0F, FW_CFG_FILE_DIR = 0x19,
};

typedef struct {
    uint16_t key;
    uint8_t *data;
    uint32_t len;
    char     name[56];       // file name, or "" for numbered items
} fwcfg_item_t;

typedef struct {
    fwcfg_item_t items[FWCFG_MAX_ITEMS];
    int      n_items;
    uint16_t next_file_key;
    uint8_t *dir;            // FW_CFG_FILE_DIR contents, rebuilt on each add
    uint16_t sel;
    uint32_t pos;
} fwcfg_t;

void fwcfg_init(fwcfg_t *f);
void fwcfg_free(fwcfg_t *f);
void fwcfg_register(fwcfg_t *f, io_bus_t *io);
// Items copy their data.
void fwcfg_add_bytes(fwcfg_t *f, uint16_t key, const void *data, uint32_t len);
void fwcfg_add_u16(fwcfg_t *f, uint16_t key, uint16_t v);
void fwcfg_add_u32(fwcfg_t *f, uint16_t key, uint32_t v);
void fwcfg_add_u64(fwcfg_t *f, uint16_t key, uint64_t v);
void fwcfg_add_file(fwcfg_t *f, const char *name, const void *data, uint32_t len);

#endif
