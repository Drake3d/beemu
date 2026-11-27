#include "mem.h"
#include <stdlib.h>
#include <string.h>

#define HOST_ALIGN 16384u

mem_bus_t *mem_create(void)
{
    return calloc(1, sizeof(mem_bus_t));
}

void mem_destroy(mem_bus_t *bus) { free(bus); }

uint8_t *mem_alloc_host(size_t size)
{
    size = (size + HOST_ALIGN - 1) & ~(size_t)(HOST_ALIGN - 1);
    void *p = NULL;
    if (posix_memalign(&p, HOST_ALIGN, size) != 0) return NULL;
    memset(p, 0, size);
    return p;
}

void mem_free_host(uint8_t *p) { free(p); }

/* ------------------------------------------------------------------ */
/* Mapping                                                             */
/* ------------------------------------------------------------------ */

static bool range_ok(uint32_t base, uint32_t size)
{
    if (size == 0) return false;
    if ((base | size) & (MEM_PAGE_SIZE - 1)) return false;
    return (uint64_t)base + size <= 0x100000000ull;
}

static void map_changed(mem_bus_t *bus)
{
    if (bus->map_change_cb) bus->map_change_cb(bus->map_change_opaque);
}

static bool map_direct(mem_bus_t *bus, uint32_t base, uint32_t size, uint8_t *host, uint8_t flags)
{
    if (!range_ok(base, size) || !host) return false;
    uint32_t first = base >> MEM_PAGE_SHIFT;
    uint32_t count = size >> MEM_PAGE_SHIFT;
    for (uint32_t i = 0; i < count; i++) {
        bus->page_ptr[first + i]   = host + (size_t)i * MEM_PAGE_SIZE;
        bus->page_flags[first + i] = flags;
        bus->page_mmio[first + i]  = 0;
    }
    map_changed(bus);
    return true;
}

bool mem_map_ram(mem_bus_t *bus, uint32_t base, uint32_t size, uint8_t *host)
{
    return map_direct(bus, base, size, host, PG_RAM);
}

bool mem_map_rom(mem_bus_t *bus, uint32_t base, uint32_t size, const uint8_t *host)
{
    return map_direct(bus, base, size, (uint8_t *)host, PG_ROM);
}

bool mem_map_mmio(mem_bus_t *bus, uint32_t base, uint32_t size,
                  mmio_read_fn rd, mmio_write_fn wr, void *opaque)
{
    if (!range_ok(base, size) || bus->n_mmio >= MEM_MAX_MMIO) return false;
    int idx = bus->n_mmio++;
    bus->mmio[idx] = (mmio_region_t){ base, size, rd, wr, opaque };
    uint32_t first = base >> MEM_PAGE_SHIFT;
    uint32_t count = size >> MEM_PAGE_SHIFT;
    for (uint32_t i = 0; i < count; i++) {
        bus->page_ptr[first + i]   = NULL;
        bus->page_flags[first + i] = 0;
        bus->page_mmio[first + i]  = (uint8_t)(idx + 1);
    }
    map_changed(bus);
    return true;
}

void mem_unmap(mem_bus_t *bus, uint32_t base, uint32_t size)
{
    if (!range_ok(base, size)) return;
    uint32_t first = base >> MEM_PAGE_SHIFT;
    uint32_t count = size >> MEM_PAGE_SHIFT;
    for (uint32_t i = 0; i < count; i++) {
        bus->page_ptr[first + i]   = NULL;
        bus->page_flags[first + i] = 0;
        bus->page_mmio[first + i]  = 0;
    }
    map_changed(bus);
}

/* ------------------------------------------------------------------ */
/* Access                                                              */
/* ------------------------------------------------------------------ */

static uint32_t size_mask(int size) { return size == 4 ? 0xFFFFFFFFu : (1u << (size * 8)) - 1; }

// Access that lies entirely inside one page.
static uint32_t read_in_page(mem_bus_t *bus, uint32_t addr, int size)
{
    uint32_t pg = addr >> MEM_PAGE_SHIFT;
    uint8_t *p = bus->page_ptr[pg];
    if (p) {
        uint32_t v = 0;
        memcpy(&v, p + (addr & (MEM_PAGE_SIZE - 1)), (size_t)size);
        return v;
    }
    unsigned m = bus->page_mmio[pg];
    if (m) {
        const mmio_region_t *r = &bus->mmio[m - 1];
        return r->read ? r->read(r->opaque, addr - r->base, size) & size_mask(size) : size_mask(size);
    }
    return size_mask(size);   // open bus
}

static void write_in_page(mem_bus_t *bus, uint32_t addr, uint32_t value, int size)
{
    uint32_t pg = addr >> MEM_PAGE_SHIFT;
    uint8_t *p = bus->page_ptr[pg];
    if (p) {
        uint8_t flags = bus->page_flags[pg];
        if (flags & PG_ROM) return;
        if ((flags & PG_CODE) && bus->code_write_cb &&
            bus->code_write_cb(bus->code_write_opaque, addr, (uint32_t)size))
            bus->code_written = true;
        memcpy(p + (addr & (MEM_PAGE_SIZE - 1)), &value, (size_t)size);
        return;
    }
    unsigned m = bus->page_mmio[pg];
    if (m) {
        const mmio_region_t *r = &bus->mmio[m - 1];
        if (r->write) r->write(r->opaque, addr - r->base, value & size_mask(size), size);
    }
}

uint32_t mem_read(mem_bus_t *bus, uint32_t addr, int size)
{
    uint32_t off = addr & (MEM_PAGE_SIZE - 1);
    if (off + (uint32_t)size <= MEM_PAGE_SIZE)
        return read_in_page(bus, addr, size);
    // page-crossing: assemble byte by byte (little-endian)
    uint32_t v = 0;
    for (int i = 0; i < size; i++)
        v |= read_in_page(bus, addr + (uint32_t)i, 1) << (8 * i);
    return v;
}

void mem_write(mem_bus_t *bus, uint32_t addr, uint32_t value, int size)
{
    uint32_t off = addr & (MEM_PAGE_SIZE - 1);
    if (off + (uint32_t)size <= MEM_PAGE_SIZE) {
        write_in_page(bus, addr, value, size);
        return;
    }
    for (int i = 0; i < size; i++)
        write_in_page(bus, addr + (uint32_t)i, (value >> (8 * i)) & 0xFF, 1);
}

void mem_read_block(mem_bus_t *bus, uint32_t addr, void *dst, size_t len)
{
    uint8_t *d = dst;
    while (len) {
        uint32_t off = addr & (MEM_PAGE_SIZE - 1);
        size_t chunk = MEM_PAGE_SIZE - off;
        if (chunk > len) chunk = len;
        uint8_t *p = bus->page_ptr[addr >> MEM_PAGE_SHIFT];
        if (p) memcpy(d, p + off, chunk); else memset(d, 0xFF, chunk);
        d += chunk; addr += (uint32_t)chunk; len -= chunk;
    }
}

void mem_write_block(mem_bus_t *bus, uint32_t addr, const void *src, size_t len)
{
    const uint8_t *s = src;
    while (len) {
        uint32_t pg = addr >> MEM_PAGE_SHIFT;
        uint32_t off = addr & (MEM_PAGE_SIZE - 1);
        size_t chunk = MEM_PAGE_SIZE - off;
        if (chunk > len) chunk = len;
        uint8_t *p = bus->page_ptr[pg];
        if (p && !(bus->page_flags[pg] & PG_ROM)) {
            if ((bus->page_flags[pg] & PG_CODE) && bus->code_write_cb)
                bus->code_write_cb(bus->code_write_opaque, addr, (uint32_t)chunk);
            memcpy(p + off, s, chunk);
        }
        s += chunk; addr += (uint32_t)chunk; len -= chunk;
    }
}

void mem_set_code_page(mem_bus_t *bus, uint32_t addr, bool on)
{
    uint32_t pg = addr >> MEM_PAGE_SHIFT;
    if (on) bus->page_flags[pg] |= PG_CODE;
    else    bus->page_flags[pg] &= (uint8_t)~PG_CODE;
}

void mem_set_code_write_cb(mem_bus_t *bus, code_write_fn cb, void *opaque)
{
    bus->code_write_cb = cb;
    bus->code_write_opaque = opaque;
}

void mem_set_map_change_cb(mem_bus_t *bus, void (*cb)(void *opaque), void *opaque)
{
    bus->map_change_cb = cb;
    bus->map_change_opaque = opaque;
}
