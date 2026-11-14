# VMC10 for Linux: Technical Reference

This document describes the emulated hardware in detail, and exactly how
the emulator models each part.  Source file references are to `src/`.

## Contents

1. [Architecture](#1-architecture)
2. [Timing](#2-timing)
3. [The 6803 CPU](#3-the-6803-cpu)
4. [Memory maps](#4-memory-maps)
5. [I/O: ports and the `$BFFF` latch](#5-io-ports-and-the-bfff-latch)
6. [Keyboard matrix](#6-keyboard-matrix)
7. [MC6847 video](#7-mc6847-video)
8. [Sound](#8-sound)
9. [Cassette](#9-cassette)
10. [Printer](#10-printer)
11. [Serial port](#11-serial-port)
12. [Typing: ROM hook and matrix fallback](#12-typing-rom-hook-and-matrix-fallback)
13. [MCX-128](#13-mcx-128)
14. [MCX32-SD](#14-mcx32-sd)
15. [Emcee protocol and the built-in server](#15-emcee-protocol-and-the-built-in-server)
16. [File formats](#16-file-formats)
17. [The BASIC tokenizer](#17-the-basic-tokenizer)
18. [Source map](#18-source-map)

---

## 1. Architecture

```
            +-------------------------- main.c (SDL2) ------------------------+
            |  window/texture, audio queue, input, hotkeys, dialogs, settings |
            +-------------------------------+----------------------------------+
                                            | mc10_run_frame(), fb[], audio[]
+------------------------------------------ mc10.c --------------------------------------------+
|  memory map | keyboard matrix | VDG renderer (per scan line) | speaker sampler | typing queue |
|  printer decoder | cassette in/out glue | ROM hook detection                                 |
+---------+-------------+---------------+---------------+----------------+-----------------------+
          |             |               |               |                |
      m6803.c        tape.c        mcx128.c        mcx32sd.c         serial.c
   CPU + timer    C10/WAV in,     GAL-exact       regs, flash,      38400 baud line,
   + port regs    CSAVE decode,   bank logic      remap, SRAM       TCP client
                  WAV writer                          |                 |
                                                   sdcard.c  <-----------+
                                       SD controller (ATmega) emulation +
                                       built-in Emcee server, tokenizer,
                                       format conversion, host-folder FS
```

The core (everything except `main.c` and `debugger.c`) has no SDL
dependency.  The headless test harness (`tests/headless.c`) links the same
core.

## 2. Timing

* **CPU clock:** 3.579545 MHz / 4 = **894,886 Hz** (E clock).
* **Video:** 262 lines x **57 CPU cycles** per line = **14,934 cycles per
  frame**, giving 59.92 Hz.
* **Frame layout:** the emulator renders a line each time 57 cycles have
  elapsed:

| Lines | Framebuffer rows | Content |
|---|---|---|
| 0-23 | 0-23 | top border |
| 24-215 | 24-215 | active display (192 lines) |
| 216-239 | 216-239 | bottom border |
| 240-261 | - | vertical blanking (not displayed) |

* **Mid-screen changes:** the VDG mode latch and video RAM are read when
  each line is rendered, so mode or colour-set changes part-way down the
  screen appear where they happen.  The original VMC10 didn't support
  this.
* **Pacing:** the frontend runs one frame, queues its audio (about 735
  samples at 44.1 kHz), and waits while more than about 50 ms of audio is
  queued.  Without audio it paces on `SDL_GetPerformanceCounter`.
* **Warp:** F7, tape reading, or typing without the ROM hook makes the
  frontend run frames for up to 14 ms of real time per displayed frame
  (at most 200 frames), and drop the audio.

## 3. The 6803 CPU

`m6803.c` implements the Motorola MC6801/6803 from the data sheet.

### Registers and flags

A, B (D = A:B), X, SP, PC, CC = `11HINZVC`.  Undocumented opcodes execute
as 2-cycle NOPs and are counted (`illegal_count`, `last_illegal`).  Set
`VMC10_TRACE_ILLEGAL=1` to log them.

### Instruction set

All documented 6800 instructions, plus the 6801 additions:

* `ABX`, `ADDD`, `SUBD`, `LDD`, `STD`;
* `ASLD`/`LSLD`, `LSRD`, `MUL`;
* `PSHX`, `PULX`, `BRN`, `JSR` direct;
* `CPX` setting all flags.

Flag behaviour follows the data sheet:

* H on 8-bit add;
* V for NEG of `$80`, DEC of `$80`, INC of `$7F`;
* V = N xor C after shifts and rotates;
* C on MUL = bit 7 of B;
* DAA with the 6800 correction rules.

### Cycle counts (6803 table)

```
      0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F
 0:   2  2  -  -  3  3  2  2  3  3  2  2  2  2  2  2
 1:   2  2  -  -  -  -  2  2  -  2  -  2  -  -  -  -
 2:   3  3  3  3  3  3  3  3  3  3  3  3  3  3  3  3
 3:   3  3  4  4  3  3  3  3  5  5  3 10  4 10  9 12
 4:   2  -  -  2  2  -  2  2  2  2  2  -  2  2  -  2
 5:   2  -  -  2  2  -  2  2  2  2  2  -  2  2  -  2
 6:   6  -  -  6  6  -  6  6  6  6  6  -  6  6  3  6
 7:   6  -  -  6  6  -  6  6  6  6  6  -  6  6  3  6
 8:   2  2  2  4  2  2  2  -  2  2  2  2  4  6  3  -
 9:   3  3  3  5  3  3  3  3  3  3  3  3  5  5  4  4
 A:   4  4  4  6  4  4  4  4  4  4  4  4  6  6  5  5
 B:   4  4  4  6  4  4  4  4  4  4  4  4  6  6  5  5
 C:   2  2  2  4  2  2  2  -  2  2  2  2  3  -  3  -
 D:   3  3  3  5  3  3  3  3  3  3  3  3  4  4  4  4
 E:   4  4  4  6  4  4  4  4  4  4  4  4  5  5  5  5
 F:   4  4  4  6  4  4  4  4  4  4  4  4  5  5  5  5
```

Interrupt entry (stacking all registers) costs 12 cycles.  WAI stacks
immediately and then idles one cycle at a time until an interrupt arrives.

### On-chip registers (`$00-$1F`)

| Address | Register | Notes |
|---|---|---|
| `$00` / `$01` | DDR1 / DDR2 | data direction, 1 = output |
| `$02` / `$03` | Port 1 / Port 2 data | read = (output & DDR) or (pins & ~DDR); port 2 has 5 lines, bits 5-7 read from the pins |
| `$08` | TCSR | ICF OCF TOF EICI EOCI ETOI IEDG OLVL |
| `$09-$0A` | free-running counter | a write to `$09` sets the counter to `$FFF8` (6801 behaviour) |
| `$0B-$0C` | output compare | |
| `$0D-$0E` | input capture | (not driven) |
| `$10-$13` | SCI | inert; TDRE always reads 1 |
| `$14` | RAM control | stored |
| **`$04-$07`, `$0F`, `$15-$1F`** | **external** | Not decoded on chip.  On a 6803 these go out to the expansion bus, and the MCX boards use them. |

The external-register rule matters: VMC10's original core, and early
versions of this one, swallowed these addresses, which made the MCX32-SD's
registers unreachable.

### Timer

The counter advances by each instruction's cycle count:

* **OCF** is set when the counter passes the output-compare value.
* **TOF** is set when the counter wraps.
* **Clearing:** OCF is cleared by reading TCSR (with OCF set) and then
  writing the output-compare register.  TOF is cleared by reading TCSR and
  then the counter high byte.

### Interrupts

Interrupts are checked between instructions, in priority order:

| Priority | Source | Vector |
|---|---|---|
| 1 | NMI | `$FFFC` |
| 2 | IRQ1 (external) | `$FFF8` |
| 3 | ICF (with EICI) | `$FFF6` |
| 4 | OCF (with EOCI) | `$FFF4` |
| 5 | TOF (with ETOI) | `$FFF2` |

Everything after NMI is masked by I.  BASIC's vectors point into a RAM
jump table:

| Interrupt | Hook |
|---|---|
| SCI | `$4200` |
| TOF | `$4203` |
| OCF | `$4206` |
| ICF | `$4209` |
| IRQ | `$420C` |
| SWI | `$420F` |
| NMI | `$4212` |

The SOUND command uses the OCF flag (by polling), and some programs (such
as clocks) use OCF interrupts.  The regression test installs an OCF
interrupt handler through `$4206`.

## 4. Memory maps

### Stock MC-10 (`mc10.c`)

| Range | Contents |
|---|---|
| `$0000-$001F` | 6803 registers (see above) |
| `$0080-$00FF` | 6803 internal RAM (BASIC's direct page) |
| `$4000-$4FFF` | 4K RAM; video memory starts at `$4000` |
| `$5000-$7FFF` | without the 16K pack: reads mirror the 4K RAM, writes are ignored |
| `$5000-$8FFF` | Tandy 16K RAM pack (default `--ram 20`) |
| `$BF00-$BFFF` | write: VDG/sound latch; read: keyboard rows |
| `$C000-$DFFF` | ROM mirror |
| `$E000-$FFFF` | 8K MicroColor BASIC ROM |
| everything else | unconnected, reads `$FF` |

The read-only mirror comes from the original VMC10, whose memory handler
table installs a different handler for writes there.  It's also what makes
BASIC size memory correctly: its RAM test complements each byte and checks
the result, so a writable mirror would make a 4K machine report 15K.

### Internal 8K mod (`--8k`)

`$4000-$5FFF` is on-board RAM.  The VDG address mask becomes `$1FFF`
instead of `$0FFF`, so all 6144 bytes of a CG6/RG6 screen are displayed.
With the 16K pack as well, `$4000-$8FFF` is one continuous RAM area.

### MCX-128

See [section 13](#13-mcx-128).

### MCX32-SD

See [section 14](#14-mcx32-sd).

## 5. I/O: ports and the `$BFFF` latch

### Port 1 (`$02`): keyboard column strobe

A column is selected when its line is driven low: **DDR1 bit = 1 and data
bit = 0**.  Both normal scanning (walking a 0 through the data register)
and the "walking 1s through the DDR" trick used by some programs work.

### Port 2 (`$03`)

| Bit | Direction | Function |
|---|---|---|
| 0 | out | Cassette output / printer / RS-232 TxD (idle high) |
| 1 | in | Keyboard row 6: CONTROL (column 0), BREAK (column 2), SHIFT (column 7), active low |
| 2 | in | RS-232 RxD.  The printer routine also reads it as "busy".  With nothing connected it reads 0 ("printer ready"); with a serial peer it follows the received data (idle 1). |
| 3 | in | reads 1 |
| 4 | in | Cassette input |

### `$BF00-$BFFF`

**Read:** keyboard rows 0-5 for the selected columns, active low (bits 6-7
read 1).

**Write:** the VDG mode and sound latch:

| Bit | Function |
|---|---|
| 7 | Speaker (1-bit sound) |
| 6 | CSS: colour set select |
| 5 | A/G: alphanumeric (0) or graphics (1) |
| 4 | **GM0** |
| 3 | **GM1** |
| 2 | **GM2**, also wired to the VDG's INT/EXT input |

The GM bit order is easy to get wrong.  Modes `111` (RG6) and `101` (RG3)
read the same either way, so a reversed decoder looks fine in the
highest-resolution mode.  MCX BASIC's PMODE 1, which writes `$26`, exposed
it.

## 6. Keyboard matrix

Key index = row x 8 + column.  Rows 0-5 are read at `$BFFF`; row 6 is port
2 bit 1.

| Row \ Col | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| 0 | @ | A | B | C | D | E | F | G |
| 1 | H | I | J | K | L | M | N | O |
| 2 | P | Q | R | S | T | U | V | W |
| 3 | X | Y | Z | - | - | - | ENTER | SPACE |
| 4 | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
| 5 | 8 | 9 | : | ; | , | - | . | / |
| 6 | CONTROL | - | BREAK | - | - | - | - | SHIFT |

SHIFT with the number row gives `! " # $ % & ' ( ) * + < = > ?` (SHIFT-0
gives nothing).  CONTROL with a letter types a BASIC keyword token;
CONTROL-A is backspace.

## 7. MC6847 video

`render_line()` in `mc10.c` draws one 320-pixel line from the current
latch value and video RAM.  The VDG always reads the MC-10's internal RAM
at `$4000`: masked to 4K on a stock machine, 8K with the mod.

### Alphanumeric mode (A/G = 0)

32 x 16 characters, each 8 x 12 pixels, from `$4000-$41FF`.  Each byte:

| Byte | Meaning |
|---|---|
| bit 7 = 0 | Character: bits 0-5 are the code (64 characters); bit 6 = INV (inverse).  Character pixels use the foreground colour, the background the dark one.  MC-10 BASIC stores normal upper-case text with bit 6 set, so it appears dark on bright green; "lower case" is the non-inverted form. |
| bit 7 = 1, GM2 (INT/EXT) = 0 | **Semigraphics 4:** bits 0-3 are the quadrants (bit 3 top-left, 2 top-right, 1 bottom-left, 0 bottom-right), bits 4-6 the colour, black background |
| bit 7 = 1, GM2 = 1 | **Semigraphics 6:** bits 0-5 are the elements (2 x 3, bit 5 top-left ... bit 0 bottom-right), bits 6-7 plus CSS x 4 select the colour |
| bit 7 = 0, GM2 = 1, `--chrx` loaded | External character ROM: 16 bytes per character, code `& $7F` |

Colours:

| CSS | Foreground | Background |
|---|---|---|
| 0 | green | dark green |
| 1 | orange | dark orange |

The border is black in alphanumeric and semigraphics modes.

### Graphics modes (A/G = 1)

| GM2..0 | Mode | Resolution | Colours | Bytes/row | Rows | Bytes |
|---|---|---|---|---|---|---|
| 000 | CG1 | 64x64 | 4 | 16 | 64 | 1024 |
| 001 | RG1 | 128x64 | 2 | 16 | 64 | 1024 |
| 010 | CG2 | 128x64 | 4 | 32 | 64 | 2048 |
| 011 | RG2 | 128x96 | 2 | 16 | 96 | 1536 |
| 100 | CG3 | 128x96 | 4 | 32 | 96 | 3072 |
| 101 | RG3 | 128x192 | 2 | 16 | 192 | 3072 |
| 110 | CG6 | 128x192 | 4 | 32 | 192 | 6144 |
| 111 | RG6 | 256x192 | 2 | 32 | 192 | 6144 |

Colours:

| Mode type | CSS | Colours | Border |
|---|---|---|---|
| CG | 0 | green, yellow, blue, red | green |
| CG | 1 | buff, cyan, magenta, orange | buff |
| RG | 0 | black / green | green |
| RG | 1 | black / buff | buff |

Each source row is repeated 192 / rows times.  Addresses are
`$4000 + (offset & mask)`, so on a stock 4K MC-10 the bottom third of a 6K
screen repeats bytes `$000-$7FF`, as on real hardware.

### MCX BASIC PMODE to VDG mode

| PMODE | VDG mode |
|---|---|
| 0 | RG2 |
| 1 | CG3 |
| 2 | RG3 |
| 3 | CG6 |
| 4 | RG6 |

### Character set

`font6847.h` holds camennie's 64-character, 12-row 6847 font, extracted
from VMC10.exe at file offset `0x2E404`.  `--chr` replaces it with any
768-byte file.

## 8. Sound

The speaker is latch bit 7.  `audio_run()` integrates the speaker level
over exact CPU cycles into 44.1 kHz samples (about 20.29 cycles per
sample), using fractional box filtering.  Each sample then passes through:

1. a one-pole low-pass filter (coefficient 0.35), approximating the real
   speaker's capacitor coupling and removing harsh aliasing;
2. a DC-blocking high-pass filter (pole 0.995);
3. scaling by volume x 12000, clipped to 16 bits.

The ROM's SOUND loop toggles the speaker every 11 x (256 - pitch) + 21
cycles, and the emulator reproduces the resulting frequencies exactly.

## 9. Cassette

### Encoding (MC-10 standard)

Each bit is **one full square-wave cycle**, least significant bit first:

| Bit | Frequency | Cycle length |
|---|---|---|
| 1 | 2400 Hz | about 373 CPU cycles |
| 0 | 1200 Hz | about 746 CPU cycles |

The ROM measures cycle lengths with a counting loop and compares them with
a threshold (21 counts, stored at `$422C`).

### Recording layout

```
leader:  $55 x 128
block:   $55 $3C <type> <len> <data x len> <checksum> $55
         checksum = (type + len + sum of data) mod 256
types:   $00  header, 15 bytes:
              name (8, space padded), file type (0 BASIC, 2 machine code,
              4 data/array), ASCII flag ($00 binary, $FF ASCII), gap flag,
              exec address (2), load address (2)
         $01  data, up to 255 bytes
         $FF  end of file, length 0
```

A gap with a second leader follows the header block, giving BASIC time to
print `F NAME`.

### Input (`tape.c`)

* **Half-wave list:** an image is turned into a list of half-wave
  durations in CPU cycles.  For `.C10`/`.CAS`/`.K7`, each byte produces
  eight bits of two halves each.  If the image has fewer than 64 leader
  bytes, 128 are added in front.  16 bytes of `$55` are added at the end.
* **WAV:** the input is DC-corrected, and zero crossings are detected with
  hysteresis at 8% of the peak.  Their spacing is converted from samples
  to cycles.
* **Reading:** the input bit is the parity of the current half-wave
  index.
* **"Motor control":** on every read of port 2, the tape advances by the
  cycles elapsed since the previous read, but only if that is less than
  20,000 cycles.  So the tape stops when software stops polling.
* **Auto-warp:** the frontend runs at full speed while the tape has been
  read within the last 20,000 cycles.

### Output (CSAVE)

Writes to port 2 bit 0 are watched; the ROM writes 1 then 0 for every bit.

* **Bit decoding:** each bit is timed from the last write of 1 to the
  following write of 0.  If that half-period is less than 290 cycles, the
  bit is a 1.  The ROM's loops give about 197 and 389 cycles.
* **Byte framing:** a pause of more than 3000 cycles restarts the framing.
* **End of recording:** after one second of silence, the bytes are written
  as `NAME.C10`, but only if they contain a real block (`$55 $3C`).
* **Printer exception:** writes coming from the printer routine are
  excluded (section 10).

WAV output (`--c10-to-wav`) produces 8-bit mono 22050 Hz square waves at
the exact bit timings, with half a second of silence at each end.

## 10. Printer

The ROM's printer routine sends 600-baud serial data on port 2 bit 0:

1. it waits until port 2 bit 2 reads 0;
2. then sends an idle bit, a start bit, 8 data bits (least significant
   first) and a stop bit.

Each bit is a separate `STAB $03` at `$FA0C`.  The emulator watches for
writes made by an instruction at `$FA0C` whose code really is `STAB $03`
(`D7 03`), which is true for stock BASIC and both MCX BASICs.  It
reassembles the bytes and appends them to the printer file (CR becomes a
newline, a following LF is dropped).  These writes are kept out of the
cassette and serial decoders.

## 11. Serial port

MCX BASIC's Emcee driver bit-bangs **38400 baud, 8-N-1**: TxD on port 2
bit 0, RxD on port 2 bit 2, idle high, least significant bit first.  At
894,886 Hz one bit is 23.30 CPU cycles.  The ROM routines' loops are
tuned to about 24 cycles per bit.

**Transmit decoding (`serial.c`):**

1. Every change of TxD is recorded with its CPU cycle.
2. A falling edge starts a frame.
3. Each data bit k is sampled at start + (1.5 + k) x 23.3 cycles, from the
   last level change before that moment.
4. After 9.5 bit times, the byte is complete and is passed on (to TCP, or
   to the built-in server).

**Receive encoding:**

1. Received bytes wait in a queue.
2. When software reads port 2 and a byte is waiting, transmission starts
   one bit time later: start bit (0), 8 data bits, stop bit (1).
3. Further bytes follow back to back, as a real UART sends them.
4. The level at any moment is worked out from the CPU cycle of the read.
   So the ROM's own timing loops see a real waveform, with no special
   cases in the emulator.

**TCP backend:** a non-blocking client socket with `TCP_NODELAY`.  It
reconnects if the server drops, and is polled once per frame.

## 12. Typing: ROM hook and matrix fallback

The typing queue (Quicktype, paste, sensible keyboard mode) is delivered
in one of two ways:

1. **ROM hook.**  When the CPU reaches `$F883`, the ROM's keyscan routine,
   and the code there starts with `BD xx A9` (JSR to the keyscan RAM hook:
   `$42A9` in stock BASIC, `$01A9` in MCX BASIC 2.1, `$02A9` in MCX BASIC
   3), the emulator puts the next character in A, sets the flags like
   `TSTA`, and returns as if from the routine.  The check is made against
   the code mapped in at that moment, so it follows the MCX boards' ROM
   switching.
2. **Matrix fallback.**  The keys are pressed on the emulated matrix (with
   SHIFT/CONTROL as needed), held 2 frames, then released for 2 frames, or
   20 frames after ENTER.  This is used when:
   * the hook doesn't apply, or
   * the hook hasn't fired for 6 frames while software is reading the
     keyboard matrix (`$BFxx`), for example a boot menu or a game.

   While a key is held, the hook is suspended, so the character arrives
   exactly once.  The fallback only starts if software is actually reading
   the matrix, so characters aren't lost while BASIC is busy (printing,
   inserting long lines).

## 13. MCX-128

`mcx128.c` is implemented from the board's GAL22v10 equations
(`MCX128.pld`, ATK Software, 2010).  The board has 128K of SRAM (U3) and a
16K EPROM (U4).

### Address decoding

| Range | Contents |
|---|---|
| `$0000-$3FFF` | SRAM, A16 = P0 (the 6803's own registers and `$80-$FF` RAM take priority for reads; writes to `$80-$FF` also reach the SRAM) |
| `$4000-$4FFF` | **MC-10 internal RAM while P1 = 0** (`$4000-$5FFF` with the 8K GAL / `--8k`), otherwise SRAM with A16 = P1 |
| `$5000-$BEFF` | SRAM, A16 = P1 |
| `$BF00-$BF7F` | registers: **even addresses = RAM bank (P0 = D0, P1 = D1)**, odd addresses = ROM map (M0 = D0, M1 = D1); reads return the two bits |
| `$BF80-$BFFF` | MC-10 keyboard / VDG / sound |
| `$C000-$FFFF` | ROM map mode, below; **writes always go to SRAM** (A16 = P0, except `$FF00-$FFFF`, which always uses A16 = 0) |

The SRAM is addressed by the CPU address itself plus A16, so the emulator
indexes it as `(A16 << 16) | address`.  Both registers clear on reset.

### ROM map modes

| M1 M0 | `$C000-$DFFF` reads | `$E000-$FFFF` reads |
|---|---|---|
| 0 0 | EPROM `$0000-$1FFF` | EPROM `$2000-$3FFF` |
| 0 1 | SRAM | EPROM `$2000-$3FFF` |
| 1 0 | SRAM | MC-10 internal ROM |
| 1 1 | SRAM | SRAM |

### Boot

The EPROM's reset vector is `$C006`.  The boot menu tests memory, copies
the selected BASIC into RAM, and selects the map: option 0 = MicroColor
BASIC, 1 = MCX BASIC standard, 2 = LARGE (BASIC's program and variables in
P1's 64K, separate from video).

MAME's `mcx128` device uses a different, equivalent internal layout.  The
GAL-exact model gave identical results on all tests.

## 14. MCX32-SD

`mcx32sd.c` + `sdcard.c`.  The full register, memory map and protocol
reference is in **[MCX32-SD_protocol.md](MCX32-SD_protocol.md)**.  In
brief:

| Address | Function |
|---|---|
| `$04` | SD controller status (bit0 data waiting, bit1 ready for a byte, bit2 idle, bit7 error); write `$80` = abort |
| `$05` | SD controller data |
| `$06` / `$07` | Flash bank at `$C000` / `$E000` (`$07` = 0: internal ROM; bank 0 is shown at `$E000` from reset until the first write to `$07`) |
| `$0F` | Mode (`$30` on Alice 32/90, 0 on MC-10) |
| `$1D` | Flash write enable |
| `$1E` / `$1F` | `$4200-$4345` → `$0200-$0345` remap off / on |
| `$0020-$1FFF`, `$5000-$AFFF` | SRAM (8K + 24K = 32K); `$4000-$4FFF` is the MC-10's RAM |
| `$2000-$3FFF` | Flash bank 0 (BIOS, jump table `$2000`) |

**Flash:** an SST39SF010, 128K in 16 banks of 8K:

* 4K sector erase (`$5555`=`$AA`, `$2AAA`=`$55`, `$5555`=`$80`, again
  `$AA`/`$55`, then `$30` at the sector);
* chip erase (`$10`);
* byte program (`$A0`);
* autoselect ID `$BF`/`$B5`.

Operations complete instantly, and changes are saved to the working copy
on exit.

**SD controller:** emulated at the mailbox level.  Commands:

* `$24 <letter> <name> <trailer>`: path commands L, S, D, C, X, R, K, n/N,
  o/O;
* `$25 <letter> <file#> [arg32]`: handle commands D, E, F, R, T, W, P, Z;
* `$2A`;
* `$23` (bootloader, not supported).

Status bits are computed from the queue state, so every BIOS wait loop
sees the right condition at once.

## 15. Emcee protocol and the built-in server

Every request begins with `!` (`$21`), then a command byte.  Multi-byte
numbers are most significant byte first.

| Cmd | Request (after `!` + cmd) | Response |
|---|---|---|
| `L` load | mode (0 BASIC, 2 ML, 4 array, 8 screen), name length, name | address, size, checksum (size 0 = error, code in checksum low byte) |
| `G` get block | file# | the block's data |
| `N` next block | file# | address, size, checksum; size 0 = EOF, address = exec address for ML |
| `S` save | mode, name length, exec, size (or load address for mode 2), name | status |
| `W` / `w` write block / retry | file#, size, data | 16-bit sum of the data |
| `O` open | (mode << 6) \| file# (mode 1 = I, 2 = O, 3 = A), name length, name | status |
| `F` dir file | flag (0 first, `$FF` next), argument length, argument | status, name length (0 = end) |
| `$` retrieve name | length | name, padded to length |
| `D` dir name (DIR LIST) | flag, reserved | status, name length |
| `C` set dir | reserved, name length, name | status |
| `P` print | one byte | (none) |

Checksums are the 16-bit sum of the data bytes.  Error codes are listed in
[section 16](#error-codes).

**The built-in server (`sdcard.c`, "Built-in Emcee file server"):**

* **Files:** it shares the SD card's file and format engine, with its own
  current directory.
* **Load blocks:** loads are served in blocks of up to 1024 bytes.  For
  LOADM, each DECB segment becomes addressed blocks, and the exec address
  comes in the EOF descriptor.
* **Block sequencing:** `N` describes the next unread block and `G`
  returns it, so a repeated `G` (a retry) resends the same block.
* **SAVE:** the `W` blocks are collected and the file is written at the
  empty end block, converted according to its extension.
* **Data files** are read in 256-byte blocks; output and append files are
  written as blocks arrive, and `w` rewinds the last block.
* **DIR:** listings show files and folders (with a trailing `/`), sorted.
  DIR LIST shows the subfolders.

## 16. File formats

### `.C10` / `.CAS` / `.K7`

The raw tape byte stream (section 9), from the first leader byte to the
end.  An optional leading leader is tolerated.

### CoCo DECB binary (`.BIN`)

```
repeat:  $00 <len hi> <len lo> <addr hi> <addr lo> <len bytes>
end:     $FF $00 $00 <exec hi> <exec lo>
```

The MCX32-SD BIOS parses this format itself for LOADM.  It also accepts an
optional CRC-16 marker block (`$00 0001 FFFF xx`).  The emulator's
controller doesn't send that marker.

### Tokenized BASIC program (MC-10 memory image)

```
line:  <link hi> <link lo> <line# hi> <line# lo> <tokens and text> $00
end:   $00 $00
```

TXTTAB (start of program) is `$4346` in MicroColor BASIC and MCX BASIC.
Links are absolute addresses, and BASIC relinks after every load.

### Error codes

| Code | | Meaning |
|---|---|---|
| 8 | FC | illegal function call |
| 34 | IO | I/O error |
| 36 | FM | wrong file mode |
| 38 | DN | bad file/device number |
| 40 | NE | not found |
| 42 | WP | write protected / cannot write |
| 44 | FN | bad name |
| 46 | FS | file system error |
| 48 | IE | input past end |
| 50 | FD | bad file data |
| 52 | AO | already open / directory not empty |
| 54 | NO | not open |
| 56 | DS | direct statement in file |
| 58 | AE | already exists |
| 60 | DF | disk full |
| 62 | CR | CRC error |

## 17. The BASIC tokenizer

`sd_tokenize()` / `sd_detokenize()` in `sdcard.c`.  They're used by the SD
controller and the Emcee server to convert between ASCII listings and
program images.

**Token table:** the keyword table is read from the BASIC that is running,
so the tokens always match that BASIC.  It's located by its first two
entries, `FOR` and `GOTO`, with the high bit marking the last character of
each keyword:

| BASIC | Table location | Tokens |
|---|---|---|
| MCX BASIC 3 | flash bank 0, `$2E00` | `$80` FOR ... `$F7` ERRN |
| MCX BASIC 2.1 | MCX-128 EPROM | its own set |
| MicroColor BASIC | the ROM | its own set |

**Tokenizing ("crunching")** follows the MCX BASIC 3 crunch routine
(`$E4B6` and the code before it):

* Outside quotes, at every position, the keyword table is searched in
  order and the **first** match wins, even inside variable names, as in
  every Microsoft BASIC of the era.
* `?` becomes PRINT (`$86`).
* `'` becomes `:` + `$DD`, and the rest of the line is literal.
* ELSE becomes `:` + `$C9`.
* After REM (`$83`) the rest of the line is literal.
* After DATA (`$85`) text is literal until a `:` outside quotes.
* Letters outside quotes, REM and DATA are upper-cased; spaces are kept.
* Lines are linked from `$4346`, and the program ends with `$00 $00`.

The output is **byte-identical** to what MCX BASIC 3.1 stores when the
same lines are typed, link pointers included.  The test suite checks this.

**Detokenizing** prints `line# ` followed by the text.  It drops the `:`
in front of `$C9`/`$DD`, expands tokens outside quotes, REM and DATA, and
copies bytes after REM/`'` and inside DATA unchanged, so semigraphics
characters survive.  Lines end with LF.

## 18. Source map

| File | Lines | Contents |
|---|---|---|
| `m6803.c/h` | 700 | CPU, timer, port registers, disassembler |
| `mc10.c/h` | 930 | Machine: memory map, keyboard, VDG, sound, typing, printer, cassette glue, binary load/CSAVEM |
| `tape.c/h` | 430 | Cassette image input (C10/WAV), CSAVE decoder, WAV writer, WAV → bytes |
| `mcx128.c` | 90 | MCX-128 decoding from the GAL equations |
| `mcx32sd.c` | 210 | MCX32-SD registers, flash, memory map, remap |
| `sdcard.c` | 1290 | SD controller, Emcee server, tokenizer, format conversion, host file system |
| `serial.c` | 220 | Bit-level serial line, TCP client |
| `debugger.c/h` | 160 | Terminal monitor |
| `main.c` | 860 | SDL2 frontend, settings, command line |
| `font6847.h` | | camennie's character set |
| `tests/headless.c` | | Scriptable test harness |
| `tests/run_tests.sh` | | Regression tests |
| `tools/make_mcx32sd_flash.py` | | Builds the MCX32-SD flash image from Darren's update |
| `tools/MCXPROBE.BAS` | | Probe program for checking the MCX32-SD model against real hardware |
