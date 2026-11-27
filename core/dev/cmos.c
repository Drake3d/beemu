#include "cmos.h"
#include <string.h>
#include <time.h>

enum { RTC_SEC = 0, RTC_MIN = 2, RTC_HOUR = 4, RTC_WDAY = 6, RTC_MDAY = 7, RTC_MON = 8, RTC_YEAR = 9,
       RTC_A = 10, RTC_B = 11, RTC_C = 12, RTC_D = 13, RTC_CENTURY = 0x32 };
#define B_SET 0x80
#define B_PIE 0x40
#define B_UIE 0x10
#define B_BIN 0x04
#define B_24H 0x02
#define C_IRQF 0x80
#define C_PF   0x40
#define C_UF   0x10

static uint8_t enc(const cmos_t *c, int v)
{
    return (c->ram[RTC_B] & B_BIN) ? (uint8_t)v : (uint8_t)(((v / 10) << 4) | (v % 10));
}

static int dec(const cmos_t *c, uint8_t v)
{
    return (c->ram[RTC_B] & B_BIN) ? v : (v >> 4) * 10 + (v & 0xF);
}

static time_t guest_time(const cmos_t *c) { return time(NULL) + c->offset_s; }

static void fill_time(cmos_t *c)
{
    time_t t = guest_time(c);
    struct tm tm;
    localtime_r(&t, &tm);
    c->ram[RTC_SEC] = enc(c, tm.tm_sec);
    c->ram[RTC_MIN] = enc(c, tm.tm_min);
    if (c->ram[RTC_B] & B_24H) {
        c->ram[RTC_HOUR] = enc(c, tm.tm_hour);
    } else {
        int h = tm.tm_hour % 12;
        c->ram[RTC_HOUR] = (uint8_t)(enc(c, h ? h : 12) | (tm.tm_hour >= 12 ? 0x80 : 0));
    }
    c->ram[RTC_WDAY] = enc(c, tm.tm_wday + 1);
    c->ram[RTC_MDAY] = enc(c, tm.tm_mday);
    c->ram[RTC_MON] = enc(c, tm.tm_mon + 1);
    c->ram[RTC_YEAR] = enc(c, tm.tm_year % 100);
    c->ram[RTC_CENTURY] = enc(c, 19 + tm.tm_year / 100);
}

// The guest wrote a time register: move the offset so the clock reads that value.
static void store_time(cmos_t *c)
{
    struct tm tm = { 0 };
    tm.tm_sec = dec(c, c->ram[RTC_SEC]);
    tm.tm_min = dec(c, c->ram[RTC_MIN]);
    uint8_t h = c->ram[RTC_HOUR];
    if (c->ram[RTC_B] & B_24H) tm.tm_hour = dec(c, h);
    else tm.tm_hour = dec(c, h & 0x7F) % 12 + ((h & 0x80) ? 12 : 0);
    tm.tm_mday = dec(c, c->ram[RTC_MDAY]);
    tm.tm_mon = dec(c, c->ram[RTC_MON]) - 1;
    tm.tm_year = dec(c, c->ram[RTC_CENTURY]) * 100 + dec(c, c->ram[RTC_YEAR]) - 1900;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    if (t != (time_t)-1) c->offset_s = (int64_t)(t - time(NULL));
}

static uint64_t periodic_ns(const cmos_t *c)
{
    unsigned rate = c->ram[RTC_A] & 0xF;
    if (!rate) return 0;
    if (rate < 3) rate += 7;                       // 1, 2 behave as 8, 9
    return 1000000000ull / (32768u >> (rate - 1));
}

static uint32_t cmos_read(void *opaque, uint16_t port, int size)
{
    (void)size;
    cmos_t *c = opaque;
    if (port == 0x70) return 0xFF;
    uint8_t i = c->index;
    if (i <= RTC_YEAR || i == RTC_CENTURY) {
        if (!(c->ram[RTC_B] & B_SET)) fill_time(c);
        return c->ram[i];
    }
    if (i == RTC_A) return c->ram[RTC_A] & 0x7F;   // update-in-progress never shows
    if (i == RTC_C) {
        uint8_t v = c->ram[RTC_C];
        c->ram[RTC_C] = 0;
        c->set_irq(c->irq_opaque, 8, 0);
        return v;
    }
    return c->ram[i];
}

static void cmos_write(void *opaque, uint16_t port, uint32_t v, int size)
{
    (void)size;
    cmos_t *c = opaque;
    if (port == 0x70) { c->index = (uint8_t)(v & 0x7F); return; }   // bit 7: NMI mask (ignored)
    uint8_t i = c->index;
    switch (i) {
    case RTC_C: case RTC_D: return;                                  // read-only
    case RTC_A:
        c->ram[RTC_A] = (uint8_t)(v & 0x7F);
        c->next_periodic_ns = 0;
        return;
    case RTC_B: {
        uint8_t old = c->ram[RTC_B];
        if (!(old & B_SET)) fill_time(c);            // keep the time across a mode change
        c->ram[RTC_B] = (uint8_t)v;
        if ((old & B_SET) && !(v & B_SET)) store_time(c);
        c->next_periodic_ns = 0;
        return;
    }
    default:
        c->ram[i] = (uint8_t)v;
        if ((i <= RTC_YEAR || i == RTC_CENTURY) && !(c->ram[RTC_B] & B_SET)) store_time(c);
        return;
    }
}

void cmos_init(cmos_t *c, clock_fn now, void *now_opaque,
               void (*set_irq)(void *, int, int), void *irq_opaque)
{
    memset(c, 0, sizeof *c);
    c->now = now; c->now_opaque = now_opaque;
    c->set_irq = set_irq; c->irq_opaque = irq_opaque;
    c->ram[RTC_A] = 0x26;          // 32.768 kHz base, 1024 Hz periodic rate
    c->ram[RTC_B] = B_24H;         // BCD, 24-hour
    c->ram[RTC_D] = 0x80;          // valid RAM and time
    fill_time(c);
}

void cmos_register(cmos_t *c, io_bus_t *io)
{
    io_register(io, 0x70, 2, cmos_read, cmos_write, c);
}

void cmos_set(cmos_t *c, int index, uint8_t value) { c->ram[index & 0x7F] = value; }

uint64_t cmos_update(cmos_t *c)
{
    uint64_t now = c->now(c->now_opaque), wait = 10000000;
    uint8_t b = c->ram[RTC_B];
    uint64_t per = periodic_ns(c);
    if (per && (b & B_PIE)) {
        if (!c->next_periodic_ns) c->next_periodic_ns = now + per;
        if (now >= c->next_periodic_ns) {
            c->ram[RTC_C] |= C_IRQF | C_PF;
            c->set_irq(c->irq_opaque, 8, 1);
            c->next_periodic_ns = now + per;
        }
        if (c->next_periodic_ns - now < wait) wait = c->next_periodic_ns - now;
    }
    if ((b & B_UIE) && !(b & B_SET)) {
        if (!c->next_update_ns) c->next_update_ns = now + 1000000000ull;
        if (now >= c->next_update_ns) {
            c->ram[RTC_C] |= C_IRQF | C_UF;
            c->set_irq(c->irq_opaque, 8, 1);
            c->next_update_ns = now + 1000000000ull;
        }
        if (c->next_update_ns - now < wait) wait = c->next_update_ns - now;
    }
    return wait;
}
