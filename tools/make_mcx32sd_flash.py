#!/usr/bin/env python3
"""
make_mcx32sd_flash.py - build the 128K MCX32-SD flash image for VMC10 from
Darren Atkinson's free "MCX Basic 3.1 Update" download.

The update is distributed as "MCX Basic 3.1 Update.zip", containing a
LOADM-able installer file named "MCXBASIC 3.1".  That file is seven 8K
blocks: block 0 is the BASIC installer program, blocks 1-5 are flash banks
1-5, and block 6 is flash bank 0 (the BIOS, programmed last by the
installer).  Banks 6-15 are left erased ($FF), as on a fresh board.

usage:
    tools/make_mcx32sd_flash.py "MCX Basic 3.1 Update.zip" roms/mcx32sd_flash.bin
    tools/make_mcx32sd_flash.py "MCXBASIC 3.1"             roms/mcx32sd_flash.bin
"""
import sys
import zipfile
import hashlib
import zlib

BANK = 0x2000


def read_installer(path):
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as z:
            names = [n for n in z.namelist() if n.rstrip("/").split("/")[-1].upper() == "MCXBASIC 3.1"]
            if not names:
                sys.exit("error: no 'MCXBASIC 3.1' file inside %s" % path)
            return z.read(names[0])
    with open(path, "rb") as f:
        return f.read()


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    data = read_installer(sys.argv[1])
    if len(data) != 7 * BANK:
        sys.exit("error: expected a %d-byte installer, got %d bytes" % (7 * BANK, len(data)))

    flash = bytearray(b"\xff" * (16 * BANK))
    flash[0:BANK] = data[6 * BANK:7 * BANK]                    # bank 0: BIOS
    for bank in range(1, 6):
        flash[bank * BANK:(bank + 1) * BANK] = data[bank * BANK:(bank + 1) * BANK]

    # sanity checks: BIOS jump table at $2000, reset vector $2B00,
    # MCX BASIC signatures in banks 1 and 2
    checks = [
        (flash[0:3] == b"\x7e\x27\xed", "BIOS jump table"),
        (flash[BANK - 2:BANK] == b"\x2b\x00", "BIOS reset vector"),
        (flash[BANK:BANK + 4] == b"MX4K", "bank 1 signature 'MX4K'"),
        (flash[2 * BANK + 0x45:2 * BANK + 0x49] == b"MX10", "bank 2 signature 'MX10'"),
    ]
    for ok, what in checks:
        if not ok:
            sys.exit("error: %s not found - is this the MCX Basic 3.1 update?" % what)

    with open(sys.argv[2], "wb") as f:
        f.write(flash)
    print("wrote %s (%d bytes, CRC32 %08x, SHA-1 %s)" % (
        sys.argv[2], len(flash), zlib.crc32(flash) & 0xFFFFFFFF, hashlib.sha1(flash).hexdigest()))


if __name__ == "__main__":
    main()
