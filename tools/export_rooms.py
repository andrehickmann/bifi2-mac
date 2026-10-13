"""Export all room backgrounds from BFROOM.DAT as PNG files."""
import sys

from PIL import Image

import rib
import room


def main(src, dst):
    data = open(src, 'rb').read()
    for index, member in enumerate(rib.fgroup_members(data)):
        if not rib.is_rib(member):
            continue
        rdv = rib.fgroup_members(rib.unpack(member))[5]
        if rdv[:8] != b'RDV1.00\0':
            continue
        info, pixels = room.render(rdv)
        img = Image.frombytes('P', (info.width, info.height), pixels)
        img.putpalette(info.palette)
        img.convert('RGB').save(f'{dst}/room{index:02d}.png')


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
