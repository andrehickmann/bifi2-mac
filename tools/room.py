"""Room backgrounds from BFROOM.DAT.

Each room is an FGROUP with six members; member 5 is an 'RDV1.00' block:

  0x00  b'RDV1.00\\0'
  0x08  palette: start index, count, count * RGB (6 bit per channel)
  ...   image blocks: uint16 w, uint16 h, uint8 encoding, 3 unknown bytes
          encoding 0: w*h raw pixels
          encoding 1: transparent run-length rows, see sprite.py
  end   trailer (36 bytes):
          uint16 width, height, tile_w, tile_h, cols, rows,
          uint16 layer_count, unknown, unknown, block_count,
          uint32 unknown, layers_ptr, blocks_ptr, blocks_ptr (again)

Layer record (20 bytes, at layers_ptr): uint16 kind, uint16 depth,
uint32 map_ptr, uint32 tiles_ptr, uint32 unknown, uint32 unknown.
Only kind 1 layers carry tiles; kind 0 layers have no map (purpose unknown,
possibly walk areas or hotspots). Depth is presumably the draw order.
The tile map holds one byte per grid cell; 0 is an empty cell, n >= 1
selects the block whose offset is the (n-1)-th uint32 at tiles_ptr.
All pointers are relative to the start of the RDV block.

Palette index 0 (magenta) acts as the transparent colour. Indices 1 and 2
(bright green) appear in areas later covered at runtime, likely masks.
"""
import struct
from dataclasses import dataclass

import sprite

TRAILER = struct.Struct('<10H4I')
LAYER = struct.Struct('<2H4I')


@dataclass
class Layer:
    kind: int
    depth: int
    map_ptr: int
    tiles_ptr: int


@dataclass
class RoomImage:
    width: int
    height: int
    tile_w: int
    tile_h: int
    cols: int
    rows: int
    palette: list          # 768 ints, 8 bit per channel
    layers: list


def parse_palette(block):
    start, count = block[0], block[1]
    pal = [0] * 768
    for i in range(count):
        rgb = block[2 + 3 * i:5 + 3 * i]
        pal[(start + i) * 3:(start + i) * 3 + 3] = [c * 255 // 63 for c in rgb]
    return pal


def parse(rdv):
    if rdv[:8] != b'RDV1.00\0':
        raise ValueError('not an RDV1.00 block')
    (w, h, tw, th, cols, rows, nlayers, _u1, _u2, _nblocks,
     _u3, layers_ptr, _blocks, _blocks2) = TRAILER.unpack_from(rdv, len(rdv) - TRAILER.size)
    layers = []
    for i in range(nlayers):
        a, b, map_ptr, tiles_ptr, _p2, _p3 = LAYER.unpack_from(rdv, layers_ptr + i * LAYER.size)
        layers.append(Layer(a, b, map_ptr, tiles_ptr))
    return RoomImage(w, h, tw, th, cols, rows, parse_palette(rdv[8:]), layers)


def tile_layers(room):
    return [layer for layer in room.layers if layer.kind == 1]


def render_layer(rdv, room, layer, canvas):
    """Draw one layer into `canvas` (bytearray, width*height, indexed).
    Pixels with index 0 are transparent, except on the first layer."""
    opaque = layer is tile_layers(room)[0]
    grid = rdv[layer.map_ptr:layer.map_ptr + room.cols * room.rows]
    for cell, tile in enumerate(grid):
        if not tile:
            continue
        blk = struct.unpack_from('<I', rdv, layer.tiles_ptr + 4 * (tile - 1))[0]
        bw, bh, enc = struct.unpack_from('<HHB', rdv, blk)
        x0 = (cell % room.cols) * room.tile_w
        y0 = (cell // room.cols) * room.tile_h
        if enc == 0:
            rows = [(rdv[blk + 8 + y * bw:blk + 8 + (y + 1) * bw], None) for y in range(bh)]
        else:
            px, mask = sprite.decode(rdv, blk + 8, bw, bh)
            rows = [(px[y * bw:(y + 1) * bw], mask[y * bw:(y + 1) * bw]) for y in range(bh)]
        for y, (row, m) in enumerate(rows):
            pos = (y0 + y) * room.width + x0
            for i, v in enumerate(row):
                if (m[i] if m is not None else opaque or v):
                    canvas[pos + i] = v
    return canvas


def render(rdv):
    """Compose all tile layers; returns (room, indexed pixels)."""
    room = parse(rdv)
    canvas = bytearray(room.width * room.height)
    for layer in tile_layers(room):
        render_layer(rdv, room, layer, canvas)
    return room, bytes(canvas)
