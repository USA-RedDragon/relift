#!/usr/bin/env bash
# The relift crate's end-to-end test targets (every tests/e2e*.rs). With the
# argument slow, only the targets listed below; with rest, every other one, so
# a new target lands in exactly one of the two; with none, all of them.
# Usage: tests-relift-e2e.sh [slow|rest]
# The targets that build Switch programs run a second time with emit.trampoline.
. "$(dirname "$0")/lib.sh"

slow=(e2e_gb e2e_hotload e2e_n64)

all=()
while read -r name; do
  all+=("$name")
done < <(cargo metadata --no-deps --format-version 1 --locked |
  jq -r '.packages[] | select(.name == "relift") | .targets[] | select(.kind | index("test")) | .name | select(startswith("e2e"))')

for name in "${slow[@]}"; do
  case " ${all[*]} " in
    *" $name "*) ;;
    *) echo "tests-relift-e2e.sh lists $name as slow, but relift has no such e2e target" >&2; exit 1 ;;
  esac
done

args=()
for name in "${all[@]}"; do
  is_slow=
  for s in "${slow[@]}"; do
    [ "$s" != "$name" ] || is_slow=1
  done
  case ${1:-all} in
    all) args+=(--test "$name") ;;
    slow) [ -z "$is_slow" ] || args+=(--test "$name") ;;
    rest) [ -n "$is_slow" ] || args+=(--test "$name") ;;
    *) echo "usage: tests-relift-e2e.sh [slow|rest]" >&2; exit 2 ;;
  esac
done
[ ${#args[@]} -gt 0 ] || { echo "no e2e targets selected" >&2; exit 1; }

cargo test -p relift --locked "${args[@]}"

# The targets whose programs exercise control flow that trampoline mode changes (unwinding,
# returns to rewritten addresses, missed calls, the tiny program) run again with every return
# through dispatch (emit.trampoline); their output must not change. Nothing else is repeated:
# the Linux corpus check runs every program in both modes.
both=()
for name in "${all[@]}"; do
  case " e2e e2e_eh e2e_ret e2e_hotload " in
    *" $name "*)
      for a in "${args[@]}"; do
        [ "$a" != "$name" ] || both+=(--test "$name")
      done ;;
  esac
done
if [ ${#both[@]} -gt 0 ]; then
  RELIFT_EMIT_TRAMPOLINE=true cargo test -p relift --locked "${both[@]}"
fi
