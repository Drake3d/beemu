// NE2000-compatible PCI Ethernet: Realtek RTL8029 (PCI 10EC:8029), what QEMU calls ne2k_pci.
// A National DP8390 core behind a 32-byte I/O BAR:
//   0x00-0x0F  DP8390 registers, three pages selected by CR.PS (page 3: RTL8029 config)
//   0x10-0x17  data port: remote DMA into/out of the card's buffer memory, byte or word
//              wide by DCR.WTS (a dword access moves 4 bytes)
//   0x18-0x1F  reset port: a read resets the chip
// Buffer memory (card addresses):
//   0x0000-0x001F  station address PROM: MAC in bytes 0-5, 0x57 'W' in 14-15 (word card),
//                  every byte stored twice (what drivers see through 16-bit remote DMA)
//   0x4000-0xBFFF  packet RAM, in 256-byte pages (32 KB like QEMU; a real RTL8029 has 16 KB
//                  at 0x4000-0x7FFF, which drivers use)
// Transmit sends TBCR bytes from page TPSR. Receive writes [status, next page, length lo/hi]
// + frame into the PSTART..PSTOP ring at CURR; the driver frees pages by moving BNRY.
// The length field counts the 4-byte header and the frame (no CRC), as QEMU does.
// INTA level = (ISR & IMR & 0x7F) != 0.
#ifndef NE2K_H
#define NE2K_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "io.h"
#include "pci.h"

#define NE2K_MEM_SIZE   0xC000u
#define NE2K_PMEM_START 0x4000u

typedef struct ne2k {
    pci_dev_t pci;
    io_bus_t *io;
    uint16_t  io_base;            // where BAR 0 is mapped
    uint8_t   mac[6];             // in the PROM (PAR0-5 are what the driver programs)

    uint8_t   cr;                 // command: STP/STA/TXP/RD/PS
    uint8_t   isr, imr;
    uint8_t   tsr, rsr;
    uint8_t   rcr, tcr, dcr;
    uint16_t  pstart, pstop;      // ring bounds as card addresses (page << 8)
    uint8_t   bnry, curr, tpsr;
    uint16_t  tbcr;
    uint16_t  rsar, rbcr;         // remote DMA address and remaining count
    uint8_t   par[6], mar[8];
    uint8_t   mem[NE2K_MEM_SIZE];

    // wiring
    void    (*set_intx)(void *opaque, int level);   // PCI INTA#
    void     *intx_opaque;
    void    (*send)(void *opaque, const uint8_t *frame, size_t len);   // to the network
    void     *send_opaque;
    uint64_t  tx_frames, rx_frames, rx_dropped;
} ne2k_t;

void ne2k_init(ne2k_t *n, io_bus_t *io, const uint8_t mac[6]);
void ne2k_reset(ne2k_t *n);      // power-on reset (unmaps the BAR too)

// Frames from the network. can_receive is false while the ring has no room for a
// full-size frame (the caller keeps the frame and retries). With the chip stopped
// frames are dropped. receive returns false if the frame could not be stored.
bool ne2k_can_receive(const ne2k_t *n, size_t len);
bool ne2k_receive(ne2k_t *n, const uint8_t *frame, size_t len);

#endif
