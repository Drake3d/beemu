#include "sb16.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PNP_WAIT_KEY, PNP_SLEEP, PNP_ISOLATION, PNP_CONFIG };

static bool trace_sound(void)
{
    static int on = -1;
    if (on < 0) on = getenv("BEEMU_TRACE_SOUND") != NULL;
    return on;
}

/* ------------------------------------------------------------------ */
/* ISA Plug and Play                                                   */
/* ------------------------------------------------------------------ */

// Serial identifier (vendor id CTL0028, serial number, LFSR checksum) and resource data.
static uint8_t g_ident[9];
static uint8_t g_res[256];
static int     g_res_len;
static uint8_t g_key[32];

static void eisa_id(uint8_t out[4], const char *s)    // "CTL0031" -> compressed EISA id
{
    int a = s[0] - 0x40, b = s[1] - 0x40, c = s[2] - 0x40;
    unsigned hex = (unsigned)strtoul(s + 3, NULL, 16);
    out[0] = (uint8_t)(a << 2 | b >> 3);
    out[1] = (uint8_t)((b & 7) << 5 | c);
    out[2] = (uint8_t)(hex >> 8);
    out[3] = (uint8_t)hex;
}

static void build_pnp_data(void)
{
    if (g_res_len) return;
    uint8_t v = 0x6A;                                   // initiation key: the LFSR sequence
    for (int i = 0; i < 32; i++) { g_key[i] = v; v = (uint8_t)(v >> 1 | ((v ^ (v >> 1)) & 1) << 7); }

    eisa_id(g_ident, "CTL0028");
    uint32_t serial = 0xB33E5B16u;
    memcpy(g_ident + 4, &serial, 4);
    uint8_t lfsr = 0x6A;                                // checksum over the 64 bits, LSB first
    for (int i = 0; i < 64; i++) {
        int bit = (g_ident[i >> 3] >> (i & 7)) & 1;
        lfsr = (uint8_t)(lfsr >> 1 | (((lfsr ^ (lfsr >> 1) ^ bit) & 1) << 7));
    }
    g_ident[8] = lfsr;

    uint8_t *r = g_res;
    int n = 0;
    r[n++] = 0x0A; r[n++] = 0x10; r[n++] = 0x10;                      // PnP version 1.0
    static const char card[] = "Creative SB16 PnP";
    r[n++] = 0x82; r[n++] = sizeof card - 1; r[n++] = 0;              // ANSI identifier
    memcpy(r + n, card, sizeof card - 1); n += sizeof card - 1;
    r[n++] = 0x15; eisa_id(r + n, "CTL0031"); n += 4; r[n++] = 0x00;   // logical device 0: audio
    static const char ld[] = "Audio";
    r[n++] = 0x82; r[n++] = sizeof ld - 1; r[n++] = 0;
    memcpy(r + n, ld, sizeof ld - 1); n += sizeof ld - 1;
    r[n++] = 0x22; r[n++] = 0xA0; r[n++] = 0x06;                      // IRQ 5, 7, 9, 10
    r[n++] = 0x2A; r[n++] = 0x0A; r[n++] = 0x08;                      // 8-bit DMA 1 or 3
    r[n++] = 0x2A; r[n++] = 0xE0; r[n++] = 0x12;                      // 16-bit DMA 5, 6 or 7
    static const uint16_t io[3][4] = { { 0x220, 0x280, 0x20, 16 },   // DSP / mixer
                                       { 0x300, 0x330, 0x30, 2 },    // MPU-401
                                       { 0x388, 0x388, 0x01, 4 } };  // FM (not emulated)
    for (int i = 0; i < 3; i++) {
        r[n++] = 0x47; r[n++] = 0x01;
        r[n++] = (uint8_t)io[i][0]; r[n++] = (uint8_t)(io[i][0] >> 8);
        r[n++] = (uint8_t)io[i][1]; r[n++] = (uint8_t)(io[i][1] >> 8);
        r[n++] = (uint8_t)io[i][2]; r[n++] = (uint8_t)io[i][3];
    }
    r[n++] = 0x79;                                                    // end tag + checksum
    uint8_t sum = 0;
    for (int i = 0; i < n; i++) sum = (uint8_t)(sum + r[i]);
    r[n++] = (uint8_t)-sum;
    g_res_len = n;
}

static void update_irq(sb16_t *s);
static void dsp_io_wr(void *o, uint16_t port, uint32_t v, int size);
static uint32_t dsp_io_rd(void *o, uint16_t port, int size);
static void mpu_io_wr(void *o, uint16_t port, uint32_t v, int size);
static uint32_t mpu_io_rd(void *o, uint16_t port, int size);

static void unmap(sb16_t *s)
{
    if (s->base) io_unregister(s->io, s->base, 16);
    if (s->mpu_base) io_unregister(s->io, s->mpu_base, 2);
    if (s->irq_line && s->set_irq && s->irq > 0) s->set_irq(s->irq_opaque, s->irq, 0);
    s->irq_line = false;
    s->base = s->mpu_base = 0;
}

static void activate(sb16_t *s, bool on)
{
    unmap(s);
    s->run = false;
    if (!on) return;
    uint16_t base = (uint16_t)(s->ldn_regs[0x60] << 8 | s->ldn_regs[0x61]);
    uint16_t mpu = (uint16_t)(s->ldn_regs[0x62] << 8 | s->ldn_regs[0x63]);
    s->irq = s->ldn_regs[0x70] & 0x0F;
    s->dma8 = s->ldn_regs[0x74] & 7;
    s->dma16 = s->ldn_regs[0x75] & 7;
    if (base && io_register(s->io, base, 16, dsp_io_rd, dsp_io_wr, s)) s->base = base;
    if (mpu && io_register(s->io, mpu, 2, mpu_io_rd, mpu_io_wr, s)) s->mpu_base = mpu;
    if (trace_sound())
        fprintf(stderr, "[sb16] activated: base %X (%s) mpu %X irq %d dma %d/%d\n", base, s->base ? "ok" : "CONFLICT",
                mpu, s->irq, s->dma8, s->dma16);
    update_irq(s);
}

static uint32_t pnp_read_(sb16_t *s);
static uint32_t pnp_read(void *o, uint16_t port, int size)
{
    sb16_t *s = o;
    (void)port; (void)size;
    uint32_t v = pnp_read_(s);
    if (trace_sound() && s->pnp_addr != 0x01 && s->pnp_state == PNP_CONFIG)
        fprintf(stderr, "[sb16] PnP rd reg %02X = %02X (pos %d)\n", s->pnp_addr, v, s->res_pos);
    return v;
}

static uint32_t pnp_read_(sb16_t *s)
{
    if (s->pnp_state == PNP_WAIT_KEY || s->pnp_state == PNP_SLEEP) return 0xFF;
    switch (s->pnp_addr) {
    case 0x00: return s->read_port >> 2;
    case 0x01: {                                        // serial isolation: 0x55, 0xAA for a 1 bit
        if (s->pnp_state != PNP_ISOLATION || s->iso_bit >= 72) return 0xFF;
        int bit = (g_ident[s->iso_bit >> 3] >> (s->iso_bit & 7)) & 1;
        uint8_t v = bit ? (s->iso_second ? 0xAA : 0x55) : 0xFF;
        if (s->iso_second && ++s->iso_bit == 72) s->res_pos = 9;   // isolation shifted the serial id out
        s->iso_second = !s->iso_second;
        return v;
    }
    case 0x04: {                                        // resource data: serial id, then resources
        if (s->pnp_state != PNP_CONFIG) return 0xFF;
        int p = s->res_pos++;
        if (p < 9) return g_ident[p];
        return p - 9 < g_res_len ? g_res[p - 9] : 0;
    }
    case 0x05: return 0x01;                             // resource data always ready
    case 0x06: return s->csn;
    case 0x07: return 0;                                // one logical device
    default:
        if (s->pnp_addr >= 0x30) return s->ldn_regs[s->pnp_addr];
        return 0x00;
    }
}

static void set_read_port(sb16_t *s, uint16_t port)
{
    if (s->read_port == port) return;
    if (s->read_port) io_unregister(s->io, s->read_port, 1);
    s->read_port = 0;
    if (port >= 0x203 && port <= 0x3FF && io_register(s->io, port, 1, pnp_read, NULL, s)) s->read_port = port;
}

static void pnp_addr_wr(void *o, uint16_t port, uint32_t v, int size)
{
    sb16_t *s = o;
    (void)port; (void)size;
    if (s->pnp_state == PNP_WAIT_KEY) {                 // watch for the 32-byte initiation key
        if ((uint8_t)v == g_key[s->key_pos]) {
            if (++s->key_pos == 32) { s->pnp_state = PNP_SLEEP; s->key_pos = 0; }
        } else {
            s->key_pos = (uint8_t)v == g_key[0] ? 1 : 0;
        }
        return;
    }
    s->pnp_addr = (uint8_t)v;
}

static void pnp_data_wr(void *o, uint16_t port, uint32_t v, int size)
{
    sb16_t *s = o;
    (void)port; (void)size;
    if (s->pnp_state == PNP_WAIT_KEY) return;
    uint8_t b = (uint8_t)v;
    if (trace_sound()) fprintf(stderr, "[sb16] PnP wr reg %02X = %02X (state %d)\n", s->pnp_addr, b, s->pnp_state);
    switch (s->pnp_addr) {
    case 0x00: set_read_port(s, (uint16_t)(b << 2 | 3)); break;
    case 0x02:                                          // config control
        if (b & 1) { memset(s->ldn_regs + 0x30, 0, 0xD0); s->ldn_regs[0x74] = s->ldn_regs[0x75] = 4; s->ldn_regs[0x71] = 2; activate(s, false); }
        if (b & 4) s->csn = 0;
        if (b & 2) { s->pnp_state = PNP_WAIT_KEY; s->key_pos = 0; }
        break;
    case 0x03:                                          // wake[CSN]
        if (b == s->csn) {
            s->pnp_state = s->csn == 0 ? PNP_ISOLATION : PNP_CONFIG;
            s->iso_bit = 0; s->iso_second = false; s->res_pos = 0;
        } else {
            s->pnp_state = PNP_SLEEP;
        }
        break;
    case 0x06:                                          // card select number
        s->csn = b;
        if (s->pnp_state == PNP_ISOLATION) { s->pnp_state = PNP_CONFIG; s->res_pos = 9; }   // resource data follows
        if (trace_sound()) fprintf(stderr, "[sb16] PnP: CSN %d (read port %X)\n", b, s->read_port);
        break;
    case 0x07: break;                                   // logical device number (only 0)
    default:
        if (s->pnp_state != PNP_CONFIG || s->pnp_addr < 0x30) break;
        s->ldn_regs[s->pnp_addr] = b;
        if (s->pnp_addr == 0x30) activate(s, b & 1);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Mixer (CT1745)                                                      */
/* ------------------------------------------------------------------ */

static void mixer_reset(sb16_t *s)
{
    memset(s->mix, 0, sizeof s->mix);
    for (int r = 0x30; r <= 0x35; r++) s->mix[r] = 0xC0;     // master, voice, FM: 24 (-14 dB)
    s->mix[0x3C] = 0x1F;                                     // output switches
    s->mix[0x3D] = 0x15; s->mix[0x3E] = 0x0B;                // input switches
    for (int r = 0x44; r <= 0x47; r++) s->mix[r] = 0x80;     // treble / bass
}

static uint8_t mixer_read(sb16_t *s, uint8_t r)
{
    switch (r) {
    case 0x80: {                                             // IRQ select
        static const uint8_t bit[16] = { [2] = 1, [9] = 1, [5] = 2, [7] = 4, [10] = 8 };
        return s->irq >= 0 && s->irq < 16 ? bit[s->irq] : 0;
    }
    case 0x81: {                                             // DMA select
        uint8_t v = 0;
        if (s->dma8 == 0 || s->dma8 == 1 || s->dma8 == 3) v |= (uint8_t)(1u << s->dma8);
        if (s->dma16 >= 5 && s->dma16 <= 7) v |= (uint8_t)(1u << s->dma16);
        return v;
    }
    case 0x82: return (uint8_t)((s->irq8 ? 1 : 0) | (s->irq16 ? 2 : 0) | (s->irq_mpu ? 4 : 0));
    default: return s->mix[r];
    }
}

static void mixer_write(sb16_t *s, uint8_t r, uint8_t v)
{
    if (trace_sound()) fprintf(stderr, "[sb16] mixer %02X = %02X\n", r, v);
    if (r == 0x00) { mixer_reset(s); return; }
    if (r == 0x80 || r == 0x81 || r == 0x82) return;         // set through PnP on this card
    s->mix[r] = v;
    if (r == 0x22) { s->mix[0x30] = (uint8_t)((v & 0xF0) | 0x08); s->mix[0x31] = (uint8_t)(v << 4 | 0x08); }   // SBPro master
    if (r == 0x04) { s->mix[0x32] = (uint8_t)((v & 0xF0) | 0x08); s->mix[0x33] = (uint8_t)(v << 4 | 0x08); }   // SBPro voice
}

/* ------------------------------------------------------------------ */
/* DSP                                                                 */
/* ------------------------------------------------------------------ */

static void update_irq(sb16_t *s)
{
    bool on = s->irq8 || s->irq16;
    if (on != s->irq_line && s->set_irq && s->irq > 0 && s->base) {
        s->irq_line = on;
        s->set_irq(s->irq_opaque, s->irq, on);
    }
}

static void out_byte(sb16_t *s, uint8_t v)
{
    if (s->out_n < (int)sizeof s->out) s->out[(s->out_r + s->out_n++) % sizeof s->out] = v;
}

static void dsp_reset(sb16_t *s)
{
    s->run = false;
    s->out_r = s->out_n = 0;
    s->need = s->n_params = 0;
    s->irq8 = s->irq16 = false;
    s->paused = s->exit_auto = false;
    out_byte(s, 0xAA);
    update_irq(s);
}

static int param_count(uint8_t c)
{
    if (c >= 0xB0 && c <= 0xCF) return 3;
    switch (c) {
    case 0x10: case 0x38: case 0x40: case 0xE0: case 0xE4: return 1;
    case 0x14: case 0x16: case 0x17: case 0x24: case 0x41: case 0x42: case 0x48: case 0x74: case 0x75:
    case 0x76: case 0x77: case 0x80: return 2;
    default: return 0;
    }
}

static void start(sb16_t *s, bool w16, bool autoinit, bool input, uint8_t mode, uint32_t units)
{
    s->run = true;
    s->run16 = w16;
    s->autoinit = autoinit;
    s->input = input;
    s->stereo = (mode & 0x20) != 0;
    s->is_signed = (mode & 0x10) != 0;
    s->units = s->left = units ? units : 1;
    s->paused = s->exit_auto = false;
    s->phase = 0;
    memset(s->prev, 0, sizeof s->prev);
    memset(s->cur, 0, sizeof s->cur);
    if (trace_sound())
        fprintf(stderr, "[sb16] start %d-bit %s%s%s %s, %u Hz, %u units per block, dma %d\n", w16 ? 16 : 8,
                autoinit ? "auto-init " : "single ", input ? "input " : "output ", s->stereo ? "stereo" : "mono",
                s->is_signed ? "signed" : "unsigned", s->rate, s->units, w16 ? s->dma16 : s->dma8);
}

static void dsp_exec(sb16_t *s)
{
    uint8_t c = s->cmd, *p = s->params;
    if (trace_sound()) fprintf(stderr, "[sb16 %.3f] dsp %02X %02X %02X %02X (left %u)\n", (double)s->produced / SOUND_RATE, c, p[0], p[1], p[2], s->left);
    if (c >= 0xB0 && c <= 0xCF) {                       // SB16 DMA: mode, length - 1
        start(s, c < 0xC0, (c & 4) != 0, (c & 8) != 0, p[0], (uint32_t)(p[1] | p[2] << 8) + 1);
        return;
    }
    switch (c) {
    case 0x14: case 0x16: case 0x17: start(s, false, false, false, 0, (uint32_t)(p[0] | p[1] << 8) + 1); break;
    case 0x1C: case 0x1F: case 0x90: start(s, false, true, false, 0, (uint32_t)s->block8 + 1); break;
    case 0x91: start(s, false, false, false, 0, (uint32_t)s->block8 + 1); break;
    case 0x24: start(s, false, false, true, 0, (uint32_t)(p[0] | p[1] << 8) + 1); break;
    case 0x2C: case 0x98: start(s, false, true, true, 0, (uint32_t)s->block8 + 1); break;
    case 0x99: start(s, false, false, true, 0, (uint32_t)s->block8 + 1); break;
    case 0x40: s->time_const = p[0]; s->rate = 1000000u / (256u - p[0]); break;
    case 0x41: case 0x42: s->rate = (uint32_t)(p[0] << 8 | p[1]); break;
    case 0x48: s->block8 = (uint16_t)(p[0] | p[1] << 8); break;
    case 0x80: s->irq8 = true; update_irq(s); break;    // silence: interrupt straight away
    case 0xD0: case 0xD5: s->paused = true; break;
    case 0xD4: case 0xD6: s->paused = false; break;
    case 0xD1: s->speaker = true; break;
    case 0xD3: s->speaker = false; break;
    case 0xD8: out_byte(s, s->speaker ? 0xFF : 0x00); break;
    case 0xD9: case 0xDA: s->exit_auto = true; break;
    case 0xE0: out_byte(s, (uint8_t)~p[0]); break;
    case 0xE1: out_byte(s, 4); out_byte(s, 13); break;  // DSP 4.13
    case 0xE3: {
        static const char c_str[] = "COPYRIGHT (C) CREATIVE TECHNOLOGY LTD, 1992.";
        for (unsigned i = 0; i < sizeof c_str; i++) out_byte(s, (uint8_t)c_str[i]);
        break;
    }
    case 0xE4: s->test_reg = p[0]; break;
    case 0xE8: out_byte(s, s->test_reg); break;
    case 0xF2: s->irq8 = true; update_irq(s); break;
    case 0xF3: s->irq16 = true; update_irq(s); break;
    case 0x20: out_byte(s, 0x80); break;                // direct ADC: silence
    default: break;
    }
}

static uint32_t dsp_io_rd(void *o, uint16_t port, int size)
{
    sb16_t *s = o;
    (void)size;
    sb16_update(s);
    switch (port - s->base) {
    case 0x5: return mixer_read(s, s->mix_idx);
    case 0x4: return s->mix_idx;
    case 0xA: {
        if (!s->out_n) return 0xFF;
        uint8_t v = s->out[s->out_r];
        s->out_r = (s->out_r + 1) % (int)sizeof s->out;
        s->out_n--;
        return v;
    }
    case 0xC: return 0x7F;                              // write buffer ready
    case 0xE: s->irq8 = false; update_irq(s); return s->out_n ? 0xFF : 0x7F;   // data available? (+ack 8-bit)
    case 0xF:                                                                  // ack 16-bit
        if (trace_sound() && s->irq16) fprintf(stderr, "[sb16 %.3f] ack16\n", (double)s->produced / SOUND_RATE);
        s->irq16 = false; update_irq(s); return 0xFF;
    default: return 0xFF;                               // FM: not emulated
    }
}

static void dsp_io_wr(void *o, uint16_t port, uint32_t v, int size)
{
    sb16_t *s = o;
    (void)size;
    sb16_update(s);
    uint8_t b = (uint8_t)v;
    switch (port - s->base) {
    case 0x4: s->mix_idx = b; break;
    case 0x5: mixer_write(s, s->mix_idx, b); break;
    case 0x6:
        if (b & 1) s->reset_high = true;
        else if (s->reset_high) { s->reset_high = false; dsp_reset(s); }
        break;
    case 0xC:
        if (s->need) {
            s->params[s->n_params++] = b;
            if (s->n_params == s->need) { s->need = 0; dsp_exec(s); }
        } else {
            s->cmd = b;
            s->n_params = 0;
            s->need = param_count(b);
            if (!s->need) dsp_exec(s);
        }
        break;
    default: break;
    }
}

/* ---- MPU-401 (UART mode handshake only) ---- */

static uint32_t mpu_io_rd(void *o, uint16_t port, int size)
{
    sb16_t *s = o;
    (void)size;
    if (port == s->mpu_base) {                          // data
        if (!s->mpu_has_ack) return 0xFF;
        s->mpu_has_ack = false;
        return s->mpu_ack;
    }
    return s->mpu_has_ack ? 0x00 : 0x80;                // status: bit 7 set = nothing to read
}

static void mpu_io_wr(void *o, uint16_t port, uint32_t v, int size)
{
    sb16_t *s = o;
    (void)size; (void)v;
    if (port == s->mpu_base + 1) { s->mpu_ack = 0xFE; s->mpu_has_ack = true; }   // any command: ACK
}

/* ------------------------------------------------------------------ */
/* Sample transport                                                    */
/* ------------------------------------------------------------------ */

static void fetch(sb16_t *s)
{
    int ch = s->run16 ? s->dma16 : s->dma8, nch = s->stereo ? 2 : 1, bps = s->run16 ? 2 : 1;
    uint8_t b[4] = { 0 };
    if (s->input) {
        uint8_t z[4];
        if (s->is_signed) memset(z, 0, sizeof z);
        else if (s->run16) { z[0] = 0; z[1] = 0x80; z[2] = 0; z[3] = 0x80; }
        else memset(z, 0x80, sizeof z);
        dma_write(s->dma, ch, z, nch * bps);
    } else {
        int got = dma_read(s->dma, ch, b, nch * bps);
        memcpy(s->prev, s->cur, sizeof s->prev);
        for (int c = 0; c < 2; c++) {
            int k = nch == 2 ? c : 0;
            int v;
            if (got < nch * bps) v = 0;
            else if (s->run16) { int16_t w = (int16_t)(b[2 * k] | b[2 * k + 1] << 8); v = s->is_signed ? w : (int16_t)(w ^ 0x8000); }
            else v = s->is_signed ? (int8_t)b[k] * 256 : (b[k] - 128) * 256;
            s->cur[c] = (int16_t)v;
        }
    }
    if (s->left > (uint32_t)nch) { s->left -= (uint32_t)nch; return; }
    // End of block: interrupt, then reload (auto-init) or stop.
    if (trace_sound()) fprintf(stderr, "[sb16 %.3f] block done, irq%s\n", (double)s->produced / SOUND_RATE, (s->run16 ? s->irq16 : s->irq8) ? " (previous still pending)" : "");
    if (s->run16) s->irq16 = true; else s->irq8 = true;
    if (s->autoinit && !s->exit_auto) s->left = s->units;
    else s->run = false;
}

static double vol(uint8_t reg) { return pow(10.0, (-62.0 + 2.0 * (reg >> 3)) / 20.0); }

uint64_t sb16_update(sb16_t *s)
{
    uint64_t now = s->clock(s->clock_opaque);
    uint64_t target = (now - s->t0) / 1000u * SOUND_RATE / 1000000u;
    if (target <= s->produced) goto next_event;
    if (target - s->produced > SOUND_RATE / 5) s->produced = target - SOUND_RATE / 50;
    // Stopped: nothing to play. Paused (the driver does this when its writer is late): silence,
    // so the output keeps real time.
    if (!s->run || !s->rate) { s->produced = target; goto next_event; }
    {
        double step = (double)s->rate / SOUND_RATE;
        // Output gain (0x41/0x42) x1..x8 is analog on the real card; BeOS sets x2, so x2 is unity
        // here and full-scale samples don't clip.
        double gl = vol(s->mix[0x30]) * vol(s->mix[0x32]) * (double)(1 << (s->mix[0x41] >> 6)) / 2.0;
        double gr = vol(s->mix[0x31]) * vol(s->mix[0x33]) * (double)(1 << (s->mix[0x42] >> 6)) / 2.0;
        int16_t buf[512 * 2];
        while (s->produced < target) {
            int n = target - s->produced > 512 ? 512 : (int)(target - s->produced);
            for (int i = 0; i < n; i++) {
                double l = 0, r = 0;
                if (s->run && !s->paused) {
                    s->phase += step;
                    while (s->phase >= 1.0 && s->run) { s->phase -= 1.0; fetch(s); }
                    if (!s->input) {
                        double f = s->phase;
                        l = (s->prev[0] + (s->cur[0] - s->prev[0]) * f) * gl;
                        r = (s->prev[1] + (s->cur[1] - s->prev[1]) * f) * gr;
                    }
                }
                buf[2 * i]     = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
                buf[2 * i + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
            }
            s->produced += (uint64_t)n;
            if (!s->input && s->out_fn) s->out_fn(s->out_opaque, buf, n);
        }
        update_irq(s);
    }
next_event:
    if (!s->run || s->paused || !s->rate) return UINT64_MAX;
    {
        int nch = s->stereo ? 2 : 1;
        double frames = (double)((s->left + (uint32_t)nch - 1) / (uint32_t)nch) - s->phase;
        return (uint64_t)(frames / s->rate * 1e9) + 1000;
    }
}

/* ------------------------------------------------------------------ */
/* Setup                                                               */
/* ------------------------------------------------------------------ */

void sb16_init(sb16_t *s, io_bus_t *io, dma_t *dma, uint64_t (*clock)(void *), void *clock_opaque)
{
    memset(s, 0, sizeof *s);
    s->io = io;
    s->dma = dma;
    s->clock = clock;
    s->clock_opaque = clock_opaque;
    build_pnp_data();
    io_register(io, 0x279, 1, NULL, pnp_addr_wr, s);
    io_register(io, 0xA79, 1, NULL, pnp_data_wr, s);
    sb16_reset(s);
}

void sb16_reset(sb16_t *s)
{
    activate(s, false);
    set_read_port(s, 0);
    s->pnp_state = PNP_WAIT_KEY;
    s->key_pos = 0;
    s->csn = 0;
    memset(s->ldn_regs, 0, sizeof s->ldn_regs);
    s->ldn_regs[0x74] = s->ldn_regs[0x75] = 4;          // no DMA
    s->ldn_regs[0x71] = 2;                              // edge triggered, high
    s->irq = 0; s->dma8 = s->dma16 = 4;
    s->rate = 11025;
    s->block8 = 0x7FF;
    s->speaker = false;
    mixer_reset(s);
    s->reset_high = false;
    dsp_reset(s);
    s->t0 = s->clock(s->clock_opaque);
    s->produced = 0;
}
