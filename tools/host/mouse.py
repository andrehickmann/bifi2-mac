"""INT 33h mouse driver. Position is kept in virtual 640x200 coordinates."""
from unicorn.x86_const import *


# Per-program location of the game's own cursor state. The mouse library
# keeps it in the program's data segment; XP00 uses the same layout as XP01
# shifted by 0x216 bytes. Offsets are for XP01.
CURSOR_LAYOUT = {
    'XP00.EXE': (0x0DD7, -0x216),
    'XP01.EXE': (0x250A, 0),
}
CUR_X, CUR_Y, HOT_X_OUT, HOT_Y_OUT = 0x486, 0x488, 0x48A, 0x48C
HOT_X, HOT_Y = 0x3A2, 0x3A4
MIN_X, MIN_Y, MAX_X, MAX_Y = 0x3AA, 0x3AC, 0x3AE, 0x3B0
MOVED = 0x396
ACC_X, ACC_Y = 0x482, 0x484


class Mouse:
    def __init__(self, m):
        self.m = m
        self.target = None            # absolute position in 320x200 pixels
        self.x, self.y = 320, 100
        self.buttons = 0
        self.mickey_x = self.mickey_y = 0
        self.press_count = [0, 0, 0]
        self.release_count = [0, 0, 0]
        self.min_x, self.max_x, self.min_y, self.max_y = 0, 639, 0, 199

    def move(self, dx, dy):
        """Queue relative motion in mickeys; the position advances as the
        game reads the counters (default ratios: 8 mickeys per 8 virtual
        pixels horizontally, 16 per 8 vertically)."""
        self.mickey_x += dx
        self.mickey_y += dy

    def set_buttons(self, b):
        for i in range(3):
            bit = 1 << i
            if b & bit and not self.buttons & bit:
                self.press_count[i] += 1
            if not b & bit and self.buttons & bit:
                self.release_count[i] += 1
        self.buttons = b

    def int33(self, ax):
        m = self.m
        bx = m.reg(UC_X86_REG_BX)
        if ax == 0x00:
            m.setreg(UC_X86_REG_AX, 0xFFFF)
            m.setreg(UC_X86_REG_BX, 2)
        elif ax in (0x01, 0x02, 0x0A, 0x0F, 0x1A, 0x1D):
            pass
        elif ax == 0x03:
            m.setreg(UC_X86_REG_BX, self.buttons)
            m.setreg(UC_X86_REG_CX, self.x)
            m.setreg(UC_X86_REG_DX, self.y)
        elif ax == 0x04:
            self.x, self.y = m.reg(UC_X86_REG_CX), m.reg(UC_X86_REG_DX)
        elif ax in (0x05, 0x06):
            i = min(bx, 2)
            counts = self.press_count if ax == 5 else self.release_count
            if counts[i] or self.buttons:
                m.log(f'mouse fn{ax} bx={bx} count={counts[i]} buttons={self.buttons}')
            m.setreg(UC_X86_REG_AX, self.buttons)
            m.setreg(UC_X86_REG_BX, counts[i])
            counts[i] = 0
            m.setreg(UC_X86_REG_CX, self.x)
            m.setreg(UC_X86_REG_DX, self.y)
        elif ax == 0x07:
            self.min_x, self.max_x = m.reg(UC_X86_REG_CX), m.reg(UC_X86_REG_DX)
        elif ax == 0x08:
            self.min_y, self.max_y = m.reg(UC_X86_REG_CX), m.reg(UC_X86_REG_DX)
        elif ax == 0x0B and self.target is not None and self._write_cursor():
            m.setreg(UC_X86_REG_CX, 0)
            m.setreg(UC_X86_REG_DX, 0)
        elif ax == 0x0B:
            # hand out large moves in small steps, as a real mouse would
            dx = max(-40, min(40, self.mickey_x))
            dy = max(-40, min(40, self.mickey_y))
            self.mickey_x -= dx
            self.mickey_y -= dy
            self.fx = getattr(self, 'fx', float(self.x)) + dx
            self.fy = getattr(self, 'fy', float(self.y)) + dy / 2
            self.fx = max(self.min_x, min(self.max_x, self.fx))
            self.fy = max(self.min_y, min(self.max_y, self.fy))
            self.x, self.y = int(self.fx), int(self.fy)
            m.setreg(UC_X86_REG_CX, dx & 0xFFFF)
            m.setreg(UC_X86_REG_DX, dy & 0xFFFF)
        else:
            m.log(f'unhandled INT 33 AX={ax:04x}')

    def _write_cursor(self):
        """Place the game's cursor at self.target. Returns False if the
        running program is unknown."""
        m = self.m
        layout = CURSOR_LAYOUT.get(getattr(m, 'program', ''))
        if not layout:
            return False
        seg, shift = layout
        base = (m.load_seg + seg) * 16 + shift
        rd = lambda o: m.rd16(base + o)
        wr = lambda o, v: m.wr16(base + o, v)
        x = max(rd(MIN_X), min(rd(MAX_X), self.target[0]))
        y = max(rd(MIN_Y), min(rd(MAX_Y), self.target[1]))
        flags = 0
        if rd(CUR_X) != x:
            wr(CUR_X, x)
            flags |= 1
        if rd(CUR_Y) != y:
            wr(CUR_Y, y)
            flags |= 2
        wr(HOT_X_OUT, x + rd(HOT_X))
        wr(HOT_Y_OUT, y + rd(HOT_Y))
        wr(ACC_X, 0)
        wr(ACC_Y, 0)
        if flags:
            wr(MOVED, rd(MOVED) | flags)
        return True
