// Ensoniq AudioPCI ES1370 (PCI 1274:5000) with its AK4531 codec, for BeOS's stock es137x
// driver (R5 and Dano). One 64-byte I/O BAR.
//
// What the driver uses (found by disassembling R5's es137x; see CLAUDE.md):
//   - DAC2 for playback: frame buffer address/size through memory page 0xC (0x38/0x3C), the
//     interrupt interval in the DAC2 sample count (0x28), rate in CTRL bits 16-28
//     (1411200 / rate - 2), format in SCTRL bits 2-3; play position = high half of 0x3C.
//   - interrupts acknowledged by clearing and setting the channel's SCTRL interrupt enable.
// DAC1 (fixed rates, "FM") is played too; the ADC records silence so capture doesn't hang.
// Channels are read from guest memory as host time passes, resampled to SOUND_RATE and mixed;
// the result goes to out() in blocks.
#ifndef ES1370_H
#define ES1370_H

#include <stdbool.h>
#include <stdint.h>
#include "io.h"
#include "mem.h"
#include "pci.h"
#include "sound.h"

typedef struct {
    uint32_t scount;        // low: samples per interrupt - 1, high: current countdown
    uint32_t frame_addr;    // guest physical address of the buffer
    uint32_t frame_cnt;     // low: buffer size in dwords - 1, high: current dword
    uint32_t sub;           // byte offset inside the current dword
    double   phase;         // resampling: fraction of a channel sample per output frame
    int16_t  prev[2], cur[2];
} es1370_chan_t;

typedef struct es1370 {
    pci_dev_t pci;
    mem_bus_t *bus;
    io_bus_t  *io;
    uint16_t   io_base;
    bool       io_on;

    uint32_t ctl, status, sctl, mempage;
    uint8_t  uart_ctl;
    uint8_t  codec[0x20];   // AK4531 registers (write-only on the real chip)
    double   gain[2][2];    // [DAC1/DAC2][L/R] from the codec volumes, recomputed when they change
    bool     gain_dirty;
    es1370_chan_t ch[3];    // DAC1, DAC2, ADC

    uint64_t (*clock)(void *opaque);
    void     *clock_opaque;
    uint64_t  t0;           // host time of output frame 0
    uint64_t  produced;     // output frames made since t0

    void (*set_intx)(void *opaque, int level);
    void  *intx_opaque;
    sound_out_fn out;       // NULL: sound is computed and dropped
    void  *out_opaque;
} es1370_t;

void es1370_init(es1370_t *e, mem_bus_t *bus, io_bus_t *io, uint64_t (*clock)(void *), void *clock_opaque);
void es1370_reset(es1370_t *e);
// Advance to the current host time: fetch samples, raise interrupts, emit sound.
// Returns ns until the next interrupt is due (UINT64_MAX if none).
uint64_t es1370_update(es1370_t *e);

// Register access (exposed for tests): offset inside the I/O BAR.
uint32_t es1370_read(es1370_t *e, uint32_t off, int size);
void     es1370_write(es1370_t *e, uint32_t off, uint32_t v, int size);

#endif
