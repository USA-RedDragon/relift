#!/usr/bin/env bash
# Run every CI job this host can run, in parallel, the same scripts the
# workflows call. Prints each job's wall time and exits nonzero if any failed.
. "$(dirname "$0")/lib.sh"

logs="$CI_WORK/logs"
mkdir -p "$logs"

jobs=(checks gb-corpus generated workflows runtime-charter runtime-tests release-guard)
[ "$(uname -s)" = Linux ] && jobs+=(appimage)

for j in "${jobs[@]}"; do
  (
    s=$(date +%s)
    if "$ROOT/scripts/ci/$j.sh" >"$logs/$j.log" 2>&1; then r=ok; else r=FAIL; fi
    printf '%-16s %-4s %5ss  %s\n' "$j" "$r" "$(($(date +%s) - s))" "$logs/$j.log" >"$logs/$j.status"
  ) &
done
wait

fail=0
for j in "${jobs[@]}"; do
  cat "$logs/$j.status"
  grep -q ' ok ' "$logs/$j.status" || fail=1
done
exit "$fail"
