# VMC10 for Linux: User Guide

This guide covers everything you can do with the emulator.  For how things
work inside, see [TECHNICAL.md](TECHNICAL.md).

## Contents

1. [Installing](#1-installing)
2. [ROM images](#2-rom-images)
3. [Starting the emulator](#3-starting-the-emulator)
4. [The settings file](#4-the-settings-file)
5. [The window: display, status line, screenshots](#5-the-window)
6. [Keyboard](#6-keyboard)
7. [Joystick / gamepad](#7-joystick--gamepad)
8. [Sound](#8-sound)
9. [Cassettes](#9-cassettes)
10. [Quicktype, paste and drag-and-drop](#10-quicktype-paste-and-drag-and-drop)
11. [Printer](#11-printer)
12. [Memory configurations and the 8K mod](#12-memory-configurations-and-the-8k-mod)
13. [Matra Alice](#13-matra-alice)
14. [MCX-128](#14-mcx-128)
15. [MCX32-SD and its SD card](#15-mcx32-sd-and-its-sd-card)
16. [Emcee file server](#16-emcee-file-server)
17. [Debugger](#17-debugger)
18. [Environment variables](#18-environment-variables)
19. [Troubleshooting and FAQ](#19-troubleshooting-and-faq)

---

## 1. Installing

```
sudo apt install build-essential libsdl2-dev     # Debian / Ubuntu / Mint
make
```

This produces `./vmc10`.  You can run it from the project folder, or
install it with `sudo make install`, which puts the binary in
`/usr/local/bin` and creates `/usr/local/share/vmc10/roms/`.  Set
`PREFIX=...` to install elsewhere.

Runtime extras:

* **zenity** (or **kdialog**): the file pickers for F2 and F3.  Without
  them the emulator says so, and you can drag files onto the window or name
  them on the command line instead.
* **A gamepad**: optional, see [section 7](#7-joystick--gamepad).

## 2. ROM images

No ROMs are included.  [`roms/README.md`](../roms/README.md) lists each one
with its size, CRC32 and SHA-1.  In summary:

| Configuration | Needs |
|---|---|
| MC-10 | `roms/MC10.BIN` |
| Alice (`--alice`) | `roms/alice.bin` |
| MCX-128 (`--mcx128`) | `roms/MC10.BIN`, `roms/mcx128bas.rom` |
| MCX32-SD (`--mcx32sd`) | `roms/MC10.BIN`, `roms/mcx32sd_flash.bin` (build it with `tools/make_mcx32sd_flash.py`) |

ROMs are searched for in `./roms`, `roms/` next to the executable,
`share/vmc10/roms` under the install prefix, `~/.local/share/vmc10/roms`,
`/usr/local/share/vmc10/roms` and `/usr/share/vmc10/roms`.

## 3. Starting the emulator

```
vmc10 [options] [file]
```

A file named on the command line is loaded according to its type:

* `.txt` / `.bas`: Quicktyped after boot.
* Anything else (`.c10`, `.cas`, `.k7`, `.wav`): inserted as a cassette.

### All options

| Option | Meaning |
|---|---|
| `--rom FILE` | BASIC ROM image (default `roms/MC10.BIN`) |
| `--alice` | Use the Matra Alice ROM (`roms/alice.bin`) |
| `--chr FILE` | 768-byte internal character set, replacing the built-in font |
| `--chrx FILE` | External character set (2K or 4K), used when the VDG's INT/EXT line is high |
| `--ram 4\|20` | Without / with the Tandy 16K RAM pack (default 20) |
| `--8k` | Internal 8K RAM mod: `$4000-$5FFF` on board; the VDG sees 8K, so CG6/RG6 fill the screen |
| `--4k` | Stock 4K internal RAM (default) |
| `--no-artifact` | Show RG6 with CSS=1 as raw 256x192 white on black, without NTSC artifact colours (artifacting is on by default) |
| `--artifact-phase 0\|1` | Artifact colour phase: 0 = pixel pairs `10` orange and `01` blue, 1 = swapped (Alt-A toggles) |
| `--mcx128` | Darren Atkinson's MCX-128 (128K RAM + MCX BASIC EPROM) |
| `--mcx128-rom FILE` | MCX-128 EPROM image (default `roms/mcx128bas.rom`) |
| `--mcx32sd` | Darren Atkinson's MCX32-SD (32K RAM, 128K flash, SD card) |
| `--mcx32sd-flash FILE` | Seed the MCX32-SD flash working copy from FILE |
| `--sd DIR` | Folder that acts as the MCX32-SD's SD card (default `~/mcx32sd`) |
| `--remap` / `--no-remap` | MCX32-SD: enable / disable the board's `$4200-$43FF` mapping of BASIC's old variable addresses (default: disabled, Alt-M toggles; see section 15) |
| `--emcee [DIR]` | Built-in Emcee file server on the serial port.  DIR defaults to the SD folder, or `~/emcee`.  It is only taken as the folder if it exists. |
| `--serial HOST:PORT` | Connect the serial port to an external TCP server instead (e.g. pyDriveWire) |
| `--debug-port ADDR[,FILE]` | For programmers: log every write to address ADDR (hex) to the terminal or FILE, with the frame number, beam line, active-display row `y`, cycle within the line, total cycles, cycles since the previous write (`dt`) and the value.  The write still reaches the bus.  `BE00` is unmapped in every configuration, so `STAA $BE00` makes a free timing marker.  Off by default; **Alt-D** toggles it at run time (at the configured address, default `BE00`), and the `debug`, `debug_port` and `debug_log` settings set it up permanently |
| `--scale N` | Window scale 1-6 (default 3, i.e. 960x720) |
| `--fullscreen` | Start full screen |
| `--tape FILE` | Insert a cassette image |
| `--quicktype FILE` | Type a text file after boot |
| `--type TEXT` | Type TEXT after boot; `\n` = ENTER |
| `--load FILE@ADDR` | Load a raw binary at hex ADDR after boot (default `4346`) |
| `--printer FILE` | Printer output file (default `LPRINT.TXT` in the current folder, appended to) |
| `--tapedir DIR` | Where CSAVE writes `.C10` files (default `.`) |
| `--sensible` | Start in "sensible" keyboard mode |
| `--volume N` | 0-100 (default 50) |
| `--nosound` | Start with sound off |
| `--warp` | Start at maximum speed |
| `--debug` | Start in the debugger |
| `--config FILE` | Use a different settings file |
| `--c10-to-wav IN OUT` | Convert a cassette image to an 8-bit 22050 Hz WAV and exit |
| `-h`, `--help` | Show the option summary |

The emulator boots BASIC for 1.5 emulated seconds before applying
`--tape`, `--quicktype`, `--type` and `--load`, so BASIC is ready for them.

## 4. The settings file

`~/.config/vmc10/vmc10.ini` (or `--config FILE`) holds `key = value` lines.
Lines starting with `#`, `;` or `[` are ignored.  Command-line options
override it.

| Key | Values | Meaning |
|---|---|---|
| `rom` | path | BASIC ROM |
| `chr`, `chrx` | path | Character sets |
| `ram` | `4` / `20` | Tandy 16K pack |
| `expansion` | `none`, `16k`, `mcx128`, `mcx32sd` | Cartridge slot (overrides `ram`) |
| `internal8k` | `0` / `1` | 8K mod |
| `mcx128rom` | path | MCX-128 EPROM |
| `mcx32sdflash` | path | MCX32-SD flash seed |
| `sdcard` | path | SD card folder |
| `emcee` | `1` or path | Enable the built-in Emcee server (optionally with its folder) |
| `serial` | `host:port` | External serial/TCP peer |
| `scale` | 1-6 | Window scale |
| `fullscreen` | `0` / `1` | |
| `volume` | 0-100 | |
| `sound` | `0` / `1` | |
| `keyboard` | `emulated` / `sensible` | |
| `printer` | path | Printer file |
| `tapedir` | path | CSAVE folder |
| `fast_typing` | `0` / `1` | Deliver typed text through the ROM's keyscan routine (default 1) |
| `artifact` | `0` / `1` | NTSC artifact colours in RG6/CSS=1 (default 1) |
| `artifact_phase` | `0` / `1` | Artifact colour phase (saved when changed with Alt-A) |
| `remap` | `0` / `1` | MCX32-SD `$4200` variable mapping (default 0; saved when changed with Alt-M) |
| `debug` | `0` / `1` | Debug port on at start (default 0; saved when changed with Alt-D) |
| `debug_port` | hex address | Address the debug port watches (default `BE00`) |
| `debug_log` | path | Debug port log file (default: the terminal) |
| `lowercase_inverse` | `0` / `1` | Quicktype keeps lower case (shown as inverse video) instead of upper-casing it |
| `color.NAME` | `R,G,B` or `#RRGGBB` | Palette.  NAME is one of `green yellow blue red buff cyan magenta orange black darkgreen darkorange artifactblue artifactorange`.  Each line overrides one colour. |

**Only settings changed with hotkeys are saved back** (window size with
F10, sound with F6, keyboard mode with F5, artifact phase with Alt-A, MCX32-SD
mapping with Alt-M, debug port with Alt-D).  Command-line options are never
written to the file, and other lines in it are preserved.

The default palette is the one from the original VMC10:

| Name | RGB |
|---|---|
| green | 40, 224, 40 |
| yellow | 240, 240, 112 |
| blue | 32, 32, 216 |
| red | 168, 32, 32 |
| buff | 240, 240, 240 |
| cyan | 40, 168, 168 |
| magenta | 211, 97, 250 |
| orange | 240, 136, 40 |
| black | 16, 16, 16 |
| darkgreen | 16, 96, 16 |
| darkorange | 128, 48, 16 |
| artifactblue | 48, 120, 216 |
| artifactorange | 216, 136, 80 |

## 5. The window

The emulated screen is 320x240: the 6847's 256x192 picture plus a 32-pixel
side border and a 24-line top and bottom border, in the border colour the
VDG produces.  It is scaled with square pixels by `--scale` / F10 (1x-6x),
or fills the screen with F12.  Resizing the window keeps the aspect ratio.

**Status line:** messages appear for three seconds in the bottom border,
drawn with the MC-10's own font: tape inserted, CSAVE written, printer
started, screenshot taken, keyboard mode, and so on.  While a tape is being
read it shows `TAPE name nn%`.  While text is being typed it shows
`TYPING... n`.  In warp mode it shows `WARP`.  The window title also shows
`[WARP]`, `[sensible keys]` and `[tape]`.

**Screenshots:** F11 saves the 320x240 picture as
`screenshots/vmc10_NNNN.bmp` in the current folder.

**F1** overlays a key summary on the screen.

**Artifact colours.**  The MC-10 was meant for a composite TV.  In RG6
(256x192) with the buff colour set (CSS=1, e.g. `POKE 49151,124`), a TV
can't resolve single pixels: the 6847's pixel clock interacts with the
colour subcarrier, so each **pair** of pixels shows as one colour and the
picture is effectively 128x192 in four colours.  Many games and pictures
depend on this.

| Pixel pair | Shown as |
|---|---|
| `00` | black |
| `11` | white (buff) |
| `10` | orange (phase 0) / blue (phase 1) |
| `01` | blue (phase 0) / orange (phase 1) |

* **Which pair gets which colour** depends on the clock phase the VDG
  happens to lock onto at reset, so real machines vary from power-on to
  power-on.  Press **Alt-A** to swap the colours if a game looks wrong, or
  set `--artifact-phase` / `artifact_phase`.
* **The colours themselves** depend on the TV.  The defaults are a muted
  orange and a cyan leaning towards blue.  Tune them with
  `color.artifactorange` and `color.artifactblue`.
* **Monitor look:** `--no-artifact` (setting `artifact = 0`) shows the raw
  256x192 white-on-black picture, as an RGB-style monitor would.
* **Other modes** (including RG6 with the green colour set) are shown
  unchanged.

## 6. Keyboard

The MC-10 keyboard has 48 keys in a matrix, plus CONTROL, BREAK and SHIFT.
Its layout differs from a PC's: the top row ends `9 0 : -`, `@` sits after
`P`, and many symbols are SHIFT-ed digits.  There are no cursor keys.
CONTROL plus a letter types whole BASIC keywords.

### Emulated mode (default)

PC keys are mapped by **position** onto the MC-10 matrix, and the MC-10
works out what character it is.  This is right for games, which read the
matrix directly, and for anything that uses CONTROL or SHIFT combinations.

| PC key | MC-10 key |
|---|---|
| A-Z, 0-9 | same |
| `-` | `:` |
| `=` | `-` |
| `[` | `@` |
| `;` `,` `.` `/` | same |
| Space, Enter | SPACE, ENTER |
| Shift | SHIFT |
| Ctrl | CONTROL |
| Esc, Pause | BREAK |
| Backspace | CONTROL-A (the MC-10's rubout) |
| Up / Left / Right / Down | W / A / S / Z (the usual MC-10 game keys) |

So to type `"` press Shift-2, `+` is Shift-`;`, `*` is Shift-`-` (the
`:` key), and so on, exactly as on the real keyboard.

### Sensible mode (F5)

The **characters** you type on the PC are typed into the MC-10, whatever
keys produce them on the MC-10.  `"`, `+`, `(` and so on just work, and
lower case is turned into upper case.  Enter and Backspace work as
expected.  Esc (BREAK) and the arrow keys still go straight to the matrix.

Typed characters are delivered through the ROM's keyboard routine, so this
mode is for typing at BASIC or into programs that read the keyboard
through the ROM (INKEY$, INPUT).  Use emulated mode for games that scan
the keyboard themselves.

The mode is saved in the settings file.

## 7. Joystick / gamepad

Any SDL game controller is mapped onto the keys most MC-10 games use: the
D-pad and left stick give W (up), A (left), S (right) and Z (down), and
buttons A and B give SPACE.  Controllers can be connected while the
emulator is running.

## 8. Sound

The MC-10's speaker is a single bit (bit 7 of `$BFFF`).  The emulator
samples it cycle-exactly at 44.1 kHz and applies a gentle low-pass filter,
so high-pitched sounds don't get the harsh overtones the original VMC10
was known for.  The SOUND command's pitch is exact: `SOUND 100,5` measures
258 Hz, as the ROM's timing formula predicts.  F6 toggles sound.
`--volume` sets the level.

The emulator paces itself on the audio clock, keeping about 50 ms queued.
Without an audio device it paces on the system timer.

## 9. Cassettes

### Loading

1. Insert a tape: F2, `--tape FILE`, a file argument, or drag and drop.
2. Type `CLOAD` (BASIC) or `CLOADM` (machine code); `CLOAD "NAME"` finds a
   named program.
3. `RUN` or `EXEC`.

You can also type CLOAD first and insert the tape afterwards.

Supported images:

| Extension | Format |
|---|---|
| `.c10`, `.cas`, `.k7` | The raw byte stream as recorded on tape (leaders, blocks, checksums), as used by VMC10 and other emulators |
| `.wav` | A recording of a real tape: 8- or 16-bit PCM, mono or stereo, any sample rate.  The signal is DC-corrected and zero-crossings are found with hysteresis. |

Details:

* **The tape moves only while software reads it**, a stand-in for motor
  control, which the MC-10 doesn't have.  Inserting a tape doesn't start
  anything, and the tape pauses while BASIC is busy between blocks.  The
  timing is exact, so custom tape loaders work.
* **Automatic warp:** while the tape is being read, the emulator runs as
  fast as it can.  A 16K program loads in a few seconds.
* **Short leaders:** images with fewer than 64 leader bytes get a leader
  added in front.
* **Shift-F2 rewinds** the tape, for programs saved as several files.

### Saving

Just type `CSAVE "NAME"` or `CSAVEM "NAME",start,end,exec`.

* **No dialog:** the emulator decodes the cassette output signal back into
  bytes.  When the recording ends (one second of silence, or quitting), it
  writes `NAME.C10` in the tape folder (`--tapedir`, default the current
  folder).  The name comes from the tape header.
* **Existing files** are never overwritten; a suffix is added
  (`NAME-1.C10`, ...).
* **Only real recordings are saved:** output without a tape block (for
  example serial-port traffic on the same line) is ignored.

### Converting to WAV

`vmc10 --c10-to-wav game.c10 game.wav` writes a WAV you can play into a
real MC-10's cassette input, with the right square-wave frequencies (1200
and 2400 Hz).

## 10. Quicktype, paste and drag-and-drop

**Quicktype** (F3, `--quicktype`, or a `.txt`/`.bas` file argument) types
a text file into the MC-10 as if you typed it.  It's the easiest way to
enter a BASIC listing written on the PC.

**Paste** (F4) types the clipboard the same way.

**Drag and drop** onto the window: `.txt`/`.bas` files are Quicktyped,
anything else is inserted as a tape.

How typing works:

* **Line endings:** CR, LF or CRLF become ENTER; tabs become spaces.
* **Case:** lower case becomes upper case.  With `lowercase_inverse = 1`
  it's kept, and shows as inverse video, the MC-10's lower case.
* **Graphics characters:** bytes `$80-$FF` are delivered unchanged, so
  listings containing semigraphics characters can be typed.
* **Speed:** text goes straight into the ROM's keyboard routine (`$F883`),
  so typing is about as fast as BASIC can take it.
* **Fallback:** if software isn't calling that routine but is scanning the
  keyboard itself (a boot menu, a game, a non-Microsoft ROM), each
  character is pressed on the emulated keyboard matrix instead, for a few
  frames each, with a pause after ENTER.  While text is waiting to be typed
  under a non-stock ROM, the emulator runs in warp.

## 11. Printer

LPRINT, LLIST and `PRINT #-2` go to the MC-10's serial printer port.  The
emulator decodes the ROM's 600-baud serial output and appends the text to
`LPRINT.TXT` (`--printer FILE`).  CR becomes a newline.  The printer always
reports "ready".

Under MCX BASIC, LLIST is sent to the Emcee server's printer instead: use
`--emcee` and it lands in the same printer file.

## 12. Memory configurations and the 8K mod

| Configuration | RAM | BASIC `PRINT MEM` |
|---|---|---|
| `--ram 4` | 4K at `$4000-$4FFF` | 3142 |
| default (`--ram 20`) | 4K + Tandy 16K pack, `$4000-$8FFF` | 19526 |
| `--ram 4 --8k` | 8K at `$4000-$5FFF` | 7238 |
| `--8k` | 8K + 16K pack | 19526 |

**The 8K mod** emulates the piggyback internal RAM upgrade: Dennis
Bathory-Kitsz's from *Color Computer Magazine* (September 1983), or Ed
Snider's simpler modern version.  On a stock MC-10 the 6847 can address
only 4K, so the two 6K graphics modes, CG6 (128x192, 4 colours) and RG6
(256x192, 2 colours), repeat the top two-thirds of the picture in the
bottom third.  With the mod the VDG addresses 8K and those modes fill the
screen.  It combines with every other option, including the MCX boards.

Note that stock MicroColor BASIC keeps its variables at `$4200-$4345`,
inside a 6K graphics screen.  Full-screen graphics are for machine code, or
for MCX BASIC 3 (which moves the variables) on an MCX32-SD.

## 13. Matra Alice

`--alice` boots the Alice 4K ROM.  The hardware is the same as the MC-10's.
The keyboard keeps the MC-10's positions, so the AZERTY legends aren't
remapped.  Typing works through the matrix fallback (the Alice ROM's
keyboard routine is at a different address).

## 14. MCX-128

`--mcx128` adds Darren Atkinson's MCX-128: 128K of static RAM and a 16K
EPROM with MCX BASIC 2.1 (`roms/mcx128bas.rom`).  At power-on the boot
menu offers:

| Key | Choice | Free memory |
|---|---|---|
| 0 | MicroColor BASIC (internal ROM, with the extra RAM) | 31,558 |
| 1 | MCX BASIC | 28,703 |
| 2 | MCX BASIC (LARGE): programs in a separate 64K bank from video | 47,454 |

MCX BASIC 2.1 adds Extended-Color-BASIC-style commands:

* graphics: PMODE 0-2, PCLS, SCREEN, LINE, CIRCLE, PAINT, DRAW, PSET,
  PRESET;
* `&H` hex constants;
* loading and saving through an Emcee server.

All of this is emulated.  PMODE 3 and 4 are not part of MCX BASIC 2.1
(they give `?FC ERROR`); they arrived with MCX BASIC 3 on the MCX32-SD.

The hardware is emulated from the board's GAL22v10 equations:

* RAM bank register at even addresses `$BF00-$BF7E`;
* ROM map register at odd addresses `$BF01-$BF7F`;
* keyboard/VDG moved to `$BF80-$BFFF`;
* the MC-10's own RAM serves `$4000-$4FFF` (`$5FFF` with `--8k`) while
  bank bit 1 is 0;
* four ROM map modes.

See [TECHNICAL.md](TECHNICAL.md#13-mcx-128).

**Compatibility:** MCX BASIC moves BASIC's system variables from `$42xx`
to `$01xx`, and the MCX-128 (unlike the MCX32-SD) has no hardware to hide
that.  So older BASIC programs that PEEK/POKE `$42xx`, and machine code
that uses them, can misbehave.  Atkinson published patches for some Jim
Gerrie games for this.  Programs written for the MCX32-SD (which call its
BIOS at `$2000` or use its registers) don't run on an MCX-128 at all.

## 15. MCX32-SD and its SD card

`--mcx32sd` adds Darren Atkinson's MCX32-SD: 32K of static RAM, 128K of
flash holding the BIOS and MCX BASIC 3.1, and an SD card.

### Setting up

1. Build the flash image: `tools/make_mcx32sd_flash.py "MCX Basic 3.1
   Update.zip" roms/mcx32sd_flash.bin` (see `roms/README.md`).
2. Run `vmc10 --mcx32sd`.  The SD card is the folder `~/mcx32sd` (created
   if needed); choose another with `--sd DIR`.

The emulator runs from a working copy of the flash in
`~/.local/share/vmc10/mcx32sd_flash.bin`, so `KILL MEM`, `WRITE MEM` and
firmware updates persist across sessions without touching your original.
`--mcx32sd-flash FILE` re-seeds the working copy.

At power-on you see the MicroColor BASIC banner followed by
`MCX BASIC 3.1`.  `PRINT MEM` gives 27618, as the MCX32-SD user guide says.

### The SD card folder

Copy files into the folder and use them from MCX BASIC.  The emulated SD
controller handles file formats itself, as the real ATmega controller
does:

| On the card | LOAD / RUN / MERGE (BASIC) | LOADM / RUNM | LOAD* (arrays) |
|---|---|---|---|
| `.BAS` | ASCII text, tokenized on the fly | - | - |
| `.C10` / `.CAS` / `.K7` | BASIC program (binary or ASCII) from the tape image | machine code from the tape image | array data |
| `.WAV` | decoded tape recording | decoded tape recording | decoded tape recording |
| `.BIN` | - | CoCo DECB binary (multi-segment) | - |
| `.ARY` | - | - | raw |

**Default extensions** are tried when a name has none:

| Command | Extensions tried |
|---|---|
| LOAD, RUN, MERGE | `.BAS`, `.C10`, `.WAV` |
| LOADM, RUNM | `.BIN`, `.C10`, `.WAV` |
| LOAD* | `.ARY`, `.C10`, `.WAV` |
| LOAD SCREEN | `.SCR` |
| OPEN | `.DAT` |

**SAVE picks the format from the extension:**

| SAVE to | Writes |
|---|---|
| `NAME` or `NAME.BAS` | plain ASCII text, ready to edit |
| `NAME.C10` | a tape image |
| `NAME.WAV` | a playable recording |
| SAVEM to `NAME` / `NAME.BIN` | a DECB binary |
| SAVE* | `.ARY` |
| SAVE SCREEN | `.SCR` |

The tokenizer is byte-identical to MCX BASIC's own: a program loaded from
`.BAS` is exactly what you would get by typing it in.  That includes `'`
(stored as `:` + `$DD`), ELSE (stored as `:` + `$C9`), `?` for PRINT, and
literal DATA and REM text.

File names are matched without regard to case (the MC-10 types capitals,
Linux is case-sensitive).  Names longer than 38 characters are hidden,
because the BIOS can't receive them.

### Commands

* `DIR`: lists files with sizes and folders as `<DIR>`.  Large files are
  shown in K or M.
* `DIR "M*"`: wildcards `*` and `?`.
* `DIR "GAMES/"`: a subfolder.
* `DIR = "GAMES"`: change folder.  `DIR = ".."` goes up, `DIR = "0:/"` back
  to the root.
* `LOAD`, `RUN`, `MERGE`, `CHAIN`, `SAVE`, `LOADM`, `RUNM`, `SAVEM`,
  `LOAD*`, `SAVE*`, `LOAD SCREEN`, `SAVE SCREEN`.
* `OPEN "I"/"O"/"A",#1,"NAME"`, `PRINT #1`, `INPUT #1`, `LINE INPUT #1`,
  `EOF(1)`, `CLOSE`: file numbers 1 and 2 on the SD card.  INPUT# isn't
  allowed in direct mode (`?ID ERROR`); use it in a program.
* `KILL "NAME.EXT"` deletes a file or an empty folder (`?AO ERROR` if
  it's not empty).
* `RENAME "OLD" TO "NEW"`: NEW is relative to OLD's folder.
  `RENAME "F.BAS" TO "GAMES/"` moves a file.
* `NEWDIR "NAME"` creates a folder.

**Paths** work everywhere:

* `[device:][/]folder/folder/file`, with device 0 = SD card and 1 = Emcee
  server;
* `.` and `..`;
* `$/NAME` for the folder the running program was loaded from.

**AUTOEXEC:** a BASIC program named `/MCX32-SD/AUTOEXEC` (`.BAS`, `.C10`
or `.WAV`) runs at power-on.  Hold BREAK to skip it.

**Flash:**

* `KILL MEM @n` erases bank n (banks 0 and the ones in use are refused with
  `?FC ERROR`).
* `WRITE MEM @n,source,offset,count` programs bytes.
* The flash is an SST39SF010 with 4K sectors; a bank is two sectors.

**Bypass:** holding CONTROL at power-on boots plain MicroColor BASIC
without MCX BASIC.

**The `$4200` variable mapping (Alt-M, `--remap`):**

* **What the real board does:** MicroColor BASIC kept its system variables
  at `$4200-$4345`, inside the area a 6K graphics screen needs.  MCX BASIC 3
  moves them down by `$4000`, and the real MCX32-SD maps `$4200-$43FF` onto
  the new location, so old programs that PEEK or POKE the original
  addresses keep working.  The side effect: plain PEEK and POKE can't reach
  that part of the graphics screen.  On a real board you need `PEEK@` and
  `POKE@`, and `POKE 30,0` (register `$1E`) only switches the mapping off
  until BASIC next waits for input.
* **The emulator's default:** the mapping is **disabled**.  The area is
  plain memory, so BASIC's PEEK and POKE (and machine code) reach the whole
  graphics screen directly.  MCX BASIC itself never uses the old addresses,
  so nothing in BASIC depends on it.
* **To get the real board's behaviour,** use `--remap` (setting
  `remap = 1`) or toggle it with **Alt-M**.  Old BASIC programs that POKE
  MicroColor BASIC's variable addresses need it.  When it's on, writes go
  to both the relocated variables and the screen, as on the real board, so
  graphics still display.

**Graphics:** MCX BASIC 3 offers PMODE 0-4.  Modes 3 and 4 (CG6/RG6) need
the internal RAM upgrade to show the whole screen: use `--8k`.

## 16. Emcee file server

MCX BASIC (on both MCX boards) can load and save files on an Emcee server:
a PC program that talks to the MC-10 through its serial port, at 38400
baud.  The emulator reproduces that serial port bit by bit, and offers two
ends for it.

### Built-in server: `--emcee [DIR]`

```
vmc10 --mcx128 --emcee ~/mc10files
vmc10 --mcx32sd --emcee                 # server = device 1:, same folder as the SD card
vmc10 --mcx32sd --emcee ~/server        # or a separate folder
```

It implements Darren Atkinson's documented filing protocol plus the print
command:

* LOAD, RUN, MERGE, LOADM, RUNM, SAVE, SAVEM;
* OPEN (input, output, append), PRINT#, INPUT#;
* DIR (`F` + `$`), DIR LIST (`D`), SETDIR (`C`);
* LLIST / `PRINT #-2`, which go to the printer file.

It uses the same format conversion and default extensions as the SD card.
Paths may contain subfolders and `..`.

Because the protocol's file listing has no "folder" flag, **folders are
listed with a trailing `/`** (`ARCADE/`), so you can see them and change
into them.

Syntax:

* **MCX BASIC 2.1 (MCX-128):** everything goes to the server.  Use
  `SETDIR "GAMES"` to change folder.
* **MCX BASIC 3.1 (MCX32-SD):** use device `1:`.  For example
  `LOAD "1:GAMES/PROG"`, `DIR 1`, `DIR "1:GAMES/"`, `DIR = "1:GAMES"`, or
  `DIR = 1` to make the server the default device.  Some commands (e.g.
  KILL) are SD-only.

### External servers: `--serial HOST:PORT`

```
pyDriveWire --accept --port 65504        # run in the folder to serve
vmc10 --mcx128 --serial localhost:65504
```

The emulated serial line is connected to a TCP socket.  It works with
anything that speaks the protocol over TCP: pyDriveWire, or a real Emcee
server on another computer through a TCP-to-serial bridge.  The connection
is retried if the server isn't up yet.

Notes on pyDriveWire's EmCee support (experimental, as of its 2023 source):

* **Limited commands:** DIR, SETDIR, LOAD and LOADM of `.C10`/`.CAS`, plus
  printing.  No SAVE or data files.
* **Exact file names:** it needs the exact file name including extension,
  and it's case-sensitive, so name your files in capitals.
* **SETDIR bug:** SETDIR stores the name without combining it with the
  current folder, and DIR lists the folder from before it changed
  directory.  After one SETDIR, DIR shows the old folder once, then fails
  with `?NE ERROR` until pyDriveWire is restarted.  Give SETDIR a full
  path, or use its console command `mc setdir`.
* **Python 3:** its source tree still contains Python 2 syntax in the EmCee
  handlers; use its prebuilt release binaries.

## 17. Debugger

F8 (or `--debug`) stops the emulator and opens a monitor in the terminal
you started it from.  The window freezes until you continue.  Numbers are
hex (`$` or `0x` prefixes allowed); prefix `#` for decimal.

| Command | Action |
|---|---|
| `r` | Registers, flags, cycle count, and the instruction at PC |
| `s [n]` | Step n instructions (default 1) |
| `c` or `g` | Continue (close the debugger) |
| `d [addr] [n]` | Disassemble n instructions (default 16) from addr (default: where you left off) |
| `m addr [n]` | Hex and ASCII dump of n bytes (default 64) |
| `w addr byte...` | Write bytes |
| `pc addr` | Set the program counter |
| `b addr` / `b` | Add a breakpoint (up to 16) / list them |
| `bc [addr]` | Clear one breakpoint / all |
| `bload file addr` | Load a raw binary file into memory |
| `csavem file name start end exec` | Save memory as a `.C10` machine-code tape |
| `reset` / `hard` | Soft reset / power-on reset |
| `q` | Quit the emulator |
| `h` | Help |

When a breakpoint is hit, the debugger opens automatically.  `c` continues
past it.  Memory reads in the debugger have no side effects on the 6803's
timer flags.

## 18. Environment variables

| Variable | Effect |
|---|---|
| `VMC10_TRACE_SD=1` | Log MCX32-SD controller commands, errors, flash saves and remap changes |
| `VMC10_TRACE_SERIAL=1` | Log every byte on the serial line, both directions |
| `VMC10_TRACE_ILLEGAL=1` | Log the first ten undefined opcodes executed, with their addresses |
| `SDL_*` | The usual SDL variables, e.g. `SDL_AUDIODRIVER`, `SDL_VIDEODRIVER` |

## 19. Troubleshooting and FAQ

* **"cannot load the BASIC ROM":** put `MC10.BIN` in `roms/` (see
  `roms/README.md`).  The file must be 8192 bytes, or 4096 bytes (it's
  mirrored).
* **Nothing happens after inserting a tape:** type `CLOAD` / `CLOADM`.
  The tape moves only while the MC-10 reads it.
* **`?ID ERROR` with INPUT#:** INPUT# only works inside a program.
* **`[` or `]` disappear when typing:** the MC-10 keyboard can't type
  them, so they're dropped.
* **PMODE 4 gives `?FC ERROR` on the MCX-128:** MCX BASIC 2.1 supports
  only PMODE 0-2.
* **A game crashes on the MCX-128 but works on the MCX32-SD:** it's
  probably written for the MCX32-SD.  For example, the DOttery games load
  their high-score files through the MCX32-SD BIOS at `$2000`.  See
  section 14.
* **Graphics repeat in the bottom third:** that's a stock 4K MC-10's
  behaviour in CG6/RG6.  Use `--8k`.
* **No file dialog on F2/F3:** install `zenity`, or drag files onto the
  window.
* **Choppy sound:** the emulator paces on the audio device.  Check that
  nothing else is starving the CPU, or try `--nosound` to see whether the
  audio device is the problem.
* **Where did my CSAVE go?** Into the tape folder (default: the current
  folder), named after the program, with the full path on the status line
  and in the terminal.
