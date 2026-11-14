#!/bin/sh
# Tests relift_frames.py: builds a C program whose functions carry relift
# host names, stops in the innermost one under gdb, and checks that the
# backtrace shows the guest names with the filter and the raw names without.
# Then builds a program with two fibers (core/fiber.c) and checks that
# relift-fibers lists them and relift-fiber-bt shows the parked one.
#
#   sh tools/gdb/test.sh [build dir]    (from runtime/; CC defaults to cc)
#
# Skips with exit status 0 when gdb is not installed.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
build=${1:-build/host}/gdb
cc=${CC:-cc}

if ! command -v gdb >/dev/null 2>&1; then
    echo "gdb-test: skipped, gdb is not installed"
    exit 0
fi

mkdir -p "$build"
core=$here/../../core
$cc -std=c11 -O0 -g -fno-omit-frame-pointer -o "$build/frames" "$here/test/frames.c"
$cc -std=c11 -O0 -g -fno-omit-frame-pointer -D_GNU_SOURCE -I"$core" -o "$build/fibers" "$here/test/fibers.c" \
    "$core/fiber.c" "$core"/fiber_*.S

run_gdb() {
    gdb -batch -nx -iex 'set debuginfod enabled off' "$@" \
        -ex 'break _ZN5guest4main3fooEv' -ex run -ex bt \
        "$build/frames" 2>&1 | grep '^#' || true
}

with=$(run_gdb -ex "source $here/relift_frames.py")
without=$(run_gdb)
name=$(gdb -batch -nx -iex 'set debuginfod enabled off' -ex "source $here/relift_frames.py" \
    -ex 'relift-guest-name _ZN5guest3sdk3foo3barERKS1_' 2>&1)

fibers=$(gdb -batch -nx -iex 'set debuginfod enabled off' -ex "source $here/relift_frames.py" \
    -ex 'break _ZN5guest4main3fooEv' -ex run -ex relift-fibers -ex 'echo ---parked\n' -ex 'relift-fiber-bt 1' \
    -ex 'echo ---after\n' -ex bt -ex 'relift-fiber-bt 2' "$build/fibers" 2>&1)
parked=$(printf '%s\n' "$fibers" | sed -n '/^---parked/,/^---after/p')
after=$(printf '%s\n' "$fibers" | sed -n '/^---after/,$p')

fail=0
expect() {
    case "$2" in
        *"$3"*) ;;
        *) echo "gdb-test: $1 lacks: $3"; fail=1 ;;
    esac
}
reject() {
    case "$2" in
        *"$3"*) echo "gdb-test: $1 has: $3"; fail=1 ;;
        *) ;;
    esac
}

expect "backtrace" "$with" "#0  guest::main::foo() ("
expect "backtrace" "$with" "guest::main::bar(int) ("
expect "backtrace" "$with" "guest::main::fn_00123450 ("
expect "backtrace" "$with" " main () at "
reject "backtrace" "$with" "_ZN5guest4main3fooEv ("
expect "control backtrace" "$without" "_ZN5guest4main3fooEv ("
reject "control backtrace" "$without" "guest::main::foo() ("
expect "relift-guest-name" "$name" "guest::sdk::foo::bar(guest::sdk::foo const&)"
expect "relift-fibers" "$fibers" "guest CPU 0    running"
expect "relift-fibers" "$fibers" "guest CPU 1    parked"
expect "relift-fibers" "$fibers" "guest CPU 2    not started"
expect "relift-fiber-bt" "$parked" "guest::main::deep(int) ("
expect "relift-fiber-bt" "$parked" "#12 "
expect "relift-fiber-bt" "$parked" "cpu1 ("
expect "relift-fiber-bt" "$parked" "rl_fiber_start"
reject "relift-fiber-bt" "$parked" "guest::main::foo()"
reject "relift-fiber-bt" "$parked" "Backtrace stopped"
expect "bt after relift-fiber-bt" "$after" "#0  guest::main::foo() ("
expect "bt after relift-fiber-bt" "$after" "cpu0 ("
reject "bt after relift-fiber-bt" "$after" "guest::main::deep"
expect "relift-fiber-bt of a fiber that never ran" "$after" "has not run yet"

if [ "$fail" -ne 0 ]; then
    echo "--- with the filter"
    echo "$with"
    echo "--- without"
    echo "$without"
    echo "--- fibers"
    echo "$fibers"
    exit 1
fi
echo "$with"
echo "$parked"
echo "gdb-test: ok"
