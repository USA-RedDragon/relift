#!/usr/bin/env bash
# Check that all.sh stops cleanly: run it on a copy of scripts/ci whose lint
# job is a slow fake that starts three sleepers (one in a session of its own,
# which a process group kill misses), replace the copy of all.sh while it
# runs, send it TERM, and check that it exits 143 and that no process of the
# fake job is left.
. "$(dirname "$0")/lib.sh"

tree="$CI_WORK/all-stops"
rm -rf "$tree"
mkdir -p "$tree/scripts/ci" "$tree/work" "$tree/.github"
cp -r .github/workflows "$tree/.github/"
cp scripts/ci/all.sh scripts/ci/lib.sh scripts/ci/cross-image.sh "$tree/scripts/ci/"
marks="$tree/pids"
: >"$marks"
cat >"$tree/scripts/ci/lint.sh" <<'EOF'
#!/usr/bin/env bash
sleep 600 &
echo "$!" >>"$MARKS"
sleep 600 &
echo "$!" >>"$MARKS"
setsid sleep 600 &
echo "$!" >>"$MARKS"
wait
EOF
chmod +x "$tree/scripts/ci/lint.sh"

MARKS="$marks" CI_WORK="$tree/work" CI_MAC= "$tree/scripts/ci/all.sh" lint >"$tree/out.log" 2>&1 &
run=$!
for _ in $(seq 1 600); do
  [ "$(wc -l <"$marks")" -ge 3 ] && break
  sleep 0.1
done
[ "$(wc -l <"$marks")" -ge 3 ] || {
  echo "all-stops: the fake job did not start" >&2
  kill -KILL "$run" 2>/dev/null
  exit 1
}

printf '#!/usr/bin/env bash\nexit 9\n' >"$tree/scripts/ci/all.sh"
kill -TERM "$run"
set +e
wait "$run"
rc=$?
set -e

fail=0
[ "$rc" -eq 143 ] || {
  echo "all-stops: all.sh exited $rc after TERM, not 143" >&2
  fail=1
}
for _ in $(seq 1 50); do
  left=0
  while read -r p; do
    kill -0 "$p" 2>/dev/null && left=1
  done <"$marks"
  [ "$left" -eq 0 ] && break
  sleep 0.1
done
while read -r p; do
  if kill -0 "$p" 2>/dev/null; then
    echo "all-stops: process $p of the stopped job is still running" >&2
    kill -KILL "$p" 2>/dev/null
    fail=1
  fi
done <"$marks"
[ ! -e "$tree/work/logs/lint.pgid" ] || {
  echo "all-stops: the job's process group file was left behind" >&2
  fail=1
}
[ "$fail" -eq 0 ] && echo "all-stops: ok"
exit "$fail"
