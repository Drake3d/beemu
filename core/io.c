#include "io.h"
#include <stdlib.h>

io_bus_t *io_create(void) { return calloc(1, sizeof(io_bus_t)); }
void io_destroy(io_bus_t *bus) { free(bus); }

bool io_register(io_bus_t *bus, uint16_t base, uint16_t count,
                 io_read_fn rd, io_write_fn wr, void *opaque)
{
    if (count == 0 || (uint32_t)base + count > 65536u || bus->n_handlers >= IO_MAX_HANDLERS)
        return false;
    for (uint32_t p = base; p < (uint32_t)base + count; p++)
        if (bus->port_handler[p]) return false;
    int slot = -1;
    for (int i = 0; i < bus->n_handlers; i++) {
        io_handler_t *h = &bus->handlers[i];
        if (h->read == rd && h->write == wr && h->opaque == opaque && h->count == count) { slot = i; break; }
    }
    if (slot < 0) slot = bus->n_handlers++;
    bus->handlers[slot] = (io_handler_t){ base, count, rd, wr, opaque };
    for (uint32_t p = base; p < (uint32_t)base + count; p++)
        bus->port_handler[p] = (uint8_t)(slot + 1);
    return true;
}

void io_unregister(io_bus_t *bus, uint16_t base, uint16_t count)
{
    for (uint32_t p = base; p < (uint32_t)base + count && p < 65536; p++) bus->port_handler[p] = 0;
}

static uint32_t size_mask(int size) { return size >= 4 ? 0xFFFFFFFFu : (1u << (8 * size)) - 1u; }

uint32_t io_read(io_bus_t *bus, uint16_t port, int size)
{
    if (bus && bus->port_handler[port]) {
        const io_handler_t *h = &bus->handlers[bus->port_handler[port] - 1];
        if (h->read) return h->read(h->opaque, port, size) & size_mask(size);
    }
    return size_mask(size);
}

void io_write(io_bus_t *bus, uint16_t port, uint32_t value, int size)
{
    if (bus && bus->port_handler[port]) {
        const io_handler_t *h = &bus->handlers[bus->port_handler[port] - 1];
        if (h->write) h->write(h->opaque, port, value & size_mask(size), size);
    }
}
