#include "fwcfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void fwcfg_init(fwcfg_t *f)
{
    memset(f, 0, sizeof *f);
    f->next_file_key = FWCFG_FILE_FIRST;
    fwcfg_add_bytes(f, FW_CFG_SIGNATURE, "QEMU", 4);
    fwcfg_add_u32(f, FW_CFG_ID, 1);                 // traditional interface only, no DMA
}

void fwcfg_free(fwcfg_t *f)
{
    for (int i = 0; i < f->n_items; i++) free(f->items[i].data);
    free(f->dir);
    memset(f, 0, sizeof *f);
}

static fwcfg_item_t *find(fwcfg_t *f, uint16_t key)
{
    for (int i = 0; i < f->n_items; i++)
        if (f->items[i].key == key) return &f->items[i];
    return NULL;
}

static void set_item(fwcfg_t *f, uint16_t key, const void *data, uint32_t len, const char *name)
{
    fwcfg_item_t *it = find(f, key);
    if (!it) {
        if (f->n_items == FWCFG_MAX_ITEMS) return;
        it = &f->items[f->n_items++];
        it->key = key;
    }
    free(it->data);
    it->data = malloc(len ? len : 1);
    if (it->data && len) memcpy(it->data, data, len);
    it->len = it->data ? len : 0;
    snprintf(it->name, sizeof it->name, "%s", name ? name : "");
}

void fwcfg_add_bytes(fwcfg_t *f, uint16_t key, const void *data, uint32_t len)
{
    set_item(f, key, data, len, NULL);
}

// Numbered items are little-endian.
void fwcfg_add_u16(fwcfg_t *f, uint16_t key, uint16_t v) { uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; set_item(f, key, b, 2, NULL); }
void fwcfg_add_u32(fwcfg_t *f, uint16_t key, uint32_t v)
{
    uint8_t b[4];
    for (int i = 0; i < 4; i++) b[i] = (uint8_t)(v >> (8 * i));
    set_item(f, key, b, 4, NULL);
}
void fwcfg_add_u64(fwcfg_t *f, uint16_t key, uint64_t v)
{
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
    set_item(f, key, b, 8, NULL);
}

// The file directory is big-endian: count, then {size, select, reserved, name[56]}.
static void rebuild_dir(fwcfg_t *f)
{
    int n = 0;
    for (int i = 0; i < f->n_items; i++) if (f->items[i].name[0]) n++;
    uint32_t len = 4 + 64u * (uint32_t)n;
    uint8_t *d = calloc(1, len);
    if (!d) return;
    d[0] = (uint8_t)(n >> 24); d[1] = (uint8_t)(n >> 16); d[2] = (uint8_t)(n >> 8); d[3] = (uint8_t)n;
    uint8_t *e = d + 4;
    for (int i = 0; i < f->n_items; i++) {
        const fwcfg_item_t *it = &f->items[i];
        if (!it->name[0]) continue;
        e[0] = (uint8_t)(it->len >> 24); e[1] = (uint8_t)(it->len >> 16);
        e[2] = (uint8_t)(it->len >> 8);  e[3] = (uint8_t)it->len;
        e[4] = (uint8_t)(it->key >> 8);  e[5] = (uint8_t)it->key;
        memcpy(e + 8, it->name, 56);
        e += 64;
    }
    set_item(f, FW_CFG_FILE_DIR, d, len, NULL);
    free(d);
}

void fwcfg_add_file(fwcfg_t *f, const char *name, const void *data, uint32_t len)
{
    for (int i = 0; i < f->n_items; i++)
        if (f->items[i].name[0] && !strcmp(f->items[i].name, name)) {
            set_item(f, f->items[i].key, data, len, name);
            rebuild_dir(f);
            return;
        }
    set_item(f, f->next_file_key++, data, len, name);
    rebuild_dir(f);
}

static uint32_t fw_read(void *opaque, uint16_t port, int size)
{
    fwcfg_t *f = opaque;
    if (port != 0x511) return 0;
    uint32_t v = 0;
    fwcfg_item_t *it = find(f, f->sel & 0xBFFF);   // bit 14 = write, bit 15 = arch-local namespace
    for (int i = 0; i < size; i++) {
        uint8_t b = 0;
        if (it && f->pos < it->len) b = it->data[f->pos];
        if (it && f->pos < it->len) f->pos++;
        v |= (uint32_t)b << (8 * i);
    }
    return v;
}

static void fw_write(void *opaque, uint16_t port, uint32_t v, int size)
{
    (void)size;
    fwcfg_t *f = opaque;
    if (port == 0x510) { f->sel = (uint16_t)v; f->pos = 0; }   // data writes are not supported
}

void fwcfg_register(fwcfg_t *f, io_bus_t *io)
{
    io_register(io, 0x510, 2, fw_read, fw_write, f);
}
