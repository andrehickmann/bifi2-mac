# Bi-Fi 2 – Action in Hollywood for macOS

A native macOS host for the 1994 DOS adventure. The original game code runs
unchanged under [Unicorn](https://www.unicorn-engine.org/) (dynamic binary
translation x86 → ARM64). Everything around it is implemented natively:

| Layer | File | Notes |
|---|---|---|
| CPU glue, IVT, program loading | `src/machine.c` | BIOS entries are `HLT; IRET` stubs, slicing via a block hook |
| DOS (INT 21h) | `src/dos.c` | files on a host directory acting as drive C:, memory arena, find first/next |
| BIOS (INT 10h/16h/1Ah/2Fh/33h) | `src/bios.c` | VGA detection tables, absolute mouse via the game's cursor state |
| VGA mode 13h | `src/vga.c` | DAC, retrace status, frame conversion |
| Timer / PIC | `src/pit.c` | live channel 0 (IRQ 0) and channel 2 (used for delay calibration) |
| Keyboard | `src/input.c` | port 60h, IRQ 1, BIOS key buffer in the BDA |
| Sound Blaster 2.0 + AdLib | `src/sb.c`, `src/opl.cpp` | 8-bit DMA playback; OPL2 via AdPlug's Nuked OPL3 |
| Snapshots | `src/snapshot.c` | whole-machine save/restore (used for tests) |
| Frontend | `src/main.c` | SDL2 window, input, audio, program sequence XP00 → XP01 |

## Build

Requirements (Homebrew): `unicorn`, `sdl2` (sdl2-compat), `sdl3`, `adplug`.

```sh
make                                  # build/bifi2
./build/bifi2 <dir containing BIFI/>  # run from an installed game directory
./bundle.sh <dir containing BIFI/>    # build/Bi-Fi 2.app, self-contained
```

The game directory is the result of the original installer (`INSTALL.EXE`),
i.e. `BIFI/` with `XP00.EXE`, `XP01.EXE` and `RESOURCE/`. The bundle ships a
`PSMCFG4.DAT` for Sound Blaster 2.0 at 220h/DMA 1 (`assets/`), created with
the original configuration tool running in this host.

Inside the app the pristine files live in `Contents/Resources/game` and are
copied to `~/Library/Application Support/Bi-Fi 2/C` on first start; savegames
(`SAVEGAME.SAV`) are written there.

## Controls

- Mouse: as in the original (left click, verb panel, inventory)
- Esc: game menu (save, load, quit)
- Cmd+F: toggle full screen, Cmd+Q: quit

## Test hooks (environment variables)

| Variable | Effect |
|---|---|
| `BIFI_PROGRAM=XP01.EXE` | run a single program |
| `BIFI_SECONDS=n` | stop each program after n seconds |
| `BIFI_SHOTS=dir` | save a BMP every two seconds |
| `BIFI_EVENTS=prog@t:x,y[:click\|rclick];…` | scripted mouse input |
| `BIFI_KEYS=chars` | scripted keys (`!` Esc, `~` Enter) |
| `BIFI_WAV=file` | raw 44.1 kHz stereo PCM dump of the audio output |
| `BIFI_SAVESTATE=file@t`, `BIFI_LOADSTATE=file` | machine snapshots |
| `BIFI_SPEED=bytes/s` | emulated CPU speed in executed code bytes (default 40e6) |
| `BIFI_TRACE=1`, `BIFI_SBTRACE=1` | log services, ports and DSP commands |

## Emulated time

All devices run on an emulated clock (`Host.vtime`) that advances with
executed code (bytes / speed) and by 1 µs per port access, like an ISA bus
cycle. The frontend keeps it in step with the host clock. Using the host
clock directly breaks the sound driver: it calibrates its AdLib delay loops
by timing 256 port reads with PIT channel 2, and with free port accesses it
ended up spinning thousands of reads per register write with interrupts
off, starving the timer and the Sound Blaster (tinny, broken music).

## Pitfalls (Unicorn on Apple Silicon)

- Never call `uc_emu_stop` from another thread: JIT write protection is per
  thread and the engine crashes with SIGBUS.
- Code hooks are skipped while a stop request is pending, but the instruction
  still executes; do not implement services in code hooks.
- Changing CS:IP inside an interrupt hook does not survive a pending stop.
- A `count` argument to `uc_emu_start` instruments every instruction.
