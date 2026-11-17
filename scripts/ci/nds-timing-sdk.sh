#!/usr/bin/env bash
# The Nintendo DS timing test ROM that ran on the consoles (check/nds-timing/sdk),
# built in the pinned devkitPro image, through relift and compared with the
# consoles' results (e2e_nds_timing), and its main RAM arbitration build
# (make ARB=1, with short give-ups) the same way.
. "$(dirname "$0")/lib.sh"

DEVKITARM_IMAGE=devkitpro/devkitarm@sha256:116afba8df8453961de2936ffab20dd441edf4d682856c1ec8b0e53d7ed0bbf5
OWNER="$(id -u):$(id -g)"
export OWNER

src="$CI_WORK/nds-timing-sdk"
arb="$CI_WORK/nds-timing-arb"
rm -rf "$src" "$arb"
mkdir -p "$src" "$arb"
cp -r "$ROOT/check/nds-timing/." "$src/"
cp -r "$ROOT/check/nds-timing/." "$arb/"
docker pull -q "$DEVKITARM_IMAGE" >/dev/null
docker run --rm -v "$src:/src" -v "$arb:/arb" -w /src/sdk -e OWNER "$DEVKITARM_IMAGE" bash -euo pipefail -c '
  trap "chown -R \"\$OWNER\" /src /arb" EXIT
  make relift-hwtest-ntr.nds
  make -C /arb/sdk ARB=1 ARB_SPINS=16384u ARB_WAIT9=4194304u relift-hwtest-ntr.nds
'
# The workspace crates optimized as their dependencies are: relift lifts the ROM
# about twice as fast as at opt-level 0, without the LTO of a release build.
RELIFT_NDS_TIMING_ROM="$src/sdk/relift-hwtest-ntr.nds" RELIFT_NDS_ARB_ROM="$arb/sdk/relift-hwtest-ntr.nds" \
  CARGO_PROFILE_NDSTIMING_INHERITS=dev CARGO_PROFILE_NDSTIMING_OPT_LEVEL=2 \
  cargo test -p relift --locked --profile ndstiming --test e2e_nds_timing -- --nocapture
