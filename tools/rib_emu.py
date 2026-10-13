"""Reference decompressor for RIB-packed resources.

Runs the game's own decompression routine (XP01.EXE, segment 1e84) in a
16-bit x86 emulator. Slow, but authoritative: used to verify the pure Python
implementation.

RIB layout: b'RIB\\0', uint32 unpacked size, packed bytes. The routine is
called with the packed bytes at the start of the destination buffer and
expands them in place, working backwards.
"""
import struct

from unicorn import Uc, UC_ARCH_X86, UC_MODE_16
from unicorn.x86_const import (
    UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS,
    UC_X86_REG_SP, UC_X86_REG_IP,
)

EXE = 'work/XP01.unpacked.exe'
DECOMP_SEG = 0x1E84          # segment of the routine inside the image
CODE_SEG = 0x1000            # where the routine's segment is mapped
STACK_SEG = 0x2000
RET_SEG = 0x2F00             # far return lands on a HLT here
DATA_SEG = 0x3000


def _load_routine():
    d = open(EXE, 'rb').read()
    hdr = struct.unpack_from('<H', d, 8)[0] * 16
    return d[hdr + DECOMP_SEG * 16: hdr + DECOMP_SEG * 16 + 0x400]


_ROUTINE = None


def unpack(blob):
    global _ROUTINE
    if _ROUTINE is None:
        _ROUTINE = _load_routine()
    assert blob[:4] == b'RIB\0', 'not a RIB block'
    size = struct.unpack_from('<I', blob, 4)[0]
    packed = blob[8:]
    buf_len = max(size, len(packed)) + 0x40

    mu = Uc(UC_ARCH_X86, UC_MODE_16)
    assert DATA_SEG * 16 + buf_len <= 0x110000, "block too large for real mode"
    mu.mem_map(0, 0x110000)
    mu.mem_write(CODE_SEG * 16, _ROUTINE)
    mu.mem_write(RET_SEG * 16, b'\xF4')
    mu.mem_write(DATA_SEG * 16, packed)

    # Far call: decompress(void far *buf, long packed_len, long unpacked_len)
    args = struct.pack('<HHHHIi', 0, RET_SEG, 0, DATA_SEG, len(packed), size)
    sp = 0xFF00 - len(args)
    mu.mem_write(STACK_SEG * 16 + sp, args)
    mu.reg_write(UC_X86_REG_SS, STACK_SEG)
    mu.reg_write(UC_X86_REG_SP, sp)
    mu.reg_write(UC_X86_REG_DS, 0)
    mu.reg_write(UC_X86_REG_ES, 0)
    mu.reg_write(UC_X86_REG_CS, CODE_SEG)
    mu.reg_write(UC_X86_REG_IP, 0)
    mu.emu_start(CODE_SEG * 16, RET_SEG * 16, count=200_000_000)
    return bytes(mu.mem_read(DATA_SEG * 16, size))
