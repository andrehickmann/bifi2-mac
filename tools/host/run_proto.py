"""Run one game executable in the prototype host and save screenshots."""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from machine import Machine          # noqa: E402
from PIL import Image                # noqa: E402

root, exe, seconds, out = sys.argv[1], sys.argv[2], float(sys.argv[3]), sys.argv[4]
m = Machine(root)
m.trace_ints = '--trace' in sys.argv
m.load(os.path.join(root, 'BIFI', exe))
shots = [0]
last = [time.time()]


keys = []
for a in sys.argv:
    if a.startswith('--keys='):
        keys = list(a[7:])
t_start = time.time()
SCAN = {'8': 0x09, 'x': 0x2D, 'j': 0x24, ' ': 0x39, 'n': 0x31, '\r': 0x1C}


clicks = []
abs_clicks = [[float(v) for v in a[5:].split(',')] + [0] for a in sys.argv if a.startswith('--at=')]
for a in abs_clicks:
    a[1], a[2] = int(a[1]), int(a[2])
relative_clicks = '--relative' in sys.argv
for a in sys.argv:
    if a.startswith('--click='):
        v = a[8:].split(',')
        c = [float(v[0]), int(v[1]), int(v[2]), 0]
        if len(v) == 5:
            c += [int(v[3]), int(v[4])]
        clicks.append(c)


def on_frame(mach):
    el = time.time() - t_start
    for a in abs_clicks:
        if a[3] == 0 and el > a[0] - 1:
            mach.mouse.target = (a[1], a[2])
            a[3] = 1
        elif a[3] == 1 and el > a[0]:
            img = Image.frombytes('P', (320, 200), mach.vga.frame())
            img.putpalette(mach.vga.palette_rgb())
            img.convert('RGB').save(f'{out}_click{a[1]}_{a[2]}.png')
            mach.mouse.set_buttons(1)
            a[3] = 2
        elif a[3] == 2 and el > a[0] + 0.3:
            mach.mouse.set_buttons(0)
            a[3] = 3
    for c in clicks:
        if c[3] == 0 and el > c[0] - 6:
            if not relative_clicks:
                mach.mouse.move(-800, -800)
            c[3] = 'homed'
        elif c[3] == 'homed' and el > c[0] - 3:
            mach.mouse.move(c[1], c[2])
            c[3] = 'moved'
        elif c[3] == 'moved' and el > c[0]:
            img = Image.frombytes('P', (320, 200), mach.vga.frame())
            img.putpalette(mach.vga.palette_rgb())
            img.convert('RGB').save(f'{out}_click.png')
            if len(c) > 4:
                mach.mouse.fx, mach.mouse.fy = c[4] * 2, c[5]
                mach.mouse.x, mach.mouse.y = c[4] * 2, c[5]
            mach.mouse.set_buttons(1)
            c[3] = 1
        elif c[3] == 1 and el > c[0] + 0.3:
            mach.mouse.set_buttons(0)
            c[3] = 2
    if keys and time.time() - t_start > key_start + 3 * (len_keys - len(keys)):
        k = keys.pop(0)
        mach.kbd.press(SCAN.get(k, 0x39), ord(k))
        mach.kbd.press(SCAN.get(k, 0x39), ord(k), release=True)
    if time.time() - last[0] > 2:
        last[0] = time.time()
        save(mach)


def save(mach):
    img = Image.frombytes('P', (320, 200), mach.vga.frame())
    img.putpalette(mach.vga.palette_rgb())
    img.convert('RGB').save(f'{out}_{shots[0]:02d}.png')
    shots[0] += 1


len_keys = len(keys)
key_start = float(next((a[12:] for a in sys.argv if a.startswith('--key-start=')), 3))
code = m.run(max_seconds=seconds, on_frame=on_frame)
save(m)
print('exit code', code, 'cs:ip %04x:%04x' % (m.reg(__import__('unicorn').x86_const.UC_X86_REG_CS),
                                             m.reg(__import__('unicorn').x86_const.UC_X86_REG_IP)))
