// Realtek RTL8139C Fast Ethernet (PCI 10EC:8139), the C-mode programming model:
//   - registers in an I/O BAR (BAR 0) and the same registers in a memory BAR (BAR 1)
//   - transmit through four descriptors (TSAD0-3 buffer address, TSD0-3 size/status)
//   - receive into one ring buffer (RBSTART, 8-64 KB) of [status, length, frame, CRC] records,
//     with CAPR (driver read pointer - 16) and CBR (device write pointer)
//   - interrupt status/mask (ISR/IMR), PCI INTA level = (ISR & IMR) != 0
//   - 93C46 serial EEPROM (MAC address at words 7-9) bit-banged through 9346CR
//   - PHY registers report a 100 Mb/s full-duplex link that is always up
// Frames go to and come from a net_backend_t (core/net.h); DMA uses the memory bus,
// so writes into translated code still invalidate it.
#ifndef RTL8139_H
#define RTL8139_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "io.h"
#include "mem.h"
#include "pci.h"

typedef struct rtl8139 {
    pci_dev_t pci;
    mem_bus_t *bus;
    io_bus_t  *io;
    uint16_t  io_base;            // where BAR 0 is mapped
    uint8_t   mac[6];

    // registers
    uint8_t   mar[8];             // multicast filter (stored; all multicast is accepted when AM is set)
    uint32_t  tsd[4], tsad[4];
    uint32_t  rbstart;
    uint8_t   cr;                 // RE / TE (RST self-clears, BUFE is computed)
    uint16_t  imr, isr;
    uint32_t  tcr, rcr;
    uint32_t  mpc;                // missed packet counter
    uint8_t   cfg9346;            // EEM bits and the pins the driver drives
    uint8_t   config[6];          // CONFIG0..5 (index 2 unused)
    uint32_t  timer_int;          // TimerInt (interrupt when TCTR reaches it)
    uint64_t  tctr_base_ns;       // TCTR counts the 33 MHz PCI clock from here
    bool      timer_fired;
    uint16_t  bmcr, anar;
    uint16_t  mulint;
    uint32_t  raw[64];            // registers with no side effects, kept as written (by dword)

    // receive ring
    uint32_t  rx_read;            // CAPR + 16, driver side
    uint32_t  rx_write;           // CBR, device side

    // 93C46
    uint16_t  eeprom[64];
    int       ee_state;           // 0 idle, 1 taking command bits, 2 reading out, 3 ignoring
    uint32_t  ee_bits, ee_count;
    uint16_t  ee_out;
    uint8_t   ee_addr;
    bool      ee_do, ee_sk;

    // wiring
    void    (*set_intx)(void *opaque, int level);   // PCI INTA#
    void     *intx_opaque;
    void    (*send)(void *opaque, const uint8_t *frame, size_t len);   // to the network
    void     *send_opaque;
    uint64_t (*clock)(void *opaque);
    void     *clock_opaque;
    uint64_t  tx_frames, rx_frames, rx_dropped;
} rtl8139_t;

void rtl8139_init(rtl8139_t *n, mem_bus_t *bus, io_bus_t *io, const uint8_t mac[6],
                  uint64_t (*clock)(void *), void *clock_opaque);
void rtl8139_reset(rtl8139_t *n);      // power-on reset (unmaps BARs too)

// Frames from the network. Returns false if the NIC cannot take it now (receiver
// off or ring full); the caller may keep it and try again later.
bool rtl8139_can_receive(const rtl8139_t *n, size_t len);
bool rtl8139_receive(rtl8139_t *n, const uint8_t *frame, size_t len);

// Advance the TCTR timer interrupt.
void rtl8139_update(rtl8139_t *n);

#endif
