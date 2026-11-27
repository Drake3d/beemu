// PIIX3 IDE (PCI 8086:7010) in legacy mode: primary channel 0x1F0-0x1F7/0x3F6
// on IRQ 14, secondary 0x170-0x177/0x376 on IRQ 15, plus bus-master DMA
// registers behind PCI BAR 4. Each channel holds up to two devices:
//   - ATA hard disks backed by raw images (PIO, READ/WRITE MULTIPLE, DMA,
//     LBA28/LBA48 and CHS addressing)
//   - ATAPI CD-ROMs backed by ISO files (MMC packet commands, PIO and DMA)
// Commands complete instantly; the guest never sees BSY.
#ifndef IDE_H
#define IDE_H

#include <stdbool.h>
#include <stdint.h>
#include "disk.h"
#include "io.h"
#include "mem.h"
#include "pci.h"

typedef enum { IDE_NONE = 0, IDE_HD, IDE_CD } ide_type_t;

typedef enum { XF_NONE, XF_PIO_IN, XF_PIO_OUT, XF_PACKET, XF_ATAPI_IN, XF_ATAPI_OUT, XF_DMA } ide_xfer_t;

#define IDE_BUF_SIZE (128u * 1024u)

typedef struct {
    ide_type_t type;
    disk_t    *disk;
    uint64_t   sectors;            // in 512-byte (HD) or 2048-byte (CD) units
    uint16_t   cyls, heads, spt;   // default CHS geometry (IDENTIFY words 1, 3, 6)
    uint16_t   cur_heads, cur_spt; // set by INITIALIZE DEVICE PARAMETERS
    uint8_t    multiple;           // READ/WRITE MULTIPLE block size, 0 = disabled
    uint8_t    status, error;
    uint8_t    dma_mode;           // SET FEATURES transfer mode (stored for IDENTIFY)

    // current data transfer
    ide_xfer_t xfer;
    uint8_t   *buf;
    uint32_t   pos, len;           // PIO: position and size of the current DRQ block in buf
    uint64_t   lba;                // next sector of a disk transfer
    uint32_t   remaining;          // sectors (HD) or bytes (ATAPI) still to transfer
    uint32_t   block;              // sectors per DRQ block (HD PIO)
    bool       dma_to_mem;         // DMA direction
    uint8_t    cmd;

    // ATAPI
    uint8_t    packet[12];
    uint8_t    sense_key, asc, ascq;
    uint32_t   byte_limit;         // PIO bytes per DRQ (cylinder registers at PACKET time)
    bool       atapi_dma;          // PACKET with the DMA feature bit
    bool       src_disk;           // reply data streams from the image (READ) or from reply[]
    uint64_t   src_off;            // next image offset (src_disk)
    uint8_t   *reply;              // non-disk reply data
    uint32_t   reply_pos;
    char       model[41], serial[21];
} ide_dev_t;

struct ide;

typedef struct {
    ide_dev_t dev[2];
    int       cur;                 // selected device (device/head bit 4)
    uint8_t   feature, count, lba_lo, lba_mid, lba_hi;
    uint8_t   hob_feature, hob_count, hob_lba_lo, hob_lba_mid, hob_lba_hi;
    uint8_t   select;              // device/head register
    uint8_t   ctl;                 // device control (nIEN, SRST, HOB)
    bool      intrq;
    int       irq;
    uint8_t   bm_cmd, bm_status;
    uint32_t  bm_prdt;
    struct ide *ide;
} ide_channel_t;

typedef struct ide {
    ide_channel_t ch[2];
    mem_bus_t    *bus;
    io_bus_t     *io;
    void        (*set_irq)(void *opaque, int irq, int level);
    void         *irq_opaque;
    pci_dev_t     pci;
    uint8_t      *dma_tmp;
} ide_t;

bool ide_init(ide_t *ide, mem_bus_t *bus, io_bus_t *io, void (*set_irq)(void *, int, int), void *irq_opaque);
void ide_free(ide_t *ide);
// Attach before ide_register. slot: 0 primary master, 1 primary slave,
// 2 secondary master, 3 secondary slave. The disk stays owned by the caller.
bool ide_attach(ide_t *ide, int slot, ide_type_t type, disk_t *disk);
void ide_register(ide_t *ide);     // claim the legacy ports of channels with devices
void ide_reset(ide_t *ide);        // hardware reset (machine reset)

#endif
