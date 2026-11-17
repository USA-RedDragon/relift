#!/usr/bin/env bash
# Run every CI job this host can run, in parallel, the same scripts the
# workflows call. Prints each job's wall time and exits nonzero if any failed.
#
# Jobs named mac-* run on the macOS host CI_MAC (default: the ssh host "mac")
# from a copy of this working tree: the macOS tests, the macOS and iOS cross
# builds of the Game Boy, Game Boy Advance and Nintendo 64 programs, and relift for both Apple targets (the CI image builds those on
# Linux). With CI_MAC empty or the host unreachable they are reported as
# skipped. A job that takes longer than the timeout-minutes of the workflow
# job it stands for is killed, with its children, and fails. Jobs named image-* need docker and the pinned
# rust-cross image (about 16 GB unpacked); without docker, or without the
# image and room to pull it, they are reported as skipped. A skip never
# fails the run but is always printed with its reason.
. "$(dirname "$0")/lib.sh"

logs="$CI_WORK/logs"
mkdir -p "$logs"
rm -f "$logs"/*.status

jobs=(lint tests-workspace tests-gb-devices tests-relift tests-relift-e2e mesh-kat docs gb-corpus z80-sst z80-zexdoc generated workflows runtime-charter runtime-tests release-guard)
image_targets=(x86_64-unknown-linux-gnu aarch64-unknown-linux-gnu x86_64-pc-windows-gnullvm aarch64-pc-windows-gnullvm)
if [ "$(uname -s)" = Linux ]; then
  jobs+=(appimage cross-windows-x64 cross-windows-arm64 cross-android-arm64)
  jobs+=(cross-gba-windows-x64 cross-gba-windows-arm64 cross-gba-android-arm64)
  jobs+=(cross-n64-windows-x64 cross-n64-windows-arm64 cross-n64-android-arm64)
  for t in "${image_targets[@]}"; do jobs+=("image-$t"); done
fi
jobs+=(mac-tests mac-cross-macos-x64 mac-cross-ios-arm64 mac-cross-gba-macos-x64 mac-cross-gba-ios-arm64)
jobs+=(mac-cross-n64-macos-x64 mac-cross-n64-ios-arm64)
jobs+=(mac-release-aarch64-apple-darwin mac-release-x86_64-apple-darwin)

CI_MAC=${CI_MAC-mac}
mac_dir="relift-ci/$(basename "$ROOT")-$(printf %s "$ROOT" | sha256sum | cut -c1-8)"
mac_skip=
if [ -z "$CI_MAC" ]; then
  mac_skip="CI_MAC is empty"
elif ! ssh -o BatchMode=yes -o ConnectTimeout=8 "$CI_MAC" true 2>/dev/null; then
  mac_skip="ssh $CI_MAC is unreachable"
elif ! ssh -o BatchMode=yes "$CI_MAC" "mkdir -p $mac_dir/src" || ! git ls-files -z | rsync -a --from0 --files-from=- "$ROOT/" "$CI_MAC:$mac_dir/src/"; then
  mac_skip="could not copy the tree to $CI_MAC"
fi

image_skip=
image=$(sed -n 's/^CROSS_IMAGE=//p' "$ROOT/scripts/ci/cross-image.sh")
if ! docker info >/dev/null 2>&1; then
  image_skip="docker is not available"
elif ! docker image inspect "$image" >/dev/null 2>&1; then
  free=$(df -Pk "$(docker info --format '{{.DockerRootDir}}' 2>/dev/null || echo /)" 2>/dev/null | awk 'NR==2 {print int($4 / 1048576)}')
  [ "${free:-0}" -ge 24 ] || image_skip="the rust-cross image is not pulled and only ${free:-0} GB are free (it needs about 16 GB)"
fi

# limit_minutes NAME: the timeout-minutes of the workflow job that runs NAME.
limit_minutes() {
  local wf=
  case $1 in
    lint | tests-workspace | tests-gb-devices | tests-relift | tests-relift-e2e | docs | gb-corpus | z80-sst | z80-zexdoc | generated | workflows | appimage) wf=ci.yaml:$1 ;;
    mac-tests) wf=ci.yaml:macos ;;
    image-*) wf=ci.yaml:cross ;;
    runtime-charter) wf=runtime.yaml:charter ;;
    runtime-tests) wf=runtime.yaml:c-tests ;;
    release-guard) wf=cross.yaml:release-guard ;;
    mac-cross-*) wf=cross.yaml:macos ;;
    cross-*) wf=cross.yaml:linux ;;
    mac-release-*) wf=release-mac.yaml:relift ;;
  esac
  [ -n "$wf" ] || return 0
  awk -v want="${wf#*:}" '
    /^  [A-Za-z0-9_-]+:$/ { in_job = ($1 == want ":") }
    in_job && $1 == "timeout-minutes:" { print $2; exit }
  ' "$ROOT/.github/workflows/${wf%%:*}"
}

# on_mac CMD: run CMD on the mac in the copied tree, with its own work dirs.
on_mac() {
  ssh -tt -o BatchMode=yes "$CI_MAC" "eval \"\$(/opt/homebrew/bin/brew shellenv 2>/dev/null || true)\"
    cd \"\$HOME/$mac_dir/src\" && export CI_WORK=\"\$HOME/$mac_dir/work\" CARGO_TARGET_DIR=\"\$HOME/$mac_dir/target\" CARGO_BUILD_JOBS=4 && $1"
}

# job NAME: run one job; exit 77 means skipped, with the reason on stdout.
job() {
  case $1 in
    cross-gba-*) "$ROOT/scripts/ci/gbcross.sh" "${1#cross-gba-}" --guest gba --jobs 4 ;;
    cross-n64-*) "$ROOT/scripts/ci/gbcross.sh" "${1#cross-n64-}" --guest n64 --jobs 4 ;;
    cross-*) "$ROOT/scripts/ci/gbcross.sh" "${1#cross-}" --jobs 4 ;;
    image-*)
      [ -z "$image_skip" ] || { echo "$image_skip"; return 77; }
      "$ROOT/scripts/ci/cross-image.sh" "${1#image-}"
      ;;
    mac-*)
      [ -z "$mac_skip" ] || { echo "$mac_skip"; return 77; }
      case $1 in
        mac-tests) on_mac scripts/ci/tests.sh ;;
        mac-cross-gba-*) on_mac "scripts/ci/gbcross.sh ${1#mac-cross-gba-} --guest gba --jobs 4" ;;
        mac-cross-n64-*) on_mac "scripts/ci/gbcross.sh ${1#mac-cross-n64-} --guest n64 --jobs 4" ;;
        mac-cross-*) on_mac "scripts/ci/gbcross.sh ${1#mac-cross-} --jobs 4" ;;
        mac-release-*) on_mac "cargo build --release --locked -p relift --target ${1#mac-release-}" ;;
      esac
      ;;
    *) "$ROOT/scripts/ci/$1.sh" ;;
  esac
}

for j in "${jobs[@]}"; do
  (
    s=$(date +%s)
    set +e
    limit=$(limit_minutes "$j")
    set -m
    job "$j" >"$logs/$j.log" 2>&1 &
    pid=$!
    killed=
    while kill -0 "$pid" 2>/dev/null; do
      if [ -n "$limit" ] && [ $(($(date +%s) - s)) -ge $((limit * 60)) ]; then
        kill -TERM -- "-$pid" 2>/dev/null
        sleep 3
        kill -KILL -- "-$pid" 2>/dev/null
        killed=1
        break
      fi
      sleep 1
    done
    wait "$pid" 2>/dev/null
    rc=$?
    set +m
    set -e
    if [ -n "$killed" ]; then
      echo "killed after $limit minutes (the workflow's timeout-minutes)" >>"$logs/$j.log"
      rc=1
    fi
    case $rc in
      0) r=ok ;;
      77) r=SKIP ;;
      *) r=FAIL ;;
    esac
    note="$logs/$j.log"
    [ "$r" = SKIP ] && note="skipped: $(tail -n 1 "$logs/$j.log")"
    printf '%-40s %-4s %5ss  %s\n' "$j" "$r" "$(($(date +%s) - s))" "$note" >"$logs/$j.status"
  ) &
done
wait

fail=0
for j in "${jobs[@]}"; do
  cat "$logs/$j.status"
  grep -qE ' (ok|SKIP) ' "$logs/$j.status" || fail=1
done
exit "$fail"
