"""Unpack PARSEC-packed DOS executables by emulating the unpacker stub.

The stub is run in a 16-bit x86 emulator until control leaves the stub's
segment. The memory image at that point is the original program. Relocations
are recovered by unpacking twice at different load segments and diffing.
"""
import struct
import sys

from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_INTR, UC_HOOK_CODE
from unicorn.x86_const import (
    UC_X86_REG_AX, UC_X86_REG_BX, UC_X86_REG_CX, UC_X86_REG_DX,
    UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS,
    UC_X86_REG_SP, UC_X86_REG_IP, UC_X86_REG_FLAGS,
)

MEM_SIZE = 0x110000
MEM_TOP_SEG = 0x9FFF


class Unpacked:
    def __init__(self, image, cs, ip, ss, sp, load_seg):
        self.image, self.cs, self.ip, self.ss, self.sp = image, cs, ip, ss, sp
        self.load_seg = load_seg


def parse_header(data):
    (magic, lastp, pages, nrel, hdrpar, minalloc, maxalloc,
     ss, sp, _csum, ip, cs, reloff, _ovl) = struct.unpack('<14H', data[:28])
    assert magic == 0x5A4D, 'not an MZ executable'
    size = (pages - 1) * 512 + (lastp or 512)
    return dict(nrel=nrel, hdr=hdrpar * 16, size=size, minalloc=minalloc,
                ss=ss, sp=sp, ip=ip, cs=cs, reloff=reloff)


def run_stub(data, load_seg, max_steps=50_000_000):
    h = parse_header(data)
    image = data[h['hdr']:h['size']]
    psp_seg = load_seg - 0x10
    mu = Uc(UC_ARCH_X86, UC_MODE_16)
    mu.mem_map(0, MEM_SIZE)
    mu.mem_write(load_seg * 16, image)
    for i in range(h['nrel']):
        off, seg = struct.unpack_from('<HH', data, h['reloff'] + i * 4)
        addr = (load_seg + seg) * 16 + off
        val = struct.unpack('<H', mu.mem_read(addr, 2))[0]
        mu.mem_write(addr, struct.pack('<H', (val + load_seg) & 0xFFFF))

    # Minimal PSP: INT 20h at 0, top of memory at 2, empty command tail.
    psp = bytearray(256)
    psp[0:2] = b'\xCD\x20'
    psp[2:4] = struct.pack('<H', MEM_TOP_SEG)
    psp[0x80:0x82] = b'\x00\x0D'
    mu.mem_write(psp_seg * 16, bytes(psp))

    stub_cs = load_seg + h['cs']
    mu.reg_write(UC_X86_REG_DS, psp_seg)
    mu.reg_write(UC_X86_REG_ES, psp_seg)
    mu.reg_write(UC_X86_REG_SS, load_seg + h['ss'])
    mu.reg_write(UC_X86_REG_SP, h['sp'])
    mu.reg_write(UC_X86_REG_CS, stub_cs)
    mu.reg_write(UC_X86_REG_IP, h['ip'])

    result = {}

    seen = [stub_cs]

    def on_code(uc, address, size, _):
        # Stop at the first segment change that lands below every stub
        # segment seen so far: that is the original program entry.
        cs = uc.reg_read(UC_X86_REG_CS)
        if cs != seen[-1]:
            if len(seen) > 2 and cs < min(seen):
                result['cs'], result['ip'] = cs, uc.reg_read(UC_X86_REG_IP)
                uc.emu_stop()
                return
            seen.append(cs)

    def set_cf(uc, on):
        fl = uc.reg_read(UC_X86_REG_FLAGS)
        uc.reg_write(UC_X86_REG_FLAGS, fl | 1 if on else fl & ~1)

    def on_int(uc, intno, _):
        ax = uc.reg_read(UC_X86_REG_AX)
        ah = ax >> 8
        if intno == 0x21 and ah == 0x30:          # DOS version
            uc.reg_write(UC_X86_REG_AX, 0x0005)
        elif intno == 0x21 and ah == 0x4A:        # resize memory block
            set_cf(uc, False)
        elif intno == 0x21 and ah == 0x62:        # get PSP
            uc.reg_write(UC_X86_REG_BX, psp_seg)
        elif intno == 0x21 and ah in (0x25, 0x1A):
            pass
        elif intno == 0x21 and ah == 0x35:
            uc.reg_write(UC_X86_REG_ES, 0)
            uc.reg_write(UC_X86_REG_BX, 0)
        elif intno == 0x21 and ah == 0x4C:
            result['exit'] = ax & 0xFF
            uc.emu_stop()
        else:
            raise RuntimeError(f'unhandled INT {intno:02x} AX={ax:04x}')

    mu.hook_add(UC_HOOK_CODE, on_code)
    mu.hook_add(UC_HOOK_INTR, on_int)
    mu.emu_start(stub_cs * 16 + h['ip'], MEM_SIZE, count=max_steps)
    if 'exit' in result:
        raise RuntimeError(f'stub exited with code {result["exit"]} (checksum failed?)')
    if 'cs' not in result:
        raise RuntimeError('stub did not transfer control')

    # The decompressed program may extend past the packed image; dump up to
    # the end of the original stack segment set up for the entry point.
    end = (mu.reg_read(UC_X86_REG_SS) - load_seg) * 16 + mu.reg_read(UC_X86_REG_SP)
    img = bytes(mu.mem_read(load_seg * 16, end))
    return Unpacked(img, result['cs'] - load_seg, result['ip'],
                    mu.reg_read(UC_X86_REG_SS) - load_seg,
                    mu.reg_read(UC_X86_REG_SP), load_seg)


def find_relocs(a, b):
    """Offsets where two images differ by exactly the load segment delta."""
    delta = b.load_seg - a.load_seg
    relocs = []
    i = 0
    n = min(len(a.image), len(b.image)) - 1
    while i < n:
        if a.image[i] != b.image[i] or a.image[i + 1] != b.image[i + 1]:
            va = struct.unpack_from('<H', a.image, i)[0]
            vb = struct.unpack_from('<H', b.image, i)[0]
            if (vb - va) & 0xFFFF == delta:
                relocs.append(i)
                i += 2
                continue
            raise RuntimeError(f'unexplained difference at {i:#x}')
        i += 1
    return relocs


def build_exe(u, relocs, minalloc):
    image = bytearray(u.image)
    for off in relocs:                       # normalize to segment base 0
        val = struct.unpack_from('<H', image, off)[0]
        struct.pack_into('<H', image, off, (val - u.load_seg) & 0xFFFF)
    reltab = b''.join(struct.pack('<HH', off & 0xF, off >> 4) for off in relocs)
    hdr_len = (28 + len(reltab) + 15) // 16 * 16
    total = hdr_len + len(image)
    header = struct.pack('<14H', 0x5A4D, total % 512, (total + 511) // 512,
                         len(relocs), hdr_len // 16, minalloc, 0xFFFF,
                         u.ss & 0xFFFF, u.sp, 0, u.ip, u.cs & 0xFFFF, 28, 0)
    return (header + reltab).ljust(hdr_len, b'\0') + bytes(image)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    data = open(src, 'rb').read()
    a = run_stub(data, 0x1000)
    b = run_stub(data, 0x2000)
    assert (a.cs, a.ip) == (b.cs, b.ip)
    relocs = find_relocs(a, b)
    h = parse_header(data)
    open(dst, 'wb').write(build_exe(a, relocs, h['minalloc']))
    print(f'{src}: entry {a.cs:04x}:{a.ip:04x}, {len(a.image)} bytes, '
          f'{len(relocs)} relocations -> {dst}')


if __name__ == '__main__':
    main()
