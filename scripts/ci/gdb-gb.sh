#!/usr/bin/env bash
# The GDB server on a Game Boy program, with a real gdb (gbz80): makes a
# 32 KiB test cart whose code sets a and b, then in a loop reads the byte at 0xC000 into e and counts c, builds it
# with `relift build --dev` against this runtime, attaches gdb to the running
# program, reads the registers and the cart's bytes, and detaches. Then it
# runs the cart to a fixed frame with and without that stop and compares the
# frame hash and the instruction count, which must not change.
# Usage: gdb-gb.sh [relift binary]   (default target/debug/relift)
# Skips with exit status 0 when gdb does not know gbz80, unless CI is set.
. "$(dirname "$0")/lib.sh"
root=$ROOT
relift=${1:-${CARGO_TARGET_DIR:-$root/target}/debug/relift}
# A distribution's plain gdb may know the host only; gdb-multiarch knows gbz80. GDB=<program> overrides the search.
gdb_bin=${GDB:-}
if [ -z "$gdb_bin" ]; then
  for cand in gdb-multiarch gdb; do
    if command -v "$cand" >/dev/null 2>&1 && "$cand" -batch -nx -ex "set architecture gbz80" >/dev/null 2>&1; then
      gdb_bin=$cand
      break
    fi
  done
fi
if [ -z "$gdb_bin" ]; then
  if [ -n "${CI:-}" ]; then echo "gdb-gb: failed, no gdb with the gbz80 target"; exit 1; fi
  echo "gdb-gb: skipped, no gdb with the gbz80 target"
  exit 0
fi
work=${GDB_GB_WORK:-$CI_WORK/gdb-gb}
rm -rf "$work"
mkdir -p "$work/proj"

python3 -I - "$work/loop.gb" <<'EOF'
import sys
logo = bytes.fromhex("CEED6666CC0D000B03730083000C000D0008111F8889000EDCCC6EE6DDDDD999BBBB67636E0EECCCDDDC999FBBB9333E")
rom = bytearray(0x8000)
rom[0x100:0x104] = bytes([0x00, 0xC3, 0x50, 0x01])  # nop; jp $0150
rom[0x104:0x134] = logo
rom[0x134:0x13A] = b"GDBGB\0"
# ld a,$42; ld b,$13; loop: ld hl,$C000; ld e,[hl]; inc c; jr loop
rom[0x150:0x15B] = bytes([0x3E, 0x42, 0x06, 0x13, 0x21, 0x00, 0xC0, 0x5E, 0x0C, 0x18, 0xF9])
c = 0
for b in rom[0x134:0x14D]:
    c = (c - b - 1) & 0xFF
rom[0x14D] = c
g = (sum(rom) - rom[0x14E] - rom[0x14F]) & 0xFFFF
rom[0x14E] = g >> 8
rom[0x14F] = g & 0xFF
open(sys.argv[1], "wb").write(rom)
EOF

cd "$work/proj"
"$relift" import "$work/loop.gb" --cache-dir "$work/cache" >"$work/import.log" 2>&1 ||
  { cat "$work/import.log"; exit 1; }
"$relift" build --dev --cache-dir "$work/cache" --runtime-dir "$root/runtime" --package.appimage never \
  >"$work/build.log" 2>&1 || { cat "$work/build.log"; exit 1; }
prog=$(sed -n 's/^  output: //p' "$work/build.log")
[ -x "$prog" ] || { echo "gdb-gb: no program in the build log"; cat "$work/build.log"; exit 1; }

frames=3000
# The reference run: no debugger.
"$prog" --headless --frames $frames --no-save --no-audio --stats >"$work/ref.out" 2>"$work/ref.err"

# The run gdb stops halfway through, by time: attach, read, detach.
port=$((30000 + $$ % 20000))
RELIFT_DEBUG_GDB=$port "$prog" --headless --frames $frames --no-save --no-audio --stats \
  >"$work/run.out" 2>"$work/run.err" &
pid=$!
for _ in $(seq 1 200); do
  grep -q 'GDB server on' "$work/run.err" && break
  sleep 0.05
done
"$gdb_bin" -batch -nx -iex "set debuginfod enabled off" -ex "set architecture gbz80" \
  -ex "target remote 127.0.0.1:$port" -ex 'info threads' -ex 'p/x $af' -ex 'p/x $bc' -ex 'p/x $pc' \
  -ex 'x/11xb 0x150' \
  -ex 'maint packet P1=ed77' -ex 'set {char}0xC000 = 0x55' -ex 'p/x {char}0xC000' \
  -ex 'set {char}0x4000 = 1' -ex 'set {char}0xFF40 = 1' -ex detach >"$work/gdb.log" 2>&1 || true
# A second attach after the program ran on: the cart read the byte into e, and b kept the value written.
sleep 0.5
"$gdb_bin" -batch -nx -iex "set debuginfod enabled off" -ex "set architecture gbz80" \
  -ex "target remote 127.0.0.1:$port" -ex 'p/x $de' -ex 'p/x $bc' -ex detach >"$work/gdb2.log" 2>&1 || true
rc=0
wait $pid || rc=$?

fail=0
expect() {
  grep -q "$2" "$1" || { echo "gdb-gb: $(basename "$1") lacks: $2"; fail=1; }
}
expect "$work/gdb.log" 'Thread 1 '
expect "$work/gdb.log" '^\$1 = 0x42[0-9a-f][0-9a-f]$'
expect "$work/gdb.log" '^\$2 = 0x13[0-9a-f][0-9a-f]$'
expect "$work/gdb.log" '0x150:.0x3e.0x42.0x06.0x13.0x21.0x00.0xc0.0x5e'
# gdb sends a register write when it resumes, not on detach, so the register write is the raw P packet (bc = 0x77ed).
# A register and a work RAM byte written while stopped are what the cart reads after: e is the byte, b the register.
expect "$work/gdb.log" '^\$4 = 0x55$'
expect "$work/gdb2.log" '^\$1 = 0x[0-9a-f]*55$'
expect "$work/gdb2.log" '^\$2 = 0x77[0-9a-f][0-9a-f]$'
# ROM, mapper and I/O writes are refused.
expect "$work/gdb.log" 'Cannot access memory at address 0x4000'
expect "$work/gdb.log" 'Cannot access memory at address 0xff40'
grep -q '= 0x15[4-9a]' "$work/gdb.log" || { echo "gdb-gb: pc is not in the loop"; fail=1; }
[ $rc -eq 0 ] || { echo "gdb-gb: the stopped run failed ($rc)"; fail=1; }
# The same frame and the same machine state at the end, stopped or not.
ref=$(grep -o 'frame [0-9a-f]*' "$work/ref.out" | tail -1)
got=$(grep -o 'frame [0-9a-f]*' "$work/run.out" | tail -1)
[ -n "$ref" ] && [ "$ref" = "$got" ] || { echo "gdb-gb: frame differs: '$ref' and '$got'"; fail=1; }
# The instructions run to that frame (--stats steps), which a moved clock would change; none interpreted.
refc=$(grep -o 'steps [0-9]*' "$work/ref.out" "$work/ref.err" | tail -1 | sed 's/.*steps //')
gotc=$(grep -o 'steps [0-9]*' "$work/run.out" "$work/run.err" | tail -1 | sed 's/.*steps //')
[ -n "$refc" ] && [ "$refc" = "$gotc" ] || { echo "gdb-gb: steps differ: '$refc' and '$gotc'"; fail=1; }
grep -q 'interpreter rom 0 entries 0 instructions, ram 0 entries 0 instructions' "$work/run.err" "$work/run.out" ||
  { echo "gdb-gb: the run interpreted instructions"; fail=1; }
[ $fail -eq 0 ] || { cat "$work/gdb.log" "$work/gdb2.log" "$work/run.err"; exit 1; }
echo "gdb-gb: ok ($ref, $refc instructions)"
