#!/usr/bin/env bash
# Regenerate the Sail templates and check they match the committed ones.
. "$(dirname "$0")/lib.sh"

[ -d "$CI_WORK/sail/lib" ] || cpus/a64/gen/fetch-sail.sh "$CI_WORK/sail"
cpus/a64/gen/regen.sh "$CI_WORK/sail/lib" "$CI_WORK/sail/model"
git diff --exit-code -- cpus/a64/src/lift/sail/generated

cpus/sm83/gen/regen.sh "$CI_WORK/sail/lib"
git diff --exit-code -- cpus/sm83/src/lift/sail/generated.rs

[ -d "$CI_WORK/sail-arm32/lib" ] || cpus/arm32/gen/fetch-sail.sh "$CI_WORK/sail-arm32"
cpus/arm32/gen/regen.sh "$CI_WORK/sail-arm32/lib" "$CI_WORK/sail-arm32/model"
git diff --exit-code -- cpus/arm32/src/decode/generated.rs cpus/arm32/src/lift/generated
