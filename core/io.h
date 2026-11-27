// Guest port I/O bus (the x86 IN/OUT address space, ports 0..0xFFFF).
//
// Devices register a port range with read/write callbacks. A multi-byte access
// goes to the handler that owns its first port, with the access size, and the
// device decides what it means. Unclaimed ports read as all ones and ignore writes.
#ifndef IO_H
#define IO_H

#include <stdbool.h>
#include <stdint.h>

#define IO_MAX_HANDLERS 128

typedef uint32_t (*io_read_fn)(void *opaque, uint16_t port, int size);
typedef void     (*io_write_fn)(void *opaque, uint16_t port, uint32_t value, int size);

typedef struct {
    uint16_t    base, count;
    io_read_fn  read;     // may be NULL: reads return all ones
    io_write_fn write;    // may be NULL: writes ignored
    void       *opaque;
} io_handler_t;

typedef struct io_bus {
    uint8_t      port_handler[65536];   // 1-based index into handlers[], 0 = unclaimed
    io_handler_t handlers[IO_MAX_HANDLERS];
    int          n_handlers;
} io_bus_t;

io_bus_t *io_create(void);
void      io_destroy(io_bus_t *bus);

// Claim ports base .. base+count-1. Fails if any port is already claimed or the table is full.
bool io_register(io_bus_t *bus, uint16_t base, uint16_t count,
                 io_read_fn rd, io_write_fn wr, void *opaque);

// Release ports base .. base+count-1 (devices whose ports move, like PCI I/O BARs).
// Registering the same callbacks and opaque again reuses their handler slot.
void io_unregister(io_bus_t *bus, uint16_t base, uint16_t count);

// size is 1, 2 or 4. A NULL bus behaves like an empty one.
uint32_t io_read (io_bus_t *bus, uint16_t port, int size);
void     io_write(io_bus_t *bus, uint16_t port, uint32_t value, int size);

#endif
