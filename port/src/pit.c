/*
 * 8253 timer channel 0 and the 8259 mask register.
 *
 * The counter runs continuously against the host clock: reads return the
 * live count, and writing a new count restarts the period, as on the real
 * chip. The sound driver relies on this - it reprograms the timer whenever
 * two consecutive reads return the same value, taking a frozen counter for
 * a stalled timer.
 *
 * Channel 2 (gated by port 61h bit 0) is used by the sound driver to
 * calibrate its AdLib delay loops: it times 256 port reads with it.
 */
#include "host.h"

#include <math.h>

#define PIT_HZ 1193182.0

void pit_init(Host *h)
{
    h->pit.reload = 0x10000;
    h->pit.write_lo = -1;
    h->pit.latch_hi = -1;
    h->pit.last = host_time(h);     /* start of the current counting run */
    h->pit.acc = 0;                  /* ticks already raised in this run */
    h->pit.imr = 0xB8;
}

static double pit_period(Host *h)
{
    return (h->pit.reload ? h->pit.reload : 0x10000) / PIT_HZ;
}

void pit_tick(Host *h)
{
    PIT *p = &h->pit;
    double ticks = floor((host_time(h) - p->last) / pit_period(h));
    if (ticks > p->acc) {
        if (ticks - p->acc > 1)
            h->irq_lost[0] += (uint32_t)(ticks - p->acc) - 1;   /* ran past a tick */
        if (h->pending_irq[0]) {
            h->irq_merged++;                                    /* previous still waiting */
            if (h->trace && h->irq_merged % 25 == 0)
                host_log("merged tick at %04x:%04x IF=%d IMR=%02x", reg16(h, UC_X86_REG_CS),
                         reg16(h, UC_X86_REG_IP), !!(reg16(h, UC_X86_REG_EFLAGS) & FLAG_IF), h->pit.imr);
        }
        p->acc = ticks;
        host_raise_irq(h, 0);
    }
}

/* The live count. Emulated code runs in bursts far faster than a 386, so
 * two reads could land within the same 0.84 us count step; on real ISA
 * hardware each port access takes microseconds. Successive reads therefore
 * always differ, which keeps the driver's stall detection quiet. */
static uint16_t live_count(Host *h)
{
    PIT *p = &h->pit;
    double phase = fmod(host_time(h) - p->last, pit_period(h));
    uint32_t reload = p->reload ? p->reload : 0x10000;
    uint32_t c = reload - (uint32_t)(phase * PIT_HZ);
    uint16_t v = (uint16_t)(c ? c : 1);
    if (v == p->last_read)
        v = v > 1 ? v - 1 : (uint16_t)(reload - 1);
    p->last_read = v;
    return v;
}

static uint16_t ch2_count(Host *h)
{
    PIT *p = &h->pit;
    uint32_t reload = p->ch2_reload ? p->ch2_reload : 0x10000;
    if (!p->ch2_counting)
        return (uint16_t)reload;
    uint32_t done = (uint32_t)((host_time(h) - p->ch2_start) * PIT_HZ);
    return (uint16_t)(reload - done % reload);
}

bool pit_in(Host *h, uint16_t port, uint8_t *v)
{
    PIT *p = &h->pit;
    switch (port) {
    case 0x42: {
        uint16_t c = p->ch2_latched ? p->ch2_latch : ch2_count(h);
        if (!p->ch2_read_hi) {
            *v = c & 0xFF;
            if (!p->ch2_latched)
                p->ch2_latch = c, p->ch2_latched = true;   /* hold for the high byte */
        } else {
            *v = c >> 8;
            p->ch2_latched = false;
        }
        p->ch2_read_hi = !p->ch2_read_hi;
        return true;
    }
    case 0x40:
        if (p->latch_hi < 0) {
            uint16_t cnt = p->latched ? p->latch_value : live_count(h);
            p->latch_hi = cnt >> 8;
            *v = cnt & 0xFF;
        } else {
            *v = p->latch_hi;
            p->latch_hi = -1;
            p->latched = false;
        }
        return true;
    case 0x21: *v = p->imr; return true;
    case 0x20: *v = 0; return true;
    case 0x61: *v = p->port61; return true;
    }
    return false;
}

bool pit_out(Host *h, uint16_t port, uint8_t v)
{
    PIT *p = &h->pit;
    switch (port) {
    case 0x43:
        if ((v & 0xC0) == 0x80) {    /* channel 2 */
            if (!(v & 0x30)) {
                p->ch2_latch = ch2_count(h);
                p->ch2_latched = true;
            } else {
                p->ch2_write_hi = false;
            }
            p->ch2_read_hi = false;
            return true;
        }
        if (v & 0xC0)
            return true;
        if (!(v & 0x30)) {           /* counter latch */
            p->latch_value = live_count(h);
            p->latched = true;
            p->latch_hi = -1;
        } else {
            p->write_lo = -1;
        }
        return true;
    case 0x40:
        if (p->write_lo < 0) {
            p->write_lo = v;
        } else {
            p->reload = p->write_lo | v << 8;
            if (!p->reload)
                p->reload = 0x10000;
            p->write_lo = -1;
            p->last = host_time(h); /* new count: the period restarts */
            h->pit_reprograms++;
            p->acc = 0;
        }
        return true;
    case 0x42:
        if (!p->ch2_write_hi) {
            p->ch2_reload = (p->ch2_reload & 0xFF00) | v;
        } else {
            p->ch2_reload = (p->ch2_reload & 0x00FF) | v << 8;
            if (p->ch2_gate) {                          /* reload while gated: restart */
                p->ch2_start = host_time(h);
                p->ch2_counting = true;
            }
        }
        p->ch2_write_hi = !p->ch2_write_hi;
        return true;
    case 0x21: p->imr = v; return true;
    case 0x20: return true;
    case 0x61: {
        bool gate = v & 1;
        if (gate && !p->ch2_gate) {                     /* rising gate starts counting */
            p->ch2_start = host_time(h);
            p->ch2_counting = true;
        }
        p->ch2_gate = gate;
        p->port61 = v;
        return true;
    }
    }
    return false;
}
