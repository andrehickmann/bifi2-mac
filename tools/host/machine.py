"""Prototype host: runs the game's real-mode code under Unicorn with
high-level emulation of DOS, BIOS and the few hardware ports the game uses.

Interrupt model: every IVT entry initially points at a stub in the BIOS
segment (F000:n*2, each an IRET). When execution reaches a stub, the host
performs the service for vector n, then the IRET returns to the caller.
Software INT instructions and injected hardware IRQs both go through the
IVT, so handlers the game installs itself (timer, keyboard) chain naturally.
"""
import os
import struct
import time

from unicorn import (Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_CODE, UC_HOOK_INTR,
                     UC_HOOK_INSN, UC_HOOK_MEM_UNMAPPED, UcError)
from unicorn.x86_const import *

MEM_SIZE = 0x110000
BIOS_SEG = 0xF000
BIOS = BIOS_SEG * 16
FLAG_CF, FLAG_ZF, FLAG_IF = 0x0001, 0x0040, 0x0200


class ExitProgram(Exception):
    def __init__(self, code):
        self.code = code


class Machine:
    def __init__(self, root, log=print):
        self.root = root
        self.log = log
        self.uc = Uc(UC_ARCH_X86, UC_MODE_16)
        self.uc.mem_map(0, MEM_SIZE)
        self.files = {}
        self.next_handle = 5
        self.dta = 0
        self.mem_blocks = {}          # segment -> paragraphs
        self.mem_top = 0x9FFF
        self.pending_irqs = []
        self.exit_code = None
        self.trace_ints = False
        self.cwd = 'C:\\BIFI'
        # devices
        from vga import VGA
        from pit import PIT
        from mouse import Mouse
        from keyboard import Keyboard
        self.vga = VGA(self)
        self.pit = PIT(self)
        self.mouse = Mouse(self)
        self.kbd = Keyboard(self)
        self._install_bios()
        self.uc.hook_add(UC_HOOK_INTR, self._on_intr)
        self.uc.hook_add(UC_HOOK_CODE, self._on_bios, begin=BIOS, end=BIOS + 0x200)
        self.uc.hook_add(UC_HOOK_INSN, self._on_in, None, 1, 0, UC_X86_INS_IN)
        self.uc.hook_add(UC_HOOK_INSN, self._on_out, None, 1, 0, UC_X86_INS_OUT)

    # ---------------------------------------------------------------- memory
    def rd(self, addr, n):
        return bytes(self.uc.mem_read(addr, n))

    def wr(self, addr, data):
        self.uc.mem_write(addr, bytes(data))

    def rd16(self, addr):
        return struct.unpack('<H', self.rd(addr, 2))[0]

    def wr16(self, addr, v):
        self.wr(addr, struct.pack('<H', v & 0xFFFF))

    def reg(self, r):
        return self.uc.reg_read(r)

    def setreg(self, r, v):
        self.uc.reg_write(r, v)

    def lin(self, seg, off):
        return (seg << 4) + off

    def cstring(self, addr):
        out = bytearray()
        while True:
            b = self.rd(addr, 1)[0]
            if not b:
                return out.decode('cp437')
            out.append(b)
            addr += 1

    # ---------------------------------------------------------------- flags
    def _flags_addr(self):
        # FLAGS pushed by INT sit at SS:SP+4 when inside a stub
        return self.lin(self.reg(UC_X86_REG_SS), self.reg(UC_X86_REG_SP) + 4)

    def set_cf(self, on):
        a = self._flags_addr()
        f = self.rd16(a)
        self.wr16(a, (f | FLAG_CF) if on else (f & ~FLAG_CF))

    def set_zf(self, on):
        a = self._flags_addr()
        f = self.rd16(a)
        self.wr16(a, (f | FLAG_ZF) if on else (f & ~FLAG_ZF))

    # ---------------------------------------------------------------- setup
    def _install_bios(self):
        stubs = b'\xCF' * 0x200                       # IRET everywhere
        self.wr(BIOS, stubs)
        for n in range(256):
            self.wr(n * 4, struct.pack('<HH', n * 2 if n < 0x100 else 0, BIOS_SEG))
        # BIOS data area
        self.wr(0x449, b'\x03')                       # video mode
        self.wr16(0x44A, 80)
        self.wr16(0x413, 640)                         # KB conventional memory
        self.wr16(0x463, 0x3D4)                       # CRTC base
        self.wr(0x484, b'\x18')                       # rows - 1

    # -------------------------------------------------------- program load
    def load(self, exe_path, args=''):
        data = open(exe_path, 'rb').read()
        (magic, lastp, pages, nrel, hdrpar, minalloc, maxalloc, ss, sp, _c, ip, cs,
         reloff, _o) = struct.unpack_from('<14H', data)
        assert magic == 0x5A4D
        size = (pages - 1) * 512 + (lastp or 512)
        image = data[hdrpar * 16:size]
        env_seg = 0x0800
        env = b'PATH=C:\\\0COMSPEC=C:\\COMMAND.COM\0\0\x01\0' + \
              ('C:\\BIFI\\' + os.path.basename(exe_path).upper()).encode() + b'\0'
        self.wr(env_seg * 16, env)
        psp = 0x0900
        load_seg = psp + 0x10
        self.wr(load_seg * 16, image)
        for i in range(nrel):
            off, seg = struct.unpack_from('<HH', data, reloff + i * 4)
            a = (load_seg + seg) * 16 + off
            self.wr16(a, self.rd16(a) + load_seg)
        p = bytearray(256)
        p[0:2] = b'\xCD\x20'
        p[2:4] = struct.pack('<H', self.mem_top + 1)
        p[0x2C:0x2E] = struct.pack('<H', env_seg)
        tail = (' ' + args if args else '').encode()
        p[0x80] = len(tail)
        p[0x81:0x81 + len(tail) + 1] = tail + b'\r'
        self.wr(psp * 16, p)
        self.psp = psp
        self.load_seg = load_seg
        self.program = os.path.basename(exe_path).upper()
        self.dta = self.lin(psp, 0x80)
        self.mem_blocks = {psp: self.mem_top + 1 - psp}
        for r, v in ((UC_X86_REG_CS, load_seg + cs), (UC_X86_REG_IP, ip),
                     (UC_X86_REG_SS, load_seg + ss), (UC_X86_REG_SP, sp),
                     (UC_X86_REG_DS, psp), (UC_X86_REG_ES, psp),
                     (UC_X86_REG_FLAGS, 0x0202)):
            self.setreg(r, v)

    # ---------------------------------------------------------------- run
    def run(self, max_seconds=None, slice_insns=20000, on_frame=None):
        start = time.time()
        while self.exit_code is None:
            cs, ip = self.reg(UC_X86_REG_CS), self.reg(UC_X86_REG_IP)
            try:
                self.uc.emu_start(self.lin(cs, ip), MEM_SIZE, count=slice_insns)
            except ExitProgram as e:
                self.exit_code = e.code
                break
            except UcError as e:
                cs, ip = self.reg(UC_X86_REG_CS), self.reg(UC_X86_REG_IP)
                raise RuntimeError(f'CPU error {e} at {cs:04x}:{ip:04x}')
            if self.exit_code is not None:
                break
            self.pit.tick()
            self._deliver_irqs()
            if on_frame:
                on_frame(self)
            if max_seconds and time.time() - start > max_seconds:
                break
        return self.exit_code

    def raise_irq(self, n):
        if n not in self.pending_irqs:
            self.pending_irqs.append(n)

    def _deliver_irqs(self):
        if not self.pending_irqs:
            return
        if not self.reg(UC_X86_REG_FLAGS) & FLAG_IF:
            return
        irq = self.pending_irqs.pop(0)
        self._interrupt(8 + irq if irq < 8 else 0x70 + irq - 8)

    def _interrupt(self, vec):
        """Real-mode interrupt entry: push FLAGS, CS, IP, jump via IVT."""
        ss, sp = self.reg(UC_X86_REG_SS), self.reg(UC_X86_REG_SP)
        flags = self.reg(UC_X86_REG_FLAGS)
        cs, ip = self.reg(UC_X86_REG_CS), self.reg(UC_X86_REG_IP)
        sp = (sp - 6) & 0xFFFF
        self.wr(self.lin(ss, sp), struct.pack('<HHH', ip, cs, flags))
        self.setreg(UC_X86_REG_SP, sp)
        self.setreg(UC_X86_REG_FLAGS, flags & ~(FLAG_IF | 0x100))
        off, seg = struct.unpack('<HH', self.rd(vec * 4, 4))
        self.setreg(UC_X86_REG_CS, seg)
        self.setreg(UC_X86_REG_IP, off)

    def _on_intr(self, uc, intno, _):
        # Software INT n: Unicorn has already advanced IP past the instruction.
        self._interrupt(intno)

    # ---------------------------------------------------------------- BIOS
    def _on_bios(self, uc, address, size, _):
        vec = (address - BIOS) // 2
        if (address - BIOS) % 2:
            return
        ah = self.reg(UC_X86_REG_AH)
        if self.trace_ints and vec not in (8, 0x1C):
            self.log(f'INT {vec:02x} AX={self.reg(UC_X86_REG_AX):04x}')
        handler = getattr(self, f'int_{vec:02x}', None)
        if handler:
            handler(ah)
        elif vec == 8:
            self.wr(0x46C, struct.pack('<I', (struct.unpack('<I', self.rd(0x46C, 4))[0] + 1) & 0xFFFFFFFF))
        elif vec in (0x1C, 0x23, 0x24, 0x1B, 0x05, 0x09):
            if vec == 9:
                self.kbd.bios_int9()
        else:
            self.log(f'unhandled INT {vec:02x} AX={self.reg(UC_X86_REG_AX):04x}')

    from dos import int_21, int_20       # noqa: E402
    from bios import int_10, int_16, int_1a, int_2f, int_15, int_13, int_33  # noqa

    # ---------------------------------------------------------------- ports
    def _on_in(self, uc, port, size, _):
        return self.io_in(port, size)

    def _on_out(self, uc, port, size, value, _):
        self.io_out(port, size, value)

    def io_in(self, port, size):
        for dev in (self.vga, self.pit, self.kbd):
            v = dev.io_in(port)
            if v is not None:
                return v
        return 0xFF

    def io_out(self, port, size, value):
        if size == 2:
            self.io_out(port, 1, value & 0xFF)
            self.io_out(port + 1, 1, value >> 8)
            return
        for dev in (self.vga, self.pit, self.kbd):
            if dev.io_out(port, value):
                return
