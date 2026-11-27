// PCI bus 0 with configuration mechanism #1 (0xCF8 address, 0xCFC-0xCFF data).
// Devices are 256-byte config spaces with per-byte write masks. BARs are sized
// by their masks; when a BAR's address or the command register's enable bits
// change, the device's map callback is told where the BAR now lives.
#ifndef PCI_H
#define PCI_H

#include <stdbool.h>
#include <stdint.h>
#include "io.h"

#define PCI_ROM_BAR 6

typedef struct pci_dev pci_dev_t;
struct pci_dev {
    uint8_t  cfg[256];
    uint8_t  wmask[256];
    uint32_t bar_size[7];     // 0 = not implemented; index 6 = expansion ROM
    bool     bar_io[7];
    uint32_t bar_addr[7];     // currently mapped address (valid if bar_on)
    bool     bar_on[7];
    // Called when BAR `bar` moves or turns on/off.
    void   (*map)(pci_dev_t *d, int bar, uint32_t addr, bool on);
    // Called after any config write (for chipset registers like PAM).
    void   (*written)(pci_dev_t *d, int off, int size);
    void    *opaque;
};

typedef struct {
    pci_dev_t *dev[32][8];
    uint32_t   addr;           // 0xCF8
    void     (*reset)(void *opaque);   // 0xCF9 reset control (PIIX)
    void      *reset_opaque;
    uint8_t    rcr;
} pci_bus_t;

void pci_bus_init(pci_bus_t *b);
void pci_bus_register(pci_bus_t *b, io_bus_t *io);
void pci_add(pci_bus_t *b, int slot, int fn, pci_dev_t *d);

// Device setup helpers
void pci_set_ids(pci_dev_t *d, uint16_t vendor, uint16_t device, uint8_t rev,
                 uint32_t class_code /* class<<16 | subclass<<8 | prog-if */,
                 uint16_t sub_vendor, uint16_t sub_device);
void pci_set_bar(pci_dev_t *d, int bar, uint32_t size, bool io, bool prefetch);
void pci_set_wmask(pci_dev_t *d, int off, int len, uint8_t mask);

uint32_t pci_cfg_read (pci_dev_t *d, int off, int size);
void     pci_cfg_write(pci_dev_t *d, int off, uint32_t v, int size);

#endif
