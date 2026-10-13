/*
 * CPU core glue: memory, interrupt vector table, BIOS stubs, program
 * loading, slice execution and hardware interrupt injection.
 *
 * Every IVT entry initially points at F000:(vector*2), where HLT; IRET sits.
 * HLT ends the current emulation run; the host then performs the native
 * service for the vector and returns from the interrupt itself. (A code
 * hook would be skipped by Unicorn while a stop request is pending.)
 * Software INT instructions and injected IRQs both enter through the IVT,
 * so handlers the game installs itself chain naturally.
 */
#include "host.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <libgen.h>
#include <ctype.h>

double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

void host_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

uint16_t reg16(Host *h, int r)
{
    uint64_t v = 0;
    uc_reg_read(h->uc, r, &v);
    return (uint16_t)v;
}

void setreg16(Host *h, int r, uint16_t v)
{
    uint64_t x = v;
    uc_reg_write(h->uc, r, &x);
}

void rdmem(Host *h, uint32_t a, void *buf, size_t n) { uc_mem_read(h->uc, a, buf, n); }
void wrmem(Host *h, uint32_t a, const void *buf, size_t n) { uc_mem_write(h->uc, a, buf, n); }

uint8_t rd8(Host *h, uint32_t a)
{
    uint8_t v;
    rdmem(h, a, &v, 1);
    return v;
}

uint16_t rd16(Host *h, uint32_t a)
{
    uint8_t b[2];
    rdmem(h, a, b, 2);
    return b[0] | b[1] << 8;
}

void wr8(Host *h, uint32_t a, uint8_t v) { wrmem(h, a, &v, 1); }

void wr16(Host *h, uint32_t a, uint16_t v)
{
    uint8_t b[2] = {v & 0xFF, v >> 8};
    wrmem(h, a, b, 2);
}

/* Inside a BIOS stub the INT frame (IP, CS, FLAGS) is on top of the stack. */
static uint32_t stub_flags_addr(Host *h)
{
    return lin(reg16(h, UC_X86_REG_SS), reg16(h, UC_X86_REG_SP) + 4);
}

void set_cf(Host *h, bool on)
{
    uint32_t a = stub_flags_addr(h);
    uint16_t f = rd16(h, a);
    wr16(h, a, on ? (f | FLAG_CF) : (f & ~FLAG_CF));
}

void set_zf(Host *h, bool on)
{
    uint32_t a = stub_flags_addr(h);
    uint16_t f = rd16(h, a);
    wr16(h, a, on ? (f | FLAG_ZF) : (f & ~FLAG_ZF));
}

/* Real-mode interrupt entry: push FLAGS, CS, IP and jump through the IVT. */
static void do_interrupt(Host *h, int vec)
{
    uint16_t ss = reg16(h, UC_X86_REG_SS), sp = reg16(h, UC_X86_REG_SP);
    uint16_t flags = reg16(h, UC_X86_REG_EFLAGS);
    uint16_t cs = reg16(h, UC_X86_REG_CS), ip = reg16(h, UC_X86_REG_IP);
    sp -= 6;
    wr16(h, lin(ss, sp), ip);
    wr16(h, lin(ss, sp + 2), cs);
    wr16(h, lin(ss, sp + 4), flags);
    setreg16(h, UC_X86_REG_SP, sp);
    setreg16(h, UC_X86_REG_EFLAGS, flags & ~(FLAG_IF | 0x100));
    setreg16(h, UC_X86_REG_CS, rd16(h, vec * 4 + 2));
    setreg16(h, UC_X86_REG_IP, rd16(h, vec * 4));
}

/* Software INT n: only note it and stop. Changing CS:IP inside the hook
 * does not survive a pending stop request (CS changes, IP is restored);
 * the IVT entry is taken by host_slice once the run has ended cleanly. */
static void on_intr(uc_engine *uc, uint32_t intno, void *user)
{
    Host *h = user;
    h->pending_swint = (int)intno;
    uc_emu_stop(uc);
}

static void bios_service(Host *h, int vec)
{
    switch (vec) {
    case 0x10: int10(h); break;
    case 0x16: int16(h); break;
    case 0x1A: int1a(h); break;
    case 0x20: h->exit_code = 0; h->exited = true; uc_emu_stop(h->uc); break;
    case 0x21: int21(h); break;
    case 0x2F: int2f(h); break;
    case 0x33: int33(h); break;
    case 0x08: {                       /* BIOS timer tick */
        uint32_t t;
        rdmem(h, 0x46C, &t, 4);
        t++;
        wrmem(h, 0x46C, &t, 4);
        break;
    }
    case 0x13:                         /* disk: pretend success */
        setreg16(h, UC_X86_REG_AX, 0);
        set_cf(h, false);
        break;
    case 0x15:
        setreg16(h, UC_X86_REG_AX, 0x8600);
        set_cf(h, true);
        break;
    case 0x09:
        bios_int9(h);
        break;
    case 0x1B: case 0x1C: case 0x23: case 0x24: case 0x05:
        break;
    default:
        if (h->trace)
            host_log("unhandled INT %02x AX=%04x", vec, reg16(h, UC_X86_REG_AX));
        break;
    }
}

/* Called when a run ended on a stub's HLT: IP points at the IRET. */
static bool handle_stub(Host *h)
{
    if (reg16(h, UC_X86_REG_CS) != BIOS_SEG)
        return false;
    uint16_t ip = reg16(h, UC_X86_REG_IP);
    if (ip >= 0x200 || !(ip & 1))
        return false;
    int vec = ip / 2;
    if (h->trace && vec != 8 && vec != 0x1C && vec != 0x33)
        host_log("INT %02x AX=%04x", vec, reg16(h, UC_X86_REG_AX));
    bios_service(h, vec);
    if (h->exited)
        return false;
    /* Return from the interrupt here instead of executing the stub's IRET:
     * a run stopped right before that IRET would otherwise look like a
     * fresh stub entry and repeat the service. */
    uint16_t ss = reg16(h, UC_X86_REG_SS), sp = reg16(h, UC_X86_REG_SP);
    setreg16(h, UC_X86_REG_IP, rd16(h, lin(ss, sp)));
    setreg16(h, UC_X86_REG_CS, rd16(h, lin(ss, sp + 2)));
    setreg16(h, UC_X86_REG_EFLAGS, rd16(h, lin(ss, sp + 4)));
    setreg16(h, UC_X86_REG_SP, sp + 6);
    return true;
}

static uint32_t on_in(uc_engine *uc, uint32_t port, int size, void *user)
{
    (void)uc;
    Host *h = user;
    h->vtime += h->io_cost;
    uint8_t v = 0xFF;
    uint32_t r;
    if (vga_in(h, port, &v) || pit_in(h, port, &v) || kbd_in(h, port, &v) || sb_in(h, port, &v))
        r = v;
    else
        r = 0xFF;
    if (size == 2) {
        uint8_t hi = 0xFF;
        if (!(vga_in(h, port + 1, &hi) || pit_in(h, port + 1, &hi) || sb_in(h, port + 1, &hi)))
            hi = 0xFF;
        r |= (uint32_t)hi << 8;
    }
    return r;
}

static void port_out8(Host *h, uint16_t port, uint8_t v)
{
    if (vga_out(h, port, v) || pit_out(h, port, v) || sb_out(h, port, v))
        return;
    if (port == 0x60 || port == 0x64)
        return;
    if (h->trace)
        host_log("OUT %04x,%02x", port, v);
}

static void on_out(uc_engine *uc, uint32_t port, int size, uint32_t value, void *user)
{
    (void)uc;
    Host *h = user;
    h->vtime += h->io_cost;
    port_out8(h, port, value & 0xFF);
    if (size == 2)
        port_out8(h, port + 1, (value >> 8) & 0xFF);
}

static void on_block(uc_engine *uc, uint64_t address, uint32_t size, void *user);

void host_init(Host *h, const char *root)
{
    memset(h, 0, sizeof *h);
    snprintf(h->root, sizeof h->root, "%s", root);
    strcpy(h->cwd, "\\BIFI");
    h->speed = 40e6;
    h->io_cost = 1e-6;
    if (uc_open(UC_ARCH_X86, UC_MODE_16, &h->uc) != UC_ERR_OK) {
        host_log("uc_open failed");
        exit(1);
    }
    uc_mem_map(h->uc, 0, MEM_SIZE, UC_PROT_ALL);

    uint8_t stubs[0x200];
    for (int n = 0; n < 256; n++) {
        stubs[n * 2] = 0xF4;                             /* HLT */
        stubs[n * 2 + 1] = 0xCF;                         /* IRET */
    }
    wrmem(h, BIOS_LIN, stubs, sizeof stubs);
    for (int n = 0; n < 256; n++) {
        wr16(h, n * 4, n * 2);
        wr16(h, n * 4 + 2, BIOS_SEG);
    }
    wr8(h, 0x449, 3);
    wr16(h, 0x44A, 80);
    wr16(h, 0x413, 640);
    wr16(h, 0x463, 0x3D4);
    wr8(h, 0x484, 24);
    wr16(h, 0x41A, 0x1E);                                /* keyboard buffer head */
    wr16(h, 0x41C, 0x1E);                                /* and tail */
    wr16(h, 0x480, 0x1E);
    wr16(h, 0x482, 0x3E);

    uc_hook hk;
    uc_hook_add(h->uc, &hk, UC_HOOK_INTR, on_intr, h, 1, 0);
    uc_hook_add(h->uc, &hk, UC_HOOK_BLOCK, on_block, h, 1, 0);
    uc_hook_add(h->uc, &hk, UC_HOOK_INSN, on_in, h, 1, 0, UC_X86_INS_IN);
    uc_hook_add(h->uc, &hk, UC_HOOK_INSN, on_out, h, 1, 0, UC_X86_INS_OUT);

    vga_init(h);
    pit_init(h);
    h->sb = sb_create(h);
}

void host_free(Host *h)
{
    for (int i = 0; i < MAX_FILES; i++)
        if (h->files[i])
            fclose(h->files[i]);
    uc_close(h->uc);
}

int host_load(Host *h, const char *exe, const char *args)
{
    FILE *f = fopen(exe, "rb");
    if (!f) {
        host_log("cannot open %s", exe);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(len);
    fread(data, 1, len, f);
    fclose(f);

    uint16_t *hd = (uint16_t *)data;
    if (hd[0] != 0x5A4D) {
        free(data);
        return -1;
    }
    uint32_t size = (hd[2] - 1) * 512 + (hd[1] ? hd[1] : 512);
    uint32_t hdr = hd[4] * 16;
    uint16_t load_seg = PSP_SEG + 0x10;
    wrmem(h, lin(load_seg, 0), data + hdr, size - hdr);
    for (int i = 0; i < hd[3]; i++) {
        uint16_t off = *(uint16_t *)(data + hd[12] + i * 4);
        uint16_t seg = *(uint16_t *)(data + hd[12] + i * 4 + 2);
        uint32_t a = lin(load_seg + seg, off);
        wr16(h, a, rd16(h, a) + load_seg);
    }

    char base[64];
    snprintf(base, sizeof base, "%s", strrchr(exe, '/') ? strrchr(exe, '/') + 1 : exe);
    for (char *p = base; *p; p++)
        *p = toupper((unsigned char)*p);
    snprintf(h->program, sizeof h->program, "%s", base);

    char env[256];
    int n = snprintf(env, sizeof env, "PATH=C:\\%cCOMSPEC=C:\\COMMAND.COM%c%c%c%cC:\\BIFI\\%s",
                     0, 0, 0, 1, 0, base);
    wrmem(h, lin(ENV_SEG, 0), env, n + 1);

    uint8_t psp[256] = {0};
    psp[0] = 0xCD;
    psp[1] = 0x20;
    psp[2] = (MEM_TOP + 1) & 0xFF;
    psp[3] = (MEM_TOP + 1) >> 8;
    psp[0x2C] = ENV_SEG & 0xFF;
    psp[0x2D] = ENV_SEG >> 8;
    char tail[127] = "";
    if (args && *args)
        snprintf(tail, sizeof tail, " %s", args);
    psp[0x80] = (uint8_t)strlen(tail);
    memcpy(psp + 0x81, tail, strlen(tail));
    psp[0x81 + strlen(tail)] = '\r';
    wrmem(h, lin(PSP_SEG, 0), psp, sizeof psp);

    h->load_seg = load_seg;
    h->dta = lin(PSP_SEG, 0x80);
    h->nblocks = 1;
    h->blocks[0].seg = PSP_SEG;
    h->blocks[0].paras = MEM_TOP + 1 - PSP_SEG;
    h->exited = false;

    setreg16(h, UC_X86_REG_CS, load_seg + hd[11]);
    setreg16(h, UC_X86_REG_IP, hd[10]);
    setreg16(h, UC_X86_REG_SS, load_seg + hd[7]);
    setreg16(h, UC_X86_REG_SP, hd[8]);
    setreg16(h, UC_X86_REG_DS, PSP_SEG);
    setreg16(h, UC_X86_REG_ES, PSP_SEG);
    setreg16(h, UC_X86_REG_EFLAGS, 0x0202);
    free(data);
    return 0;
}

void host_raise_irq(Host *h, int irq)
{
    h->pending_irq[irq] = 1;
}

static void deliver_irqs(Host *h)
{
    h->irq_waiting = false;
    int irq = -1;
    for (int i = 0; i < 8 && irq < 0; i++)
        if (h->pending_irq[i] && !(h->pit.imr & (1 << i)))
            irq = i;
    if (irq < 0)
        return;
    if (!(reg16(h, UC_X86_REG_EFLAGS) & FLAG_IF)) {
        h->irq_waiting = true;          /* the block hook watches for STI */
        return;
    }
    h->pending_irq[irq] = 0;
    h->irq_delivered[irq]++;
    do_interrupt(h, 8 + irq);
    for (int i = 0; i < 8; i++)          /* more pending: take them after this handler */
        if (h->pending_irq[i] && !(h->pit.imr & (1 << i)))
            h->irq_waiting = true;
}

/*
 * Slicing: a block hook counts translated blocks and, every 256 blocks,
 * checks the clock; once the slice time is used up it stops the engine
 * from the emulation thread itself. (Stopping from another thread crashes
 * on Apple Silicon because JIT write protection is per thread, and a count
 * argument to uc_emu_start instruments every single instruction.)
 */
static void on_block(uc_engine *uc, uint64_t address, uint32_t size, void *user)
{
    (void)address; (void)size;
    Host *h = user;
    h->exec_bytes += size;
    h->raw_bytes += size;
    h->vtime += size / h->speed;
    ++h->block_count;
    /* A hardware IRQ is waiting for IF: stop as soon as interrupts are
     * enabled again, like a real CPU taking the interrupt after STI/IRET. */
    if (h->irq_waiting && !(h->block_count & 7)) {
        uint64_t fl = 0;
        uc_reg_read(uc, UC_X86_REG_EFLAGS, &fl);
        if (fl & FLAG_IF) {
            uc_emu_stop(uc);
            return;
        }
    }
    if (h->block_count & 15)
        return;
    if (h->vtime >= h->slice_end)
        uc_emu_stop(uc);
}

void host_set_slice(Host *h, int slice_us)
{
    h->slice_us = slice_us;
}

static uc_err emu_run(Host *h, uint32_t start)
{
    h->slice_end = h->vtime + h->slice_us * 1e-6;
    return uc_emu_start(h->uc, start, MEM_SIZE, 0, 0);
}

static uc_err emu_run(Host *h, uint32_t start);

static bool run_once(Host *h, uc_err *err)
{
    uint32_t start = lin(reg16(h, UC_X86_REG_CS), reg16(h, UC_X86_REG_IP));
    h->pending_swint = -1;
    *err = emu_run(h, start);
    if (*err != UC_ERR_OK || h->exited)
        return false;
    if (h->pending_swint >= 0) {
        do_interrupt(h, h->pending_swint);
        h->pending_swint = -1;
        return true;
    }
    return handle_stub(h);
}

void host_slice(Host *h)
{
    uc_err err = UC_ERR_OK;
    /* INT calls and stub services chain quickly (INT 21h in a loop) */
    for (int i = 0; i < 64 && run_once(h, &err); i++)
        ;
    if (err != UC_ERR_OK && !h->exited) {
        host_log("CPU error %s at %04x:%04x", uc_strerror(err),
                 reg16(h, UC_X86_REG_CS), reg16(h, UC_X86_REG_IP));
        h->exited = true;
        h->exit_code = 255;
        return;
    }
    if (h->exited)
        return;
    pit_tick(h);
    sb_update(h);
    deliver_irqs(h);
}
