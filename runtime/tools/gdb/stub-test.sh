#!/bin/sh
# Tests the runtime's GDB server (core/gdbstub.c) with a real gdb: starts the
# Switch layer's test guest serving on a port (the switch_tests test
# gdb_serve_for_a_debugger), then gdb in batch mode attaches, lists the
# threads, reads x0 and guest memory, writes x2, stops at a breakpoint and a
# write watchpoint (x86-64 hosts), writes a word, and detaches.
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

if ! command -v gdb >/dev/null 2>&1; then
    echo "stub-test: skipped, gdb is not installed"
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
gdb -batch -nx -iex "set debuginfod enabled off" -ex "set architecture aarch64" ${GDB_DEBUG:+-ex "set debug remote 1"} \
    -ex "target remote 127.0.0.1:$port" -ex 'info threads' -ex 'p/x $x0' -ex "x/2gx $heap" \
    -ex 'set $x2 = 0x42' -ex 'break *0x8001000' -ex continue -ex 'p/x $pc' -ex delete \
    -ex "watch *(long *)($heap + 0x40)" -ex continue -ex continue -ex continue -ex continue -ex delete \
    -ex "set {int}($heap + 16) = 1" -ex detach >"$out/gdb.log" 2>&1 || true
wait $pid
rc=$?
fail=0
expect() {
    grep -q "$1" "$out/gdb.log" || { echo "stub-test: gdb output lacks: $1"; fail=1; }
}
expect 'Thread 1 '
expect '0x123456789abcdef'
expect '0x1122334455667788'
expect '0x99aabbccddeeff00'
if [ "$(uname -m)" = x86_64 ]; then
    expect 'Breakpoint 1, 0x0000000008001000'
    expect 'atchpoint 2: '
    expect 'New value'
fi
[ $rc -eq 0 ] || { echo "stub-test: the test guest failed ($rc)"; cat "$out/serve.log"; fail=1; }
[ $fail -eq 0 ] || { cat "$out/gdb.log"; exit 1; }
echo "stub-test: ok"
