"""VGA: mode 13h, DAC, retrace status and register files. Unchained
(Mode X) access is detected and reported; planar memory is emulated only if
the game turns chain-4 off."""
import time


class VGA:
    def __init__(self, m):
        self.m = m
        self.mode = 3
        self.dac = bytearray(768)
        self.dac_write = 0
        self.dac_read = 0
        self.seq_index = 0
        self.seq = bytearray(8)
        self.seq[4] = 0x0E
        self.gc_index = 0
        self.gc = bytearray(9)
        self.crtc_index = 0
        self.crtc = bytearray(0x19)
        self.attr_flip = False
        self.t0 = time.perf_counter()
        self.unchained_warned = False
        self.frames = 0

    def set_mode(self, mode):
        self.mode = mode
        self.m.wr(0x449, bytes([mode]))
        if mode == 0x13:
            self.m.wr(0xA0000, bytes(64000))
        self.m.log(f'video mode {mode:02x}')

    def bios_palette(self, al):
        from unicorn.x86_const import UC_X86_REG_BX, UC_X86_REG_CX, UC_X86_REG_DX, UC_X86_REG_ES
        m = self.m
        if al == 0x12:                               # set block of DAC registers
            start, count = m.reg(UC_X86_REG_BX), m.reg(UC_X86_REG_CX)
            data = m.rd(m.lin(m.reg(UC_X86_REG_ES), m.reg(UC_X86_REG_DX)), count * 3)
            self.dac[start * 3:start * 3 + count * 3] = data

    @property
    def chained(self):
        return bool(self.seq[4] & 0x08)

    def io_in(self, port):
        if port == 0x3DA:
            self.attr_flip = False
            # 70 Hz frame, retrace during the last ~5 %
            phase = ((time.perf_counter() - self.t0) * 70) % 1
            return (0x08 | 0x01) if phase > 0.95 else (0x01 if phase % 0.01 > 0.008 else 0)
        if port == 0x3C9:
            v = self.dac[self.dac_read]
            self.dac_read = (self.dac_read + 1) % 768
            return v
        if port == 0x3C5:
            return self.seq[self.seq_index & 7]
        if port == 0x3CF:
            return self.gc[self.gc_index % 9]
        if port == 0x3D5:
            return self.crtc[self.crtc_index % 0x19]
        if port == 0x3CC:
            return 0x63
        if port in (0x3C4, 0x3CE, 0x3D4, 0x3C8, 0x3C7):
            return 0
        return None

    def io_out(self, port, v):
        if port == 0x3C8:
            self.dac_write = v * 3
        elif port == 0x3C7:
            self.dac_read = v * 3
        elif port == 0x3C9:
            self.dac[self.dac_write] = v & 0x3F
            self.dac_write = (self.dac_write + 1) % 768
        elif port == 0x3C4:
            self.seq_index = v
        elif port == 0x3C5:
            self.seq[self.seq_index & 7] = v
            if (self.seq_index & 7) == 4 and not v & 0x08 and not self.unchained_warned:
                self.unchained_warned = True
                self.m.log('VGA: chain-4 disabled (Mode X)')
        elif port == 0x3CE:
            self.gc_index = v
        elif port == 0x3CF:
            self.gc[self.gc_index % 9] = v
        elif port == 0x3D4:
            self.crtc_index = v
        elif port == 0x3D5:
            self.crtc[self.crtc_index % 0x19] = v
        elif port == 0x3C0:
            self.attr_flip = not self.attr_flip
        elif port in (0x3C2, 0x3C6):
            pass
        else:
            return False
        return True

    def palette_rgb(self):
        return bytes(min(255, c * 255 // 63) for c in self.dac)

    def frame(self):
        start = (self.crtc[0x0C] << 8 | self.crtc[0x0D])
        if self.chained:
            return self.m.rd(0xA0000 + start * 4 % 0x10000 if False else 0xA0000, 64000)
        return self.m.rd(0xA0000, 64000)
