#!/usr/bin/env bash
# The Nintendo DS timing test ROM that ran on the consoles (check/nds-timing/sdk),
# built in the pinned devkitPro image, through relift and compared with the
# consoles' results (e2e_nds_timing), and its main RAM arbitration build
# (make ARB=1, with short give-ups) the same way. With the argument timing,
# only the timing ROM's tests; with arb, only the arbitration ROM's; with
# none, both.
# Usage: nds-timing-sdk.sh [timing|arb]
. "$(dirname "$0")/lib.sh"

part=${1:-all}
case $part in
  all | timing | arb) ;;
  *) echo "usage: nds-timing-sdk.sh [timing|arb]" >&2; exit 2 ;;
esac

DEVKITARM_IMAGE=devkitpro/devkitarm@sha256:116afba8df8453961de2936ffab20dd441edf4d682856c1ec8b0e53d7ed0bbf5
OWNER="$(id -u):$(id -g)"
export OWNER PART=$part

src="$CI_WORK/nds-timing-sdk"
arb="$CI_WORK/nds-timing-arb"
rm -rf "$src" "$arb"
mkdir -p "$src" "$arb"
cp -r "$ROOT/check/nds-timing/." "$src/"
cp -r "$ROOT/check/nds-timing/." "$arb/"
docker pull -q "$DEVKITARM_IMAGE" >/dev/null
docker run --rm "${DOCKER_CPUS[@]}" -v "$src:/src" -v "$arb:/arb" -w /src/sdk -e OWNER -e PART "$DEVKITARM_IMAGE" bash -euo pipefail -c '
  trap "chown -R \"\$OWNER\" /src /arb" EXIT
  [ "$PART" = arb ] || make relift-hwtest-ntr.nds
  [ "$PART" = timing ] || make -C /arb/sdk ARB=1 ARB_SPINS=16384u ARB_WAIT9=4194304u relift-hwtest-ntr.nds
'

filter=()
case $part in
  timing) filter=(--skip main_ram_waits_for_the_other_cpus_stream_to_end_as_on_the_consoles) ;;
  arb) filter=(main_ram_waits_for_the_other_cpus_stream_to_end_as_on_the_consoles --exact) ;;
esac
roms=()
[ "$part" = arb ] || roms+=(RELIFT_NDS_TIMING_ROM="$src/sdk/relift-hwtest-ntr.nds")
[ "$part" = timing ] || roms+=(RELIFT_NDS_ARB_ROM="$arb/sdk/relift-hwtest-ntr.nds")
# The workspace crates optimized as their dependencies are: relift lifts the ROM
# about twice as fast as at opt-level 0, without the LTO of a release build.
env "${roms[@]}" CARGO_PROFILE_NDSTIMING_INHERITS=dev CARGO_PROFILE_NDSTIMING_OPT_LEVEL=2 \
  cargo test -p relift --locked --profile ndstiming --test e2e_nds_timing -- --nocapture "${filter[@]}"
