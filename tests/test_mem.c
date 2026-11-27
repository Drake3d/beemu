// Memory bus tests.
#include "mem.h"
#include <stdio.h>
#include <string.h>

static int g_fail, g_run;

#define CHECK(cond, msg) do { g_run++; if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); g_fail++; } } while (0)

typedef struct { uint32_t last_off, last_val; int last_size; int reads, writes; } mmio_log_t;

static uint32_t mmio_rd(void *o, uint32_t off, int size)
{
    mmio_log_t *l = o; l->reads++; l->last_off = off; l->last_size = size;
    return 0xA5A5A5A5u;
}
static void mmio_wr(void *o, uint32_t off, uint32_t v, int size)
{
    mmio_log_t *l = o; l->writes++; l->last_off = off; l->last_val = v; l->last_size = size;
}

static int g_cb_count;
static mem_bus_t *g_bus;
static bool on_code_write(void *o, uint32_t addr, uint32_t len)
{
    (void)o; (void)len; g_cb_count++;
    mem_set_code_page(g_bus, addr & ~0xFFFu, false);   // translator drops its blocks and unflags
    return true;
}

int main(void)
{
    mem_bus_t *bus = mem_create();
    g_bus = bus;
    CHECK(bus != NULL, "create");

    // ---- RAM: sizes, little-endian layout ----
    uint8_t *ram = mem_alloc_host(1 << 20);
    CHECK(mem_map_ram(bus, 0, 1 << 20, ram), "map ram");
    mem_write(bus, 0x100, 0x11223344, 4);
    CHECK(mem_read(bus, 0x100, 4) == 0x11223344, "rw32");
    CHECK(mem_read(bus, 0x100, 1) == 0x44, "little-endian byte 0");
    CHECK(mem_read(bus, 0x101, 1) == 0x33, "little-endian byte 1");
    CHECK(mem_read(bus, 0x102, 2) == 0x1122, "rw16 upper half");
    mem_write(bus, 0x101, 0xAB, 1);
    CHECK(mem_read(bus, 0x100, 4) == 0x1122AB44, "byte write merges");

    // ---- page-crossing access ----
    mem_write(bus, 0xFFE, 0xDEADBEEF, 4);
    CHECK(mem_read(bus, 0xFFE, 4) == 0xDEADBEEF, "page-cross 32");
    CHECK(mem_read(bus, 0xFFE, 2) == 0xBEEF, "page-cross low half");
    CHECK(mem_read(bus, 0x1000, 2) == 0xDEAD, "page-cross high half");

    // ---- unmapped: open bus ----
    CHECK(mem_read(bus, 0x40000000, 4) == 0xFFFFFFFFu, "unmapped read32");
    CHECK(mem_read(bus, 0x40000000, 1) == 0xFF, "unmapped read8");
    mem_write(bus, 0x40000000, 1, 4);   // must not crash

    // ---- ROM: writes ignored, alias at two addresses (BIOS-style) ----
    uint8_t *rom = mem_alloc_host(64 * 1024);
    memset(rom, 0x90, 64 * 1024);
    rom[0xFFF0] = 0xEA;
    CHECK(mem_map_rom(bus, 0xFFFF0000u, 64 * 1024, rom), "map rom high");
    CHECK(mem_map_rom(bus, 0x000F0000u, 64 * 1024, rom), "map rom alias");   // overlaps RAM: replaces it
    CHECK(mem_read(bus, 0xFFFFFFF0u, 1) == 0xEA, "reset vector at top of 4GB");
    CHECK(mem_read(bus, 0x000FFFF0u, 1) == 0xEA, "reset vector alias");
    mem_write(bus, 0xFFFFFFF0u, 0x00, 1);
    CHECK(mem_read(bus, 0xFFFFFFF0u, 1) == 0xEA, "rom write ignored");

    // ---- MMIO dispatch ----
    mmio_log_t log = {0};
    CHECK(mem_map_mmio(bus, 0xA0000, 0x20000, mmio_rd, mmio_wr, &log), "map mmio");
    CHECK(mem_read(bus, 0xA0010, 2) == 0xA5A5, "mmio read masked to size");
    CHECK(log.last_off == 0x10 && log.last_size == 2, "mmio offset and size");
    mem_write(bus, 0xB8000, 0x12345678, 4);
    CHECK(log.writes == 1 && log.last_off == 0x18000 && log.last_val == 0x12345678 && log.last_size == 4, "mmio write");

    // ---- block copy ----
    const char msg[] = "hello, beos";
    mem_write_block(bus, 0x2000, msg, sizeof msg);
    char out[sizeof msg] = {0};
    mem_read_block(bus, 0x2000, out, sizeof out);
    CHECK(memcmp(msg, out, sizeof msg) == 0, "block roundtrip");
    uint8_t cross_in[8] = {1,2,3,4,5,6,7,8}, cross_out[8] = {0};
    mem_write_block(bus, 0x3FFC, cross_in, 8);   // spans two pages
    mem_read_block(bus, 0x3FFC, cross_out, 8);
    CHECK(memcmp(cross_in, cross_out, 8) == 0, "block page-cross");

    // ---- self-modifying-code hook ----
    mem_set_code_write_cb(bus, on_code_write, NULL);
    mem_write(bus, 0x5000, 1, 4);
    CHECK(g_cb_count == 0, "no callback for unflagged page");
    mem_set_code_page(bus, 0x5000, true);
    mem_write(bus, 0x5004, 2, 4);
    CHECK(g_cb_count == 1, "callback fires on code page write");
    mem_write(bus, 0x5008, 3, 4);
    CHECK(g_cb_count == 1, "callback cleared flag, no second fire");
    CHECK(mem_read(bus, 0x5004, 4) == 2, "write still lands after callback");

    // ---- bad arguments ----
    CHECK(!mem_map_ram(bus, 0x123, 4096, ram), "unaligned base rejected");
    CHECK(!mem_map_ram(bus, 0, 100, ram), "unaligned size rejected");
    CHECK(!mem_map_ram(bus, 0xFFFFF000u, 8192, ram), "range past 4GB rejected");

    mem_unmap(bus, 0, 1 << 20);
    CHECK(mem_read(bus, 0x100, 4) == 0xFFFFFFFFu, "unmapped after mem_unmap");

    mem_free_host(ram);
    mem_free_host(rom);
    mem_destroy(bus);
    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
