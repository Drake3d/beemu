// Guest physical memory bus.
//
// A flat 4 GB physical address space split into 4 KB pages. Each page is one of:
//   - RAM  (direct host pointer, read/write)
//   - ROM  (direct host pointer, writes ignored; the same host buffer can be
//           mapped at several addresses, which is how the BIOS alias works)
//   - MMIO (device callbacks)
//   - unmapped (reads return all ones, writes are dropped)
//
// The JIT's fast path will read page_ptr[] directly; everything else goes
// through mem_read / mem_write. Little-endian host assumed (Apple Silicon).
#ifndef MEM_H
#define MEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MEM_PAGE_SHIFT 12
#define MEM_PAGE_SIZE  (1u << MEM_PAGE_SHIFT)
#define MEM_NUM_PAGES  (1u << (32 - MEM_PAGE_SHIFT))
#define MEM_MAX_MMIO   64

enum {
    PG_RAM  = 1,
    PG_ROM  = 2,
    PG_CODE = 4,   // page holds translated code: writes trigger the callback
};

typedef uint32_t (*mmio_read_fn)(void *opaque, uint32_t offset, int size);
typedef void     (*mmio_write_fn)(void *opaque, uint32_t offset, uint32_t value, int size);

typedef struct {
    uint32_t base, size;
    mmio_read_fn  read;
    mmio_write_fn write;
    void *opaque;
} mmio_region_t;

// Called before a write to a page flagged PG_CODE, with the written range (inside one
// page). Returns true if translated code was affected (the bus then sets code_written).
typedef bool (*code_write_fn)(void *opaque, uint32_t addr, uint32_t len);

typedef struct mem_bus {
    uint8_t      *page_ptr[MEM_NUM_PAGES];    // host pointer to the page, or NULL
    uint8_t       page_flags[MEM_NUM_PAGES];  // PG_*
    uint8_t       page_mmio[MEM_NUM_PAGES];   // 1-based index into mmio[], 0 = none
    mmio_region_t mmio[MEM_MAX_MMIO];
    int           n_mmio;
    void        (*map_change_cb)(void *opaque);   // any page's mapping changed
    void         *map_change_opaque;
    code_write_fn code_write_cb;
    bool          code_written;   // set when a write hit a code page (the JIT checks and clears it)
    void         *code_write_opaque;
} mem_bus_t;

mem_bus_t *mem_create(void);
void       mem_destroy(mem_bus_t *bus);

// Host buffer suitable for guest RAM: zeroed, 16 KB aligned (Apple Silicon page size).
// Size is rounded up to a multiple of 16 KB. Free with mem_free_host().
uint8_t *mem_alloc_host(size_t size);
void     mem_free_host(uint8_t *p);

// Mapping. base and size must be multiples of 4 KB. Return false on bad arguments.
bool mem_map_ram (mem_bus_t *bus, uint32_t base, uint32_t size, uint8_t *host);
bool mem_map_rom (mem_bus_t *bus, uint32_t base, uint32_t size, const uint8_t *host);
bool mem_map_mmio(mem_bus_t *bus, uint32_t base, uint32_t size,
                  mmio_read_fn rd, mmio_write_fn wr, void *opaque);
void mem_unmap   (mem_bus_t *bus, uint32_t base, uint32_t size);

// Accesses of 1, 2 or 4 bytes. Unaligned and page-crossing accesses are handled.
uint32_t mem_read (mem_bus_t *bus, uint32_t addr, int size);
void     mem_write(mem_bus_t *bus, uint32_t addr, uint32_t value, int size);

// Bulk copy in/out of guest memory (RAM/ROM pages only; MMIO pages are skipped).
void mem_read_block (mem_bus_t *bus, uint32_t addr, void *dst, size_t len);
void mem_write_block(mem_bus_t *bus, uint32_t addr, const void *src, size_t len);

// Self-modifying-code support: flag a page as holding translated code. A write
// to a flagged page calls the callback before the write proceeds; the callback
// decides whether the bytes overlap code, drops the affected blocks, and clears
// the flag (mem_set_code_page(..., false)) once the page holds no code.
void mem_set_code_page(mem_bus_t *bus, uint32_t addr, bool on);
void mem_set_code_write_cb(mem_bus_t *bus, code_write_fn cb, void *opaque);

// Called after any mem_map_* / mem_unmap (cached host pointers must be dropped).
void mem_set_map_change_cb(mem_bus_t *bus, void (*cb)(void *opaque), void *opaque);

#endif
