"""RIB decompression and FGROUP container parsing for Bi-Fi 2 resources.

RIB block: b'RIB\\0', uint32 LE unpacked size, packed stream.

The packed stream is decoded backwards: reading starts at its last byte,
writing starts at the last byte of the output. Decoding stops once the read
position is no longer below the write position; the remaining prefix is
already in place (the original decodes in a single buffer).

Opcode = next byte read. Multi-byte operands follow in reading order.

  0x00-0x0F  fill      count = (op & 0xF) + 4,               value = next
  0x10-0x1F  fill      count = ((op & 0xF) << 8 | next) + 20, value = next
  0x20-0x2F  literal   count = (op & 0xF) + 1
  0x30-0x3F  literal   count = ((op & 0xF) << 8 | next) + 17
  0x40       literal   count = 16-bit word
  0x41       match     dist = 16-bit word, count = 16-bit word
  0x42       match     dist = 16-bit word, count = next + 17
  0x43-0x4F  match     count = (op & 0xF) + 1, dist = next << 8 | next
  0x50-0x5F  match     dist = ((op & 0xF) << 8 | next) + 2,  count = next + 14
  0x60-0xFF  match     dist = ((op & 0xF) << 8 | next) + 2,  count = (op >> 4) - 2

A 16-bit word is stored so that its high byte is read first. A match copies
from `dist` bytes above the current write position, byte by byte.
"""
import struct

MAGIC = b'RIB\0'


def is_rib(blob):
    return blob[:4] == MAGIC


def unpack(blob):
    if not is_rib(blob):
        raise ValueError('not a RIB block')
    size = struct.unpack_from('<I', blob, 4)[0]
    packed = blob[8:]
    buf = bytearray(max(size, len(packed)))
    buf[:len(packed)] = packed
    si = len(packed) - 1
    di = size - 1

    def byte():
        nonlocal si
        v = buf[si]
        si -= 1
        return v

    def word():
        hi = byte()
        return hi << 8 | byte()

    def fill(count, value):
        nonlocal di
        for _ in range(count):
            buf[di] = value
            di -= 1

    def literal(count):
        nonlocal si, di
        for _ in range(count):
            buf[di] = buf[si]
            di -= 1
            si -= 1

    def match(dist, count):
        nonlocal di
        for _ in range(count):
            buf[di] = buf[di + dist]
            di -= 1

    while si < di:
        op = byte()
        hi, lo = op >> 4, op & 0xF
        if hi == 0:
            fill(lo + 4, byte())
        elif hi == 1:
            count = (lo << 8 | byte()) + 20
            fill(count, byte())
        elif hi == 2:
            literal(lo + 1)
        elif hi == 3:
            literal((lo << 8 | byte()) + 17)
        elif op == 0x40:
            literal(word())
        elif op == 0x41:
            dist = word()
            match(dist, word())
        elif op == 0x42:
            dist = word()
            match(dist, byte() + 17)
        elif hi == 4:
            match(word(), lo + 1)
        elif hi == 5:
            dist = (lo << 8 | byte()) + 2
            match(dist, byte() + 14)
        else:
            match((lo << 8 | byte()) + 2, hi - 2)
    return bytes(buf[:size])


def fgroup_members(data):
    """Split an FGROUP into member blobs.

    Header: uint32 offsets terminated by a zero entry, followed by one uint32
    length per member. The first offset equals the header size.
    """
    count = struct.unpack_from('<I', data, 0)[0] // 4
    table = struct.unpack_from(f'<{count}I', data, 0)
    n = table.index(0)
    offsets, lengths = table[:n], table[n + 1:2 * n + 1]
    return [data[o:o + ln] for o, ln in zip(offsets, lengths)]


def is_fgroup(data):
    if len(data) < 8:
        return False
    count = struct.unpack_from('<I', data, 0)[0]
    if count % 4 or count < 12 or count > len(data):
        return False
    table = struct.unpack_from(f'<{count // 4}I', data, 0)
    return 0 in table and count // 4 == 2 * table.index(0) + 1
