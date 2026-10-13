/*
 * Sound Blaster 2.0 (DSP 2.01) with 8-bit DMA playback on channel 1 and
 * IRQ 7, plus the 8237 DMA controller registers the driver programs.
 *
 * Playback is advanced in guest time from sb_update(): samples are fetched
 * from guest memory as the DSP would consume them, block-end interrupts are
 * raised on schedule, and the samples go into a ring buffer that
 * sb_render() resamples to the host output rate.
 */
#include "host.h"
#include "opl.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define OUT_RATE  44100
#define OPL_RING  32768        /* stereo frames */

#define SB_BASE   0x220
#define SB_IRQ    7
#define SB_DMA    1
#define RING      65536

struct SB {
    Host *host;
    /* DSP */
    int reset_state;
    uint8_t out[16];
    int out_head, out_tail;
    uint8_t cmd;
    int params_needed;
    uint8_t params[4];
    int nparams;
    uint8_t time_constant;
    uint32_t block_len;        /* bytes per block (length - 1 as programmed) */
    bool speaker;
    bool playing;
    bool auto_init;
    uint32_t block_left;
    bool irq_pending;
    double sample_clock;       /* fractional DSP samples owed */
    double last_update;
    /* DMA channel 1 */
    uint16_t dma_base_addr, dma_base_count;
    uint16_t dma_addr, dma_count;
    uint8_t dma_page, dma_mode;
    bool dma_masked;
    bool dma_flipflop;
    /* output ring at the DSP rate */
    uint8_t ring[RING];
    uint32_t ring_w, ring_r;
    double resample_pos;
    uint32_t ring_rate;
    uint32_t late_irqs, total_irqs, underruns, stale_blocks;
    uint32_t block_sum[8];     /* checksum of each block when last played */
    /* AdLib / OPL2 at 388h (and mirrored at base+8) */
    uint8_t opl_index;
    uint8_t opl_regs[256];
    double opl_t1_start, opl_t2_start;
    uint8_t opl_status;
    double opl_last;
    uint32_t opl_w, opl_r;
    int16_t opl_ring[OPL_RING * 2];
};

/* The chip itself stays outside the SB struct so snapshots can copy SB
 * as plain bytes; register contents are replayed on restore. */
static Opl *chip;

static void out_push(SB *s, uint8_t v)
{
    int next = (s->out_tail + 1) % 16;
    if (next != s->out_head) {
        s->out[s->out_tail] = v;
        s->out_tail = next;
    }
}

SB *sb_create(Host *h)
{
    SB *s = calloc(1, sizeof(SB));
    s->host = h;
    s->time_constant = 256 - 45;       /* ~22 kHz */
    s->dma_masked = true;
    s->last_update = host_time(h);
    s->ring_rate = 22050;
    s->opl_last = host_time(h);
    if (!chip)
        chip = opl_create(OUT_RATE);
    (void)h;
    return s;
}

static uint32_t dsp_rate(SB *s)
{
    return 1000000 / (256 - s->time_constant);
}

static void start_dma(SB *s, bool auto_init, uint32_t len)
{
    s->auto_init = auto_init;
    s->block_len = len;
    s->block_left = len;
    s->playing = true;
    s->last_update = host_time(s->host);
    s->sample_clock = 0;
}

static void dsp_command(Host *h, SB *s)
{
    uint8_t *p = s->params;
    if (getenv("BIFI_SBTRACE"))
        host_log("SB cmd %02x %02x %02x rate=%u block=%u", s->cmd, p[0], p[1], dsp_rate(s), s->block_len);
    switch (s->cmd) {
    case 0x10:                                  /* direct DAC sample */
        break;
    case 0x14:                                  /* 8-bit single-cycle DMA */
        start_dma(s, false, (p[0] | p[1] << 8) + 1);
        break;
    case 0x1C: case 0x90:                       /* 8-bit auto-init DMA */
        start_dma(s, true, s->block_len ? s->block_len : 0x10000);
        break;
    case 0x91:                                  /* high speed single-cycle */
        start_dma(s, false, s->block_len ? s->block_len : 0x10000);
        break;
    case 0x40:
        s->time_constant = p[0];
        break;
    case 0x48:
        s->block_len = (p[0] | p[1] << 8) + 1;
        break;
    case 0xD0:                                  /* pause */
        s->playing = false;
        break;
    case 0xD4:                                  /* continue */
        if (s->block_len)
            s->playing = true;
        break;
    case 0xD1: s->speaker = true; break;
    case 0xD3: s->speaker = false; break;
    case 0xD8: out_push(s, s->speaker ? 0xFF : 0x00); break;
    case 0xDA:                                  /* exit auto-init */
        s->auto_init = false;
        break;
    case 0xE1:                                  /* version 2.01 */
        out_push(s, 2);
        out_push(s, 1);
        break;
    case 0xE0:
        out_push(s, ~p[0]);
        break;
    case 0xF2:                                  /* trigger IRQ */
        s->irq_pending = true;
        host_raise_irq(h, SB_IRQ);
        break;
    default:
        if (h->trace)
            host_log("SB: unhandled DSP command %02x", s->cmd);
        break;
    }
}

static int params_for(uint8_t cmd)
{
    switch (cmd) {
    case 0x10: case 0x40: case 0xE0: case 0x38: return 1;
    case 0x14: case 0x48: case 0x16: case 0x17: case 0x74: case 0x75: case 0x76: case 0x77: return 2;
    default: return 0;
    }
}

static bool is_dma_port(uint16_t port)
{
    return port <= 0x0F || port == 0x83 || port == 0x87 || port == 0x81 || port == 0x82;
}

/* Render FM output up to now, so register writes take effect on time. */
static void opl_catch_up(SB *s)
{
    double now = host_time(s->host);
    int frames = (int)((now - s->opl_last) * OUT_RATE);
    if (frames <= 0)
        return;
    s->opl_last += (double)frames / OUT_RATE;
    if (frames > OUT_RATE / 10)
        frames = OUT_RATE / 10;
    int16_t tmp[OUT_RATE / 10 * 2];
    opl_generate(chip, tmp, frames);
    for (int i = 0; i < frames; i++) {
        if (s->opl_w - s->opl_r >= OPL_RING)
            break;
        uint32_t w = s->opl_w++ % OPL_RING;
        s->opl_ring[w * 2] = tmp[i * 2];
        s->opl_ring[w * 2 + 1] = tmp[i * 2 + 1];
    }
}

static void opl_write_reg(SB *s, uint8_t reg, uint8_t v)
{
    opl_catch_up(s);
    s->opl_regs[reg] = v;
    if (reg == 0x04) {
        if (v & 0x80) {
            s->opl_status = 0;
            return;
        }
        double now = host_time(s->host);
        if (v & 0x01) s->opl_t1_start = now;
        if (v & 0x02) s->opl_t2_start = now;
        return;
    }
    if (reg == 0x02 || reg == 0x03)
        return;
    opl_write(chip, reg, v);
}

static uint8_t opl_read_status(SB *s)
{
    double now = host_time(s->host);
    uint8_t ctl = s->opl_regs[0x04];
    if ((ctl & 0x01) && !(ctl & 0x40) &&
        now - s->opl_t1_start >= (256 - s->opl_regs[0x02]) * 80e-6)
        s->opl_status |= 0xC0;
    if ((ctl & 0x02) && !(ctl & 0x20) &&
        now - s->opl_t2_start >= (256 - s->opl_regs[0x03]) * 320e-6)
        s->opl_status |= 0xA0;
    return s->opl_status;
}

bool sb_in(Host *h, uint16_t port, uint8_t *v)
{
    SB *s = h->sb;
    if (port == 0x388 || port == SB_BASE + 8) {
        *v = opl_read_status(s);
        return true;
    }
    if (port == 0x389 || port == SB_BASE + 9) {
        *v = 0xFF;
        return true;
    }
    if (is_dma_port(port)) {
        switch (port) {
        case SB_DMA * 2:
            *v = s->dma_flipflop ? s->dma_addr >> 8 : s->dma_addr & 0xFF;
            s->dma_flipflop = !s->dma_flipflop;
            return true;
        case SB_DMA * 2 + 1:
            *v = s->dma_flipflop ? s->dma_count >> 8 : s->dma_count & 0xFF;
            s->dma_flipflop = !s->dma_flipflop;
            return true;
        case 0x08:
            *v = (s->dma_count == 0xFFFF) ? (1 << SB_DMA) : 0;
            return true;
        default:
            *v = 0;
            return true;
        }
    }
    switch (port) {
    case SB_BASE + 0xA:                         /* read data */
        if (s->out_head != s->out_tail) {
            *v = s->out[s->out_head];
            s->out_head = (s->out_head + 1) % 16;
        } else {
            *v = 0xFF;
        }
        return true;
    case SB_BASE + 0xC:                         /* write status: never busy */
        *v = 0x7F;
        return true;
    case SB_BASE + 0xE:                         /* read status, acknowledges IRQ */
        s->irq_pending = false;
        *v = (s->out_head != s->out_tail) ? 0xFF : 0x7F;
        return true;
    case SB_BASE + 0x6:
        *v = 0xFF;
        return true;
    }
    return false;
}

bool sb_out(Host *h, uint16_t port, uint8_t v)
{
    SB *s = h->sb;
    if (port == 0x388 || port == SB_BASE + 8) {
        s->opl_index = v;
        return true;
    }
    if (port == 0x389 || port == SB_BASE + 9) {
        opl_write_reg(s, s->opl_index, v);
        return true;
    }
    if (is_dma_port(port)) {
        switch (port) {
        case SB_DMA * 2:
            if (!s->dma_flipflop) s->dma_base_addr = (s->dma_base_addr & 0xFF00) | v;
            else s->dma_base_addr = (s->dma_base_addr & 0x00FF) | v << 8;
            s->dma_addr = s->dma_base_addr;
            s->dma_flipflop = !s->dma_flipflop;
            break;
        case SB_DMA * 2 + 1:
            if (!s->dma_flipflop) s->dma_base_count = (s->dma_base_count & 0xFF00) | v;
            else s->dma_base_count = (s->dma_base_count & 0x00FF) | v << 8;
            s->dma_count = s->dma_base_count;
            s->dma_flipflop = !s->dma_flipflop;
            break;
        case 0x83:
            s->dma_page = v;
            break;
        case 0x0A:
            if ((v & 3) == SB_DMA)
                s->dma_masked = v & 4;
            break;
        case 0x0B:
            if ((v & 3) == SB_DMA)
                s->dma_mode = v;
            break;
        case 0x0C:
            s->dma_flipflop = false;
            break;
        case 0x0D:
            s->dma_masked = true;
            s->dma_flipflop = false;
            break;
        case 0x0F:
            s->dma_masked = v & (1 << SB_DMA);
            break;
        }
        return true;
    }
    switch (port) {
    case SB_BASE + 0x6:
        if (v & 1) {
            s->reset_state = 1;
        } else if (s->reset_state) {
            s->reset_state = 0;
            s->out_head = s->out_tail = 0;
            s->playing = false;
            s->nparams = s->params_needed = 0;
            out_push(s, 0xAA);
        }
        return true;
    case SB_BASE + 0xC:
        if (s->params_needed) {
            s->params[s->nparams++] = v;
            if (s->nparams == s->params_needed) {
                s->params_needed = 0;
                dsp_command(h, s);
            }
        } else {
            s->cmd = v;
            s->nparams = 0;
            s->params_needed = params_for(v);
            if (!s->params_needed)
                dsp_command(h, s);
        }
        return true;
    case SB_BASE + 0x4: case SB_BASE + 0x5:     /* mixer: ignore */
        return true;
    }
    return false;
}

/* Consume one byte from the DMA channel; returns false if it cannot run. */
static bool dma_read(Host *h, SB *s, uint8_t *v)
{
    if (s->dma_masked)
        return false;
    uint32_t a = (uint32_t)s->dma_page << 16 | s->dma_addr;
    *v = rd8(h, a);
    s->dma_addr++;
    s->dma_count--;
    if (s->dma_count == 0xFFFF) {
        if (s->dma_mode & 0x10) {               /* auto-initialize */
            s->dma_addr = s->dma_base_addr;
            s->dma_count = s->dma_base_count;
        } else {
            s->dma_masked = true;
        }
    }
    return true;
}

/* Advance playback to the current time. Called after every CPU slice. */
void sb_update(Host *h)
{
    SB *s = h->sb;
    opl_catch_up(s);
    double now = host_time(h);
    double dt = now - s->last_update;
    s->last_update = now;
    if (!s->playing)
        return;
    if (dt > 0.1)
        dt = 0.1;
    uint32_t rate = dsp_rate(s);
    s->ring_rate = rate;
    s->sample_clock += dt * rate;
    while (s->sample_clock >= 1.0 && s->playing) {
        s->sample_clock -= 1.0;
        uint8_t v = 0x80;
        if (!dma_read(h, s, &v)) {
            s->playing = false;
            break;
        }
        if ((s->ring_w - s->ring_r) < RING)
            s->ring[s->ring_w++ % RING] = s->speaker ? v : 0x80;
        if (s->block_left == s->block_len && s->block_len && s->block_len <= 4096) {
            /* starting a block: was it rewritten since it was last played? */
            uint32_t base = (uint32_t)s->dma_page << 16 | s->dma_addr;
            uint32_t idx = ((s->dma_addr - s->dma_base_addr) / s->block_len) & 7, sum = 0;
            uint8_t lo = 255, hi = 0;
            for (uint32_t i = 0; i < s->block_len; i += 2) {
                uint8_t b = rd8(h, base + i);
                sum = sum * 31 + b;
                if (b < lo) lo = b;
                if (b > hi) hi = b;
            }
            if (sum == s->block_sum[idx] && hi - lo > 2)   /* ignore silence */
                s->stale_blocks++;
            s->block_sum[idx] = sum;
        }
        if (--s->block_left == 0) {
            if (s->irq_pending)
                s->late_irqs++;                 /* guest has not serviced the previous block */
            s->total_irqs++;
            if (getenv("BIFI_SBTRACE"))
                host_log("SB block irq dma addr=%04x count=%04x", s->dma_addr, s->dma_count);
            s->irq_pending = true;
            host_raise_irq(h, SB_IRQ);
            if (s->auto_init)
                s->block_left = s->block_len;
            else
                s->playing = false;
        }
    }
}

void sb_stats(Host *h, uint32_t *late, uint32_t *total, uint32_t *underruns)
{
    *underruns = h->sb->stale_blocks;
    *late = h->sb->late_irqs;
    *total = h->sb->total_irqs;
}

/*
 * Output filter for the DAC channel. The real card smooths its 8-bit
 * output with an analog low-pass; without it the images of the ~21 kHz
 * sample rate sound metallic. Second-order Butterworth, fc 8 kHz.
 */
typedef struct {
    double b0, b1, b2, a1, a2, x1, x2, y1, y2;
} Biquad;

static double biquad_run(Biquad *f, double x)
{
    double y = f->b0 * x + f->b1 * f->x1 + f->b2 * f->x2 - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1;
    f->x1 = x;
    f->y2 = f->y1;
    f->y1 = y;
    return y;
}

static Biquad dac_filter(int rate)
{
    const double fc = 8000.0, q = 0.7071;
    double w = 2 * 3.14159265358979 * fc / rate, alpha = sin(w) / (2 * q), c = cos(w);
    double a0 = 1 + alpha;
    Biquad f = {0};
    f.b0 = (1 - c) / 2 / a0;
    f.b1 = (1 - c) / a0;
    f.b2 = (1 - c) / 2 / a0;
    f.a1 = -2 * c / a0;
    f.a2 = (1 - alpha) / a0;
    return f;
}

int sb_render(Host *h, int16_t *out, int frames, int rate)
{
    SB *s = h->sb;
    static Biquad lp;
    static int lp_rate;
    if (lp_rate != rate) {
        lp = dac_filter(rate);
        lp_rate = rate;
    }
    double step = (double)s->ring_rate / rate;
    for (int i = 0; i < frames; i++) {
        int16_t v = 0;
        if (s->playing && s->ring_w - s->ring_r <= 1)
            s->underruns++;
        if (s->ring_w - s->ring_r > 1) {
            int a = s->ring[s->ring_r % RING] - 128;
            int b = s->ring[(s->ring_r + 1) % RING] - 128;
            double f = s->resample_pos;
            v = (int16_t)((a + (b - a) * f) * 256);
            s->resample_pos += step;
            while (s->resample_pos >= 1.0 && s->ring_w - s->ring_r > 1) {
                s->resample_pos -= 1.0;
                s->ring_r++;
            }
        }
        int f = (int)biquad_run(&lp, v);
        int l = f, r = f;
        if (s->opl_w != s->opl_r) {
            uint32_t o = s->opl_r++ % OPL_RING;
            l += s->opl_ring[o * 2];
            r += s->opl_ring[o * 2 + 1];
        }
        out[i * 2] = l > 32767 ? 32767 : l < -32768 ? -32768 : l;
        out[i * 2 + 1] = r > 32767 ? 32767 : r < -32768 ? -32768 : r;
    }
    /* keep FM latency bounded if output fell behind */
    if (s->opl_w - s->opl_r > OUT_RATE / 5)
        s->opl_r = s->opl_w - OUT_RATE / 10;
    return frames;
}

size_t sb_state_size(void)
{
    return sizeof(SB);
}

void sb_state_get(Host *h, void *buf)
{
    memcpy(buf, h->sb, sizeof(SB));
}

void sb_state_set(Host *h, const void *buf)
{
    memcpy(h->sb, buf, sizeof(SB));
    SB *s = h->sb;
    s->host = h;
    s->last_update = s->opl_last = host_time(h);
    s->opl_w = s->opl_r = 0;
    for (int reg = 0x20; reg < 0x100; reg++)     /* restore the chip's registers */
        opl_write(chip, reg, s->opl_regs[reg]);
    opl_write(chip, 0x01, s->opl_regs[0x01]);
    opl_write(chip, 0x08, s->opl_regs[0x08]);
}
