/* VGA in mode 13h: DAC, register files and retrace status. */
#include "host.h"

#include <string.h>

void vga_init(Host *h)
{
    memset(&h->vga, 0, sizeof h->vga);
    h->vga.mode = 3;
    h->vga.seq[4] = 0x0E;
}

void vga_set_mode(Host *h, uint8_t mode)
{
    h->vga.mode = mode;
    wr8(h, 0x449, mode);
    if (mode == 0x13) {
        static const uint8_t zero[64000];
        wrmem(h, 0xA0000, zero, sizeof zero);
    }
}

bool vga_in(Host *h, uint16_t port, uint8_t *v)
{
    VGA *g = &h->vga;
    switch (port) {
    case 0x3DA: {
        /* 70 Hz refresh; vertical retrace during the last ~5 % of a frame */
        double phase = host_time(h) * 70.0;
        phase -= (long)phase;
        *v = phase > 0.95 ? 0x09 : ((long)(phase * 400) & 1);
        return true;
    }
    case 0x3C9:
        *v = g->dac[g->dac_read];
        g->dac_read = (g->dac_read + 1) % 768;
        return true;
    case 0x3C5: *v = g->seq[g->seq_index & 7]; return true;
    case 0x3CF: *v = g->gc[g->gc_index % 9]; return true;
    case 0x3D5: *v = g->crtc[g->crtc_index % 0x19]; return true;
    case 0x3CC: *v = 0x63; return true;
    case 0x3C4: case 0x3CE: case 0x3D4: case 0x3C7: case 0x3C8:
        *v = 0;
        return true;
    }
    return false;
}

bool vga_out(Host *h, uint16_t port, uint8_t v)
{
    VGA *g = &h->vga;
    switch (port) {
    case 0x3C8: g->dac_write = v * 3; return true;
    case 0x3C7: g->dac_read = v * 3; return true;
    case 0x3C9:
        g->dac[g->dac_write] = v & 0x3F;
        g->dac_write = (g->dac_write + 1) % 768;
        return true;
    case 0x3C4: g->seq_index = v; return true;
    case 0x3C5: g->seq[g->seq_index & 7] = v; return true;
    case 0x3CE: g->gc_index = v; return true;
    case 0x3CF: g->gc[g->gc_index % 9] = v; return true;
    case 0x3D4: g->crtc_index = v; return true;
    case 0x3D5: g->crtc[g->crtc_index % 0x19] = v; return true;
    case 0x3C0: case 0x3C2: case 0x3C6: return true;
    }
    return false;
}

/* Convert the mode 13h frame buffer to 320x200 RGBA pixels. */
void vga_render(Host *h, uint32_t *rgba)
{
    static uint8_t fb[64000];
    uint32_t pal[256];
    for (int i = 0; i < 256; i++) {
        uint32_t r = h->vga.dac[i * 3] * 255 / 63;
        uint32_t g = h->vga.dac[i * 3 + 1] * 255 / 63;
        uint32_t b = h->vga.dac[i * 3 + 2] * 255 / 63;
        pal[i] = 0xFF000000u | r << 16 | g << 8 | b;
    }
    if (h->vga.mode != 0x13) {
        memset(rgba, 0, 64000 * 4);
        return;
    }
    rdmem(h, 0xA0000, fb, sizeof fb);
    for (int i = 0; i < 64000; i++)
        rgba[i] = pal[fb[i]];
}
