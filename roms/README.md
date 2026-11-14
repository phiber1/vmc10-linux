# ROM and firmware images

**No ROM or firmware images are distributed with VMC10 for Linux.**  They
are copyrighted by their respective owners (Microsoft/Tandy for MicroColor
BASIC, Matra & Hachette for the Alice, Darren Atkinson for MCX BASIC).  You
must supply your own copies.  This folder is where the emulator looks for
them.  Everything in it except this README is ignored by git.

The emulator searches, in order:

1. `./roms/` (relative to the current directory)
2. `roms/` next to the `vmc10` executable
3. `../share/vmc10/roms/` relative to the executable (after `make install`)
4. `~/.local/share/vmc10/roms/`
5. `/usr/local/share/vmc10/roms/`, then `/usr/share/vmc10/roms/`

File names are matched exactly, then all lower case, then all upper case.
Any image can also be given explicitly on the command line (`--rom`,
`--mcx128-rom`, `--mcx32sd-flash`, `--chr`, `--chrx`) or in the settings
file.  If a required image is missing, the emulator prints its name and
refers you here.

Use the checksums below to make sure you have the right file, for example
`sha1sum roms/MC10.BIN` or `crc32 roms/MC10.BIN`.

## Required for each configuration

| Configuration | Files needed |
|---|---|
| Stock MC-10 (4K or 20K, with or without `--8k`) | `MC10.BIN` |
| Matra Alice (`--alice`) | `alice.bin` |
| MCX-128 expansion (`--mcx128`) | `MC10.BIN` + `mcx128bas.rom` |
| MCX32-SD expansion (`--mcx32sd`) | `MC10.BIN` + `mcx32sd_flash.bin` |
| Built-in Emcee server (`--emcee`) | nothing extra (used with MCX BASIC on either MCX board) |

## The images

### `MC10.BIN`: TRS-80 MC-10 MicroColor BASIC 1.0

| | |
|---|---|
| Size | 8,192 bytes (mapped at `$E000-$FFFF`, mirrored at `$C000`) |
| CRC32 | `11fda97e` |
| SHA-1 | `4afff2b4c120334481aab7b02c3552bf76f1bc43` |
| Contents | Microsoft MicroColor BASIC 1.0, the MC-10's built-in ROM (Tandy, 1982) |
| Used by | every configuration except `--alice` |

* **Where to find it:** this is the same image as `mc10.rom` in MAME's
  `mc10` set, so you can rename that file or pass `--rom path/to/mc10.rom`.
  The original Virtual MC-10 0.73 archive also contains it as `rom/MC10.BIN`.
* **Other ROMs:** any 8K MC-10-compatible ROM can be used with `--rom`.
  4K images are mirrored automatically.
* **Fast typing:** this ROM's keyboard routine is what Quicktype, paste and
  "sensible" keyboard mode hook into.  With other ROMs, typing falls back
  to pressing keys on the emulated keyboard matrix, which is slower but
  works with any software.

### `alice.bin`: Matra & Hachette Ordinateur Alice

| | |
|---|---|
| Size | 8,192 bytes |
| CRC32 | `f876abe9` |
| SHA-1 | `c2166b91e6396a311f486832012aa43e0d2b19f8` |
| Contents | BASIC ROM of the French Alice 4K, a close relative of the MC-10 |
| Used by | `--alice` |

The same image is `alice.rom` in MAME's `alice` set and `rom/Alice.bin` in
the Virtual MC-10 0.73 archive.  The keyboard is emulated with the MC-10's
layout; the Alice's AZERTY legends are not remapped.

### `mcx128bas.rom`: MCX BASIC 2.1 for the MCX-128

| | |
|---|---|
| Size | 16,384 bytes (the board's 27128 EPROM) |
| CRC32 | `11202e4b` |
| SHA-1 | `36c30d0f198a1bffee88ef29d92f2401447a91f4` |
| Contents | Darren Atkinson's boot menu and MCX BASIC 2.1 ("BUILT MAR 18, 2011") |
| Used by | `--mcx128` |

* **Where to find it:** Darren Atkinson distributes this image with his
  MCX128 project files ("MCX128 Essentials": schematic, GAL equations and
  `MCX_Basic_21.rom`).  See his MCX Wares blog,
  <https://mcxwares.blogspot.com/>.  It is also the `mcx128bas.rom` file in
  MAME's `mcx128` device set.
* **AZERTY version:** the `MCX_Basic_21_AZERTY.rom` variant in the same
  bundle is for an Alice 4K or a French-keyboard MC-10.
* **Not this image:** "MCX Basic for VMC-10.bin", found in the Virtual MC-10
  archive, is a different build (2.0, with a loader prepended) for that
  emulator's flat "enhanced" memory mode.  It does not work with
  `--mcx128`.

### `mcx32sd_flash.bin`: MCX32-SD flash (BIOS + MCX BASIC 3.1)

| | |
|---|---|
| Size | 131,072 bytes (SST39SF010, 16 banks of 8K) |
| CRC32 | `cc231283` |
| SHA-1 | `c735de84c6e2a86e2348d896116aaa1e35776635` |
| Contents | bank 0: BIOS; banks 1-5: MCX BASIC 3.1 for MC-10, Alice 4K and Alice 32/90; banks 6-15 erased |
| Used by | `--mcx32sd` |

**Build it yourself** from Darren Atkinson's free "MCX Basic 3.1 Update"
(the installer he published for MCX32-SD owners), using the script in this
repository:

```
# download "MCX Basic 3.1 Update.zip" from https://atkware.short.gy/mcx-basic-update
tools/make_mcx32sd_flash.py "MCX Basic 3.1 Update.zip" roms/mcx32sd_flash.bin
```

The script extracts the flash banks from the installer, checks the BIOS and
MCX BASIC signatures, and writes the 128K image.  The result has exactly
the checksums above.

The flash is writable: `KILL MEM`, `WRITE MEM` and firmware updates change
it.  The emulator therefore runs from a working copy at
`~/.local/share/vmc10/mcx32sd_flash.bin`, created from `roms/` the first
time.  Your original is never modified.  Use `--mcx32sd-flash FILE` to
re-seed the working copy.

## Optional character generator images

The built-in 6847 character set is camennie's (see the main README), so
none of these are needed.  They change how text looks.

| File (any name) | Size | Option | Effect |
|---|---|---|---|
| internal character set | 768 bytes (64 characters x 12 rows) | `--chr FILE` | replaces the built-in font for normal text |
| external character set | 2,048 or 4,096 bytes (16 bytes per character) | `--chrx FILE` | used for character codes `$00-$7F` when the VDG's INT/EXT line is high (shared with GM2 on the MC-10) |

The Virtual MC-10 0.73 archive includes several such files:
`mess_ntscsquare_chr.bin` and `mess_ntscround_chr.bin` (768 bytes, from
MESS), `match_chr.bin`, `match_chrx.bin`, `nectrek_chrx.bin` and
`necpc_chrx.bin`.
