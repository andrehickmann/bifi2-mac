"""Proportional font (CMBMOU.DAT member 15).

Header (10 bytes): uint16 magic 0x2e2c, first char, last char, unknown (7),
line height. Then one record per char: uint16 unknown (1), width, height,
offset. Offsets are relative to the end of the header and point at sprite
blocks (see image.py). Glyph pixels are small colour indices (1..4) that the
game maps to the text colour at draw time. Characters use code page 437.
"""
import struct

import image


class Font:
    def __init__(self, blob):
        magic, self.first, self.last, _u, self.line_height = struct.unpack_from('<5H', blob)
        assert magic == 0x2E2C, 'not a font block'
        self.glyphs = {}
        for i in range(self.last - self.first + 1):
            _k, w, h, off = struct.unpack_from('<4H', blob, 10 + 8 * i)
            self.glyphs[self.first + i] = image.decode(blob[10 + off:])

    def width(self, text):
        return sum(self.glyphs[c][0] for c in text.encode('cp437') if c in self.glyphs)

    def draw(self, canvas, cw, x, y, text, colours):
        """Draw text into an indexed canvas of width cw. `colours` maps glyph
        indices to palette indices."""
        for c in text.encode('cp437'):
            if c not in self.glyphs:
                continue
            w, h, px, mask = self.glyphs[c]
            for gy in range(h):
                for gx in range(w):
                    if mask[gy * w + gx] and px[gy * w + gx]:
                        canvas[(y + gy) * cw + x + gx] = colours.get(px[gy * w + gx], px[gy * w + gx])
            x += w
        return x
