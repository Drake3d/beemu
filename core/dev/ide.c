#include "ide.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_trace_burst;      // detailed register trace for a while after IDENTIFY (debugging)

#define ST_ERR  0x01
#define ST_DRQ  0x08
#define ST_DSC  0x10
#define ST_DRDY 0x40
#define ST_BSY  0x80
#define ER_ABRT 0x04
#define ER_IDNF 0x10
#define ER_UNC  0x40

#define IR_COD 0x01        // ATAPI interrupt reason (sector count register)
#define IR_IO  0x02

#define BM_ACTIVE 0x01
#define BM_ERROR  0x02
#define BM_INT    0x04

#define SENSE_NONE 0
#define SENSE_NOT_READY 2
#define SENSE_ILLEGAL 5

static const uint16_t LEGACY_BASE[2] = { 0x1F0, 0x170 };
static const uint16_t LEGACY_CTL[2]  = { 0x3F6, 0x376 };
static const int      LEGACY_IRQ[2]  = { 14, 15 };

/* ------------------------------------------------------------------ */
/* Basics                                                              */
/* ------------------------------------------------------------------ */

static ide_dev_t *cur_dev(ide_channel_t *ch) { return &ch->dev[ch->cur]; }

static bool present(const ide_channel_t *ch, int i) { return ch->dev[i].type != IDE_NONE; }

static void update_irq(ide_channel_t *ch)
{
    ch->ide->set_irq(ch->ide->irq_opaque, ch->irq, ch->intrq && !(ch->ctl & 2));
}

static void raise_irq(ide_channel_t *ch)
{
    // The PIIX bus-master status Interrupt bit follows the drive's INTRQ edge, DMA or not;
    // drivers that share IRQs (BeOS's) check it to see whether the interrupt is theirs.
    ch->bm_status |= 0x04;
    if (g_trace_burst) fprintf(stderr, "[ide] irq raise (nIEN %d)\n", (ch->ctl & 2) != 0);
    ch->intrq = true;
    update_irq(ch);
}

static void set_signature(ide_channel_t *ch, const ide_dev_t *d)
{
    ch->count = 1;
    ch->lba_lo = 1;
    ch->lba_mid = d->type == IDE_CD ? 0x14 : 0;
    ch->lba_hi = d->type == IDE_CD ? 0xEB : 0;
}

static void reset_dev(ide_dev_t *d)
{
    d->xfer = XF_NONE;
    d->error = 0x01;                               // diagnostics passed
    d->status = d->type == IDE_HD ? (ST_DRDY | ST_DSC) : 0;
    d->multiple = 0;
    d->cur_heads = d->heads;
    d->cur_spt = d->spt;
    d->sense_key = d->asc = d->ascq = 0;
}

static void finish_ok(ide_channel_t *ch, ide_dev_t *d, bool irq)
{
    d->xfer = XF_NONE;
    d->status = ST_DRDY | ST_DSC;
    d->error = 0;
    if (irq) raise_irq(ch);
}

static void finish_err(ide_channel_t *ch, ide_dev_t *d, uint8_t err)
{
    d->xfer = XF_NONE;
    d->status = ST_DRDY | ST_DSC | ST_ERR;
    d->error = err;
    raise_irq(ch);
}

/* ------------------------------------------------------------------ */
/* IDENTIFY                                                            */
/* ------------------------------------------------------------------ */

static void put_str(uint16_t *w, int first, int words, const char *s)
{
    for (int i = 0; i < words * 2; i++) {
        char c = *s ? *s++ : ' ';
        uint16_t *p = &w[first + i / 2];
        if (i & 1) *p = (uint16_t)((*p & 0xFF00) | (uint8_t)c);
        else       *p = (uint16_t)((*p & 0x00FF) | ((uint8_t)c << 8));
    }
}

static void identify(ide_dev_t *d, uint8_t *out)
{
    uint16_t w[256];
    memset(w, 0, sizeof w);
    put_str(w, 10, 10, d->serial);
    put_str(w, 23, 4, "1.0");
    put_str(w, 27, 20, d->model);
    uint8_t mw = d->dma_mode >> 3 == 4 ? d->dma_mode & 7 : 0xFF;   // selected multiword mode
    uint8_t ud = d->dma_mode >> 3 == 8 ? d->dma_mode & 7 : 0xFF;   // selected Ultra DMA mode
    if (d->type == IDE_CD) {
        w[0] = 0x85C0;                                 // ATAPI, CD-ROM, removable, 12-byte packets
        w[49] = 0x0300;                                // LBA, DMA
        w[53] = 0x0006;                                // words 64-70 and 88 valid
        w[63] = (uint16_t)(0x0007 | (mw <= 2 ? 0x100u << mw : 0));
        w[64] = 0x0003;                                // PIO 3, 4
        w[65] = w[66] = w[67] = w[68] = 120;
        w[71] = 30; w[72] = 30;
        w[80] = 0x001E;                                // ATA/ATAPI-1..4
        w[88] = (uint16_t)(0x003F | (ud <= 5 ? 0x100u << ud : 0));
    } else {
        uint64_t s = d->sectors;
        uint32_t lba28 = s > 0x0FFFFFFF ? 0x0FFFFFFF : (uint32_t)s;
        uint32_t chs = (uint32_t)d->cur_heads * d->cur_spt;
        uint32_t cur_cyls = chs ? (uint32_t)((s / chs) > 65535 ? 65535 : s / chs) : 0;
        uint32_t cur_cap = cur_cyls * chs;
        w[0] = 0x0040;                                 // fixed disk
        w[1] = d->cyls; w[3] = d->heads; w[6] = d->spt;
        w[47] = 0x8010;                                // READ/WRITE MULTIPLE up to 16 sectors
        w[49] = 0x0300;                                // LBA, DMA
        w[51] = 0x0200;
        w[53] = 0x0007;                                // words 54-58, 64-70, 88 valid
        w[54] = (uint16_t)cur_cyls; w[55] = d->cur_heads; w[56] = d->cur_spt;
        w[57] = (uint16_t)cur_cap; w[58] = (uint16_t)(cur_cap >> 16);
        w[59] = d->multiple ? (uint16_t)(0x100 | d->multiple) : 0;
        w[60] = (uint16_t)lba28; w[61] = (uint16_t)(lba28 >> 16);
        w[63] = (uint16_t)(0x0007 | (mw <= 2 ? 0x100u << mw : 0));
        w[64] = 0x0003;
        w[65] = w[66] = w[67] = w[68] = 120;
        w[80] = 0x007E;                                // ATA-1..6
        w[82] = 0x4000; w[83] = 0x7400;                // LBA48, FLUSH CACHE (EXT)
        w[84] = 0x4000; w[85] = 0x4000; w[86] = 0x3400; w[87] = 0x4000;
        w[88] = (uint16_t)(0x003F | (ud <= 5 ? 0x100u << ud : 0));
        w[93] = 0x6001;
        for (int i = 0; i < 4; i++) w[100 + i] = (uint16_t)(s >> (16 * i));
    }
    for (int i = 0; i < 256; i++) { out[2 * i] = (uint8_t)w[i]; out[2 * i + 1] = (uint8_t)(w[i] >> 8); }
}

/* ------------------------------------------------------------------ */
/* ATA data transfers                                                  */
/* ------------------------------------------------------------------ */

static void pio_start_in(ide_channel_t *ch, ide_dev_t *d, uint32_t len)
{
    d->pos = 0;
    d->len = len;
    d->xfer = XF_PIO_IN;
    d->status = ST_DRDY | ST_DSC | ST_DRQ;
    raise_irq(ch);
}

static void hd_read_block(ide_channel_t *ch, ide_dev_t *d)
{
    uint32_t n = d->remaining < d->block ? d->remaining : d->block;
    if (!disk_read(d->disk, d->lba * 512, d->buf, (size_t)n * 512)) { finish_err(ch, d, ER_UNC | ER_ABRT); return; }
    d->lba += n;
    d->remaining -= n;
    pio_start_in(ch, d, n * 512);
}

static void hd_write_next(ide_channel_t *ch, ide_dev_t *d, bool irq)
{
    uint32_t n = d->remaining < d->block ? d->remaining : d->block;
    d->pos = 0;
    d->len = n * 512;
    d->xfer = XF_PIO_OUT;
    d->status = ST_DRDY | ST_DSC | ST_DRQ;
    if (irq) raise_irq(ch);
}

static void atapi_done(ide_channel_t *ch, ide_dev_t *d);
static void atapi_pio_chunk(ide_channel_t *ch, ide_dev_t *d);

// Next bytes of an ATAPI reply: from the image (READ) or the reply buffer.
static bool reply_read(ide_dev_t *d, uint8_t *dst, uint32_t n)
{
    if (d->src_disk) {
        if (!disk_read(d->disk, d->src_off, dst, n)) return false;
        d->src_off += n;
    } else {
        memcpy(dst, d->reply + d->reply_pos, n);
        d->reply_pos += n;
    }
    return true;
}

static void dma_run(ide_channel_t *ch);

static void dma_start(ide_channel_t *ch, ide_dev_t *d, bool to_mem)
{
    d->xfer = XF_DMA;
    d->dma_to_mem = to_mem;
    d->status = ST_DRDY | ST_DSC | ST_DRQ;
    if (ch->bm_cmd & 1) dma_run(ch);
}

// Run a pending DMA transfer through the bus master's PRD table.
static void dma_run(ide_channel_t *ch)
{
    ide_dev_t *d = cur_dev(ch);
    if (d->xfer != XF_DMA) return;
    ide_t *ide = ch->ide;
    // The bus-master direction bit must match the command; a mismatch is a host bug.
    bool to_mem = (ch->bm_cmd & 0x08) != 0;
    uint32_t prd = ch->bm_prdt;
    uint32_t total = d->remaining;               // bytes
    bool ok = to_mem == d->dma_to_mem;
    for (int guard = 0; ok && total && guard < 8192; guard++) {
        uint32_t addr = mem_read(ide->bus, prd, 4) & ~1u;
        uint32_t w = mem_read(ide->bus, prd + 4, 4);
        uint32_t cnt = w & 0xFFFE;
        if (!(w & 0xFFFF)) cnt = 0x10000;
        prd += 8;
        while (cnt && total) {
            uint32_t n = cnt < total ? cnt : total;
            if (n > IDE_BUF_SIZE) n = IDE_BUF_SIZE;
            if (d->dma_to_mem) {
                if (d->type == IDE_HD) ok = disk_read(d->disk, d->src_off, ide->dma_tmp, n);
                else                   ok = reply_read(d, ide->dma_tmp, n);
                if (d->type == IDE_HD) d->src_off += n;
                mem_write_block(ide->bus, addr, ide->dma_tmp, n);
            } else {
                mem_read_block(ide->bus, addr, ide->dma_tmp, n);
                ok = disk_write(d->disk, d->src_off, ide->dma_tmp, n);
                d->src_off += n;
            }
            if (!ok) break;
            addr += n; cnt -= n; total -= n;
        }
        if (w & 0x80000000u) break;              // end of table
    }
    d->remaining = total;
    ch->bm_status = (uint8_t)((ch->bm_status & ~BM_ACTIVE) | BM_INT);
    if (!ok || total) {
        ch->bm_status |= BM_ERROR;
        if (d->type == IDE_CD) { d->sense_key = SENSE_ILLEGAL; d->asc = 0x21; atapi_done(ch, d); }
        else finish_err(ch, d, ER_ABRT);
        return;
    }
    if (d->type == IDE_CD) atapi_done(ch, d);
    else finish_ok(ch, d, true);
}

/* ------------------------------------------------------------------ */
/* ATAPI                                                               */
/* ------------------------------------------------------------------ */

static void be16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t rd_be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t rd_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// Status phase: command complete (with CHECK CONDITION if sense is set).
static void atapi_done(ide_channel_t *ch, ide_dev_t *d)
{
    d->xfer = XF_NONE;
    ch->count = IR_IO | IR_COD;
    if (d->sense_key) {
        d->status = ST_DRDY | ST_ERR;
        d->error = (uint8_t)(d->sense_key << 4);
    } else {
        d->status = ST_DRDY | ST_DSC;
        d->error = 0;
    }
    raise_irq(ch);
}

static void atapi_error(ide_channel_t *ch, ide_dev_t *d, uint8_t key, uint8_t asc)
{
    d->sense_key = key;
    d->asc = asc;
    d->ascq = 0;
    atapi_done(ch, d);
}

// One PIO data chunk of at most the host's byte count limit.
static void atapi_pio_chunk(ide_channel_t *ch, ide_dev_t *d)
{
    uint32_t lim = d->byte_limit;
    if (lim == 0 || lim == 0xFFFF) lim = 0xFFFE;
    lim &= ~1u;
    uint32_t n = d->remaining < lim ? d->remaining : lim;
    if (!reply_read(d, d->buf, n)) { atapi_error(ch, d, 3, 0x11); return; }   // medium error
    d->remaining -= n;
    d->pos = 0;
    d->len = n;
    d->xfer = XF_ATAPI_IN;
    ch->lba_mid = (uint8_t)n;
    ch->lba_hi = (uint8_t)(n >> 8);
    ch->count = IR_IO;
    d->status = ST_DRDY | ST_DSC | ST_DRQ;
    raise_irq(ch);
}

// Send `total` bytes of reply (reply[] or the image, as set up by the caller).
static void atapi_reply(ide_channel_t *ch, ide_dev_t *d, uint32_t total)
{
    d->remaining = total;
    if (!total) { atapi_done(ch, d); return; }
    if (d->atapi_dma) { dma_start(ch, d, true); return; }
    atapi_pio_chunk(ch, d);
}

static void reply_buf(ide_channel_t *ch, ide_dev_t *d, uint32_t len, uint32_t alloc)
{
    d->src_disk = false;
    d->reply_pos = 0;
    atapi_reply(ch, d, len < alloc ? len : alloc);
}

static uint32_t mode_page(uint8_t *p, int page)
{
    switch (page) {
    case 0x01:                                     // read error recovery
        memset(p, 0, 8);
        p[0] = 0x01; p[1] = 0x06; p[3] = 0x05;
        return 8;
    case 0x0E:                                     // CD audio control
        memset(p, 0, 16);
        p[0] = 0x0E; p[1] = 0x0E; p[2] = 0x04;
        p[8] = 1; p[9] = 0xFF; p[10] = 2; p[11] = 0xFF;
        return 16;
    case 0x2A:                                     // capabilities and mechanical status
        memset(p, 0, 20);
        p[0] = 0x2A; p[1] = 0x12;
        p[4] = 0x71;                               // audio play, composite, digital ports, multisession
        p[5] = 0x03;                               // CD-DA commands, accurate stream
        p[6] = 0x29;                               // lock, eject, tray loader
        be16(&p[8], 1408);                         // max read speed (8x, kB/s)
        be16(&p[10], 256);                         // volume levels
        be16(&p[12], 512);                         // buffer size (KB)
        be16(&p[14], 1408);                        // current read speed
        return 20;
    default:
        return 0;
    }
}

static int trace_ide(void);

static void atapi_packet(ide_channel_t *ch, ide_dev_t *d)
{
    if (trace_ide()) fprintf(stderr, "[ide] packet %02X dma %d limit %u\n", d->packet[0], d->atapi_dma, d->byte_limit);
    const uint8_t *p = d->packet;
    uint8_t *r = d->reply;
    uint64_t blocks = d->sectors;
    if (p[0] != 0x03) d->sense_key = d->asc = d->ascq = 0;   // REQUEST SENSE reports the previous one

    switch (p[0]) {
    case 0x00:                                     // TEST UNIT READY
    case 0x1B:                                     // START STOP UNIT
    case 0x1E:                                     // PREVENT ALLOW MEDIUM REMOVAL
    case 0x2B:                                     // SEEK
    case 0xBB:                                     // SET CD SPEED
        atapi_done(ch, d);
        return;
    case 0x03:                                     // REQUEST SENSE
        memset(r, 0, 18);
        r[0] = 0x70; r[2] = d->sense_key; r[7] = 10; r[12] = d->asc; r[13] = d->ascq;
        d->sense_key = d->asc = d->ascq = 0;
        reply_buf(ch, d, 18, p[4]);
        return;
    case 0x12:                                     // INQUIRY
        memset(r, 0, 36);
        r[0] = 0x05; r[1] = 0x80; r[2] = 0x00; r[3] = 0x21; r[4] = 31;
        memcpy(&r[8], "BEEMU   ", 8);
        memcpy(&r[16], "CD-ROM          ", 16);
        memcpy(&r[32], "1.0 ", 4);
        reply_buf(ch, d, 36, p[4]);
        return;
    case 0x25:                                     // READ CAPACITY
        be32(&r[0], (uint32_t)(blocks - 1));
        be32(&r[4], 2048);
        reply_buf(ch, d, 8, 8);
        return;
    case 0x28: case 0xA8: case 0xBE: {             // READ(10), READ(12), READ CD
        uint32_t lba = rd_be32(&p[2]);
        uint32_t n = p[0] == 0x28 ? rd_be16(&p[7]) : p[0] == 0xA8 ? rd_be32(&p[6])
                                                 : ((uint32_t)p[6] << 16 | (uint32_t)p[7] << 8 | p[8]);
        if (p[0] == 0xBE && (p[9] & 0xF8) != 0x10) {          // READ CD: only plain user data
            if (!p[9]) { atapi_done(ch, d); return; }
            atapi_error(ch, d, SENSE_ILLEGAL, 0x24);
            return;
        }
        if ((uint64_t)lba + n > blocks) { atapi_error(ch, d, SENSE_ILLEGAL, 0x21); return; }
        d->src_disk = true;
        d->src_off = (uint64_t)lba * 2048;
        atapi_reply(ch, d, n * 2048);
        return;
    }
    case 0x43: {                                   // READ TOC
        bool msf = p[1] & 2;
        int format = p[2] & 0xF;
        if (!format && (p[9] >> 6)) format = p[9] >> 6;   // old-style format field
        uint32_t alloc = rd_be16(&p[7]), len;
        memset(r, 0, 32);
        if (format == 0) {
            r[2] = 1; r[3] = 1;
            uint8_t *e = &r[4];
            e[1] = 0x14; e[2] = 1;                 // track 1: data
            if (msf) { e[5] = 0; e[6] = 2; e[7] = 0; } // 00:02:00
            e += 8;
            e[1] = 0x14; e[2] = 0xAA;              // lead-out
            uint32_t lo = (uint32_t)blocks;
            if (msf) { uint32_t f = lo + 150; e[5] = (uint8_t)(f / 4500); e[6] = (uint8_t)(f / 75 % 60); e[7] = (uint8_t)(f % 75); }
            else be32(&e[4], lo);
            len = 20;
        } else if (format == 1) {                  // session info
            r[2] = 1; r[3] = 1; r[5] = 0x14; r[6] = 1;
            if (msf) r[10] = 2;
            len = 12;
        } else {
            atapi_error(ch, d, SENSE_ILLEGAL, 0x24);
            return;
        }
        be16(&r[0], len - 2);
        reply_buf(ch, d, len, alloc);
        return;
    }
    case 0x1A: case 0x5A: {                        // MODE SENSE(6) / (10)
        bool ten = p[0] == 0x5A;
        int page = p[2] & 0x3F;
        uint32_t hdr = ten ? 8 : 4, len = hdr;
        if (page == 0x3F) {
            len += mode_page(r + len, 0x01);
            len += mode_page(r + len, 0x0E);
            len += mode_page(r + len, 0x2A);
        } else {
            uint32_t n = mode_page(r + hdr, page);
            if (!n) { atapi_error(ch, d, SENSE_ILLEGAL, 0x24); return; }
            len += n;
        }
        memset(r, 0, hdr);
        if (ten) { be16(&r[0], len - 2); r[2] = 0x01; }     // 120 mm data disc
        else     { r[0] = (uint8_t)(len - 1); r[1] = 0x01; }
        reply_buf(ch, d, len, ten ? rd_be16(&p[7]) : p[4]);
        return;
    }
    case 0x15: case 0x55: {                        // MODE SELECT: accept and ignore the parameters
        uint32_t n = p[0] == 0x55 ? rd_be16(&p[7]) : p[4];
        if (!n) { atapi_done(ch, d); return; }
        d->remaining = n;
        d->pos = 0;
        d->len = n < IDE_BUF_SIZE ? n : IDE_BUF_SIZE;
        d->xfer = XF_ATAPI_OUT;
        ch->count = 0;
        ch->lba_mid = (uint8_t)d->len; ch->lba_hi = (uint8_t)(d->len >> 8);
        d->status = ST_DRDY | ST_DSC | ST_DRQ;
        raise_irq(ch);
        return;
    }
    case 0x42:                                     // READ SUB-CHANNEL
        memset(r, 0, 4);
        r[1] = 0x15;                               // no current audio status
        reply_buf(ch, d, 4, rd_be16(&p[7]));
        return;
    case 0x46:                                     // GET CONFIGURATION: header, current profile CD-ROM
        memset(r, 0, 8);
        be32(&r[0], 4);
        be16(&r[6], 0x0008);
        reply_buf(ch, d, 8, rd_be16(&p[7]));
        return;
    case 0x4A:                                     // GET EVENT STATUS NOTIFICATION (polled)
        if (!(p[1] & 1)) { atapi_error(ch, d, SENSE_ILLEGAL, 0x24); return; }
        memset(r, 0, 8);
        be16(&r[0], 6); r[2] = 0x04; r[3] = 0x10;  // media class
        r[5] = 0x02;                               // media present
        reply_buf(ch, d, 8, rd_be16(&p[7]));
        return;
    case 0x51:                                     // READ DISC INFORMATION
        memset(r, 0, 34);
        be16(&r[0], 32); r[2] = 0x0E; r[3] = 1; r[4] = 1; r[5] = 1; r[6] = 1;
        reply_buf(ch, d, 34, rd_be16(&p[7]));
        return;
    case 0xBD:                                     // MECHANISM STATUS
        memset(r, 0, 8);
        reply_buf(ch, d, 8, rd_be16(&p[8]));
        return;
    default:
        atapi_error(ch, d, SENSE_ILLEGAL, 0x20);   // invalid command operation code
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static bool taskfile_lba(ide_channel_t *ch, ide_dev_t *d, bool lba48, uint64_t *lba)
{
    if (lba48) {
        *lba = (uint64_t)ch->hob_lba_hi << 40 | (uint64_t)ch->hob_lba_mid << 32 | (uint64_t)ch->hob_lba_lo << 24 |
               (uint64_t)ch->lba_hi << 16 | (uint64_t)ch->lba_mid << 8 | ch->lba_lo;
    } else if (ch->select & 0x40) {
        *lba = (uint64_t)(ch->select & 0xF) << 24 | (uint64_t)ch->lba_hi << 16 | (uint64_t)ch->lba_mid << 8 | ch->lba_lo;
    } else {                                       // CHS
        uint32_t cyl = ch->lba_mid | (uint32_t)ch->lba_hi << 8, head = ch->select & 0xF, sec = ch->lba_lo;
        if (!sec || sec > d->cur_spt || head >= d->cur_heads) return false;
        *lba = ((uint64_t)cyl * d->cur_heads + head) * d->cur_spt + sec - 1;
    }
    return true;
}

static uint32_t taskfile_count(ide_channel_t *ch, bool lba48)
{
    if (lba48) { uint32_t c = (uint32_t)ch->hob_count << 8 | ch->count; return c ? c : 65536; }
    return ch->count ? ch->count : 256;
}

static int trace_ide(void)
{
    static int t = -1;
    if (t < 0) { const char *e = getenv("BEEMU_TRACE_IDE"); t = e && *e == '1'; }
    return t;
}

static void exec_cmd(ide_channel_t *ch, uint8_t cmd)
{
    ide_dev_t *d = cur_dev(ch);
    if (trace_ide() && (cmd == 0xEC || cmd == 0xA1)) g_trace_burst = 600;
    if (trace_ide())
        fprintf(stderr, "[ide%d.%d] cmd %02X feat %02X count %02X lba %02X%02X%02X sel %02X ctl %02X%s\n",
                (int)(ch - ch->ide->ch), ch->cur, cmd, ch->feature, ch->count, ch->lba_hi, ch->lba_mid, ch->lba_lo,
                ch->select, ch->ctl, d->type == IDE_NONE ? " (no device)" : "");
    if (d->type == IDE_NONE) return;               // commands to a missing device are ignored
    d->cmd = cmd;
    ch->intrq = false;
    update_irq(ch);

    if (d->type == IDE_CD) {
        switch (cmd) {
        case 0x08:                                 // DEVICE RESET
            reset_dev(d);
            set_signature(ch, d);
            return;
        case 0xA0:                                 // PACKET
            d->atapi_dma = ch->feature & 1;
            d->byte_limit = ch->lba_mid | (uint32_t)ch->lba_hi << 8;
            d->pos = 0;
            d->len = 12;
            d->xfer = XF_PACKET;
            ch->count = IR_COD;
            d->status = ST_DRDY | ST_DSC | ST_DRQ;
            return;
        case 0xA1:                                 // IDENTIFY PACKET DEVICE
            identify(d, d->buf);
            d->remaining = 0;
            pio_start_in(ch, d, 512);
            return;
        case 0xEC:                                 // IDENTIFY DEVICE: abort and show the ATAPI signature
            set_signature(ch, d);
            finish_err(ch, d, ER_ABRT);
            return;
        case 0x90: case 0xEF: case 0xE5: case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0x00:
            break;                                 // handled below like a disk
        default:
            finish_err(ch, d, ER_ABRT);
            return;
        }
    }

    bool lba48 = false;
    switch (cmd) {
    case 0x24: case 0x29: case 0x34: case 0x39: case 0x25: case 0x35: case 0x42: case 0x27:
        lba48 = true;
        break;
    default:
        break;
    }

    switch (cmd) {
    case 0x20: case 0x21: case 0x24: case 0xC4: case 0x29:     // READ SECTORS / MULTIPLE
    case 0x30: case 0x31: case 0x34: case 0xC5: case 0x39:     // WRITE SECTORS / MULTIPLE
    case 0xC8: case 0xC9: case 0x25:                           // READ DMA
    case 0xCA: case 0xCB: case 0x35: {                         // WRITE DMA
        bool multiple = cmd == 0xC4 || cmd == 0x29 || cmd == 0xC5 || cmd == 0x39;
        if (multiple && !d->multiple) { finish_err(ch, d, ER_ABRT); return; }
        uint64_t lba;
        uint32_t n = taskfile_count(ch, lba48);
        if (!taskfile_lba(ch, d, lba48, &lba) || lba + n > d->sectors) { finish_err(ch, d, ER_IDNF | ER_ABRT); return; }
        d->lba = lba;
        if (cmd == 0xC8 || cmd == 0xC9 || cmd == 0x25 || cmd == 0xCA || cmd == 0xCB || cmd == 0x35) {
            d->src_off = lba * 512;
            d->remaining = n * 512;
            dma_start(ch, d, cmd == 0xC8 || cmd == 0xC9 || cmd == 0x25);
            return;
        }
        d->remaining = n;
        d->block = multiple ? d->multiple : 1;
        if (cmd == 0x20 || cmd == 0x21 || cmd == 0x24 || cmd == 0xC4 || cmd == 0x29) hd_read_block(ch, d);
        else hd_write_next(ch, d, false);
        return;
    }
    case 0x40: case 0x41: case 0x42: {             // READ VERIFY
        uint64_t lba;
        uint32_t n = taskfile_count(ch, lba48);
        if (!taskfile_lba(ch, d, lba48, &lba) || lba + n > d->sectors) { finish_err(ch, d, ER_IDNF | ER_ABRT); return; }
        finish_ok(ch, d, true);
        return;
    }
    case 0x90:                                     // EXECUTE DEVICE DIAGNOSTIC
        ch->cur = 0;
        ch->select &= (uint8_t)~0x10;
        for (int i = 0; i < 2; i++) if (present(ch, i)) reset_dev(&ch->dev[i]);
        set_signature(ch, &ch->dev[0]);
        raise_irq(ch);
        return;
    case 0x91:                                     // INITIALIZE DEVICE PARAMETERS
        d->cur_heads = (uint16_t)((ch->select & 0xF) + 1);
        d->cur_spt = ch->count;
        finish_ok(ch, d, true);
        return;
    case 0xC6:                                     // SET MULTIPLE MODE
        if (ch->count > 16 || (ch->count & (ch->count - 1))) { finish_err(ch, d, ER_ABRT); return; }
        d->multiple = ch->count;
        finish_ok(ch, d, true);
        return;
    case 0xEC:                                     // IDENTIFY DEVICE
        identify(d, d->buf);
        d->remaining = 0;
        pio_start_in(ch, d, 512);
        return;
    case 0xEF:                                     // SET FEATURES
        if (ch->feature == 0x03) d->dma_mode = ch->count;
        finish_ok(ch, d, true);
        return;
    case 0xF8: case 0x27: {                        // READ NATIVE MAX ADDRESS (EXT)
        uint64_t m = d->sectors - 1;
        ch->lba_lo = (uint8_t)m; ch->lba_mid = (uint8_t)(m >> 8); ch->lba_hi = (uint8_t)(m >> 16);
        if (lba48) { ch->hob_lba_lo = (uint8_t)(m >> 24); ch->hob_lba_mid = (uint8_t)(m >> 32); ch->hob_lba_hi = (uint8_t)(m >> 40); }
        else ch->select = (uint8_t)((ch->select & 0xF0) | ((m >> 24) & 0xF));
        finish_ok(ch, d, true);
        return;
    }
    case 0xE5: case 0x98:                          // CHECK POWER MODE: active
        ch->count = 0xFF;
        finish_ok(ch, d, true);
        return;
    case 0xE7: case 0xEA:                          // FLUSH CACHE (EXT)
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0x94: case 0x95: case 0x96: case 0x97: case 0x99:
        finish_ok(ch, d, true);
        return;
    default:
        if ((cmd & 0xF0) == 0x10 || (cmd & 0xF0) == 0x70) { finish_ok(ch, d, true); return; }   // RECALIBRATE, SEEK
        finish_err(ch, d, ER_ABRT);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Data port                                                           */
/* ------------------------------------------------------------------ */

static void block_read_done(ide_channel_t *ch, ide_dev_t *d)
{
    if (d->xfer == XF_ATAPI_IN) {
        if (d->remaining) atapi_pio_chunk(ch, d);
        else atapi_done(ch, d);
        return;
    }
    if (d->type == IDE_HD && d->remaining) { hd_read_block(ch, d); return; }
    d->xfer = XF_NONE;
    d->status = ST_DRDY | ST_DSC;
}

static uint32_t data_read(ide_channel_t *ch, int size)
{
    ide_dev_t *d = cur_dev(ch);
    if (d->xfer != XF_PIO_IN && d->xfer != XF_ATAPI_IN) return 0xFFFFFFFFu;
    uint32_t v = 0;
    for (int i = 0; i < size && d->pos < d->len; i++) v |= (uint32_t)d->buf[d->pos++] << (8 * i);
    if (d->pos >= d->len) block_read_done(ch, d);
    return v;
}

static void data_write(ide_channel_t *ch, uint32_t v, int size)
{
    ide_dev_t *d = cur_dev(ch);
    if (d->xfer == XF_PACKET) {
        for (int i = 0; i < size && d->pos < 12; i++) d->packet[d->pos++] = (uint8_t)(v >> (8 * i));
        if (d->pos >= 12) { d->xfer = XF_NONE; atapi_packet(ch, d); }
        return;
    }
    if (d->xfer != XF_PIO_OUT && d->xfer != XF_ATAPI_OUT) return;
    for (int i = 0; i < size && d->pos < d->len; i++) d->buf[d->pos++] = (uint8_t)(v >> (8 * i));
    if (d->pos < d->len) return;
    if (d->xfer == XF_ATAPI_OUT) {                 // MODE SELECT data: discarded
        d->remaining -= d->len < d->remaining ? d->len : d->remaining;
        if (d->remaining) { d->pos = 0; d->len = d->remaining < IDE_BUF_SIZE ? d->remaining : IDE_BUF_SIZE; raise_irq(ch); }
        else atapi_done(ch, d);
        return;
    }
    uint32_t n = d->len / 512;
    if (!disk_write(d->disk, d->lba * 512, d->buf, d->len)) { finish_err(ch, d, ER_ABRT); return; }
    d->lba += n;
    d->remaining -= n;
    if (d->remaining) hd_write_next(ch, d, true);
    else finish_ok(ch, d, true);
}

/* ------------------------------------------------------------------ */
/* Ports                                                               */
/* ------------------------------------------------------------------ */

static ide_channel_t *chan_for(ide_t *ide, uint16_t port)
{
    return (port >= 0x170 && port <= 0x177) || port == 0x376 || port == 0x377 ? &ide->ch[1] : &ide->ch[0];
}

static uint32_t cmd_read_(void *opaque, uint16_t port, int size);
static uint32_t cmd_read(void *opaque, uint16_t port, int size)
{
    uint32_t v = cmd_read_(opaque, port, size);
    if (g_trace_burst > 0) {
        g_trace_burst--;
        static int words;
        if ((port & 7) == 0) { if (words++ % 64 == 0) fprintf(stderr, "[ide] data rd size %d (#%d)\n", size, words); }
        else fprintf(stderr, "[ide] rd %03X -> %02X\n", port, v);
    }
    return v;
}
static uint32_t cmd_read_(void *opaque, uint16_t port, int size)
{
    ide_t *ide = opaque;
    ide_channel_t *ch = chan_for(ide, port);
    int reg = port & 7;
    if (reg == 0) return data_read(ch, size);
    bool missing = ch->cur == 1 && !present(ch, 1);
    bool hob = ch->ctl & 0x80;
    ide_dev_t *d = cur_dev(ch);
    switch (reg) {
    case 1: return missing ? 0 : d->error;
    case 2: return missing ? 0 : hob ? ch->hob_count : ch->count;
    case 3: return missing ? 0 : hob ? ch->hob_lba_lo : ch->lba_lo;
    case 4: return missing ? 0 : hob ? ch->hob_lba_mid : ch->lba_mid;
    case 5: return missing ? 0 : hob ? ch->hob_lba_hi : ch->lba_hi;
    case 6: return missing ? 0 : ch->select;
    default:
        ch->intrq = false;                         // reading status acknowledges the interrupt
        update_irq(ch);
        return missing ? 0 : d->status;
    }
}

static void cmd_write(void *opaque, uint16_t port, uint32_t v, int size)
{
    ide_t *ide = opaque;
    ide_channel_t *ch = chan_for(ide, port);
    int reg = port & 7;
    if (reg == 0) { data_write(ch, v, size); return; }
    uint8_t b = (uint8_t)v;
    ch->ctl &= 0x7F;                               // any taskfile write clears HOB
    switch (reg) {
    case 1: ch->hob_feature = ch->feature; ch->feature = b; break;
    case 2: ch->hob_count = ch->count; ch->count = b; break;
    case 3: ch->hob_lba_lo = ch->lba_lo; ch->lba_lo = b; break;
    case 4: ch->hob_lba_mid = ch->lba_mid; ch->lba_mid = b; break;
    case 5: ch->hob_lba_hi = ch->lba_hi; ch->lba_hi = b; break;
    case 6: ch->select = b | 0xA0; ch->cur = (b >> 4) & 1; break;
    default: exec_cmd(ch, b); break;
    }
}

static uint32_t ctl_read(void *opaque, uint16_t port, int size)
{
    (void)size;
    ide_channel_t *ch = chan_for(opaque, port);
    if (g_trace_burst > 0) { g_trace_burst--; fprintf(stderr, "[ide] altstatus -> %02X\n", cur_dev(ch)->status); }
    if (port & 1) return 0xFF;                     // drive address register: obsolete
    if (ch->cur == 1 && !present(ch, 1)) return 0;
    return cur_dev(ch)->status;                    // alternate status: no interrupt acknowledge
}

static void ctl_write(void *opaque, uint16_t port, uint32_t v, int size)
{
    (void)size;
    if (port & 1) return;
    ide_channel_t *ch = chan_for(opaque, port);
    uint8_t old = ch->ctl;
    ch->ctl = (uint8_t)v;
    if (trace_ide() && ((old ^ v) & 6)) fprintf(stderr, "[ide%d] ctl %02X\n", (int)(ch - ((ide_t *)opaque)->ch), (unsigned)v);
    if ((old & 4) && !(v & 4)) {                   // end of software reset
        for (int i = 0; i < 2; i++) if (present(ch, i)) reset_dev(&ch->dev[i]);
        ch->cur = 0;
        ch->select = 0xA0;
        set_signature(ch, &ch->dev[0]);
    } else if (v & 4) {
        for (int i = 0; i < 2; i++) if (present(ch, i)) { ch->dev[i].status = ST_BSY; ch->dev[i].xfer = XF_NONE; }
    }
    update_irq(ch);
}

/* ---- bus master ---- */

static uint32_t bm_read(void *opaque, uint16_t port, int size)
{
    ide_t *ide = opaque;
    uint32_t base = ide->pci.bar_addr[4] & ~0xFu, v = 0;
    for (int i = 0; i < size; i++) {
        uint32_t off = (port + (uint32_t)i - base) & 15;
        ide_channel_t *ch = &ide->ch[off >> 3];
        uint8_t b;
        switch (off & 7) {
        case 0: b = ch->bm_cmd; break;
        case 2: b = ch->bm_status; break;
        case 4: case 5: case 6: case 7: b = (uint8_t)(ch->bm_prdt >> (8 * ((off & 7) - 4))); break;
        default: b = 0; break;
        }
        v |= (uint32_t)b << (8 * i);
    }
    return v;
}

static void bm_write(void *opaque, uint16_t port, uint32_t v, int size)
{
    ide_t *ide = opaque;
    uint32_t base = ide->pci.bar_addr[4] & ~0xFu;
    for (int i = 0; i < size; i++) {
        uint32_t off = (port + (uint32_t)i - base) & 15;
        ide_channel_t *ch = &ide->ch[off >> 3];
        uint8_t b = (uint8_t)(v >> (8 * i));
        switch (off & 7) {
        case 0: {
            bool was = ch->bm_cmd & 1;
            ch->bm_cmd = b & 0x09;
            if ((b & 1) && !was) {
                if (trace_ide()) fprintf(stderr, "[ide%d] bm start dir %s prdt %08X xfer %d\n", (int)(off >> 3),
                                         (b & 8) ? "to-mem" : "from-mem", ch->bm_prdt, cur_dev(ch)->xfer);
                ch->bm_status |= BM_ACTIVE;
                dma_run(ch);
            }
            else if (!(b & 1)) ch->bm_status &= (uint8_t)~BM_ACTIVE;
            break;
        }
        case 2:
            ch->bm_status = (uint8_t)(((ch->bm_status & ~0x60) | (b & 0x60)) & ~(b & 0x06));
            break;
        case 4: case 5: case 6: case 7: {
            int sh = 8 * (int)((off & 7) - 4);
            ch->bm_prdt = (ch->bm_prdt & ~(0xFFu << sh)) | ((uint32_t)b << sh);
            ch->bm_prdt &= ~3u;
            break;
        }
        default: break;
        }
    }
}

static void pci_map(pci_dev_t *d, int bar, uint32_t addr, bool on)
{
    ide_t *ide = d->opaque;
    if (bar != 4) return;
    if (on) io_register(ide->io, (uint16_t)addr, 16, bm_read, bm_write, ide);
    else    io_unregister(ide->io, (uint16_t)addr, 16);
}

/* ------------------------------------------------------------------ */
/* Setup                                                               */
/* ------------------------------------------------------------------ */

bool ide_init(ide_t *ide, mem_bus_t *bus, io_bus_t *io, void (*set_irq)(void *, int, int), void *irq_opaque)
{
    memset(ide, 0, sizeof *ide);
    ide->bus = bus;
    ide->io = io;
    ide->set_irq = set_irq;
    ide->irq_opaque = irq_opaque;
    ide->dma_tmp = malloc(IDE_BUF_SIZE);
    if (!ide->dma_tmp) return false;
    for (int c = 0; c < 2; c++) {
        ide->ch[c].ide = ide;
        ide->ch[c].irq = LEGACY_IRQ[c];
        ide->ch[c].select = 0xA0;
    }
    pci_dev_t *p = &ide->pci;
    pci_set_ids(p, 0x8086, 0x7010, 0x00, 0x010180, 0x1AF4, 0x1100);
    pci_set_bar(p, 4, 16, true, false);
    pci_set_wmask(p, 0x40, 4, 0xFF);              // IDE timing registers
    p->map = pci_map;
    p->opaque = ide;
    return true;
}

void ide_free(ide_t *ide)
{
    for (int c = 0; c < 2; c++)
        for (int i = 0; i < 2; i++) { free(ide->ch[c].dev[i].buf); free(ide->ch[c].dev[i].reply); }
    free(ide->dma_tmp);
    memset(ide, 0, sizeof *ide);
}

bool ide_attach(ide_t *ide, int slot, ide_type_t type, disk_t *disk)
{
    if (slot < 0 || slot > 3 || !disk || type == IDE_NONE) return false;
    ide_dev_t *d = &ide->ch[slot >> 1].dev[slot & 1];
    d->type = type;
    d->disk = disk;
    d->buf = malloc(IDE_BUF_SIZE);
    d->reply = malloc(4096);
    if (!d->buf || !d->reply) return false;
    if (type == IDE_CD) {
        d->sectors = disk_size(disk) / 2048;
        snprintf(d->model, sizeof d->model, "BEEMU CD-ROM");
    } else {
        d->sectors = disk_size(disk) / 512;
        uint64_t cyl = d->sectors / (16 * 63);
        d->cyls = (uint16_t)(cyl > 16383 ? 16383 : cyl);
        d->heads = 16;
        d->spt = 63;
        snprintf(d->model, sizeof d->model, "BEEMU HARDDISK");
    }
    snprintf(d->serial, sizeof d->serial, "BEEMU%05d", slot + 1);
    reset_dev(d);
    return true;
}

void ide_register(ide_t *ide)
{
    for (int c = 0; c < 2; c++) {
        if (!present(&ide->ch[c], 0) && !present(&ide->ch[c], 1)) continue;   // floating bus: reads 0xFF
        io_register(ide->io, LEGACY_BASE[c], 8, cmd_read, cmd_write, ide);
        io_register(ide->io, LEGACY_CTL[c], 2, ctl_read, ctl_write, ide);
    }
}

void ide_reset(ide_t *ide)
{
    for (int c = 0; c < 2; c++) {
        ide_channel_t *ch = &ide->ch[c];
        for (int i = 0; i < 2; i++) if (present(ch, i)) reset_dev(&ch->dev[i]);
        ch->cur = 0;
        ch->select = 0xA0;
        ch->ctl = 0;
        ch->intrq = false;
        ch->bm_cmd = ch->bm_status = 0;
        ch->bm_prdt = 0;
        set_signature(ch, &ch->dev[0]);
        update_irq(ch);
    }
    // PCI state: BARs unassigned again.
    if (ide->pci.bar_on[4]) { pci_map(&ide->pci, 4, ide->pci.bar_addr[4], false); ide->pci.bar_on[4] = false; }
    pci_cfg_write(&ide->pci, 0x04, 0, 2);
    pci_cfg_write(&ide->pci, 0x20, 1, 4);
}
