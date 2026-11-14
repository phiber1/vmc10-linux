# VMC10 for Linux

**A native Linux emulator for the Tandy TRS-80 MC-10 Micro Color Computer
(1983) and the Matra & Hachette Alice, with its classic expansions: the
16K RAM pack, the internal 8K RAM mod, Darren Atkinson's MCX-128 and
MCX32-SD, and the Emcee file server.**

VMC10 for Linux started as a re-engineering of *Virtual MC-10* 0.73, the
Windows 95/98 emulator by **James the Animal Tamer**.  That program was a
closed DirectX 6 executable that is no longer maintained.  This project is
a clean C11 + SDL2 rewrite.  The original binary was dissected for its
memory map, palette, character set and feature set, and the hardware
behaviour was re-derived from the MC-10 ROM disassembly and, for the MCX
boards, from Darren Atkinson's firmware and logic equations.

It goes well beyond the original:

* a cycle-counted 6803 core with the on-chip timer and its interrupts;
* scan-line accurate 6847 video with every mode;
* MCX-128 emulation built from the board's own GAL equations;
* MCX32-SD emulation reverse engineered from its firmware, with a folder on
  your computer acting as the SD card;
* a bit-level emulation of the MC-10's software serial port, with a
  built-in Emcee file server.

```
make
./vmc10                                  # MC-10 with 16K RAM pack
./vmc10 --mcx32sd --8k --emcee           # the deluxe 2020s MC-10
```

> **ROMs are not included.**  You need your own MC-10 BASIC ROM, plus
> Darren Atkinson's MCX BASIC images for the MCX boards.
> [`roms/README.md`](roms/README.md) lists every file, what it is for,
> its size and its checksums.

## Features at a glance

| Area | What's emulated |
|---|---|
| CPU | Motorola MC6803: full documented 6801/6803 instruction set with the 6803 cycle table; on-chip ports; 16-bit free-running timer with output compare and overflow interrupts; the external-bus register holes at `$04-$07`, `$0F` and `$15-$1F` that expansion hardware uses |
| Video | MC6847 VDG rendered per scan line: alphanumerics (internal and external character sets), semigraphics 4 and 6, all eight graphics modes (CG1-CG6, RG1-RG6), CSS colour sets, border colours; the original VMC10 palette; 4K video wrap on a stock machine |
| Memory | 4K or 20K (Tandy 16K pack); internal 8K mod (Kitsz/Snider) for full-screen 256x192; MCX-128 (128K banked RAM + EPROM, from its GAL22v10 equations); MCX32-SD (32K SRAM + 128K flash) |
| Keyboard | Full MC-10 matrix (including CONTROL, BREAK, SHIFT); positional "emulated" mode for games, character-based "sensible" mode for typing; arrows = W/A/S/Z; gamepad support |
| Sound | 1-bit speaker sampled cycle-exactly at 44.1 kHz with a gentle low-pass filter |
| Cassette | CLOAD/CLOADM from `.C10`, `.CAS`, `.K7` and `.WAV` (8/16-bit PCM); the tape moves only while software reads it; automatic warp while loading; CSAVE/CSAVEM decoded straight to `.C10`; C10 → WAV converter |
| Printer | LPRINT/LLIST decoded from the ROM's 600-baud bit-banging, written to a text file |
| MCX-128 | Boot menu, MicroColor BASIC / MCX BASIC / MCX BASIC (LARGE), PMODE 0-2 graphics, all four ROM map modes, both RAM banks |
| MCX32-SD | MCX BASIC 3.1 from flash (BIOS, bank switching, `$4200` remap, writable SST39SF010 flash), and an SD card emulated as a host folder: DIR, `DIR=`, LOAD, SAVE, RUN, MERGE, LOADM, SAVEM, OPEN/PRINT#/INPUT#, KILL, RENAME, NEWDIR, AUTOEXEC, subdirectories, ASCII BASIC tokenized and detokenized on the fly |
| Serial / Emcee | The MC-10's bit-banged 38400 baud RS-232 port, emulated bit by bit; built-in Emcee server (LOAD, SAVE, LOADM, SAVEM, OPEN, DIR, SETDIR, LLIST printing); or a TCP link to any external server, such as pyDriveWire |
| Tools | Quicktype (type a text file), clipboard paste, drag-and-drop loading, screenshots, full screen, terminal debugger (step, breakpoints, disassembly, memory, binary load, CSAVEM) |
| Quality | 53 automated regression tests (headless) with self-made fixtures: see [`docs/TESTING.md`](docs/TESTING.md) |

## Building

Requirements: a C11 compiler, `make`, and SDL2 development files.

```
sudo apt install build-essential libsdl2-dev      # Debian, Ubuntu, Mint
sudo dnf install gcc make SDL2-devel              # Fedora
sudo pacman -S base-devel sdl2                    # Arch

make              # builds ./vmc10
make test         # builds the headless harness and runs the regression tests
sudo make install # optional: /usr/local/bin/vmc10 and /usr/local/share/vmc10/roms
```

Optional at run time: `zenity` or `kdialog` for the file dialogs (F2, F3).
Drag and drop always works without them.

## Quick start

1. Put `MC10.BIN` in `roms/` (see [`roms/README.md`](roms/README.md)).
2. Run `./vmc10`.  Press **F1** at any time for the key help.
3. Load a tape: run `./vmc10 game.c10` (or press F2, or drop the file on
   the window), then type `CLOAD` (or `CLOADM` for machine code) and `RUN`
   (or `EXEC`).
4. Type in a listing from a text file: run `./vmc10 program.txt` (or press F3).

| Key | Action |
|---|---|
| F1 | On-screen help |
| F2 / Shift-F2 | Insert a cassette image / rewind |
| F3 | Quicktype a text file |
| F4 | Paste the clipboard |
| F5 | Keyboard mode: emulated (games) / sensible (typing) |
| F6 | Sound on/off |
| F7 | Warp speed |
| F8 | Debugger (in the terminal) |
| F9 / Shift-F9 | Reset / power cycle |
| F10 | Window size |
| F11 | Screenshot |
| F12 | Full screen |
| Esc | BREAK |
| Ctrl | CONTROL |
| Arrows | W A S Z |
| Alt-Q | Quit |

### Common configurations

```
./vmc10 --ram 4                       # a stock 4K MC-10
./vmc10 --8k                          # with the internal 8K RAM mod
./vmc10 --alice                       # Matra & Hachette Alice
./vmc10 --mcx128                      # MCX-128 (boot menu: 0, 1 or 2)
./vmc10 --mcx32sd --sd ~/mc10files    # MCX32-SD, SD card = ~/mc10files
./vmc10 --mcx128 --emcee ~/mc10files  # MCX-128 + built-in Emcee server
./vmc10 --mcx32sd --emcee             # MCX32-SD with SD (0:) and server (1:)
./vmc10 --mcx128 --serial localhost:65504   # an external Emcee server (pyDriveWire)
```

## Documentation

| Document | Contents |
|---|---|
| [`docs/USER_GUIDE.md`](docs/USER_GUIDE.md) | Everything about using the emulator: every option and setting, keyboard modes and mapping, cassettes, Quicktype, printer, debugger, 8K mod, MCX-128, MCX32-SD, SD card, Emcee, troubleshooting |
| [`docs/TECHNICAL.md`](docs/TECHNICAL.md) | Hardware and emulation reference: CPU core, timing, memory maps of every configuration, VDG modes, keyboard matrix, I/O ports, sound, cassette and printer encoding, serial line, file formats, BASIC tokenizer |
| [`docs/MCX32-SD_protocol.md`](docs/MCX32-SD_protocol.md) | The MCX32-SD's registers, memory map and SD controller mailbox protocol, reverse engineered from its firmware (no official documentation exists) |
| [`docs/REVERSE_ENGINEERING.md`](docs/REVERSE_ENGINEERING.md) | How it was done: dissecting VMC10.exe, the MC-10 ROM, the MCX-128 GAL, and the MCX32-SD firmware, plus the bugs and surprises along the way |
| [`docs/TESTING.md`](docs/TESTING.md) | The test harness, every automated test and what it proves, manual verification, and what hasn't been tested |
| [`roms/README.md`](roms/README.md) | The ROM images: names, purposes, sizes, checksums, where to find them |

## Credits

**VMC10 for Linux** was re-engineered by **Mark Abene** (phiber1), with
Claude Code (Anthropic).

It would not exist without the work of:

* **James the Animal Tamer** (emucompboy): author of the original
  *Virtual MC-10* for Windows (versions 0.6-0.73, 2000-2008), whose
  feature set, memory map, palette and conventions this project follows.
  He also wrote the Virtual Aquarius and other emulators.
* **camennie** of the Yahoo! MC-10 club: drew the 6847 character set that
  VMC10 used, which is built into this emulator.
* **Kevin Timmerman** (MC-10 ROM disassembly, 1999), with annotations by
  **Greg Dionne**, and fixes by James the Animal Tamer for TASM.  This was
  the key reference for the MC-10's I/O, keyboard, cassette and printer
  routines.  Greg Dionne also contributed much to the original VMC10
  (debugger features, undocumented opcode research).
* **Darren Atkinson**: designer of the MCX-128 and MCX32-SD expansions,
  author of MCX BASIC and the Emcee file server and protocol.  His logic
  equations, documentation and freely published firmware made the MCX
  emulation possible.  His material is available from:
  * MCX Wares blog: <https://mcxwares.blogspot.com/> (MCX32-SD news, user
    guide and the MCX Basic 3.1 update)
  * TRS-80 Color Computer Archive, MC-10 section:
    <https://colorcomputerarchive.com/repo/MC-10/> (MCX BASIC Reference
    Manual and hardware documents)
  * Internet Archive: MCX 128 Hardware Info,
    <https://archive.org/details/MCX128HardwareInfo>
* **Tim Lindner** and the **MAME** team: MAME's `mcx128` device was the
  first reference for the MCX-128 bank layout, before this project moved to
  the board's own GAL equations.
* **Michael Furman** (n6il), **pyDriveWire**: its source documented the
  Emcee printer command.
* **Dennis Bathory-Kitsz** ("Custom Color", *Color Computer Magazine*,
  September 1983) and **Ed Snider**: the internal 8K RAM upgrade.
* Everyone who kept the MC-10 alive: the Yahoo! MC-10 club, the MC-10
  groups.io list, and the MC-10 Facebook community.

TRS-80 and Micro Color Computer are trademarks of Tandy Corporation.
MicroColor BASIC is © Microsoft.  Alice is a Matra & Hachette product.
This project is not affiliated with any of them.

## Licence

VMC10 for Linux is released into the public domain under the
[Unlicense](LICENSE).  The bundled character set keeps its own status (see
`LICENSE`).  ROM images are not included and are not covered.
