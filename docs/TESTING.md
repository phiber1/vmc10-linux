# VMC10 for Linux: Testing

This document describes how the emulator is tested, every automated test
and what it proves, the manual verification done during development, and,
just as important, what has **not** been verified.

## Running the tests

```
make test
```

This builds `build/headless`, a scriptable, display-less build of the
emulator core, and runs `tests/run_tests.sh`:

* **ROMs:** the tests use the ROMs in `roms/` (see `roms/README.md`).
  Without `MC10.BIN` nothing can run, and the script says so.  The MCX-128
  and MCX32-SD groups run only when `mcx128bas.rom` / `mcx32sd_flash.bin`
  are present, and are reported as `SKIP` otherwise.
* **Fixtures:** no third-party software is needed.  The test programs are
  made at the start of each run from `tests/fixtures/`, by the emulator
  itself:

| Fixture | Made by | Purpose |
|---|---|---|
| `PROG.C10` | Quicktyping `TESTPROG.BAS`, then `CSAVE` | BASIC tape image (tests the cassette output decoder) |
| `PROG.WAV` | Converting `PROG.C10` | WAV loading |
| `TIMER.C10` | Hand-assembled 6803 code, then CSAVEM | Installs an output-compare interrupt handler through BASIC's `$4206` hook that increments the first screen byte |
| `SHOW.C10` | Hand-assembled code, then CSAVEM | Stores `$13` at `$4000` and returns (LOADM/EXEC check) |
| `TESTPROG.BAS` | (in the repo) | Stock MicroColor BASIC: FOR/NEXT, IF/THEN, READ/DATA, line 1000 |
| `MCXTOKENS.BAS` | (in the repo) | MCX BASIC syntax: `&H`, ELSE, `'`, `?`, DATA with quoted commas and keyword text |

A full run takes about two minutes and needs no display or audio device.

### Result on the reference system

With all three ROM images present: **53 passed, 0 failed, 0 skipped**
(Linux Mint 22.3, gcc 13, SDL 2.30).  With only `MC10.BIN`: 18 passed, 2
groups skipped.

## The headless harness

`tests/headless.c` runs the same core as the GUI, controlled by command
words on its command line, which are executed in order:

```
build/headless ROM [ram:4|20] [mcx128:FILE] [mcx32sd:FILE] [slowtype] [8k] [sd:DIR] COMMANDS...
```

| Command | Action |
|---|---|
| `frames:N` | Run N video frames (N/59.92 seconds of emulated time) |
| `type:TEXT` | Queue text for typing (`\n` = ENTER) |
| `typefile:FILE` | Queue a file for typing (Quicktype) |
| `until-typed` | Run until the typing queue is empty |
| `tape:FILE` | Insert a cassette image |
| `until-tape` | Run until the tape has been read to the end (30,000 frames at most) |
| `screen` | Print the 32x16 text screen (inverse = upper case, normal = lower case, semigraphics = `#`) |
| `ppm:FILE` | Save the 320x240 framebuffer as a PPM |
| `regs` | Print the VDG latch and CPU registers, plus the undefined-opcode count |
| `prn:FILE` / `out:DIR` | Printer file / CSAVE folder |
| `audio:FILE` | Dump the following frames' audio as raw signed 16-bit samples |
| `vdg:XX` | Set the VDG latch, fill 6K of video RAM with a test pattern, render one frame |
| `mem:ADDR,N` | Hex dump through the CPU bus (with the current bank mapping) |
| `sum:ADDR,LEN` | Checksum of memory, for comparing two load paths |
| `poke:ADDR,HEX...` | Write bytes |
| `csavem:FILE,NAME,START,END,EXEC` | Save memory as a machine-code `.C10` |
| `c10wav:IN,OUT` | Convert a cassette image to WAV |
| `flash:OFFSET,N` | Dump MCX32-SD flash bytes |
| `serial:builtin` / `serial:HOST:PORT` | Connect the serial port to the built-in Emcee server / a TCP server |
| `bp:ADDR` | Run until PC = ADDR (typing keeps working) |
| `until:ADDR[,SKIP]` | Single-step until PC = ADDR and print the last 40 instructions |
| `until-illegal` | Single-step until an undefined opcode runs and print the last 60 instructions, with the stack pointer and MCX bank registers |

Example: find out why a game crashes.

```
build/headless roms/MC10.BIN mcx128:roms/mcx128bas.rom sd:~/games serial:builtin \
    frames:100 'type:1' frames:150 'type:LOADM "GAME"\n' until-typed frames:600 \
    'type:EXEC\n' until-typed until-illegal
```

## The automated tests

Each test runs the emulator from power-on, types commands as a user would,
and checks the text screen, files written, memory, flash, or rendered
pixels.

### Core machine (needs `MC10.BIN`)

| Test | What it proves |
|---|---|
| boot to MicroColor BASIC | ROM mapping, reset vector, 6803 port set-up, video text mode, the ROM's RAM test |
| BASIC arithmetic | `PRINT 2+3*4;SQR(2)` gives `14  1.41421356`: hundreds of thousands of instructions through BASIC's floating-point code, exercising arithmetic flags, MUL, the shifts and DAA paths |
| 20K RAM (MEM) | `PRINT MEM` = 19526 with the Tandy 16K pack |
| 4K RAM (MEM) | `PRINT MEM` = 3142: the read-only `$5000-$7FFF` mirror defeats BASIC's RAM test, as on real hardware |
| Quicktype + RUN | Typing a file through the ROM keyscan hook; FOR/NEXT and IF/THEN execute correctly |
| READ/DATA after Quicktype | DATA statements typed in are tokenized and read back |
| keyboard-matrix typing | The fallback path: keys (including SHIFT for `"`) pressed on the emulated matrix are decoded by the ROM's scanner |
| CSAVE writes a .C10 | The cassette output decoder turns the ROM's square wave back into a valid C10 file named from the header |
| CLOAD of the CSAVEd image | The cassette input encoder plus the ROM's tape reader, with LIST of the last line |
| CSAVE -> CLOAD -> RUN | The full round trip runs |
| C10 -> WAV and CLOAD from WAV | WAV writer and WAV reader (DC correction, hysteresis), read by the real ROM routine |
| LLIST / LPRINT to the printer file | The 600-baud printer decoder |
| CLOADM + EXEC, timer interrupt | Machine-code tape loading; EXEC; **the 6803 output-compare interrupt**, vectored through BASIC's `$4206` hook, firing repeatedly (the screen byte keeps changing) |
| SOUND 100,5 pitch | Zero crossings in the emulated audio give 258 Hz (250-266 accepted), matching the ROM's loop timing: verifies the CPU cycle table and the speaker sampling |

### Internal 8K mod

| Test | What it proves |
|---|---|
| internal 8K RAM mod (MEM) | `PRINT MEM` = 7238 with `--ram 4 --8k` |
| RG6 repeats with stock 4K | A 6K RG6 test pattern: the bottom line is not the pattern's (the top repeats) |
| RG6 full screen with 8K | The same pattern's bottom line is fully set: the VDG addresses 6K |

### MCX-128 (needs `mcx128bas.rom`)

| Test | What it proves |
|---|---|
| boot menu | EPROM mapped at reset (ROM map mode 0), menu drawn |
| MCX BASIC (standard): 28703 free, `&H7F` = 127 | Option 1, MCX BASIC memory map, hex constants |
| MCX BASIC (large): 47454 free | Option 2: banked RAM with P1, from the GAL equations |
| MicroColor BASIC option: 31558 free | Option 0: internal ROM via ROM map mode 2, with expansion RAM |
| MCX BASIC CLOAD | Cassette through MCX BASIC's ROM in RAM |
| PMODE 1 colour graphics | A circle filled by PAINT: the pixel at the centre is blue (`2020d8`) and the background green: CG3 rendering, the GM bit order, and MCX graphics code |

### MCX32-SD (needs `mcx32sd_flash.bin`)

| Test | What it proves |
|---|---|
| boots MCX BASIC 3.1 | Reset vector from BIOS bank 0, bank registers `$06`/`$07` reachable on the external bus, machine detection |
| memory map: 27618 free | Matches the MCX32-SD user guide exactly: SRAM at `$0000-$1FFF` + `$5000-$AFFF` |
| SD: DIR lists folders / files | Mailbox protocol, DIR records (attribute, size, name), BIOS paging |
| SD: RUN an ASCII .BAS | Path command `L` mode 0, default extensions, the controller's tokenizer |
| SD: DIR= and LOAD a .C10 | Directory change (`C`), `.C10` extraction |
| SD: DIR of a subdirectory | `"0:GAMES/"` lists `<DIR>` entries |
| SD: MCX syntax via the tokenizer | ELSE, `?`, `&H` run correctly after loading from `.BAS` |
| SD: DATA with quoted comma and keyword text | DATA is literal until `:`; quoted commas survive |
| SD: tokenizer byte-identical | The same listing typed into MCX BASIC and loaded from `.BAS` produces identical images (data blocks compared) |
| SD: SAVE .BAS writes plain text | The detokenizer reproduces ELSE and `'` exactly |
| SD: SAVEM/LOADM .BIN round trip | DECB writer, DECB parsing by the BIOS |
| SD: LOADM a .C10 and EXEC it | C10 → DECB conversion; the loaded code runs |
| SD: OPEN O/A/I, PRINT#, LINE INPUT#, EOF | `n`/`o` open, `W` writes, append via `P`, `R` reads with a short read and IE, EOF = -1 at the end |
| SD: KILL of a non-empty directory | Error 52 (AO) propagated to BASIC |
| SD: NEWDIR/RENAME/KILL, missing dir | mkdir `K`, move-rename, delete, then `?NE ERROR` for a missing folder |
| SD: AUTOEXEC | `/MCX32-SD/AUTOEXEC.BAS` runs at power-on |
| flash: WRITE MEM / KILL MEM / bank 0 protected | SST39SF010 command sequences issued by MCX BASIC's own RAM-resident code; 4K sector erase; `?FC ERROR` for bank 0 |
| Emcee device 1: under MCX BASIC 3.1 | `LOAD "1:TESTPROG"`: the bit-level serial line and the built-in server |
| Emcee: DIR shows subdirectories | Folders listed as `ARCADE/` |

### Emcee on the MCX-128 (needs `mcx128bas.rom`)

| Test | What it proves |
|---|---|
| DIR over the 38400 baud line | `!F` / `!$` exchanges decoded from TxD edges and replayed onto RxD with exact bit timing |
| RUN an ASCII .BAS | `!L` + `!G`/`!N` blocks, tokenized with **MCX BASIC 2.1's** token table (found in the EPROM) |
| SETDIR and LOAD a .C10 | `!C` |
| SAVE writes plain text | `!S` + `!W` blocks, detokenized |
| SAVEM writes a DECB binary | Mode 2 with the load address in the size field |
| OPEN O / PRINT# | Data-file output |
| LLIST prints through the server | The `!P` print command into the printer file |

## Manual verification during development

These checks were done by hand, by looking at the results:

* **Real SDL window:** run on X11 and captured with `xwd`.  A tape was
  inserted from the command line, `CLOAD` typed, and auto-warp loaded it
  in a few seconds.
* **Screenshots of every video mode:** text, semigraphics-4 colour bars,
  RG6 with and without the 8K mod, MCX BASIC PMODE 0/1/2 drawings (LINE,
  CIRCLE, PAINT, DRAW), and MCX BASIC 3 PMODE 4 at full 256x192 with
  `--8k`.
* **Debugger:** commands piped through stdin (`r`, `d`, `m`, `b`, `c`).
  The breakpoint was hit and reported.
* **External Emcee over TCP:** `--serial` against a minimal stand-in
  server: the emulator connected, MCX BASIC's DIR went over the wire, and
  the reply was listed.
* **All 51 cassette images of the VMC10 archive** loaded over the built-in
  Emcee server on the MCX-128 (LOAD for BASIC, LOADM for machine code)
  with no errors.
* **Third-party games:**
  * GregD's Pac-Man loads byte-identically through CLOADM and Emcee LOADM.
  * The five DOttery games (DEFENDER, DIGDUG, GALAGOR, SPATROL, FROGLER)
    run on the MCX32-SD.
  * On the MCX-128, the ones that call the MCX32-SD BIOS crash, as they
    would on real hardware (see REVERSE_ENGINEERING.md).
* **CLOAD of VMC10 archive tapes:** HANGMAN, HIRESCOL, BLOCK DROP, CLOCK.
  The clock's interrupt-driven display kept running alongside BASIC.
* **Sound pitch measurement** (also automated).
* **Missing ROMs:** with an empty `roms/`, the emulator prints the file
  name and points to `roms/README.md`, and the tests skip cleanly.

## Not verified

To be honest about the limits:

* **Real hardware:** nothing has been compared against a real MC-10,
  MCX-128 or MCX32-SD.  The MCX32-SD model rests on firmware analysis and
  self-consistency (the guide's `PRINT MEM`, the tokenizer identity,
  MCX BASIC's own behaviour).  `tools/MCXPROBE.BAS` is ready to check the
  memory map on a real board.
* **Listening:** audio quality hasn't been judged by ear, only its pitch
  measured.
* **Untried interfaces:** gamepads, the zenity/kdialog file dialogs,
  full-screen mode and Wayland sessions.
* **pyDriveWire:** not run against `--serial` (running it was outside this
  environment).  A real Emcee server hasn't been tried either.
* **Real recordings:** WAV files of real tapes (only WAVs produced by the
  emulator were tested), and noisy or weak recordings.
* **Alice:** the Alice ROM boots and types through the matrix, but its
  AZERTY keyboard and Alice-specific software are untested.
* **MCX32-SD on an Alice:** the Alice 32/90 paths of the MCX32-SD firmware
  (banks 4/5, `$0F` = `$30`) are not emulated; the Alice 32/90 has
  different video hardware.
* **Emcee details:** the built-in server's DIR LIST semantics follow the
  document only loosely, since real servers differ.
* **Rarely used SD commands:** 'Z' and 'T' are implemented on a best guess
  (see the protocol document), and BASIC never uses them.

Reports from real-hardware owners are very welcome: run
`tools/MCXPROBE.BAS` on an MCX32-SD and compare `PROBE.TXT` with the
emulator's memory map.
