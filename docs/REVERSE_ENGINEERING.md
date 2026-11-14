# How VMC10 for Linux was re-engineered

This is the story of how a closed, unmaintained Windows emulator became a
documented native Linux one, and how two expansion boards with little or
no hardware documentation were brought to life.  It records the methods,
the evidence, and the dead ends.

## Contents

1. [Starting point: VMC10.exe](#1-starting-point-vmc10exe)
2. [Why a rewrite, not a decompile](#2-why-a-rewrite-not-a-decompile)
3. [The MC-10 itself: the ROM disassembly](#3-the-mc-10-itself-the-rom-disassembly)
4. [Building and validating the core](#4-building-and-validating-the-core)
5. [The internal 8K mod](#5-the-internal-8k-mod)
6. [MCX-128: from MAME to the GAL equations](#6-mcx-128-from-mame-to-the-gal-equations)
7. [MCX32-SD: reverse engineering the firmware](#7-mcx32-sd-reverse-engineering-the-firmware)
8. [The Emcee serial link](#8-the-emcee-serial-link)
9. [Bugs found along the way](#9-bugs-found-along-the-way)
10. [Tools and techniques](#10-tools-and-techniques)

---

## 1. Starting point: VMC10.exe

*Virtual MC-10* 0.73 by James the Animal Tamer is a 303,104-byte PE32
executable built with Microsoft Visual C++ 6 for Windows 95/98.  It uses
DirectDraw, DirectSound and DirectInput (DirectX 6), GDI, and the common
dialogs.  No source was ever released.

The binary was examined with `objdump` (which reads PE files) and short
Python scripts:

| Section | Size | Notes |
|---|---|---|
| `.text` | `0x25157` | code |
| `.rdata` | `0x239A` | imports, strings |
| `.data` | `0xF000` on disk, about 1.8 MB in memory | uninitialised arrays: emulated RAM and per-address handler tables |
| `.rsrc` | `0x107D0` | dialogs, menus, icons |

**Embedded data, found by searching the binary for the known images:**

| What | File offset | Notes |
|---|---|---|
| MC-10 BASIC ROM | `0x317DC` | byte-identical to the archive's `rom/MC10.BIN` |
| Alice ROM | `0x337DC` | identical to `rom/Alice.bin` |
| NEC Trek external charset | `0x2E7C4` | 4K |
| **Built-in 6847 font** | **`0x2E404`** | 768 bytes.  Not in the archive's `rom/` folder; located by scanning for a 768-byte block whose 12-row glyphs have blank top 3 and bottom 2 rows.  This is camennie's font, now `src/font6847.h`. |

**The memory map:** the code at `0x40F41D` fills a 64K table of
per-address handler pointers, one entry per byte of the address space.
Reading it showed:

* without the expander, `$5000-$7FFF` maps onto the 4K RAM (`and ecx,0xfff`
  plus the RAM base) with a *separate write handler*, so it's a read-only
  mirror;
* expander types 1 and 2;
* a "RAM at `$C000`" option;
* an unconnected-memory handler for `$8000` and up.

**Strings and settings:** the INI keys (`[MEMORY] enhancedflag
expandertype writeROMflag ...`, `[SPEED] activescancust=11364
vblankcust=3551`, `[COLOR] c00..c2c`) and the menu texts gave the full
feature list: Quicktype, CSAVEM, Load Binary, Paste, LPRINT to file,
screenshots, full screen, the debugger, Alice mode, and a serial joystick.

**Palette:** taken from the original's `default.ini`.

## 2. Why a rewrite, not a decompile

A literal decompilation of an MSVC 6 / DirectX 6 program would have
produced tens of thousands of lines of anonymous C tied to Win32,
DirectDraw surfaces and message loops, with the emulation buried inside.
Instead, the binary was treated as a **specification**: what memory map,
palette, font, defaults and features does it have?  The emulation was
then written from scratch in portable C11 with SDL2.  The CPU core was
also new; the original used MAME's 6800 core.

## 3. The MC-10 itself: the ROM disassembly

The VMC10 archive includes Kevin Timmerman's annotated MC-10 ROM
disassembly (1999), with additions by Greg Dionne.  It provided the
hardware behaviour directly:

* **Keyboard:** `$F883` scans the matrix.  Port 1 strobes columns, `$BFFF`
  returns rows 0-5, port 2 bit 1 is row 6.  The decode tables at
  `$F956`, `$F969` and `$F97C` give the full matrix, SHIFT characters and
  CONTROL keyword tokens.
* **Cassette:** `$FCB7` writes the leader, `$FCC0` a block, `$FD03` a
  character.  Each bit is one square-wave cycle with half-periods of
  `$20`/`$40` loop counts.  On input, `$FF4E` reads the leader and `$FF14`
  a character, timing cycles against the threshold at `$422C`.
* **Printer:** `$F9C6`: wait for port 2 bit 2, then bit-bang 600 baud on
  port 2 bit 0 through `STAB $03` at `$FA0C`.
* **SOUND:** `$FFAB`: toggle `$BFFF` bit 7, timed by the output-compare
  flag, giving a frequency of 1 MHz / (11 x (256 - pitch) + 21).
* **Reset and memory sizing:** `$F72E` sets up the ports.  The RAM test
  complements each byte from `$4200` upward, which is why a writable
  mirror would break the size check.
* **Interrupt vectors:** RAM hooks `$4200-$4212`.

## 4. Building and validating the core

The first boot reached `MICROCOLOR BASIC 1.0 / COPYRIGHT 1982 MICROSOFT /
OK`.  The CPU was then checked through BASIC's own floating-point code:

* `PRINT 2+3*4,SIN(1),SQR(2)` gave `14  .841470985  1.41421356`.
* CLOAD of archive tapes, CSAVE → CLOAD round trips, LLIST to file, and
  graphics modes were checked with screenshots of the framebuffer.
* SOUND was measured with an FFT-free zero-crossing count on the emulated
  audio: `SOUND 100,5` came out at 258 Hz, exactly what the ROM's loop
  timing predicts.

## 5. The internal 8K mod

The mod is Dennis Bathory-Kitsz's piggyback upgrade from *Color Computer
Magazine*, September 1983, and Ed Snider's simpler modern version.  It
puts 8K of RAM at `$4000-$5FFF`, so the VDG's address mask widens from
`$0FFF` to `$1FFF`.  A test pattern with a band at line 128 confirmed both
behaviours: stock (the bottom third repeats the top) and modded (full
256x192).  BASIC's memory size went from 3142 to 7238 bytes, exactly 4096
more.

## 6. MCX-128: from MAME to the GAL equations

The first implementation followed MAME's `mcx128` device (Tim Lindner,
BSD-3-Clause): 8 memory "views", writes mirrored into the internal RAM for
the VDG, and registers at `$BF00/$BF01`.  It booted Atkinson's menu at
once.  MCX BASIC 2.1 reported 28,703 bytes free (standard) and 47,454
(LARGE), and its graphics commands worked.

Then Darren Atkinson's **MCX128 Essentials** package (published with his
project files) provided the actual GAL22v10 source, `MCX128.pld`, and the
emulation was rewritten to follow it exactly:

* **Registers:** decoded across all of `$BF00-$BF7F`, even addresses for
  the bank register and odd for the ROM map.  The keyboard and VDG move to
  `$BF80-$BFFF`.
* **Internal RAM:** while P1 = 0, the MC-10's own RAM serves CPU reads and
  writes at `$4000-$4FFF` (or `$5FFF` with the 8K jedec file), rather than
  a write mirror.
* **SRAM addressing:** the SRAM uses the CPU address directly, with A16
  from P0 (page 0 and `$C000-$FEFF`) or P1 (`$4000-$BFFF`).  `$FF00-$FFFF`
  is always A16 = 0, so the vectors are shared.

Both models gave identical results on every test, which cross-validates
them.

## 7. MCX32-SD: reverse engineering the firmware

The MCX32-SD (2020) has a user guide but **no published hardware
documentation**: no memory map, no register list, no description of the
protocol between the 6803 and the board's ATmega328P SD controller.  Its
owner couldn't run tests on real hardware during development.  Everything
below was deduced from the firmware alone, then confirmed by MCX BASIC
3.1 itself running in the emulator.

### 7.1 Getting the firmware

Darren Atkinson's public "MCX Basic 3.1 Update" contains an installer,
`MCXBASIC 3.1` (57,344 bytes = seven 8K blocks):

* **Block 0** is a tokenized BASIC program, saved as a DECB binary at
  `$4345`.  It reads the rest of its own file and programs the flash
  through the BIOS (`EXEC &H2003`, parameter block at `$0120`).
* **Blocks 1-5** are flash banks 1-5.
* **Block 6** is **bank 0, the BIOS**, programmed last.

Two features identify each block's purpose: where most of its `JSR`/`JMP`
targets point, and its signatures.  Banks 1 and 4 are `$C000` code
("MX4K", "MX32"), and banks 2, 3 and 5 are `$E000` code with vector tables
("MX10", "MXA4", "MX90").  Bank 0 is `$2000` code, and its last two bytes
are `$2B00`: a reset vector.
`tools/make_mcx32sd_flash.py` reassembles the 128K image.

### 7.2 Registers

The BIOS reset code at `$2B00` revealed the register set:

* **`$06`/`$07` (flash bank selects):** it does `LDD #$0102 / STD $06`,
  then checks `$C000` for "MX4K" and `$E045` for "MX10", so `$06` selects
  the `$C000` bank and `$07` the `$E000` bank.
* **`$07` = 0 means the internal ROM:** with `$07` = 0 the code reads
  internal-ROM signatures at `$F7CB` and `$F9C4` to tell an MC-10 from an
  Alice 4K or an Alice 32/90.
* **CONTROL bypass:** holding CONTROL (port 1 = `$FE`, port 2 bit 1) at
  reset clears both bank registers and jumps through the internal reset
  vector, giving plain MicroColor BASIC.
* **`$0F` (mode register):** written `$30` on an Alice 32/90 and 0 on an
  MC-10.  The serial routines test bit 4 to choose the Alice's 1 MHz
  timings.
* **`$04`/`$05` (SD mailbox):** `$04` is polled with `BITA` masks `$81`,
  `$82` and `$85`, and `$05` carries data.  This is the byte mailbox to
  the ATmega.
* **`$1D` (flash write enable):** written (`$F1`) before each flash
  command byte.
* **`$1E`/`$1F` (remap off/on):** PEEK@ and POKE@ write `$1E` just before
  their access; MCX BASIC writes `$1F` at idle points.

None of these addresses reached the cartridge at first, because the 6803
core treated all of `$00-$1F` as internal (see [section 9](#9-bugs-found-along-the-way)).

### 7.3 Memory map: calibrated by `PRINT MEM`

* **Low SRAM:** the BIOS clears `$0020-$1FFF`, so there is RAM there.
* **Upper SRAM:** BASIC sizes memory upward from `$4200`.  With SRAM
  assumed at `$4000-$9FFF`, MCX BASIC reported **23,522** bytes free.  The
  user guide says a freshly booted MC-10 shows **27,618**, exactly 4096
  more.
* **The answer:** SRAM is `$0000-$1FFF` plus `$5000-$AFFF` (8K + 24K =
  32K, the board's size), with the MC-10's own 4K at `$4000-$4FFF`.  The
  result: **27,618**.

### 7.4 The remap range: deduced from a failing SAVE

The guide says the hardware maps `$4200-$43FF` onto MCX BASIC's relocated
variables.  But with that range, SAVE wrote a file of zeros.  Tracing the
remap register showed MCX BASIC turns the remap **off** (`$1E` at `$C37B`)
around SAVE.

TXTTAB is `$4346`.  If `$4346-$43FF` were remapped, the start of the
program would sit in low SRAM while typed (remap on) and be read from
video RAM during SAVE (remap off).  So the remap must stop at **`$4345`**,
exactly the end of MicroColor BASIC's old system-variable area.  MCX
BASIC's own variables end at `$0345`, which confirms it.

### 7.5 The flash chip: identified by watching KILL MEM

The first model was an AMD 29F010 with 16K sectors.  Tracing `KILL MEM @6`
showed the BIOS issuing **two** sector-erase commands, at bank offsets 0
and `$1000`.  So the chip has 4K sectors: an **SST39SF010**.  With 16K
sectors, erasing bank 6 would also have wiped bank 7.

### 7.6 The SD controller protocol

A focused analysis of the BIOS's 16 SD functions (dispatch table at
`$2452`) produced the full mailbox protocol:

* **Command prefixes:** `$24` path commands, `$25` handle commands, `$2A`,
  and `$23` bootloader commands.
* **Parameter block layout.**
* **Status-bit handshakes and timeouts**, for example: the first byte of
  every command is written without waiting for bit 1, bit 2 seen during a
  byte wait means failure, and bit 7 must come after all data.
* **Reply formats:** among them the 5-byte DIR records (FAT attribute,
  32-bit big-endian size, NUL-terminated name).

It's all in [MCX32-SD_protocol.md](MCX32-SD_protocol.md).

The protocol is close to Atkinson's *documented* Emcee serial protocol:
the same command letters (L, S, O, D, C, ...) and the same error codes.
That document, provided later by the project owner, confirmed the
reading.

The decisive finding: **the controller, not the BIOS, owns the file
formats.**

* LOAD expects a tokenized program image, which BASIC relinks.
* SAVE sends raw memory.
* LOADM expects a CoCo DECB stream, which the BIOS parses.

So the emulated controller must tokenize ASCII `.BAS`, extract programs
from `.C10`/`.WAV`, and convert back on SAVE according to the extension.

### 7.7 The tokenizer: proven byte-identical

MCX BASIC's crunch routine (`$E4B6` and the code before it) has special
cases: `'` → `:` `$DD`, ELSE → `:` `$C9`, and `?` → PRINT.  The test:

1. type a program into the emulated MCX BASIC and `SAVE "TYPED.C10"`, so
   BASIC's own tokenizer produces the image;
2. load the same text from `.BAS` through the emulated controller, and
   `SAVE "MINE.C10"`;
3. compare the data blocks.

They are byte-identical, link pointers included.  This is part of the
regression suite.

### 7.8 Things that remain inferred

These are listed with code addresses in the protocol document's open
questions:

* when OPEN "O" truncates a file (the emulator does it at the first write
  or at close);
* the meaning of the unused commands `$2A`, 'Z' and 'T';
* the default extension for LOAD SCREEN (`.SCR` is assumed).

`tools/MCXPROBE.BAS` is ready to check the memory map on real hardware.

## 8. The Emcee serial link

MCX BASIC talks to an Emcee server by bit-banging **38400 baud** through
the MC-10's serial port: TxD on port 2 bit 0, RxD on bit 2.  That's 23.3
CPU cycles per bit.

* **The serial line:** rather than trapping ROM routines, the emulator
  reproduces the line itself.  Transmitted bits are decoded from the
  cycle timestamps of TxD edges, and received bytes are replayed onto RxD
  in CPU time, so the ROM's own loops do the work.
* **Pins and polarity:** confirmed from the BIOS routines at `$2154` (TX,
  `ASLB`/`STAB $03`) and `$2083` (RX, `LSRA LSRA LSRD` sampling bit 2).
* **Built-in server:** follows Atkinson's documented filing protocol.
  pyDriveWire's EmCee source supplied the undocumented print command (`P`)
  and the detail that OPEN for input answers only a status byte.
* **External servers:** a TCP client is provided, and was tested against a
  minimal stand-in server.

While investigating pyDriveWire, two bugs in it were found by reading its
source:

* **SETDIR** stores the name without combining it with the current folder.
* **DIR** lists the folder from before it changes directory.

Together they explain the "one wrong listing, then `?NE ERROR` forever"
behaviour seen with it.

## 9. Bugs found along the way

Some of these were in early versions of this emulator.  They are recorded
because they are instructive.

| Bug | Symptom | How found | Fix |
|---|---|---|---|
| 6803 external register holes | MCX32-SD hung in the BIOS RAM clear: writes to `$06`/`$07` never reached the board, so the reset vector stayed in the BIOS | Instruction trace up to the second reset | `$04-$07`, `$0F` and `$15-$1F` go to the external bus |
| VDG GM bit order reversed | Every graphics mode except RG6/RG3 was wrong; MCX BASIC PMODE 1 drew garbage | PMODE 1 wrote `$26` to `$BFFF`, which only makes sense with GM0 in bit 4 | GM0 = bit 4, GM1 = bit 3, GM2 = bit 2 |
| Writable 4K mirror | A 4K MC-10 reported 15K | BASIC's complement-and-compare RAM test | Read-only mirror, as in VMC10's handler table |
| CSAVE first bit | Saved images shifted by one bit | The idle-high line made the first "1" a non-edge | Time each bit from the last write of 1 |
| Serial seen as tape | MCX LLIST created a bogus `CSAVE.C10` | Port 2 bit 0 is shared by cassette, printer and serial | A recording must contain `$55 $3C` |
| Flash sector size | KILL MEM would erase the neighbouring bank | Trace of erase commands | SST39SF010, 4K sectors |
| Remap range | SAVE of a program wrote zeros | Trace of `$1E`/`$1F` around SAVE | Remap `$4200-$4345` only |
| Detokenizer buffer | Tiny programs SAVEd as empty `.BAS` | Emcee SAVE test | Larger output headroom |
| Keyscan hook signature | MCX BASIC 3's routine (`JSR $02A9`) wasn't recognised; typing on the MCX32-SD silently used the slow fallback | Reviewing the hook while writing documentation | Accept `$42A9`, `$01A9` and `$02A9` |
| Typing fallback losing keys | Quicktype dropped characters while BASIC was busy | Regression tests (LPRINT, Quicktype) | Fall back only while software reads the matrix |

Not every report was an emulator bug.  The DOttery games crash on the
MCX-128 because they call the **MCX32-SD BIOS** at `$2000`, to load their
hall-of-fame files such as `DEFHOF.DAT`.  They were written for that
board.

## 10. Tools and techniques

* **objdump and Python** for the PE file: sections, imports, strings,
  locating embedded images by content, and a pattern scanner to find the
  font.
* **A small 6803 disassembler** (`m6803_disasm`, also in the debugger)
  run over raw flash banks.
* **A scriptable headless harness** (`tests/headless.c`):
  * `type:`, `tape:`, `screen`, `ppm:`;
  * `until:ADDR` and `until-illegal` (single-step and print the last
    instructions executed);
  * `bp:`, `mem:`, `sum:` (memory checksums, for comparing load paths);
  * `poke:`, `csavem:`, `flash:`.

  Most of the findings above came from these.
* **Trace switches** (`VMC10_TRACE_SD`, `VMC10_TRACE_SERIAL`,
  `VMC10_TRACE_ILLEGAL`).
* **Framebuffer screenshots** compared side by side (stock vs 8K, MCX-128
  vs MCX32-SD).
* **Measurement against documented numbers:** `PRINT MEM` values from the
  user guides, the SOUND formula, and MAME's ROM checksums.
