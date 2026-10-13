"""Export sprites of PANELS, CMBMOU and PLAYER as contact sheets."""
import sys

from PIL import Image

import image
import rib
import room

FILES = ['PANELS', 'CMBMOU', 'PLAYER']


def base_palette(resdir, room_index=10):
    bf = open(f'{resdir}/BFROOM.DAT', 'rb').read()
    rdv = rib.fgroup_members(rib.unpack(rib.fgroup_members(bf)[room_index]))[5]
    pal = room.parse(rdv).palette[:]
    for name in FILES:
        for m in load(resdir, name):
            if image.is_palette(m):
                image.apply_palette(m, pal)
    return pal


def load(resdir, name):
    return rib.fgroup_members(rib.unpack(open(f'{resdir}/{name}.DAT', 'rb').read()))


def to_rgba(blob, pal):
    w, h, px, mask = image.decode(blob)
    im = Image.frombytes('P', (w, h), px)
    im.putpalette(pal)
    rgba = im.convert('RGBA')
    rgba.putalpha(Image.frombytes('L', (w, h), bytes(255 * v for v in mask)))
    return rgba


def sheet(imgs, max_w=960, pad=4):
    cw = max(i.width for i in imgs) + pad
    ch = max(i.height for i in imgs) + pad
    cols = max(1, min(len(imgs), max_w // cw))
    rows = (len(imgs) + cols - 1) // cols
    out = Image.new('RGBA', (cols * cw, rows * ch), (40, 40, 48, 255))
    for k, im in enumerate(imgs):
        out.alpha_composite(im, ((k % cols) * cw, (k // cols) * ch))
    return out.resize((out.width * 2, out.height * 2), Image.NEAREST)


def main(resdir, dst):
    pal = base_palette(resdir)
    for name in FILES:
        imgs = [to_rgba(m, pal) for m in load(resdir, name)
                if image.is_screen(m) or image.is_sprite(m)]
        sheet(imgs).save(f'{dst}/{name}.png')


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
