"""Image blocks shared by all resource files.

Sprite block: uint16 width, uint16 height, uint8 encoding, 3 unknown bytes,
then the pixel data.
  encoding 0  width*height raw pixels, opaque
  encoding 1  transparent run-length rows (see sprite.py)
  encoding 2  width*height raw pixels, index 0 is transparent

Screen block (intros, panel background): uint16 width, uint16 height,
width*height raw pixels, no encoding byte.

Palette block: uint8 first index, uint8 count, count * RGB (6 bit).
"""
import struct

import sprite


def is_palette(blob):
    return len(blob) >= 2 and blob[1] and len(blob) == 2 + 3 * blob[1]


def apply_palette(blob, pal):
    start, count = blob[0], blob[1]
    for i in range(count):
        pal[(start + i) * 3:(start + i) * 3 + 3] = [c * 255 // 63 for c in blob[2 + 3 * i:5 + 3 * i]]
    return pal


def is_screen(blob):
    if len(blob) < 4:
        return False
    w, h = struct.unpack_from('<HH', blob)
    return w and h and len(blob) == 4 + w * h


def is_sprite(blob):
    if len(blob) < 8:
        return False
    w, h, enc = struct.unpack_from('<HHB', blob)
    if not (0 < w <= 320 and 0 < h <= 200):
        return False
    if enc in (0, 2):
        return len(blob) == 8 + w * h
    return enc == 1


def decode(blob):
    """Return (width, height, pixels, mask) for a screen or sprite block."""
    if is_screen(blob):
        w, h = struct.unpack_from('<HH', blob)
        return w, h, blob[4:4 + w * h], b'\x01' * (w * h)
    w, h, enc = struct.unpack_from('<HHB', blob)
    if enc == 1:
        px, mask = sprite.decode(blob, 8, w, h)
        return w, h, bytes(px), bytes(mask)
    px = blob[8:8 + w * h]
    mask = bytes(1 if (enc == 0 or v) else 0 for v in px)
    return w, h, px, mask
