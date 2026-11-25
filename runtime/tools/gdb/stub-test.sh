#!/bin/sh
# Tests the runtime's GDB server (core/gdbstub.c) with a real gdb: starts the
# Switch layer's test guest serving on a port (the switch_tests test
# gdb_serve_for_a_debugger), then gdb in batch mode attaches, lists the
# threads, walks the guest backtrace of three nested calls, reads x0 and
# guest memory, writes x2, stops at a breakpoint and a write watchpoint
# (x86-64 hosts), writes a word, and detaches.
#
#   sh tools/gdb/stub-test.sh [build dir]    (from runtime/, after make)
#
# Skips with exit status 0 when gdb is not installed.
set -eu

build=${1:-build/host}
tests=$build/switch_tests
here=$(cd "$(dirname "$0")" && pwd)
core=$here/../../core

# A player package builds the runtime with RL_NO_DEBUG_STUB: no server is in the program.
mkdir -p "$build/gdb-stub"
${CC:-cc} -std=c11 -D_GNU_SOURCE -DRL_BUILDING_RUNTIME -DRL_NO_DEBUG_STUB -I"$core/include" -I"$core" -c \
    "$core/gdbstub.c" -o "$build/gdb-stub/off.o"
if nm "$build/gdb-stub/off.o" | grep -q 'rl_gdb'; then
    echo "stub-test: gdbstub.c built with RL_NO_DEBUG_STUB still defines rl_gdb symbols"; exit 1
fi

# The stub speaks AArch64 registers, so the debugger must know that target. A distribution's plain gdb is often
# built for the host only (Ubuntu's: `set architecture aarch64` is an undefined item) and gdb-multiarch is the one
# that knows it. GDB=<program> overrides the search.
gdb_bin=${GDB:-}
if [ -z "$gdb_bin" ]; then
    for cand in gdb-multiarch gdb; do
        if command -v "$cand" >/dev/null 2>&1 && "$cand" -batch -nx -ex "set architecture aarch64" >/dev/null 2>&1; then
            gdb_bin=$cand
            break
        fi
    done
fi
if [ -z "$gdb_bin" ]; then
    if command -v gdb >/dev/null 2>&1 || command -v gdb-multiarch >/dev/null 2>&1; then
        msg="no installed gdb knows the aarch64 target (install gdb-multiarch)"
    else
        msg="gdb is not installed"
    fi
    if [ -n "${CI:-}" ]; then
        echo "stub-test: failed, $msg"
        exit 1
    fi
    echo "stub-test: skipped, $msg"
    exit 0
fi
out=$build/gdb-stub
mkdir -p "$out"
port=$((30000 + $$ % 20000))
RELIFT_VIDEO=headless RELIFT_GDB_TEST_PORT=$port "$tests" gdb_serve_for_a_debugger >"$out/serve.log" 2>&1 &
pid=$!
heap=
for _ in $(seq 1 200); do
    heap=$(sed -n 's/^gdb-serve: heap \(0x[0-9a-f]*\)$/\1/p' "$out/serve.log")
    [ -n "$heap" ] && break
    sleep 0.05
done
if [ -z "$heap" ]; then
    echo "stub-test: the test guest did not start"; cat "$out/serve.log"; kill $pid 2>/dev/null; exit 1
fi
"$gdb_bin" -batch -nx -iex "set debuginfod enabled off" -ex "set architecture aarch64" ${GDB_DEBUG:+-ex "set debug remote 1"} \
    -ex "source $here/relift_guest.py" -ex "target remote 127.0.0.1:$port" -ex 'info threads' -ex 'monitor bt' -ex bt -ex 'p/x $x0' -ex "x/2gx $heap" \
    -ex 'set $x2 = 0x42' -ex 'break *0x8001000' -ex continue -ex 'p/x $pc' -ex delete \
    -ex "watch *(long *)($heap + 0x40)" -ex continue -ex continue -ex continue -ex continue -ex delete \
    -ex "set {int}($heap + 16) = 1" -ex detach >"$out/gdb.log" 2>&1 || true
# The test guest ends when gdb has written its word; a failed run leaves it waiting for 30 s and exiting non-zero.
rc=0
wait $pid || rc=$?
fail=0
expect() {
    grep -q "$1" "$out/gdb.log" || { echo "stub-test: gdb output lacks: $1"; fail=1; }
}
expect 'Thread 1 '
expect '0x123456789abcdef'
expect '0x1122334455667788'
expect '0x99aabbccddeeff00'
# The test guest stops in the innermost of three nested calls (frame records at heap + 0xf00).
expect "#2 $(printf '0x%x' $((heap + 0x814)))"
expect "#2  $(printf '0x%016x' $((heap + 0x814))) in"
if [ "$(uname -m)" = x86_64 ]; then
    expect 'Breakpoint 1, 0x0000000008001000'
    expect 'atchpoint 2: '
    expect 'New value'
fi
[ $rc -eq 0 ] || { echo "stub-test: the test guest failed ($rc), $gdb_bin did not finish its session"; cat "$out/serve.log"; fail=1; }
[ $fail -eq 0 ] || { cat "$out/gdb.log"; exit 1; }
echo "stub-test: ok"
