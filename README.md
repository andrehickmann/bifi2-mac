# bifi2-mac

A native macOS host for the 1994 German DOS adventure *Bi-Fi 2 – Action in Hollywood*, plus the
reverse-engineering tools used to understand its file formats.

The original game code runs unchanged under [Unicorn](https://www.unicorn-engine.org/) (x86 → ARM64);
DOS, BIOS, VGA, timer, keyboard, mouse, Sound Blaster 2.0 and AdLib are implemented natively. No DOSBox.

**No game data is included.** You need your own copy of the original game, installed once with its
`INSTALL.EXE` (e.g. in DOSBox). This project is not affiliated with or endorsed by the owners of the
game or the Bi-Fi brand.

## Contents

| Path | What |
|---|---|
| [`port/`](port/) | The macOS host (C, SDL2, Unicorn, AdPlug): build, app bundle, test hooks, pitfalls |
| [`tools/`](tools/) | Python tools: EXE unpacker (PARSEC + LZEXE), RIB/FGROUP archives, rooms, sprites, font, text export |
| [`tools/host/`](tools/host/) | The first Python prototype of the host |

## Quick start

```sh
brew install unicorn sdl2-compat sdl3 adplug
cd port && make
./build/bifi2 <dir containing BIFI/>    # or ./bundle.sh <dir containing BIFI/> for an .app
```

Tools:

```sh
python3 -m venv tools/.venv && tools/.venv/bin/pip install -r tools/requirements.txt
tools/.venv/bin/python tools/unpack_exe.py <game>/BIFI/XP01.EXE XP01.unpacked.exe
```

## License

GPL-2.0-or-later, see [LICENSE](LICENSE).
