// ISA DMA: two 8237 controllers as in the PC/AT. Channels 0-3 (8-bit, ports 0x00-0x0F) and
// 4-7 (16-bit, ports 0xC0-0xDF; channel 4 cascades), page registers at 0x81-0x8F.
// Devices pull or push data with dma_read/dma_write; the controller walks guest memory,
// counts down, sets terminal count and reloads in auto-initialize mode. Single, demand and
// block modes all behave as "transfer what the device asks for". No memory-to-memory.
#ifndef DMA_H
#define DMA_H

#include <stdbool.h>
#include <stdint.h>
#include "io.h"
#include "mem.h"

typedef struct {
    uint16_t base_addr, base_count;   // as programmed (words on channels 4-7)
    uint16_t addr, count;             // current
    uint8_t  page;
    uint8_t  mode;                    // 8237 mode register (bits 7-2; channel bits dropped)
} dma_chan_t;

typedef struct dma {
    mem_bus_t *bus;
    dma_chan_t ch[8];
    uint8_t    mask;                  // bit n = channel n masked
    uint8_t    status[2];             // terminal count bits 0-3 per controller
    bool       flipflop[2];
    uint8_t    cmd[2], temp[2];
    uint8_t    page_extra[16];        // the unused page registers (0x80, 0x84-0x86, ...)
} dma_t;

void dma_init(dma_t *d, mem_bus_t *bus);
void dma_reset(dma_t *d);
void dma_register(dma_t *d, io_bus_t *io);

// Transfer up to len bytes for channel ch (device -> memory for dma_write, memory -> device
// for dma_read). Returns the bytes moved: fewer when the channel is masked or reaches terminal
// count without auto-init. 16-bit channels move whole words (len rounded down to even).
int  dma_read(dma_t *d, int ch, void *buf, int len);
int  dma_write(dma_t *d, int ch, const void *buf, int len);
bool dma_masked(const dma_t *d, int ch);
// Bytes left before terminal count on channel ch.
uint32_t dma_remaining(const dma_t *d, int ch);

#endif
