"""Transparent run-length sprites, as drawn by the blitter in XP01 (seg 1ea0).

A sprite is a sequence of rows. Each row starts with two bytes:
  len   byte length of the row data that follows (used for clipping)
  op    row type, an index into the blitter's jump table

  op 0   end of sprite (no data)
  op 2   empty row
  op 4   full row: width raw pixels
  op 6   skip_after, n, n pixels             -> pixels at the left edge
  op 8   skip_before, n, n pixels            -> pixels after a gap
  op 10  skip, n1, n1 pixels, n2, n2 pixels  -> run, gap, run
  op 12  skip1, skip2, n, n pixels           -> gap, run, gap
  op 14  skip, spans, spans * (skip_after, n, n pixels)

In each (skip, n) pair the skip byte comes first in the data, although for
ops 6, 10, 12 and 14 it is applied after the pixels.
"""


def decode(data, pos, width, height):
    """Return (pixels, mask) as bytearrays of width*height; mask is 1 where
    the sprite draws. `pos` points at the first row header."""
    pixels = bytearray(width * height)
    mask = bytearray(width * height)

    def put(y, x, run):
        base = y * width + x
        pixels[base:base + len(run)] = run
        mask[base:base + len(run)] = b'\x01' * len(run)

    for y in range(height):
        op = data[pos + 1]
        p = pos + 2
        pos = p + data[pos]
        if op == 0:
            break
        if op == 2:
            continue
        if op == 4:
            put(y, 0, data[p:p + width])
        elif op == 6:
            n = data[p + 1]
            put(y, 0, data[p + 2:p + 2 + n])
        elif op == 8:
            skip, n = data[p], data[p + 1]
            put(y, skip, data[p + 2:p + 2 + n])
        elif op == 10:
            skip, n1 = data[p], data[p + 1]
            put(y, 0, data[p + 2:p + 2 + n1])
            q = p + 2 + n1
            n2 = data[q]
            put(y, n1 + skip, data[q + 1:q + 1 + n2])
        elif op == 12:
            n = data[p + 2]                 # data[p + 1] is the trailing gap
            put(y, data[p], data[p + 3:p + 3 + n])
        elif op == 14:
            x, spans = data[p], data[p + 1]
            q = p + 2
            for _ in range(spans):
                skip, n = data[q], data[q + 1]
                put(y, x, data[q + 2:q + 2 + n])
                x += n + skip
                q += 2 + n
        else:
            raise ValueError(f'unknown row op {op} at {p - 2:#x}')
    return pixels, mask
