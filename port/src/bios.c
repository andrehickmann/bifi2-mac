/* BIOS services: video, keyboard, clock, multiplex and the mouse driver. */
#include "host.h"

#include <string.h>

#define FUNC_TABLE_SEG 0xF010

void int10(Host *h)
{
    uint16_t ax = reg16(h, UC_X86_REG_AX);
    uint8_t ah = ax >> 8;
    switch (ah) {
    case 0x00:
        vga_set_mode(h, ax & 0x7F);
        break;
    case 0x0F:
        setreg16(h, UC_X86_REG_AX, (h->vga.mode == 0x13 ? 40 : 80) << 8 | h->vga.mode);
        setreg16(h, UC_X86_REG_BX, reg16(h, UC_X86_REG_BX) & 0x00FF);
        break;
    case 0x1A:                                  /* display combination: VGA */
        setreg16(h, UC_X86_REG_AX, 0x001A);
        setreg16(h, UC_X86_REG_BX, 0x0008);
        break;
    case 0x1B: {                                /* functionality information */
        uint8_t buf[64] = {0};
        buf[0] = 0x00; buf[1] = 0x01;           /* -> F010:0100 */
        buf[2] = FUNC_TABLE_SEG & 0xFF; buf[3] = FUNC_TABLE_SEG >> 8;
        buf[4] = h->vga.mode;
        buf[0x27] = 0x00; buf[0x28] = 0x01;     /* 256 colours */
        buf[0x2D] = 0x02;                       /* colour display */
        wrmem(h, lin(reg16(h, UC_X86_REG_ES), reg16(h, UC_X86_REG_DI)), buf, sizeof buf);
        uint8_t table[8] = {0xFF, 0xFF, 0x0F, 0, 0, 0, 0, 0x07};
        wrmem(h, lin(FUNC_TABLE_SEG, 0x100), table, sizeof table);
        setreg16(h, UC_X86_REG_AX, (ax & 0xFF00) | 0x1B);
        break;
    }
    case 0x10:
        if ((ax & 0xFF) == 0x12) {              /* set block of DAC registers */
            uint16_t start = reg16(h, UC_X86_REG_BX), count = reg16(h, UC_X86_REG_CX);
            if (start + count <= 256)
                rdmem(h, lin(reg16(h, UC_X86_REG_ES), reg16(h, UC_X86_REG_DX)),
                      h->vga.dac + start * 3, count * 3);
        }
        break;
    case 0x11:
        if (ax == 0x1130) {                     /* font information */
            setreg16(h, UC_X86_REG_CX, 16);
            setreg16(h, UC_X86_REG_DX, 24);
            setreg16(h, UC_X86_REG_ES, 0xC000);
            setreg16(h, UC_X86_REG_BP, 0);
        }
        break;
    case 0x08:
        setreg16(h, UC_X86_REG_AX, 0x0720);
        break;
    default:
        break;
    }
}

void int16(Host *h)
{
    uint8_t ah = reg16(h, UC_X86_REG_AX) >> 8;
    if (ah == 0x00 || ah == 0x10) {
        int k = kbd_pop_bios(h);
        setreg16(h, UC_X86_REG_AX, k < 0 ? 0 : k);
    } else if (ah == 0x01 || ah == 0x11) {
        int k = kbd_peek_bios(h);
        set_zf(h, k < 0);
        if (k >= 0)
            setreg16(h, UC_X86_REG_AX, k);
    } else if (ah == 0x02 || ah == 0x12) {
        setreg16(h, UC_X86_REG_AX, reg16(h, UC_X86_REG_AX) & 0xFF00);
    }
}

void int1a(Host *h)
{
    if ((reg16(h, UC_X86_REG_AX) >> 8) == 0) {
        uint32_t t;
        rdmem(h, 0x46C, &t, 4);
        setreg16(h, UC_X86_REG_CX, t >> 16);
        setreg16(h, UC_X86_REG_DX, t & 0xFFFF);
        setreg16(h, UC_X86_REG_AX, 0);
    }
}

void int2f(Host *h)
{
    uint16_t ax = reg16(h, UC_X86_REG_AX);
    setreg16(h, UC_X86_REG_AX, ax == 0x4300 ? 0 : (ax & 0xFF00));   /* no XMS */
}

/*
 * Mouse driver. The game keeps its own cursor, integrating mickeys with
 * acceleration. For an absolute host pointer we write the target position
 * straight into the game's cursor state whenever it polls the motion
 * counters (INT 33h AX=000Bh), and report no motion.
 */
typedef struct {
    const char *program;
    uint16_t seg;          /* data segment relative to the load segment */
    int shift;             /* layout offset relative to XP01 */
} CursorLayout;

static const CursorLayout layouts[] = {
    {"XP00.EXE", 0x0DD7, -0x216},
    {"XP01.EXE", 0x250A, 0},
};

enum {
    CUR_X = 0x486, CUR_Y = 0x488, HOT_X_OUT = 0x48A, HOT_Y_OUT = 0x48C,
    HOT_X = 0x3A2, HOT_Y = 0x3A4, MIN_X = 0x3AA, MIN_Y = 0x3AC,
    MAX_X = 0x3AE, MAX_Y = 0x3B0, MOVED = 0x396, ACC_X = 0x482, ACC_Y = 0x484,
};

static bool write_cursor(Host *h)
{
    const CursorLayout *l = NULL;
    for (size_t i = 0; i < sizeof layouts / sizeof *layouts; i++)
        if (!strcmp(layouts[i].program, h->program))
            l = &layouts[i];
    if (!l)
        return false;
    uint32_t base = lin(h->load_seg + l->seg, 0) + l->shift;
    int x = h->mouse.target_x, y = h->mouse.target_y;
    int16_t minx = rd16(h, base + MIN_X), maxx = rd16(h, base + MAX_X);
    int16_t miny = rd16(h, base + MIN_Y), maxy = rd16(h, base + MAX_Y);
    if (x < minx) x = minx;
    if (x > maxx) x = maxx;
    if (y < miny) y = miny;
    if (y > maxy) y = maxy;
    uint16_t flags = 0;
    if (rd16(h, base + CUR_X) != x) { wr16(h, base + CUR_X, x); flags |= 1; }
    if (rd16(h, base + CUR_Y) != y) { wr16(h, base + CUR_Y, y); flags |= 2; }
    wr16(h, base + HOT_X_OUT, x + rd16(h, base + HOT_X));
    wr16(h, base + HOT_Y_OUT, y + rd16(h, base + HOT_Y));
    wr16(h, base + ACC_X, 0);
    wr16(h, base + ACC_Y, 0);
    if (flags)
        wr16(h, base + MOVED, rd16(h, base + MOVED) | flags);
    h->mouse.x = x * 2;
    h->mouse.y = y;
    return true;
}

void mouse_set_buttons(Host *h, int b)
{
    Mouse *m = &h->mouse;
    for (int i = 0; i < 3; i++) {
        int bit = 1 << i;
        if ((b & bit) && !(m->buttons & bit))
            m->press_count[i]++;
        if (!(b & bit) && (m->buttons & bit))
            m->release_count[i]++;
    }
    m->buttons = b;
}

void int33(Host *h)
{
    Mouse *m = &h->mouse;
    uint16_t ax = reg16(h, UC_X86_REG_AX), bx = reg16(h, UC_X86_REG_BX);
    switch (ax) {
    case 0x00:
        setreg16(h, UC_X86_REG_AX, 0xFFFF);
        setreg16(h, UC_X86_REG_BX, 2);
        break;
    case 0x03:
        setreg16(h, UC_X86_REG_BX, m->buttons);
        setreg16(h, UC_X86_REG_CX, m->x);
        setreg16(h, UC_X86_REG_DX, m->y);
        break;
    case 0x05: case 0x06: {
        int i = bx > 2 ? 2 : bx;
        int *c = ax == 5 ? m->press_count : m->release_count;
        setreg16(h, UC_X86_REG_AX, m->buttons);
        setreg16(h, UC_X86_REG_BX, c[i]);
        c[i] = 0;
        setreg16(h, UC_X86_REG_CX, m->x);
        setreg16(h, UC_X86_REG_DX, m->y);
        break;
    }
    case 0x0B:
        if (m->has_target)
            write_cursor(h);
        setreg16(h, UC_X86_REG_CX, 0);
        setreg16(h, UC_X86_REG_DX, 0);
        break;
    default:
        break;
    }
}
