#include "ps2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int trace_ps2(void)
{
    static int t = -1;
    if (t < 0) { const char *e = getenv("BEEMU_TRACE_PS2"); t = e && *e == '1'; }
    return t;
}

#define ST_OBF  0x01
#define ST_SYS  0x04
#define ST_CMD  0x08
#define ST_INH  0x10        // 1 = keyboard not inhibited
#define ST_AUX  0x20

#define CB_KINT 0x01
#define CB_AINT 0x02
#define CB_SYS  0x04
#define CB_KDIS 0x10
#define CB_ADIS 0x20
#define CB_XLAT 0x40

static void q_put(ps2_queue_t *q, uint8_t v)
{
    if (q->count == PS2_QUEUE) return;
    q->buf[(q->head + q->count) % PS2_QUEUE] = v;
    q->count++;
}

static int q_get(ps2_queue_t *q)
{
    if (!q->count) return -1;
    uint8_t v = q->buf[q->head];
    q->head = (q->head + 1) % PS2_QUEUE;
    q->count--;
    return v;
}

static void update_irq(ps2_t *p)
{
    p->set_irq(p->irq_opaque, 1, p->out_full && !p->out_aux && (p->cmd_byte & CB_KINT));
    p->set_irq(p->irq_opaque, 12, p->out_full && p->out_aux && (p->cmd_byte & CB_AINT));
}

// Move the next queued byte into the output buffer if it is free.
static void refill(ps2_t *p)
{
    if (!p->out_full) {
        int v = -1;
        if (!(p->cmd_byte & CB_KDIS) && (v = q_get(&p->kbd_q)) >= 0) p->out_aux = false;
        else if (!(p->cmd_byte & CB_ADIS) && (v = q_get(&p->aux_q)) >= 0) p->out_aux = true;
        if (v >= 0) { p->out = (uint8_t)v; p->out_full = true; }
    }
    update_irq(p);
}

// A reply from the controller itself goes in front of device data.
static void ctl_reply(ps2_t *p, uint8_t v)
{
    p->out = v; p->out_full = true; p->out_aux = false;
    update_irq(p);
}

static void kbd_put(ps2_t *p, uint8_t v) { q_put(&p->kbd_q, v); refill(p); }
static void aux_put(ps2_t *p, uint8_t v) { q_put(&p->aux_q, v); refill(p); }

/* ---- keyboard ---- */

static void kbd_command(ps2_t *p, uint8_t v)
{
    if (p->kbd_pending) {
        uint8_t cmd = p->kbd_pending;
        p->kbd_pending = 0;
        if (cmd == 0xF0 && v == 0) { kbd_put(p, 0xFA); kbd_put(p, p->kbd_set); return; }
        if (cmd == 0xF0) p->kbd_set = v >= 1 && v <= 3 ? v : 2;
        kbd_put(p, 0xFA);                                   // LEDs, typematic, set: just ack
        return;
    }
    switch (v) {
    case 0xED: case 0xF0: case 0xF3:
        p->kbd_pending = v; kbd_put(p, 0xFA); break;
    case 0xEE: kbd_put(p, 0xEE); break;                     // echo
    case 0xF2:                                              // identify: MF2 keyboard
        kbd_put(p, 0xFA); kbd_put(p, 0xAB);
        kbd_put(p, (p->cmd_byte & CB_XLAT) ? 0x41 : 0x83);
        break;
    case 0xF4: p->kbd_scan = true;  kbd_put(p, 0xFA); break;
    case 0xF5: p->kbd_scan = false; kbd_put(p, 0xFA); break;
    case 0xF6: p->kbd_scan = true;  kbd_put(p, 0xFA); break;
    case 0xFF:                                              // reset and self-test
        p->kbd_q.count = 0; p->kbd_scan = true; p->kbd_set = 2;
        kbd_put(p, 0xFA); kbd_put(p, 0xAA);
        break;
    default: kbd_put(p, 0xFA); break;
    }
}

/* ---- mouse ---- */

static void aux_command(ps2_t *p, uint8_t v)
{
    if (p->aux_pending) {
        uint8_t cmd = p->aux_pending;
        p->aux_pending = 0;
        if (cmd == 0xF3) p->aux_rate = v;
        else if (cmd == 0xE8) p->aux_res = v & 3;
        aux_put(p, 0xFA);
        return;
    }
    switch (v) {
    case 0xE6: p->aux_scale21 = false; aux_put(p, 0xFA); break;
    case 0xE7: p->aux_scale21 = true;  aux_put(p, 0xFA); break;
    case 0xE8: case 0xF3: p->aux_pending = v; aux_put(p, 0xFA); break;
    case 0xE9:                                              // status request
        aux_put(p, 0xFA);
        aux_put(p, (uint8_t)((p->aux_report ? 0x20 : 0) | (p->aux_scale21 ? 0x10 : 0) | (p->aux_buttons & 7)));
        aux_put(p, p->aux_res);
        aux_put(p, p->aux_rate);
        break;
    case 0xF2: aux_put(p, 0xFA); aux_put(p, 0x00); break;   // ID: standard mouse
    case 0xF4: p->aux_report = true;  aux_put(p, 0xFA); break;
    case 0xF5: p->aux_report = false; aux_put(p, 0xFA); break;
    case 0xF6: p->aux_report = false; p->aux_rate = 100; p->aux_res = 2; aux_put(p, 0xFA); break;
    case 0xFF:
        p->aux_q.count = 0; p->aux_report = false; p->aux_rate = 100; p->aux_res = 2;
        aux_put(p, 0xFA); aux_put(p, 0xAA); aux_put(p, 0x00);
        break;
    default: aux_put(p, 0xFA); break;                       // E1, EA, EE, F0, ...: accept
    }
}

void ps2_mouse(ps2_t *p, int dx, int dy, uint8_t buttons)
{
    p->aux_buttons = buttons & 7;
    if (!p->aux_report) return;
    dy = -dy;                                               // PS/2: up is positive
    if (dx > 255) dx = 255;
    if (dx < -256) dx = -256;
    if (dy > 255) dy = 255;
    if (dy < -256) dy = -256;
    uint8_t b0 = (uint8_t)(0x08 | (buttons & 7) | (dx < 0 ? 0x10 : 0) | (dy < 0 ? 0x20 : 0));
    aux_put(p, b0);
    aux_put(p, (uint8_t)dx);
    aux_put(p, (uint8_t)dy);
}

/* ---- controller ---- */

static void ctl_command(ps2_t *p, uint8_t v)
{
    if (trace_ps2()) fprintf(stderr, "[ps2] ctl cmd %02X\n", v);
    switch (v) {
    case 0x20: ctl_reply(p, p->cmd_byte); break;
    case 0x60: case 0xD1: case 0xD2: case 0xD3: case 0xD4: p->ctl_pending = v; break;
    case 0xA7: p->cmd_byte |= CB_ADIS; break;
    case 0xA8: p->cmd_byte &= (uint8_t)~CB_ADIS; refill(p); break;
    case 0xA9: ctl_reply(p, 0x00); break;                   // aux interface test: OK
    case 0xAA: p->cmd_byte |= CB_SYS; ctl_reply(p, 0x55); break;   // self-test passed
    case 0xAB: ctl_reply(p, 0x00); break;                   // keyboard interface test: OK
    case 0xAD: p->cmd_byte |= CB_KDIS; break;
    case 0xAE: p->cmd_byte &= (uint8_t)~CB_KDIS; refill(p); break;
    case 0xC0: ctl_reply(p, 0x80); break;                   // input port: keyboard not locked
    case 0xD0: ctl_reply(p, p->outport); break;
    default:
        if (v >= 0xF0 && !(v & 1) && p->reset) p->reset(p->reset_opaque);   // pulse reset line
        break;
    }
}

static void data_write(ps2_t *p, uint8_t v)
{
    if (trace_ps2()) fprintf(stderr, "[ps2] data wr %02X (after %02X)\n", v, p->ctl_pending);
    uint8_t cmd = p->ctl_pending;
    p->ctl_pending = 0;
    switch (cmd) {
    case 0x60: p->cmd_byte = v; refill(p); break;
    case 0xD1:
        p->outport = v;
        if (!(v & 1) && p->reset) p->reset(p->reset_opaque);
        break;
    case 0xD2: kbd_put(p, v); break;
    case 0xD3: aux_put(p, v); break;
    case 0xD4: aux_command(p, v); break;
    default:
        p->cmd_byte &= (uint8_t)~CB_KDIS;                    // writing to the keyboard enables it
        kbd_command(p, v);
        break;
    }
}

static uint32_t ps2_read(void *opaque, uint16_t port, int size)
{
    (void)size;
    ps2_t *p = opaque;
    if (port == 0x64) {
        return (uint32_t)((p->out_full ? ST_OBF : 0) | (p->out_full && p->out_aux ? ST_AUX : 0) |
                          ((p->cmd_byte & CB_SYS) ? ST_SYS : 0) | (p->ctl_pending ? ST_CMD : 0) | ST_INH);
    }
    uint8_t v = p->out;
    if (trace_ps2()) fprintf(stderr, "[ps2] rd %02X%s%s\n", v, p->out_aux ? " aux" : "", p->out_full ? "" : " (empty)");
    p->out_full = false;
    update_irq(p);          // reading drops the IRQ line; the next byte raises it again (a new edge)
    refill(p);
    return v;
}

static void ps2_write(void *opaque, uint16_t port, uint32_t v, int size)
{
    (void)size;
    ps2_t *p = opaque;
    if (port == 0x64) ctl_command(p, (uint8_t)v);
    else              data_write(p, (uint8_t)v);
}

void ps2_init(ps2_t *p, void (*set_irq)(void *, int, int), void *irq_opaque,
              void (*reset)(void *), void *reset_opaque)
{
    memset(p, 0, sizeof *p);
    p->set_irq = set_irq; p->irq_opaque = irq_opaque;
    p->reset = reset; p->reset_opaque = reset_opaque;
    p->cmd_byte = CB_KINT | CB_XLAT | CB_ADIS;
    p->outport = 0x03;
    p->kbd_scan = true;
    p->kbd_set = 2;
    p->aux_rate = 100;
    p->aux_res = 2;
}

void ps2_register(ps2_t *p, io_bus_t *io)
{
    io_register(io, 0x60, 1, ps2_read, ps2_write, p);
    io_register(io, 0x64, 1, ps2_read, ps2_write, p);
}

/* ---- HID usage -> scancode set 1 ---- */
// Low byte = make code; 0x100 = E0-prefixed. 0 = no mapping.
static const uint16_t HID_SET1[256] = {
    [0x04] = 0x1E, [0x05] = 0x30, [0x06] = 0x2E, [0x07] = 0x20, [0x08] = 0x12, [0x09] = 0x21,   // a-f
    [0x0A] = 0x22, [0x0B] = 0x23, [0x0C] = 0x17, [0x0D] = 0x24, [0x0E] = 0x25, [0x0F] = 0x26,   // g-l
    [0x10] = 0x32, [0x11] = 0x31, [0x12] = 0x18, [0x13] = 0x19, [0x14] = 0x10, [0x15] = 0x13,   // m-r
    [0x16] = 0x1F, [0x17] = 0x14, [0x18] = 0x16, [0x19] = 0x2F, [0x1A] = 0x11, [0x1B] = 0x2D,   // s-x
    [0x1C] = 0x15, [0x1D] = 0x2C,                                                               // y z
    [0x1E] = 0x02, [0x1F] = 0x03, [0x20] = 0x04, [0x21] = 0x05, [0x22] = 0x06, [0x23] = 0x07,   // 1-6
    [0x24] = 0x08, [0x25] = 0x09, [0x26] = 0x0A, [0x27] = 0x0B,                                 // 7-0
    [0x28] = 0x1C, [0x29] = 0x01, [0x2A] = 0x0E, [0x2B] = 0x0F, [0x2C] = 0x39,                  // enter esc bksp tab space
    [0x2D] = 0x0C, [0x2E] = 0x0D, [0x2F] = 0x1A, [0x30] = 0x1B, [0x31] = 0x2B, [0x32] = 0x2B,   // - = [ ] \ #
    [0x33] = 0x27, [0x34] = 0x28, [0x35] = 0x29, [0x36] = 0x33, [0x37] = 0x34, [0x38] = 0x35,   // ; ' ` , . /
    [0x39] = 0x3A,                                                                              // caps lock
    [0x3A] = 0x3B, [0x3B] = 0x3C, [0x3C] = 0x3D, [0x3D] = 0x3E, [0x3E] = 0x3F, [0x3F] = 0x40,   // F1-F6
    [0x40] = 0x41, [0x41] = 0x42, [0x42] = 0x43, [0x43] = 0x44, [0x44] = 0x57, [0x45] = 0x58,   // F7-F12
    [0x47] = 0x46,                                                                              // scroll lock
    [0x49] = 0x152, [0x4A] = 0x147, [0x4B] = 0x149, [0x4C] = 0x153, [0x4D] = 0x14F, [0x4E] = 0x151, // ins home pgup del end pgdn
    [0x4F] = 0x14D, [0x50] = 0x14B, [0x51] = 0x150, [0x52] = 0x148,                             // right left down up
    [0x53] = 0x45, [0x54] = 0x135, [0x55] = 0x37, [0x56] = 0x4A, [0x57] = 0x4E, [0x58] = 0x11C, // numlock kp/ * - + enter
    [0x59] = 0x4F, [0x5A] = 0x50, [0x5B] = 0x51, [0x5C] = 0x4B, [0x5D] = 0x4C, [0x5E] = 0x4D,   // kp 1-6
    [0x5F] = 0x47, [0x60] = 0x48, [0x61] = 0x49, [0x62] = 0x52, [0x63] = 0x53,                  // kp 7-9 0 .
    [0x64] = 0x56, [0x65] = 0x15D,                                                              // 102nd key, menu
    [0xE0] = 0x1D, [0xE1] = 0x2A, [0xE2] = 0x38, [0xE3] = 0x15B,                                // lctrl lshift lalt lgui
    [0xE4] = 0x11D, [0xE5] = 0x36, [0xE6] = 0x138, [0xE7] = 0x15C,                              // rctrl rshift ralt rgui
};

void ps2_key(ps2_t *p, int hid, bool down)
{
    if (!p->kbd_scan || hid < 0 || hid > 255) return;
    if (hid == 0x48) {                                      // Pause: make only, no break
        if (down) {
            static const uint8_t seq[] = { 0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5 };
            for (unsigned i = 0; i < sizeof seq; i++) kbd_put(p, seq[i]);
        }
        return;
    }
    if (hid == 0x46) {                                      // Print Screen
        if (down) { kbd_put(p, 0xE0); kbd_put(p, 0x2A); kbd_put(p, 0xE0); kbd_put(p, 0x37); }
        else      { kbd_put(p, 0xE0); kbd_put(p, 0xB7); kbd_put(p, 0xE0); kbd_put(p, 0xAA); }
        return;
    }
    uint16_t sc = HID_SET1[hid];
    if (!sc) return;
    if (sc & 0x100) kbd_put(p, 0xE0);
    kbd_put(p, (uint8_t)((sc & 0x7F) | (down ? 0 : 0x80)));
}
