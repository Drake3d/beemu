// Intel 8042 keyboard controller (ports 0x60/0x64) with a PS/2 keyboard and
// mouse behind it. Host keys arrive as USB HID usage codes (which SDL scancodes
// are) and are sent as scancode set 1, i.e. what the controller produces with
// translation on (the PC default).
#ifndef PS2_H
#define PS2_H

#include <stdbool.h>
#include <stdint.h>
#include "io.h"

#define PS2_QUEUE 64

typedef struct {
    uint8_t buf[PS2_QUEUE];
    int head, count;
} ps2_queue_t;

typedef struct {
    uint8_t     cmd_byte;       // controller configuration byte
    uint8_t     out;            // output buffer
    bool        out_full, out_aux;
    uint8_t     ctl_pending;    // controller command waiting for its data byte
    uint8_t     outport;        // bit 0 reset (active low), bit 1 A20
    ps2_queue_t kbd_q, aux_q;
    // keyboard
    uint8_t     kbd_pending;    // keyboard command waiting for a parameter
    bool        kbd_scan;       // scanning enabled
    uint8_t     kbd_set;        // scancode set the guest asked for
    // mouse
    uint8_t     aux_pending;
    bool        aux_report;     // data reporting enabled
    uint8_t     aux_rate, aux_res;
    bool        aux_scale21;
    uint8_t     aux_buttons;
    // wiring
    void      (*set_irq)(void *opaque, int irq, int level);
    void       *irq_opaque;
    void      (*reset)(void *opaque);       // CPU reset request (0xFE, output port bit 0)
    void       *reset_opaque;
} ps2_t;

void ps2_init(ps2_t *p, void (*set_irq)(void *, int, int), void *irq_opaque,
              void (*reset)(void *), void *reset_opaque);
void ps2_register(ps2_t *p, io_bus_t *io);
void ps2_key(ps2_t *p, int hid_usage, bool down);
void ps2_mouse(ps2_t *p, int dx, int dy, uint8_t buttons);   // buttons: bit 0 left, 1 right, 2 middle

#endif
