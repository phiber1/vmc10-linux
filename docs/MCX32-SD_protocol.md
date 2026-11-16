# MCX32-SD: reverse-engineered hardware and SD protocol

This was reconstructed from the MCX BASIC 3.1 firmware (the "MCX Basic 3.1 Update" installer
published by Darren Atkinson) for the VMC10 Linux emulator. It has **not** been checked
against real hardware. Its correctness rests on MCX BASIC 3.1 running in the emulator: it
boots, reports the guide's 27618 bytes free, and lists, loads, saves and opens files
correctly.

## Hardware summary

| Address | Function |
|---|---|
| `$04` R | SD controller status: bit0 byte waiting, bit1 ready for a byte, bit2 idle, bit7 error |
| `$04` W | `$80` = abort / resynchronise |
| `$05` | SD controller data, both directions |
| `$06` | Flash bank (0-15) shown at `$C000-$DFFF`; 0 = none |
| `$07` | Flash bank shown at `$E000-$FFFF`; 0 = internal BASIC ROM. From reset until the first write, bank 0 appears here, so the reset vector `$2B00` comes from the BIOS. |
| `$0F` | Mode register: the BIOS writes `$30` on an Alice 32/90 and 0 on an MC-10; bit 4 selects the Alice timings |
| `$1D` W | Written (`$F1`) before each flash command write |
| `$1E` W | `$4200-$43FF` → `$0200-$03FF` remap **off** (sticky); used by PEEK@/POKE@ and around SAVE.  While the remap is on, reads come from SRAM `$0200-$0345`, and writes go there and also through to the MC-10's internal RAM (inferred: otherwise every plain-MC-10 hi-res game would show a blank band) |
| `$1F` W | Remap **on** (the BIOS sets it at reset; BASIC sets it again at its idle points) |
| `$0020-$1FFF` | SRAM, 8K |
| `$2000-$3FFF` | Flash bank 0: the BIOS. Jump table at `$2000/$2003/$2006/$2009`. |
| `$4000-$4FFF` | The MC-10's own RAM (video) |
| `$5000-$AFFF` | SRAM, 24K (8K + 24K = 32K; this gives the guide's `PRINT MEM` = 27618) |
| `$C000-$FFFF` | Flash windows, or the internal ROM |

Notes:

* **The remap covers `$4200-$43FF`,** as the user guide states for PEEK@/POKE@: two pages
  covering MicroColor BASIC's old system variables (`$4200-$4345`).  Writes go to both the
  relocated area and video RAM, which keeps the program text from TXTTAB `$4346` consistent
  when MCX BASIC turns the remap off around SAVE and LOAD.  An earlier version of this
  document said the range ended at `$4345`; that came from a write model without the
  write-through, and was wrong.
* **The emulator's default:** VMC10 for Linux keeps this mapping disabled unless `--remap`
  is given (or Alt-M is pressed), so BASIC can PEEK and POKE the graphics screen directly.
  The hardware behaviour described here is what `--remap` emulates.
* **Flash is an SST39SF010.** KILL MEM erases an 8K bank as two 4K sector erases. The command
  set is the standard `$5555/$2AAA` unlock, issued through the `$C000` window with `$06`
  selecting the bank.
* **Flash layout (3.1):**
  * bank 0: BIOS
  * banks 1+2: MCX BASIC for MC-10, signatures "MX4K" / "MX10"
  * banks 1+3: Alice 4K ("MXA4")
  * banks 4+5: Alice 32/90 ("MX32" / "MX90")
* **Boot:** holding CONTROL at power-on boots plain MicroColor BASIC.
* **6803 addresses:** `$04-$07`, `$0F` and `$15-$1F` are not decoded inside the 6803 and appear
  on the external bus.

## Emulator choices for the open questions

* **'E' close truncates.** OPEN "O" erases the file at its first 'W' or at the 'E' close,
  whichever comes first; Append ('P' to EOF) cancels that.
* **'Z' sets the file size.** 'T' reads like 'R'. `$2A` and `$23` (ATmega bootloader) are
  no-ops.
* **LOAD/SAVE SCREEN (mode 8)** uses the default extension `.SCR`.
* **Formats:** ASCII `.BAS` is tokenized with the token table at BIOS `$2E00`, using MCX
  BASIC's own rules (`'` → `: $DD`, ELSE → `: $C9`, `?` → PRINT, DATA/REM literal). The result
  is byte-identical to what MCX BASIC produces from typed input. `.C10`/`.CAS`/`.K7`/`.WAV`
  are converted. SAVE picks the output format from the extension.

---

# MCX32-SD host <-> ATmega mailbox protocol

This spec was reverse-engineered from the BIOS (flash bank 0, `block6.bin` @ $2000) and from the
MC-10 MCX BASIC 3.1 extension (`bank1.asm` = `block1.bin` @ $C000, `bank2.asm` @ $E000). Addresses
are 6803 addresses. "(inferred)" marks conclusions that rest on naming or usage, not on code that
proves them.

Note: `bank1.asm` starts with the signature "MX4K" (block1). `block4.bin` ("MX32") is the Alice
variant, with its PB at $0054 instead of $0256. It uses the same BIOS calls, plus CLOAD/CSAVE
redirection (see §6).

---------------------------------------------------------------------------------------------------
## 0. Corrections to the working assumptions

* **The status bits are right**, with these refinements:
  * **bit2 (idle).** The controller is idle and the command is complete. The host requires bit2
    **clear** for the whole time it waits for a data byte or for room to send one. In `$277B`, if
    the 12-poll inner loop times out and bit2 is set, the wait fails (carry set), and the
    function returns an error (`$24B5`, giving IO when bit7 is clear). The one exception is the DIR
    loop, which polls with mask `$85`: there, bit2 is the normal "no more entries" signal.
  * **bit7 (error).** Error, with the code in `$05`. The byte loops (`$2767`, `$27C8`, `$277B`)
    test with masks `$81`/`$82`/`$85`, which include bit7. Any set bit7 stops the transfer
    immediately, even when bit0 or bit1 is also set (`BGT` fails when N=1). So the controller
    must deliver all the data first and raise bit7 after it.
  * **The first byte of every command is written without waiting for bit1.** These are the
    `$24`/`$25`/`$2A`/`$23` bytes (`$2703`, `$2749`, `$2478`, `$2952`, `$29A5`). They are written
    straight after bit2 is seen. All later bytes wait for bit1.
* **`$24C9` (abort/resync).** Writes `$80` to `$04`, then polls bit2 for about 1.5 s. Returns C=1
  on timeout. It is used at the start of every `$24`/`$25`/`$2A` command. On *every* error exit,
  `$24C2` also writes `$80` (but does not wait). The BREAK handler `$DE70` writes `$80` too.
* **`$85` is not a command byte.** It is the status mask used by the DIR loop (`$24FB`). DIR is
  sub-command `'D'` ($44) under prefix `$24`. `$53` is `'S'` (SAVE) under `$24`.
* **The `$00` after "/MCX32-SD/AUTOEXEC" is not a NUL terminator.** It is the *trailer byte*
  (PB+2): the LOAD/SAVE mode or the OPEN file number, and 0 for every other command. The name is
  sent **without a length or a NUL**. Every name byte is >= `$20` (checked at `$26F5`), and the
  trailer is always <= 8 (checked at `$26EB`). So the controller detects the end of the name as
  the first byte < `$20`, and that byte is also the parameter.
* **There are four prefix bytes:**
  * `$24` = a path-based command: letter, name, trailer.
  * `$25` = a handle-based command: letter, file#, optional 4-byte argument.
  * `$2A` = a standalone byte.
  * `$23` = a system command (ATmega flash/EEPROM, §7). It is not part of the 16 SD functions.

---------------------------------------------------------------------------------------------------
## 1. Low-level primitives, timing and status handling

| Routine | Behaviour |
|---|---|
| `$24C9` | Writes `$80` to `$04`. Waits up to $D1BD x 25 cy (about 1.50 s) for bit2. C=1 on timeout. |
| `$24E2` | **No abort.** Waits up to $45E9 x 25 cy (about 0.50 s) for bit2. Used for completion (`$2663`) and before `$23` commands. |
| `$277B` | Waits for `(status & A) != 0` (A = mask). Outer count 65536 x about 171 cy (about **12.5 s**). After each outer step, bit2 set means fail. Exits OK only if the masked bits are non-zero and bit7 is clear. |
| `$27A3` | Same as `$277B`, with outer count $28E2 (about **2.0 s**). Used only by DIR, with mask `$85`. |
| `$275E` / `$2757` | Send B / send D (A then B). Mask `$82`. |
| `$2767` / `$27A9` | Send B bytes (0 = 256) / send D bytes from X. One handshake per byte (fast-path `BITA $04; BGT`). |
| `$27BE` / `$27BA` | Receive 1 byte / receive a 16-bit value, MSB first. Mask `$81`. |
| `$27C8` / `$27DC` | Receive B bytes (0 = 256) / receive D bytes to X. One handshake per byte. |
| `$2663` | **Completion.** `$24E2` (bit2 within 0.5 s), then if bit7 set, error. Else returns C=0, B=0. |
| `$24B5` | **Error.** If bit7 set and `$05` != 0: code = `$05`. Else code = `$22` (IO). Then `$24C2`. |
| `$24C2` | PULX; write `$80` to `$04`; SEC; RTS (B = code). |

(Clock: E = 0.894886 MHz.)

**Rules the emulator must follow:**
* After a `$80` abort, drop any partial command, clear bit7, and set bit2. bit2 must appear within
  1.5 s.
* Accept the first byte of the next command at any time while idle. Clear bit2 as soon as a
  command starts, and keep it clear until the command is finished.
* Set bit1 whenever a written byte has been consumed and another is expected. Set bit0 whenever
  `$05` holds a byte for the host. The host reads `$05` exactly once per byte, so reading `$05`
  consumes the byte.
* To report an error: put the code in `$05`, set bit7, and (optionally) set bit2. The host reads
  `$04`, then `$05`, then writes `$80`.
* At the end of a successful command, set bit2 with bit7 clear, within 0.5 s of the host's last
  transfer for commands that end in `$2663` (completion).

---------------------------------------------------------------------------------------------------
## 2. BIOS entry and the parameter block (PB)

`JSR $2000` jumps to `$27ED`. A = device (0 = SD, 1 = Emcee server, anything else = error 38 DN).
B = function byte. X = PB.

* `$27EE` stores the **whole B in PB+0**, then dispatches on `B & $3E`. B values above 30 give
  error FC.
* **B bit6 and B bit0 are option flags** that some handlers read back from PB+0 (see functions 2
  and 28).
* The PB pointer stays on the stack, so handlers use `PULX/PSHX` to recover it.
* Return value: C=0, B=0 for success; C=1, B = error code for failure.

MC-10 BASIC uses PB = `$0256` and keeps the current device in `$027C`. The wrapper is at
`$C480`/`$C46F`: `JSR $C46F` makes every error fatal. `$C46C` tolerates error `$30` (IE) and
`$C469` tolerates error `$36`; a tolerated error returns C=1.

| Offset | Size | Used by | Meaning |
|---|---|---|---|
| +$00 | 1 | all | Function byte as passed in B (flags in bit6/bit0). R/W/T borrow bit0 to save the carry (`$24A7`/`$24B1`). **LOADM overwrites +0..+4** with each 5-byte DECB block header it receives (`$25C8`). |
| +$01 | 1 | `$24`/`$25` cmds | Sub-command letter (written by `$26DE` / `$2485`). |
| +$02 | 1 | `$24` cmds | Trailer byte sent after the name: `PB+$0A & $3F` for L/S/O, 0 otherwise (`$26DC`). |
| +$03 | 1 | – | Used only as scratch by LOADM (header bytes). |
| +$04..05 | 2 | `$24` cmds | Pointer to the name/path string (in). DIR replaces it with `$250B` ("*") when +$0B = 0. RENAME overwrites it with PB+$0C (`$26AA`). |
| +$06..07 | 2 | SAVE, LOAD | SAVE: **exec/transfer address** (sent 3rd). LOAD mode 0/4/8: received size (out). LOADM: exec address = postamble address + offset (out), then read by BASIC into `$021F` (`$C403`). BASIC SAVE/SAVE*/SAVE SCREEN store `$EC2E` here, the ROM's default EXEC address (`$C36F`). SAVEM stores the transfer address (default = start). |
| +$08..09 | 2 | SAVE, LOADM | SAVE: **load address** (sent 1st). Only SAVEM sets it (= start, `$C387`). For the other modes it is stale. LOADM: CRC accumulator, initially `$FFFF` (out). |
| +$0A | 1 | most | LOAD/SAVE: **mode** (0 = BASIC program, 2 = ML/LOADM/SAVEM, 4 = array `LOAD*`/`SAVE*`, 8 = raw memory `LOAD SCREEN`/`SAVE SCREEN`). OPEN: `access | file#` (access $40 = I, $80 = O, $C0 = A). Close (fn 0): bit7 set = output file. `$25` cmds: `& $3F` = file#. LOADM reuses it as the "CRC enabled" flag. |
| +$0B | 1 | `$24` cmds | Name length, 1..255 (0 gives error FN, except DIR, which treats 0 as "*"). RENAME overwrites it with the new-name length. |
| +$0C..0D | 2 | many | Buffer address (LOAD, SAVE, R/W/T, DIR record buffer). LOADM: **load offset** added to block and exec addresses. RENAME: pointer to the new name. P/Z: **high word** of the 32-bit argument. Out: R/W/T leave it advanced by the bytes transferred; LOAD mode 0/4/8 leaves the end address (buffer + size). |
| +$0E..0F | 2 | many | SAVE/R/W/T: **length** (R/W/T require < $8000, else FC at `$2489`). LOAD mode 0/4/8: **limit address**; buffer + size > limit gives error 12 OM (`$25A1`-`$25A7`). RENAME: +$0F = new-name length. P/Z: **low word** of the 32-bit argument. Out: R/W/T = bytes actually transferred (also on error); LOAD 0/4/8 = size. |
| +$10..11 | 2 | LOAD, DIR | Callback address (0 = none for LOAD). LOAD: called once, after the first byte is available and before the size is read. A non-zero B returned from it aborts the load with that error. +$10 is then overwritten with 00:B. DIR: called once per entry with X = PB. A non-zero B makes the BIOS abort the listing (`$253E`) and return that error. BASIC's DIR callback returns `$30` on BREAK, and `$C46C` tolerates that code. |

---------------------------------------------------------------------------------------------------
## 3. Command framing

Each command below starts with `JSR $24C9`: write `$80`, wait for bit2.

```
PATH command  ($26D6 / $26DB):  24  <letter>  <name bytes, all >= $20>  <trailer < $20>
                                (RENAME appends: <new name bytes> 00)
HANDLE command ($2740):         25  <letter>  <file#>                       (file# = PB+$0A & $3F)
       + R/W/T ($272E):              00 00 <lenHi> <lenLo>                   (32-bit BE, high word 0)
       + P/Z   ($2726):              <PB+$0C hi> <lo> <PB+$0E hi> <lo>       (32-bit BE)
STANDALONE ($2472):             2A
```

Checks the BIOS makes before anything goes on the wire:
* Trailer > 8 gives FC (`$26E9`).
* Name length 0 gives FN (`$26F1`).
* Any name byte < `$20` gives FN (`$26F8`).
* The second name of RENAME is **not** checked, and length 0 would send 256 bytes.

---------------------------------------------------------------------------------------------------
## 4. The 16 SD functions (dispatch table at $2452)

Notation:
* "W:" = bytes the host writes.
* "R:" = bytes the host reads.
* "wait idle" = `$2663`: bit2 within 0.5 s, then bit7 means error.

Each byte after the first waits for bit1 (writes) or bit0 (reads). Any bit7 aborts the command,
and so does bit2 seen during a byte wait.

### fn 0 — `$26B8` — CLOSE / release handle ('D' or 'E')
* W: `25 'D' file#` if PB+$0A bit7 is clear; `25 'E' file#` if it is set (output/append file).
  Then wait idle.
* Server equivalent (`$21D9`): no-op for input files; sends an EOF write block for output files.
  So **'E' = close a file that was written**, and 'D' = close a read file or a never-used handle.
* Probably 'E' also truncates the file at the current position, because OPEN "O" never
  truncates (see fn 2). (inferred)
* BASIC callers:
  * OPEN `$C6AD`: `25 'D' n` *before* every open, with PB+$0A = bare file#. Errors are ignored
    (direct `JSR $2000`).
  * CLOSE / close-all `$C617`: after flushing; 'D' for "I" files, 'E' for "O"/"A". Errors
    ignored.

### fn 2 — `$2678` — OPEN data file ('O'/'o', with 'N'/'n' and 'P')
BASIC calls it with **B = $42** (bit6 set), so the letters are lower case: `'n'`, `'o'`
(`$C6B6`). If bit6 is clear, the letters are 'N'/'O' (no BASIC caller).

The BIOS sends:
1. If PB+$0A bit7 is set (O or A): W: `24 'n' name 00`, then wait idle. Error `$3A` (58 AE, "file
   already exists") is **tolerated**; any other error aborts the OPEN (`$2684`).
2. W: `24 'o' name file#` (**trailer = file# only, access bits stripped**). Then:
   * If PB+$0A < $C0 (I or O): wait idle. Done.
   * If PB+$0A >= $C0 (Append): W: `25 'P' file# FF FF FF FF`, then wait idle. The result
     **must be error `$30` (IE)**; anything else, including success, makes OPEN fail with that
     code (`$269B`). After the IE the BIOS aborts (`$80`), waits idle, writes `2A`, and returns
     success (`$269F` -> `$2472`).

So the controller never learns the access mode from 'o'. Input and output opens look the same
except for the preceding 'n'. Output must "erase current contents" (guide), but 'n' on an
existing file is answered with AE and 'o' carries no mode. So the truncation most likely happens
at the next write or at 'E' close. (inferred, see open question Q2)

BASIC applies no default extension; the controller adds ".DAT" (guide). SD supports file numbers
1-2 only (guide); the BIOS allows 0..8.

### fn 4 — `$26B4` — 'F' (no BASIC caller)
* W: `25 'F' file#`, then wait idle.
* Server version is a no-op (`$2198`). Probably "flush/sync file". (inferred)

### fn 6 — `$2483` — 'R' read bytes
* W: `25 'R' file# 00 00 lenHi lenLo` (len < $8000).
* R: up to `len` bytes into PB+$0C. No completion wait on success.
* Out: PB+$0E = bytes received; PB+$0C advanced.
* **EOF / short read:** send the available k < len bytes (k may be 0), then raise **bit7 with
  code `$30` (IE)**. The BIOS returns C=1, B=$30, PB+$0E = k (it updates PB+$0E even on error,
  `$24A2`-`$24AF`).
* Going idle without an error after a short read is reported as IO (34).
* BASIC: INPUT#/LINE INPUT#/EOF buffer refill `$C6D6`-`$C6F5`, always len = $0100, via `$C46C`
  (IE tolerated). PB+$0E becomes the buffer byte count; count 0 means EOF.

### fn 8 — `$26CD` — 'P' set position
* W: `25 'P' file# p3 p2 p1 p0` (32-bit BE from PB+$0C:PB+$0E), then wait idle.
* Used only internally by Append (pos = $FFFFFFFF, expects IE). So seeking past EOF leaves the
  position at EOF and returns error 48 IE. (inferred from that usage)

### fn 10 — `$26D0` — 'Z' (no BASIC caller)
* W: `25 'Z' file# z3 z2 z1 z0` (32-bit BE from PB+$0C:PB+$0E), then wait idle.
* Purpose unknown. Plausibly "set file size / truncate". (Q3)

### fn 12 — `$247D` — 'W' write bytes
* W: `25 'W' file# 00 00 lenHi lenLo`, then `len` data bytes from PB+$0C.
* No completion wait and no checksum. Out: PB+$0E = bytes sent; PB+$0C advanced.
* The controller can refuse mid-stream by raising bit7 (for example 60 DF, 42 WP). After the last
  byte, errors are not seen by this call. The next command's `$80` abort follows.
* BASIC: PRINT# buffer flush `$C5DF`-`$C5F6`, at 256 bytes and at CLOSE.

### fn 14 — `$26A2` — 'R' RENAME / move (path form)
* W: `24 'R' oldname 00 newname 00`, then wait idle.
* `$26A2`-`$26B0`: sends the old name with `$26DB`, then copies PB+$0C/PB+$0F into PB+$04/PB+$0B
  and sends the new name plus PB+2 (= 0).
* The same letter as fn 6, but under prefix `$24`, not `$25`.
* BASIC: `RENAME a TO b` (`$C8DA`-`$C8FE`).
* Guide semantics: if newname does not start with "/" or "$", it is relative to old's directory.
  A trailing "/" means move without renaming.

### fn 16 — `$265D` — 'X' KILL
* W: `24 'X' name 00`, then wait idle.
* BASIC: `KILL "name"` (`$C91E`/`$C922`, B=$10).
* Guide: deletes a file or an empty directory. A non-empty directory gives 52 AO. Device 1 is
  rejected by BASIC.

### fn 18 — `$2480` — 'T' (no BASIC caller)
* Host side is identical to 'R': W: `25 'T' file# 00 00 lenHi lenLo`, then R: up to `len` bytes.
* Not supported by the server.
* Purpose unknown (text line read? tell/size query?). (Q4)

### fn 20 — `$24E9` — 'D' DIR listing
1. If PB+$0B = 0: set PB+$0B = 1 and PB+$04 = `$250B` (the byte `$2A`, "*").
2. W: `24 'D' pattern 00`.
3. **Loop:**
   * Wait up to about 2 s for bit0 | bit2 | bit7 (mask `$85`, `$27A3`).
     * bit7 means error (`$24B5`).
     * **bit2 means end of listing.** The BIOS goes to `$2663` (wait idle, check bit7) and
       returns OK.
     * bit0 means another entry follows.
   * R: **5 bytes** into PB+$0C (`$250C`-`$2510`).
   * R: name bytes, one at a time, until a `00`. At most 39 bytes including the NUL, so **at most
     38 characters**. A 39th non-NUL byte gives IO (34) (`$2530`). The BIOS **upper-cases
     a-z** itself (`$251E`-`$2526`) and stores the string NUL-terminated at PB+$0C+5.
   * Calls the callback at PB+$10 with X = PB.
     * B = 0: next entry.
     * B != 0: abort (`$253E`: `$80` + wait idle), then error B.

**Record layout** (decoded from BASIC's callback `$C13E`, buffer `$02CE`):

| Byte | Meaning |
|---|---|
| 0 | FAT attribute byte. BASIC tests only **bit4 ($10) = directory**: it prints `<DIR>` (`$C151`). |
| 1..4 | File size, 32-bit **big-endian** (`$02CF` = high word, `$02D1` = low word). Shown as exact bytes if < 10240, else KiB rounded up ((size + 1023) >> 10) with "K", else MiB with "M" (`$C15E`-`$C198`). |
| 5.. | Name, NUL-terminated (the BIOS upper-cases it). |

Pattern semantics (guide; the BIOS passes the string untouched, after BASIC strips a "0:"/"1:"
prefix in `$C901`):
* An optional path, then an optional wildcard filter, e.g. `"M*"`, `"GAMES/"`.
* A path that ends in "/" means list everything in it.
* With no argument, the BIOS sends `"*"`.

BASIC call: `$C1F4`-`$C221`, B=$14, buffer `$02CE`, callback `$C13E`, via `$C46C`. The callback
handles BREAK (returns `$30`) and "MORE.." paging.

### fn 22 — `$2571` — 'L' LOAD
W: `24 'L' name mode` (mode = PB+$0A & $3F: 0, 2, 4 or 8). Then:
1. Wait up to 12.5 s for bit0 (`$2578`; mask `$81`). An error here (NE, FN, FD, ...) is raised
   with bit7.
2. If PB+$10 != 0, call the callback. BASIC's LOAD callback `$C452` does the NEW, so a failed
   LOAD keeps the old program.
3. **Mode != 2 (BASIC program, array, raw/screen):**
   * R: 16-bit size, MSB first. **Any `FF FF` pairs before it are skipped** (`$2598`-`$259D`), so
     the controller can send `FF FF` as keep-alives while converting (each pair resets the
     12.5 s per-byte timeout).
   * If buffer + size > PB+$0E: error 12 OM (BIOS aborts with `$80`).
   * R: `size` raw bytes into PB+$0C.
   * Wait idle.
   * Out: PB+$06 = PB+$0E = size; PB+$0C = end.
4. **Mode 2 (LOADM):** the BIOS parses a **CoCo DECB binary stream** itself (`$25C1`-`$264A`):
   ```
   repeat:  R 5 bytes  [t][lenHi][lenLo][adrHi][adrLo]
     t == 0  -> data block: R len bytes to (adr + offset[PB+$0C]); continue
                special: t=0,len=$0001,adr=$FFFF -> R 1 byte (discarded), set CRC-mode flag; continue
     t != 0  -> postamble: PB+$06 = adr + offset (exec);
                if CRC-mode and (len field != CRC) -> error 62 CR ($25FE)
                wait idle; done
   ```
   * CRC = CRC-16/CCITT-FALSE (poly $1021, init $FFFF, no reflection, no xor-out). It is
     computed over the payload bytes of every data block received *after* the magic block
     (`$2619`-`$264A`). It is transmitted in the postamble's length field.
   * No range or OM checks are made in LOADM.
   * Out: PB+$06 = exec, PB+$08 = CRC.

| Mode | BASIC statements | Buffer (+$0C) / limit (+$0E) | Default extensions (guide) |
|---|---|---|---|
| 0 | LOAD, RUN "f", MERGE, CHAIN, AUTOEXEC | TXTTAB / SP-$46 (`$C43E`). MERGE: VARTAB+8 | .BAS, .C10, .WAV |
| 2 | LOADM, RUNM | offset (default 0) / – (`$C3F0`) | .BIN, .C10, .WAV |
| 4 | LOAD* array | array data / array end `$0271` | .ARY, .C10, .WAV |
| 8 | LOAD SCREEN | graphics page / page end | not documented (Q6) |

Mode-0 data must be an **MC-10 tokenized program image**: lines of
`[link(2, any non-zero)][line#(2)][tokens...][00]`. A final `00 00` is optional.
* After the load, BASIC writes `00 00` at the end address (`$C536`).
* It relinks starting at TXTTAB, stopping at the first link of 0 (`$E2F3`).
* It sets VARTAB just after that link.

### fn 24 — `$2547` — 'S' SAVE
```
W: 24 'S' name mode  [PB+$08 hi lo = load addr] [PB+$0E hi lo = length] [PB+$06 hi lo = exec]
   [length data bytes from PB+$0C]
wait idle (0.5 s after the last data byte!)
```
* The field order differs from filing.txt (load, length, exec).
* BASIC callers (`$C368`-`$C3A7`):
  * SAVE (mode 0): TXTTAB..VARTAB-1, exec = $EC2E, load = stale.
  * SAVEM (2): start, end-start+1, exec = transfer (default start), load = start.
  * SAVE* (4): array data.
  * SAVE SCREEN (8): page.
* Default extension per guide: .BAS / .BIN / .ARY. The **file extension selects the output
  format** (.BAS ASCII, .C10, .WAV; .BIN DECB).
* Errors in the name or header can be raised with bit7 while the host is still sending; the
  host's next byte wait then fails.

### fn 26 — `$265A` — 'C' change directory
* W: `24 'C' path 00`, then wait idle.
* BASIC: `DIR = "path"` (`$C1C3`-`$C1D1`, B=$1A). Not sent when the string is empty.

### fn 28 — `$264C` — 'N'/'n' create file, 'K' make directory
* The letter comes from PB+0:
  * B bit0 set: **'K'**.
  * Else B bit6 set: 'n'.
  * Else: 'N'.
* W: `24 <letter> name 00`, then wait idle.
* Server equivalent (`$219B`): open-for-append then close, i.e. *create/touch file*. So 'N'/'n'
  = create a new file; it must answer **58 AE** if the file exists (fn 2 relies on that).
* BASIC: `NEWDIR "dir"` = tokens NEW+DIR (`$C910`/`$C915`, B=**$1D**), so **'K' = mkdir**. 'n'
  is sent only through fn 2 (OPEN O/A). No caller uses plain 'N'.

### fn 30 — `$2472` — standalone `$2A`
* `$80` abort, wait idle, then W: `2A`. Returns at once with no wait.
* Server version is a no-op.
* Also sent after the Append seek (fn 2).
* Meaning unknown. The emulator should treat it as a no-op that leaves the controller idle. (Q1)

---------------------------------------------------------------------------------------------------
## 5. What the ATmega must do (emulator architecture)

**The controller owns the file system *and* the file formats.** The BIOS only moves bytes, with
one exception: LOADM, where the BIOS parses DECB block headers itself.

1. **Paths and names.**
   * The BIOS sends the raw path string. BASIC strips only a "0:"/"1:" device prefix (`$C901`).
   * The controller resolves:
     * absolute paths (`/...`) and relative paths against the current directory (set by 'C');
     * `.` and `..`;
     * `$/...`, the *program directory*: the directory of the last mode-0 'L', unless a 'C' came
       after it (guide);
     * leafless "dir/" paths for DIR and RENAME;
     * long file names up to 39 chars (the BIOS DIR loop takes at most 38, see Q5);
     * case-insensitive matching (the BIOS upper-cases names in listings).
2. **Default extensions.** The BIOS never adds one. The AUTOEXEC trace sends
   "/MCX32-SD/AUTOEXEC" with mode 0. The controller must try:
   * LOAD: mode 0 → .BAS/.C10/.WAV; mode 2 → .BIN/.C10/.WAV; mode 4 → .ARY/.C10/.WAV.
   * SAVE: .BAS/.BIN/.ARY.
   * OPEN ('n'/'o'): .DAT.
   * Exact-name operations (X, R-rename, K, C): none.
   * The mode/file# trailer byte is what tells the controller which rule applies.
3. **Format translation, as in filing.txt.**
   * LOAD mode 0 must deliver a *tokenized* image (§4 fn 22): the controller tokenizes ASCII
     .BAS and extracts programs from .C10/.WAV. The token table is the one at BIOS `$2E00`
     (FOR=$80 … ERRN=$F7), which bank2 `$E4B6` also uses.
   * SAVE mode 0 delivers the tokenized image; the controller detokenizes it for .BAS, or builds
     a C10/WAV.
   * LOADM: the controller may pass a .BIN file through **raw**, because the BIOS parses DECB.
     For .C10/.WAV it must synthesize a DECB stream (data blocks plus postamble with the exec
     address), optionally CRC-protected (§4 fn 22).
   * SAVEM: the controller receives one contiguous block (load, length, exec) and must write DECB
     (`00 len load data … FF 00 00 exec`) or C10/WAV.
   * Arrays and screens are raw bytes, size-prefixed on LOAD.
   * Evidence:
     * LOAD reads only `size` + raw bytes into TXTTAB and relinks (`$2593`-`$25BB`, `$C536`);
       there is no ASCII path in the BIOS.
     * SAVE sends a single header plus raw memory (`$2547`-`$256E`).
     * The guide says SAVE's "file's extension determines the format".
4. **Data files.** Up to 2 SD handles.
   * 'o' opens without truncating.
   * 'n' creates the file, or answers AE.
   * 'R' reads (short read → IE).
   * 'W' writes at the current position.
   * 'P' seeks (past EOF → position = EOF + IE).
   * 'D'/'E' close. Probably 'E' truncates at the current position (Q2).
   * `25 'D' n` before every open must succeed silently even when nothing is open.
5. **DIR** filtering, record generation (§4 fn 20), and directory-entry ordering (the BIOS does
   not sort).
6. **Error codes**, from the guide table: 8 FC, 34 IO, 36 FM, 38 DN, 40 NE, 42 WP, 44 FN, 46 FS,
   48 IE, 50 FD, 52 AO, 54 NO, 56 DS, **58 AE, 60 DF, 62 CR**.

---------------------------------------------------------------------------------------------------
## 6. Reset / boot / AUTOEXEC

* **Cold start** (`$F7A6`): A = $55, `JMP $C02F`.
  * `$C030` calls `$C735` (BASIC init). If A != $55 (warm start), there is no AUTOEXEC.
  * **`$C038`-`$C04C`:** up to $40D8 iterations x 27 cy (about 0.5 s):
    * strobe `$02` = $FB;
    * if `$03` bit1 = 0 (**BREAK held**), skip AUTOEXEC;
    * if `$04` bit2 = 1, stop waiting.
  * A timeout **does not** skip AUTOEXEC: it falls through to `$C04E` either way. There is no
    `$80` abort before this poll, so the controller should power up showing bit2 = 1.
  * Then `$C059` → `$DAB8`: LOAD mode 0 of "/MCX32-SD/AUTOEXEC" (string at `$C306`, length 18,
    device 0), with RUN flag `$4A` = 1.
  * Wire bytes: `$80`→`$04`, wait bit2, then `24 4C "/MCX32-SD/AUTOEXEC" 00`, then the LOAD
    response (§4 fn 22).
  * Error handling (`$C489`): errors 40 NE, 34 IO and 46 FS are silently ignored; anything else
    is reported. A missing card or controller gives a 1.5 s abort timeout, then IO, which is
    ignored.
* **BREAK** during I/O, and the BASIC error/BREAK handler at `$DE70`, write `$80` to `$04`.
* The Alice variant (block4 `$D36D`-`$D470`) also routes CLOAD/CSAVE/CLOADM-style commands to fn
  22/24, with explicit names and modes 8/2 (the assembler's ".ASM"/".BIN" files). Same protocol.

---------------------------------------------------------------------------------------------------
## 7. Prefix `$23`: ATmega system commands (not part of the 16 functions)

These are reached through `JSR $2006` with A = $23, B = letter, X = parameter, or from BASIC as
`EXEC &H2006;value,letter[,arg]` (`$28A9`).

Sequence for each command:
1. `$24E2` (wait idle, **no abort**).
2. Write `23` unconditionally.
3. Write `<letter>` (bit1 wait).
4. Write X as 2 bytes, except for 'i' and 'G'.
5. Optional reads (bit0 wait).
6. `$24E2` (wait idle).

On failure: if bit7 is set, error FC (8), else IO (34). Then `$80`.

| Call | Wire | Result |
|---|---|---|
| 'P' | `23 'i'`, wait idle, then **read `$05` without a bit0 wait**. If it is not 'P': `23 'P' 47 4D` ("GM"), wait idle, and `$05` must read 'P'. | Enter programming mode (key "GM"). (inferred) |
| 'B' | `23 'M' (hi\|$E0) lo value`, wait idle | EEPROM byte write (inferred: 10-bit address, $E0 = write flag) |
| 'M' | `23 'M' (hi&$03) lo`, R 1 byte, wait idle | EEPROM byte read (inferred) |
| 'R' | `23 'R' hi lo`, R 2 bytes (MSB first), wait idle | Read flash word (inferred) |
| 'S','E','W' | `23 <l> hi lo`, wait idle | Page-buffer word load / page erase / page write (inferred; ATmega328P SPM page = 64 words) |
| 'D' | 64 x `23 'S' w_hi w_lo`, words taken from memory at X | Fill a 128-byte page buffer |
| 'L' / 'V' | 64 x `23 'R' a_hi a_lo` (a += 2): 'L' stores the words at X, 'V' compares them and gives 62 CR on mismatch | Read back / verify a page |
| 'G' | `23 'G'`, no completion wait | Leave bootloader / run application (inferred) |

An SD emulator can safely ignore `$23`. The simplest safe behaviour is to stay idle and never
answer 'P'.

---------------------------------------------------------------------------------------------------
## 8. Open questions / uncertainties

* **Q1 — `$2A` (fn 30 `$2472`, also `$269F`).** It is sent after an abort with no reply expected.
  The server treats fn 30 as a no-op. Candidates: "clear error/continue", "sync all". Its use
  right after the Append seek's IE hints at "keep the file position despite the abort". Treat it
  as a no-op.
* **Q2 — truncation for OPEN "O"** (`$2678`-`$2697`). 'o' carries no access mode (`$26D8 ANDB
  #$3F`), and 'n' on an existing file is answered with AE and then ignored. So "Output erases
  contents" has to be done by the controller some other way: at 'E' close (truncate at the
  position; harmless for Append, which sits at EOF), or by tracking "this handle was preceded by
  'n'". Unknown which. Truncating at 'E' is the simplest consistent choice.
* **Q3 — 'Z' (fn 10 `$26D0`)** takes a 32-bit argument like 'P'. Unused by BASIC; purpose
  unknown (truncate/set size?).
* **Q4 — 'T' (fn 18 `$2480`)** is framed exactly like 'R' (host receives up to len bytes).
  Unused by BASIC and absent on the server. Unknown semantics, for example a line-oriented read
  that ends early with IE, or a position/size query.
* **Q5 — name length.** The DIR loop (`$2515`, A = $27) accepts at most 38 characters plus the
  NUL, but the guide says 39-character names are supported. The controller should never send
  more than 38.
* **Q6 — LOAD/SAVE SCREEN** (mode 8) default extension, and whether the controller converts
  .C10/.WAV for modes 4 and 8, are not visible in the code (guide lists .C10/.WAV for LOAD*).
  The Alice variant uses mode 8 for generic raw files with explicit extensions.
* **Q7 — case of letters.** BASIC always sets bit6 for OPEN, giving 'n'/'o'; 'N'/'O' are unused.
  The difference between them is unknown. A guess is that lower case means "BASIC data file,
  default .DAT"; if so, the emulator should treat 'N'≈'n' and 'O'≈'o', with the extension rule
  differing.
* **Q8 — bit2 at the start of a command.** The host writes the first command byte immediately
  after seeing bit2 and does not wait for bit2 to drop. It only notices bit2 inside byte waits,
  after a 12-poll inner loop. The controller should clear bit2 as soon as it latches the prefix
  byte.
* **Q9 — 'W' errors after the last byte** are never collected (`$249A` returns without `$2663`).
  Whether the real controller reports deferred write errors on the next command is unknown.
* **Q10 — the stale load address (PB+$08)** for SAVE modes 0/4/8 (`$C368`). The controller
  presumably ignores it, or uses TXTTAB-like defaults, when it writes a C10 header.
* **Q11 — the magic CRC block** (`$25D2`-`$25E8`). It is unknown whether the real controller
  always sends it for LOADM. The emulator may omit it, in which case no CRC check is made.
* **Q12 — reads of `$04`/`$05` when nothing is pending** (for example `$2915`/`$291E` read `$05`
  without bit0). The `$23 'i'` reply is expected to be latched in `$05` while the controller is
  idle.
