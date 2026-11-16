#!/usr/bin/env bash
# Regenerate the Sail templates in a copy of the tree and check they match
# the committed ones. The copy keeps a parallel checks.sh from compiling
# half-written templates.
. "$(dirname "$0")/lib.sh"

src="$CI_WORK/generated-src"
rm -rf "$src"
mkdir -p "$src"
git ls-files -z | rsync -a --delete --from0 --files-from=- "$ROOT/" "$src/"
cd "$src"
export CARGO_TARGET_DIR="$CI_WORK/generated-target"

[ -d "$CI_WORK/sail/lib" ] || cpus/arm64/gen/fetch-sail.sh "$CI_WORK/sail"
cpus/arm64/gen/regen.sh "$CI_WORK/sail/lib" "$CI_WORK/sail/model"
cpus/sm83/gen/regen.sh "$CI_WORK/sail/lib"
cpus/z80/gen/regen.sh "$CI_WORK/sail/lib"
[ -d "$CI_WORK/sail-arm32/lib" ] || cpus/arm32/gen/fetch-sail.sh "$CI_WORK/sail-arm32"
cpus/arm32/gen/regen.sh "$CI_WORK/sail-arm32/lib" "$CI_WORK/sail-arm32/model"
[ -d "$CI_WORK/sail-mips/lib" ] || cpus/mips/gen/fetch-sail.sh "$CI_WORK/sail-mips"
cpus/mips/gen/regen.sh "$CI_WORK/sail-mips/lib" "$CI_WORK/sail-mips/model"
cpus/rsp/gen/regen.sh "$CI_WORK/sail-mips/lib" "$CI_WORK/sail-mips/model"

fail=0
for p in cpus/arm64/src/lift/sail/generated cpus/sm83/src/lift/sail/generated.rs \
  cpus/arm32/src/decode/generated.rs cpus/arm32/src/lift/generated \
  cpus/mips/src/vr4300/decode/generated.rs cpus/mips/src/vr4300/lift/generated \
  cpus/mips/src/r3000a/decode/generated.rs cpus/mips/src/r3000a/lift/generated \
  cpus/rsp/src/decode/generated.rs cpus/rsp/src/lift/generated \
  cpus/z80/src/lift/generated.rs; do
  diff -r -u "$ROOT/$p" "$src/$p" || fail=1
done
exit "$fail"
