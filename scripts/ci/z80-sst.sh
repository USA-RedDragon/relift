#!/usr/bin/env bash
# Run the SingleStepTests z80 v1 set (1604 files, 1000 cases each) through
# relift-check: every Z80 instruction template on the IR interpreter against
# the registers, RAM, ports, T-states and bus accesses of each case, then the
# same run with every lifted block broken, which must be caught.
. "$(dirname "$0")/lib.sh"

sst_commit=ebe1875d48f374bcfd4b505d8eb8ee751568b5f7
sst_sha256=f44683f0edf1c1c37f98861ccd1bfda3b9bce65b5b258d4817a0f1d2012076a7
tgz="$CI_WORK/z80-sst-$sst_commit.tar.gz"
data="$CI_WORK/z80-sst-$sst_commit"

fetch "https://github.com/SingleStepTests/z80/archive/$sst_commit.tar.gz" "$sst_sha256" "$tgz"
if [ ! -d "$data/v1" ]; then
  rm -rf "$data"
  mkdir -p "$data"
  tar -xzf "$tgz" -C "$data" --strip-components=1 --wildcards '*/v1/*'
fi

cargo build --release --locked --manifest-path check/Cargo.toml
bin="${CARGO_TARGET_DIR:-check/target}/release/relift-check"

"$bin" --oracle sst-z80 run "$data/v1"
"$bin" --oracle sst-z80 run "$data/v1" --mutate flip-const
