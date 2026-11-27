// Creative Sound Blaster 16 PnP (card CTL0028, audio function CTL0031) for BeOS R5's stock awe64
// driver, which accepts any CTL003x / CTL004x audio function. ISA Plug and Play only: BeOS's
// ISA bus module runs the isolation protocol itself (initiation key on 0x279, read-data port it
// picks, serial isolation), reads the resource data and assigns base / MPU ports, an IRQ and two
// DMA channels. Nothing responds until it activates the card.
//
// DSP 4.13: reset, version/ID/copyright, rate (0x41/0x42), 8/16-bit single-cycle and auto-init
// DMA (0xB0-0xCF), pause/continue/exit, speaker, IRQ triggers. CT1745 mixer with master/voice
// volumes and the IRQ/DMA status registers. MPU-401 answers reset and UART mode, nothing more.
// No OPL3 FM, no EMU8000. Playback is read through the 8237 (dma.h) as host time passes,
// resampled to SOUND_RATE and handed to out(); capture writes silence.
#ifndef SB16_H
#define SB16_H

#include <stdbool.h>
#include <stdint.h>
#include "dma.h"
#include "io.h"
#include "sound.h"

typedef struct sb16 {
    io_bus_t *io;
    dma_t    *dma;

    // ISA Plug and Play
    int      pnp_state;              // wait-for-key, sleep, isolation, config
    int      key_pos;                // initiation key bytes matched so far
    uint8_t  pnp_addr;               // register selected through 0x279
    uint16_t read_port;              // 0 = none claimed
    uint8_t  csn;
    int      iso_bit;                // serial isolation: next bit (0-71), second read of the pair
    bool     iso_second;
    int      res_pos;                // resource data read pointer (serial id first)
    uint8_t  ldn_regs[256];          // logical device 0 registers (0x30-0xFF used)
    uint16_t base, mpu_base;         // claimed while active (0 = none)
    int      irq, dma8, dma16;

    // DSP
    bool     reset_high;
    uint8_t  cmd, params[3];
    int      n_params, need;
    uint8_t  out[64];
    int      out_r, out_n;
    uint8_t  test_reg, time_const;
    bool     speaker;
    uint32_t rate;                   // output sample rate
    uint16_t block8;                 // 0x48 block size (8-bit auto-init)

    // DMA playback / capture engine
    bool     run, run16, autoinit, stereo, is_signed, input, paused, exit_auto;
    uint32_t units, left;            // units per block (8 or 16-bit samples per channel x channels), left in block
    double   phase;
    int16_t  prev[2], cur[2];
    bool     irq8, irq16, irq_mpu, irq_line;

    // mixer
    uint8_t  mix_idx, mix[256];

    // MPU-401
    uint8_t  mpu_ack;
    bool     mpu_has_ack;

    uint64_t (*clock)(void *opaque);
    void     *clock_opaque;
    uint64_t  t0, produced;

    void (*set_irq)(void *opaque, int irq, int level);
    void  *irq_opaque;
    sound_out_fn out_fn;
    void  *out_opaque;
} sb16_t;

void sb16_init(sb16_t *s, io_bus_t *io, dma_t *dma, uint64_t (*clock)(void *), void *clock_opaque);
void sb16_reset(sb16_t *s);
// Advance to the current host time. Returns ns until the next interrupt is due (UINT64_MAX if none).
uint64_t sb16_update(sb16_t *s);

#endif
