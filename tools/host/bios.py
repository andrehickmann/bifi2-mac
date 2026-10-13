"""BIOS-level services: video, keyboard, time, mouse driver, misc."""
import struct
import time

from unicorn.x86_const import *


def int_10(m, ah):
    ax = m.reg(UC_X86_REG_AX)
    if ah == 0x00:
        m.vga.set_mode(ax & 0x7F)
    elif ah == 0x0F:
        m.setreg(UC_X86_REG_AX, 40 << 8 | m.vga.mode if m.vga.mode == 0x13 else 80 << 8 | m.vga.mode)
        m.setreg(UC_X86_REG_BX, m.reg(UC_X86_REG_BX) & 0x00FF)
    elif ah == 0x1A:                                # display combination: VGA colour
        m.setreg(UC_X86_REG_AX, 0x001A)
        m.setreg(UC_X86_REG_BX, 0x0008)
    elif ah == 0x1B:                                # functionality info
        es, di = m.reg(UC_X86_REG_ES), m.reg(UC_X86_REG_DI)
        buf = bytearray(64)
        buf[0:4] = struct.pack('<HH', 0x100, BIOS_TABLE_SEG)
        buf[4] = m.vga.mode
        buf[0x27:0x29] = (256).to_bytes(2, 'little')    # colours
        buf[0x2D] = 0x02                                 # misc state: colour, VGA
        m.wr(m.lin(es, di), buf)
        # static functionality table: all modes up to 13h supported
        m.wr(m.lin(BIOS_TABLE_SEG, 0x100), bytes([0xFF, 0xFF, 0x0F, 0, 0, 0, 0, 0x07]))
        m.setreg(UC_X86_REG_AX, (ax & 0xFF00) | 0x1B)
    elif ah == 0x12:
        pass
    elif ax == 0x1130:                              # font information
        m.setreg(UC_X86_REG_CX, 16)
        m.setreg(UC_X86_REG_DX, 24)
        m.setreg(UC_X86_REG_ES, 0xC000)
        m.setreg(UC_X86_REG_BP, 0)
    elif ah == 0x08:                                # read char/attribute
        m.setreg(UC_X86_REG_AX, 0x0720)
    elif ah == 0x10:
        m.vga.bios_palette(ax & 0xFF)
    else:
        m.log(f'unhandled INT 10 AX={ax:04x}')


BIOS_TABLE_SEG = 0xF010


def int_16(m, ah):
    if ah in (0x00, 0x10):
        key = m.kbd.pop_key()
        m.setreg(UC_X86_REG_AX, key or 0)
    elif ah in (0x01, 0x11):
        key = m.kbd.peek_key()
        m.set_zf(key is None)
        if key:
            m.setreg(UC_X86_REG_AX, key)
    elif ah in (0x02, 0x12):
        m.setreg(UC_X86_REG_AX, m.reg(UC_X86_REG_AX) & 0xFF00)


def int_1a(m, ah):
    if ah == 0:
        t = struct.unpack('<I', m.rd(0x46C, 4))[0]
        m.setreg(UC_X86_REG_CX, t >> 16)
        m.setreg(UC_X86_REG_DX, t & 0xFFFF)
        m.setreg(UC_X86_REG_AX, 0)


def int_2f(m, ah):
    ax = m.reg(UC_X86_REG_AX)
    if ax == 0x4300:                                # no XMS driver
        m.setreg(UC_X86_REG_AX, 0)
    else:
        m.setreg(UC_X86_REG_AX, ax & 0xFF00)


def int_15(m, ah):
    m.set_cf(True)
    m.setreg(UC_X86_REG_AX, 0x8600)


def int_13(m, ah):
    m.log(f'INT 13 AX={m.reg(UC_X86_REG_AX):04x} DX={m.reg(UC_X86_REG_DX):04x}')
    m.setreg(UC_X86_REG_AX, 0)
    m.set_cf(False)


def int_33(m, ah):
    m.mouse.int33(m.reg(UC_X86_REG_AX))
