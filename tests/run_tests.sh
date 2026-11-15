#!/bin/sh
# VMC10 regression tests, driven by the headless test harness.
# Run from the project root with "make test".
#
# The ROM images are not part of the repository (see roms/README.md).  Each
# group of tests runs only when the ROM it needs is present in roms/.  All
# test programs are generated here from tests/fixtures/, so no third-party
# software is needed.
set -u
H=./build/headless
R=roms
ROM=$R/MC10.BIN
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
pass=0; fail=0; skip=0

check() {   # name, haystack, needle
    if printf '%s' "$2" | grep -qF -- "$3"; then
        echo "PASS  $1"; pass=$((pass+1))
    else
        echo "FAIL  $1  (expected: $3)"; fail=$((fail+1))
    fi
}
skipped() { echo "SKIP  $1"; skip=$((skip+1)); }

if [ ! -f "$ROM" ]; then
    echo "roms/MC10.BIN not found: no tests can run (see roms/README.md)."
    exit 0
fi

# ---------------------------------------------------------------- fixtures
# PROG.C10: the BASIC fixture typed in and saved with CSAVE (exercises the
# cassette output decoder).
$H $ROM "out:$T" frames:100 typefile:tests/fixtures/TESTPROG.BAS until-typed frames:30 \
   'type:CSAVE "PROG"\n' until-typed frames:900 >/dev/null 2>&1
# TIMER.C10: machine code that installs an output-compare interrupt handler
# at the OCF hook ($4206) which increments the first screen byte.
#   8E00: LDAA #$7E / STAA $4206 / LDX #$8E20 / STX $4207 / LDAA #$08 / STAA $08 / CLI / RTS
#   8E20: INC $4000 / LDAA $08 / LDD $09 / ADDD #$1000 / STD $0B / RTI
$H $ROM frames:100 poke:8E00,867EB74206CE8E20FF4207860897080E39 \
   poke:8E20,7C40009608DC09C31000DD0B3B "csavem:$T/TIMER.C10,TIMER,8E00,8E2C,8E00" >/dev/null 2>&1
# SHOW.C10: machine code that stores $13 at $4000 and returns.
$H $ROM frames:100 poke:8E00,8613B7400039 "csavem:$T/SHOW.C10,SHOW,8E00,8E05,8E00" >/dev/null 2>&1
# FILL.C10: machine code that fills video RAM $4000-$57FF with $FF through
# the CPU bus, selects RG6 and loops.
#   8E00: LDX #$4000 / LDAA #$FF / STAA 0,X / INX / CPX #$5800 / BNE / LDAA #$7C / STAA $BFFF / BRA *
$H $ROM frames:100 poke:8E00,CE400086FFA700088C580026F8867CB7BFFF20FE "csavem:$T/FILL.C10,FILL,8E00,8E13,8E00" >/dev/null 2>&1
$H $ROM "c10wav:$T/PROG.C10,$T/PROG.WAV" >/dev/null 2>&1

# ---------------------------------------------------------------- core
out=$($H $ROM frames:120 screen)
check "boot to MicroColor BASIC" "$out" "MICROCOLOR BASIC 1.0"

out=$($H $ROM frames:100 'type:PRINT 2+3*4;SQR(2)\nPRINT MEM\n' until-typed frames:60 screen)
check "BASIC arithmetic (CPU + floating point ROM code)" "$out" " 14  1.41421356"
check "20K RAM with 16K expansion (MEM)" "$out" " 19526"

out=$($H $ROM ram:4 frames:100 'type:PRINT MEM\n' until-typed frames:60 screen)
check "4K RAM (MEM)" "$out" " 3142"

out=$($H $ROM frames:100 typefile:tests/fixtures/TESTPROG.BAS until-typed frames:30 'type:RUN\n' until-typed frames:120 screen)
check "Quicktype a BASIC listing and RUN it" "$out" "SUM OK"
check "READ/DATA after Quicktype" "$out" "DATA OK 42"

out=$($H $ROM slowtype frames:100 'type:PRINT "MATRIX";6*7\n' until-typed frames:60 screen)
check "typing through the keyboard matrix (incl. SHIFT)" "$out" "MATRIX 42"

check "CSAVE writes a .C10 image" "$(ls "$T")" "PROG.C10"
out=$($H $ROM frames:100 'type:CLOAD\n' until-typed "tape:$T/PROG.C10" until-tape frames:30 \
      'type:LIST 1000\nRUN\n' until-typed frames:120 screen)
check "CLOAD of the CSAVEd image (LIST)" "$out" '1000 PRINT "LAST LINE"'
check "CSAVE -> CLOAD -> RUN round trip" "$out" "VMC10 SELF TEST"

out=$($H $ROM frames:100 'type:CLOAD\n' until-typed "tape:$T/PROG.WAV" until-tape frames:30 \
      'type:LIST 1000\n' until-typed frames:60 screen)
check "C10 -> WAV conversion and CLOAD from WAV" "$out" '1000 PRINT "LAST LINE"'

$H $ROM frames:100 "prn:$T/lp.txt" 'type:10 REM LISTED\nLLIST\nLPRINT "SUM";2+2\n' until-typed frames:300 >/dev/null
check "LLIST to the printer file" "$(cat "$T/lp.txt" 2>/dev/null)" "10 REM LISTED"
check "LPRINT to the printer file" "$(cat "$T/lp.txt" 2>/dev/null)" "SUM 4"

out=$($H $ROM frames:100 'type:CLOADM\n' until-typed "tape:$T/TIMER.C10" until-tape frames:30 \
      'type:EXEC\n' until-typed frames:30 mem:4000,1 frames:60 mem:4000,1)
a=$(printf '%s\n' "$out" | grep '^mem' | sed -n 1p); b=$(printf '%s\n' "$out" | grep '^mem' | sed -n 2p)
check "CLOADM + EXEC machine code, 6803 timer interrupt advancing" \
      "$( [ -n "$a" ] && [ -n "$b" ] && [ "$a" != "$b" ] && echo ok)" "ok"

$H $ROM frames:60 'type:SOUND 100,5\n' until-typed "audio:$T/snd.raw" frames:40 >/dev/null
hz=$(python3 -c "
import struct,sys
d=open(sys.argv[1],'rb').read();s=struct.unpack('<%dh'%(len(d)//2),d)
z=[i for i in range(1,len(s)) if s[i-1]<0<=s[i]];p=sorted(z[i+1]-z[i] for i in range(len(z)-1))
print(round(44100/p[len(p)//2]))" "$T/snd.raw" 2>/dev/null)
check "SOUND 100,5 pitch ~258 Hz (got ${hz:-?})" "$( [ "${hz:-0}" -ge 250 ] && [ "${hz:-0}" -le 266 ] && echo ok)" "ok"

# ---------------------------------------------------------------- 8K mod
out=$($H $ROM ram:4 8k frames:100 'type:PRINT MEM\n' until-typed frames:60 screen)
check "internal 8K RAM mod (MEM)" "$out" " 7238"
rowfill() {   # percent of buff pixels on screen line ${2:-191} of a PPM
    python3 -c "
import sys
d=open(sys.argv[1],'rb').read().split(b'\n',3)[3]
y=24+int(sys.argv[2]); row=d[(y*320+32)*3:(y*320+288)*3]
print(sum(1 for i in range(0,len(row),3) if row[i:i+3]==b'\xf0\xf0\xf0')*100//256)" "$1" "${2:-191}" 2>/dev/null || echo 0
}
$H $ROM frames:5 vdg:7C "ppm:$T/rg4.ppm" >/dev/null
$H $ROM 8k frames:5 vdg:7C "ppm:$T/rg8.ppm" >/dev/null
check "RG6 repeats the top of the screen with stock 4K" "$( [ "$(rowfill "$T/rg4.ppm")" -lt 50 ] && echo ok)" "ok"
check "RG6 full 256x192 screen with the 8K mod" "$( [ "$(rowfill "$T/rg8.ppm")" -ge 99 ] && echo ok)" "ok"

# ---------------------------------------------------------------- MCX-128
if [ -f $R/mcx128bas.rom ]; then
    MCX="mcx128:$R/mcx128bas.rom"
    out=$($H $ROM $MCX frames:150 screen)
    check "MCX-128 boot menu" "$out" "[1]  MCX BASIC"
    out=$($H $ROM $MCX frames:100 'type:1' frames:150 'type:PRINT MEM;&H7F\n' until-typed frames:60 screen)
    check "MCX BASIC (standard): 28703 free, hex constants" "$out" " 28703  127"
    out=$($H $ROM $MCX frames:100 'type:2' frames:150 'type:PRINT MEM\n' until-typed frames:60 screen)
    check "MCX BASIC (large): 47454 free" "$out" " 47454"
    out=$($H $ROM $MCX frames:100 'type:0' frames:150 'type:PRINT MEM\n' until-typed frames:60 screen)
    check "MCX-128 MicroColor BASIC option: 31558 free" "$out" " 31558"
    out=$($H $ROM $MCX frames:100 'type:1' frames:150 'type:CLOAD\n' until-typed "tape:$T/PROG.C10" \
          until-tape frames:30 'type:LIST 1000\n' until-typed frames:30 screen)
    check "MCX BASIC CLOAD" "$out" '1000 PRINT "LAST LINE"'
    $H $ROM $MCX frames:100 'type:1' frames:150 \
       'type:10 PMODE 1,1:PCLS 1:SCREEN 1,0\n20 COLOR 4:CIRCLE(128,96),80\n30 PAINT(128,96),3,4\n40 GOTO 40\nRUN\n' \
       until-typed frames:900 "ppm:$T/pm1.ppm" >/dev/null
    px=$(python3 -c "
import sys
d=open(sys.argv[1],'rb').read().split(b'\n',3)[3]
p=lambda x,y: d[((24+y)*320+32+x)*3:((24+y)*320+33+x)*3].hex()
print(p(128,96), p(4,4))" "$T/pm1.ppm" 2>/dev/null)
    check "MCX BASIC PMODE 1 colour graphics (got $px)" "$px" "2020d8 28e028"
else
    skipped "MCX-128 tests (roms/mcx128bas.rom not present)"
fi

# ---------------------------------------------------------------- MCX32-SD
if [ -f $R/mcx32sd_flash.bin ]; then
    SD="$T/sd"; mkdir -p "$SD/GAMES/ARCADE" "$SD/MCX32-SD"
    cp "$T/PROG.C10" "$SD/GAMES/PROG.C10"
    cp "$T/SHOW.C10" "$SD/SHOW.C10"
    cp tests/fixtures/TESTPROG.BAS "$SD/testprog.bas"
    cp tests/fixtures/MCXTOKENS.BAS "$SD/mcxtokens.bas"
    X="mcx32sd:$R/mcx32sd_flash.bin sd:$SD"
    out=$($H $ROM $X frames:300 'type:PRINT MEM\n' until-typed frames:60 screen)
    check "MCX32-SD boots MCX BASIC 3.1" "$out" "MCX BASIC 3.1"
    check "MCX32-SD memory map (user guide: 27618 free)" "$out" " 27618"
    out=$($H $ROM $X frames:300 'type:DIR\n' until-typed frames:120 screen)
    check "SD: DIR lists folders" "$out" "<DIR>  GAMES"
    check "SD: DIR lists files" "$out" "SHOW.C10"
    out=$($H $ROM $X frames:300 'type:RUN "TESTPROG"\n' until-typed frames:200 screen)
    check "SD: RUN an ASCII .BAS (tokenized by the controller)" "$out" "SUM OK"
    out=$($H $ROM $X frames:300 'type:DIR="GAMES"\nLOAD "PROG"\nCLS\nLIST\n' until-typed frames:200 screen)
    check "SD: DIR= and LOAD a .C10" "$out" '1000 PRINT "LAST LINE"'
    check "SD: program text inside the remapped \$4200-\$43FF page lists intact" "$out" "10 REM VMC10 TEST PROGRAM"
    out=$($H $ROM $X frames:300 'type:DIR "0:GAMES/"\n' until-typed frames:120 screen)
    check "SD: DIR of a subdirectory shows <DIR> entries" "$out" "<DIR>  ARCADE"
    # the controller's tokenizer must match MCX BASIC's own
    $H $ROM $X frames:300 typefile:tests/fixtures/MCXTOKENS.BAS until-typed frames:30 'type:SAVE "TYPED.C10"\nSAVE "TYPED"\n' until-typed frames:200 >/dev/null
    $H $ROM $X frames:300 'type:LOAD "MCXTOKENS"\nSAVE "MINE.C10"\n' until-typed frames:200 >/dev/null
    out=$($H $ROM $X frames:300 'type:RUN "MCXTOKENS"\n' until-typed frames:200 screen)
    check "SD: MCX syntax via controller tokenizer (ELSE, ?, &H)" "$out" "ELSE OK"
    check "SD: DATA with quoted comma and keyword text" "$out" "QUOTED,STRINGPRINT 3.5"
    same=$(python3 -c "
import sys
def data(p):
    d=open(p,'rb').read(); i=0; o=b''
    while i+4<=len(d):
        if d[i]==0x55 and d[i+1]==0x3c:
            t,l=d[i+2],d[i+3]
            if t==1: o+=d[i+4:i+4+l]
            i+=5+l
        else: i+=1
    return o
print('same' if data(sys.argv[1])==data(sys.argv[2]) and data(sys.argv[1]) else 'diff')" "$SD/TYPED.C10" "$SD/MINE.C10" 2>/dev/null)
    check "SD: controller tokenizer byte-identical to MCX BASIC's" "$same" "same"
    check "SD: SAVE .BAS writes plain text (ELSE, ' and DATA)" "$(cat "$SD/TYPED.BAS" 2>/dev/null)" \
          '30 IF A=16 THEN PRINT "ELSE OK" ELSE PRINT "ELSE BAD" '"'"' COMMENT'
    out=$($H $ROM $X frames:300 'type:POKE 20000,7:SAVEM "M",20000,20000\nPOKE 20000,0:LOADM "M":PRINT PEEK(20000)\n' until-typed frames:200 screen)
    check "SD: SAVEM/LOADM .BIN round trip" "$out" " 7"
    out=$($H $ROM $X frames:300 'type:LOADM "SHOW":EXEC\n' until-typed frames:200 mem:4000,1)
    check "SD: LOADM a .C10 and EXEC it" "$out" "mem 4000: 13"
    out=$($H $ROM $X frames:300 'type:OPEN "O",#1,"D":PRINT #1,"AB":CLOSE #1:OPEN "A",#1,"D":PRINT #1,"CD":CLOSE #1\n10 OPEN "I",#1,"D":LINE INPUT #1,A$:LINE INPUT #1,B$:PRINT A$;B$;EOF(1)\nRUN\n' until-typed frames:300 screen)
    check "SD: OPEN O/A/I, PRINT#, LINE INPUT#, EOF" "$out" "ABCD-1"
    out=$($H $ROM $X frames:300 'type:NEWDIR "T"\nRENAME "D.DAT" TO "T/"\nKILL "T"\nKILL "T/D.DAT"\nKILL "T"\nDIR "T/"\n' until-typed frames:300 screen)
    check "SD: KILL of a non-empty directory -> ?AO" "$out" "?AO ERROR"
    check "SD: NEWDIR/RENAME/KILL, then missing dir -> ?NE" "$out" "?NE ERROR"
    printf '10 PRINT "AUTO OK"\n' > "$SD/MCX32-SD/AUTOEXEC.BAS"
    out=$($H $ROM $X frames:400 screen)
    check "SD: AUTOEXEC runs at power-on" "$out" "AUTO OK"
    rm "$SD/MCX32-SD/AUTOEXEC.BAS"
    out=$($H $ROM $X frames:300 'type:CLS:PRINT "Z"\nWRITE MEM @6,&H4000,0,1\n' until-typed frames:100 flash:C000,1 'type:KILL MEM @6\nKILL MEM @0\n' until-typed frames:100 flash:C000,1 screen)
    check "MCX32-SD flash: WRITE MEM programs a byte" "$out" "flash 0C000: 5A"
    check "MCX32-SD flash: KILL MEM erases the bank" "$out" "flash 0C000: FF"
    check "MCX32-SD flash: bank 0 (BIOS) is protected" "$out" "?FC ERROR"
    cp "$T/FILL.C10" "$SD/FILL.C10"
    $H $ROM $X frames:300 'type:LOADM "FILL":EXEC\n' until-typed frames:60 "ppm:$T/fill.ppm" >/dev/null
    check "MCX32-SD: graphics written over the \$4200 remap reach video RAM" "$( [ "$(rowfill "$T/fill.ppm" 20)" -ge 99 ] && echo ok)" "ok"
    out=$($H $ROM $X serial:builtin frames:300 'type:LOAD "1:TESTPROG"\nRUN\n' until-typed frames:600 screen)
    check "Emcee device 1: under MCX BASIC 3.1" "$out" "SUM OK"
    out=$($H $ROM $X serial:builtin frames:300 'type:DIR "1:GAMES/"\n' until-typed frames:300 screen)
    check "Emcee: DIR shows subdirectories with a trailing /" "$out" "ARCADE/"
else
    skipped "MCX32-SD tests (roms/mcx32sd_flash.bin not present)"
fi

# ---------------------------------------------------------------- Emcee on MCX-128
if [ -f $R/mcx128bas.rom ]; then
    ED="$T/emcee"; mkdir -p "$ED/GAMES"
    cp "$T/PROG.C10" "$ED/GAMES/PROG.C10"
    printf '10 PRINT "OVER THE WIRE"\n' > "$ED/wire.bas"
    E="mcx128:$R/mcx128bas.rom sd:$ED serial:builtin"
    out=$($H $ROM $E frames:100 'type:1' frames:150 'type:DIR\n' until-typed frames:300 screen)
    check "Emcee: DIR over the emulated 38400 baud serial line" "$out" "WIRE.BAS"
    out=$($H $ROM $E frames:100 'type:1' frames:150 'type:RUN "WIRE"\n' until-typed frames:400 screen)
    check "Emcee: RUN an ASCII .BAS" "$out" "OVER THE WIRE"
    out=$($H $ROM $E frames:100 'type:1' frames:150 'type:SETDIR "GAMES"\nLOAD "PROG"\nLIST 1000\n' until-typed frames:600 screen)
    check "Emcee: SETDIR and LOAD a .C10" "$out" '1000 PRINT "LAST LINE"'
    $H $ROM $E frames:100 'type:1' frames:150 "prn:$ED/p.txt" 'type:10 PRINT "KEPT"\nSAVE "KEEP"\nSAVEM "MM",20000,20001\nOPEN "O",#1,"DF":PRINT #1,"Q":CLOSE #1\nLLIST\n' until-typed frames:600 >/dev/null
    check "Emcee: SAVE writes plain text" "$(cat "$ED/KEEP.BAS" 2>/dev/null)" '10 PRINT "KEPT"'
    check "Emcee: SAVEM writes a DECB binary" "$(od -An -tx1 "$ED/MM.BIN" 2>/dev/null | head -1)" "00 00 02 4e 20"
    check "Emcee: OPEN O / PRINT#" "$(cat "$ED/DF.DAT" 2>/dev/null)" "Q"
    check "Emcee: LLIST prints through the server" "$(cat "$ED/p.txt" 2>/dev/null)" '10 PRINT "KEPT"'
fi

echo "----"
echo "$pass passed, $fail failed, $skip skipped"
[ "$fail" -eq 0 ]
