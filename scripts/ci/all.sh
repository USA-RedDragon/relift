#!/usr/bin/env bash
# Run every CI job this host can run, in parallel, the same scripts the
# workflows call. Prints each job's wall time and exits nonzero if any failed.
# Usage: all.sh [--help | JOB...]
# With --help, prints this text and runs nothing. With names, runs only those
# jobs; a name may be a shell pattern, such as "mac-*", and one that matches no
# job fails and lists the valid names.
#
# Jobs named mac-* run on the macOS host CI_MAC (default: the ssh host "mac")
# from a copy of this working tree: the macOS tests, the macOS and iOS cross
# builds of the Game Boy, Game Boy Advance, Nintendo 64 and Master System
# programs, and relift for both Apple targets (the CI image builds those on
# Linux). With CI_MAC empty or the host unreachable they are reported as
# skipped. At most CI_MAC_JOBS of them run at once (default: a share of the
# Mac's cores and memory), the way each runs alone on a CI runner; a job's time
# starts when it does, and the wait is printed apart. Each has its own cargo
# target directory under ~/relift-ci/cache on the Mac, kept between runs like
# rust-cache, and wiped when it grows past CI_MAC_CACHE_GB (default 5)
# gigabytes. One lock, CI_MAC_LOCK (default ~/.cache/relift/mac.lock), is held
# for the whole mac phase, so two runs of this script never share the Mac and
# the second waits for the first.
#
# Every other job runs on this host in a slot of its own: a set of
# CI_SLOT_CPUS (default 4, the size of a hosted Linux runner) cores it is
# pinned to, with its docker container, so a job sees the cores a runner has
# and its time does not depend on how many jobs share the host. There are as
# many slots as whole sets fit in the cores this script may use, or CI_JOBS
# if that is set; a job waits for a free slot, and its time starts when it
# gets one.
#
# A job that takes longer than the timeout-minutes of the workflow job it
# stands for is killed, with its children, and fails. Jobs named image-* need
# docker and the pinned rust-cross image (about 16 GB unpacked); without
# docker, or without the image and room to pull it, they are reported as
# skipped. A skip never fails the run but is always printed with its reason.
#
# On TERM, INT or HUP it stops every job's process group (with its mac ssh
# session) and lets go of the mac lock before it exits. The whole script is
# one function called on its last line, so editing or replacing this file
# while it runs (a checkout or rebase of the tree) does not change the run.
main() {
  case ${1:-} in
    -h | --help)
      sed -n '2,/^[^#]/{/^#/s/^# \{0,1\}//p}' "$0"
      exit 0
      ;;
    -*)
      echo "all.sh: unknown option $1 (see all.sh --help)" >&2
      exit 2
      ;;
  esac

  . "$(dirname "$0")/lib.sh"

  logs="$CI_WORK/logs"
  mkdir -p "$logs"
  rm -f "$logs"/*.status "$logs"/*.pgid

  jobs=(lint tests-workspace tests-gb-devices tests-relift tests-relift-e2e mesh-kat nds-timing-sdk docs gb-corpus z80-sst z80-zexdoc generated workflows runtime-charter runtime-tests release-guard)
  image_targets=(x86_64-unknown-linux-gnu aarch64-unknown-linux-gnu x86_64-pc-windows-gnullvm aarch64-pc-windows-gnullvm)
  if [ "$(uname -s)" = Linux ]; then
    jobs+=(appimage cross-windows-x64 cross-windows-arm64 cross-android-arm64)
    jobs+=(cross-gba-windows-x64 cross-gba-windows-arm64 cross-gba-android-arm64)
    jobs+=(cross-n64-windows-x64 cross-n64-windows-arm64 cross-n64-android-arm64)
    jobs+=(cross-sms-windows-x64 cross-sms-windows-arm64 cross-sms-android-arm64)
    jobs+=(cross-nds-windows-x64 cross-nds-windows-arm64 cross-nds-android-arm64)
    for t in "${image_targets[@]}"; do jobs+=("image-$t"); done
  fi
  jobs+=(mac-tests-workspace mac-release-aarch64-apple-darwin mac-release-x86_64-apple-darwin mac-tests-relift)
  jobs+=(mac-tests-relift-e2e-slow mac-tests-relift-e2e-rest mac-cross-ios-arm64 mac-cross-gba-ios-arm64)
  jobs+=(mac-cross-n64-ios-arm64 mac-cross-sms-ios-arm64 mac-cross-macos-x64 mac-cross-gba-macos-x64)
  jobs+=(mac-cross-n64-macos-x64 mac-cross-sms-macos-x64 mac-runtime-tests)

  if [ $# -gt 0 ]; then
    picked=()
    for pat in "$@"; do
      found=
      for j in "${jobs[@]}"; do
        case $j in $pat) found=1 ;; *) continue ;; esac
        case " ${picked[*]-} " in *" $j "*) ;; *) picked+=("$j") ;; esac
      done
      [ -n "$found" ] || { echo "no job matches $pat; the jobs are: ${jobs[*]}" >&2; exit 2; }
    done
    want=("${jobs[@]}")
    jobs=()
    for j in "${want[@]}"; do
      case " ${picked[*]} " in *" $j "*) jobs+=("$j") ;; esac
    done
  fi

  has_mac=
  for j in "${jobs[@]}"; do
    case $j in mac-*) has_mac=1 ;; esac
  done

  CI_MAC=${CI_MAC-mac}
  mac_dir="relift-ci/$(basename "$ROOT")-$(printf %s "$ROOT" | sha256sum | cut -c1-8)"
  mac_skip=
  mac_cap=1
  if [ -z "$has_mac" ]; then
    :
  elif [ -z "$CI_MAC" ]; then
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

  # mac_default_cap: how many mac jobs to run at once, from the Mac's cores and
  # memory: one per three cores (the size of a hosted macOS runner) and one per
  # 2.5 GB, at least one.
  mac_default_cap() {
    local cpus mem
    read -r cpus mem < <(ssh -o BatchMode=yes "$CI_MAC" 'sysctl -n hw.ncpu hw.memsize' | tr '\n' ' ')
    local by_cpu=$(((cpus + 2) / 3)) by_mem=$((mem / 2684354560))
    local cap=$((by_cpu < by_mem ? by_cpu : by_mem))
    echo $((cap < 1 ? 1 : cap))
  }

  # limit_minutes NAME: the timeout-minutes of the workflow job that runs NAME.
  limit_minutes() {
    local wf=
    case $1 in
      lint | tests-workspace | tests-gb-devices | tests-relift | tests-relift-e2e | mesh-kat | nds-timing-sdk | docs | gb-corpus | z80-sst | z80-zexdoc | generated | workflows | appimage) wf=ci.yaml:$1 ;;
      mac-runtime-tests) wf=runtime.yaml:c-tests ;;
      mac-tests-*) wf=ci.yaml:macos-${1#mac-tests-} ;;
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

  # stop_all CODE: on TERM, INT or HUP, stop every job's process group (a
  # job runs in a group of its own, with its mac ssh session), the per-job
  # watchers and the holder of the mac lock, then exit with CODE.
  stop_all() {
    trap - TERM INT HUP
    set +e
    local f g
    for f in "$logs"/*.pgid; do
      [ -f "$f" ] && g=$(cat "$f") && kill -TERM -- "-$g" 2>/dev/null
    done
    kill -TERM "${subs[@]}" $mac_holder 2>/dev/null
    sleep 2
    for f in "$logs"/*.pgid; do
      [ -f "$f" ] && g=$(cat "$f") && kill -KILL -- "-$g" 2>/dev/null
    done
    kill -KILL "${subs[@]}" $mac_holder 2>/dev/null
    rm -f "$logs"/*.pgid "$tokens" "$slots"
    exit "$1"
  }

  # on_mac JOB KIND CMD: run CMD on the mac in the copied tree (see
  # mac-run.sh), with the target cache of what KIND builds.
  mac_run=$mac_dir-$$-$(date +%s)
  on_mac() {
    ssh -tt -o BatchMode=yes "$CI_MAC" "eval \"\$(/opt/homebrew/bin/brew shellenv 2>/dev/null || true)\"
      cd \"\$HOME/$mac_dir/src\" && CI_MAC_WORK=\"\$HOME/$mac_dir/work\" CI_MAC_RUN=$mac_run ${CI_MAC_CACHE_GB:+CI_MAC_CACHE_GB=$CI_MAC_CACHE_GB} scripts/ci/mac-run.sh $1 $2 $3"
  }

  # job NAME: run one job; exit 77 means skipped, with the reason on stdout.
  job() {
    case $1 in
      cross-gba-*) "$ROOT/scripts/ci/gbcross.sh" "${1#cross-gba-}" --guest gba --jobs 4 ;;
      cross-n64-*) "$ROOT/scripts/ci/gbcross.sh" "${1#cross-n64-}" --guest n64 --jobs 4 ;;
      cross-sms-*) "$ROOT/scripts/ci/gbcross.sh" "${1#cross-sms-}" --guest sms --jobs 4 ;;
      cross-nds-*) "$ROOT/scripts/ci/gbcross.sh" "${1#cross-nds-}" --guest nds --jobs 4 ;;
      cross-*) "$ROOT/scripts/ci/gbcross.sh" "${1#cross-}" --jobs 4 ;;
      image-*)
        [ -z "$image_skip" ] || { echo "$image_skip"; return 77; }
        "$ROOT/scripts/ci/cross-image.sh" "${1#image-}"
        ;;
      nds-timing-sdk)
        docker info >/dev/null 2>&1 || { echo "docker is not available"; return 77; }
        "$ROOT/scripts/ci/nds-timing-sdk.sh"
        ;;
      mac-*)
        [ -z "$mac_skip" ] || { echo "$mac_skip"; return 77; }
        case $1 in
          mac-runtime-tests) on_mac "$1" runtime "scripts/ci/runtime-tests.sh" ;;
          mac-tests-workspace) on_mac "$1" debug "scripts/ci/tests.sh workspace" ;;
          mac-tests-relift) on_mac "$1" debug "scripts/ci/tests.sh relift" ;;
          mac-tests-relift-e2e-slow) on_mac "$1" debug "scripts/ci/tests.sh e2e-slow" ;;
          mac-tests-relift-e2e-rest) on_mac "$1" debug "scripts/ci/tests.sh e2e-rest" ;;
          mac-cross-gba-*) on_mac "$1" debug "scripts/ci/gbcross.sh ${1#mac-cross-gba-} --guest gba --jobs 4" ;;
          mac-cross-n64-*) on_mac "$1" debug "scripts/ci/gbcross.sh ${1#mac-cross-n64-} --guest n64 --jobs 4" ;;
          mac-cross-sms-*) on_mac "$1" debug "scripts/ci/gbcross.sh ${1#mac-cross-sms-} --guest sms --jobs 4" ;;
          mac-cross-*) on_mac "$1" debug "scripts/ci/gbcross.sh ${1#mac-cross-} --jobs 4" ;;
          mac-release-*) on_mac "$1" "release-${1#mac-release-}" "cargo build --release --locked -p relift --target ${1#mac-release-}" ;;
        esac
        ;;
      *) "$ROOT/scripts/ci/$1.sh" ;;
    esac
  }

  # slot_cpus: the CPU list of each slot, CI_SLOT_CPUS cores of the ones
  # this script may run on, CI_JOBS of them if set, at least one. A slot is
  # handed out as one byte, its index as a letter, because readers of a FIFO
  # read a line a byte at a time and would interleave longer tokens.
  slot_cpus=()
  local per=${CI_SLOT_CPUS:-4} cpus=() n i
  mapfile -t cpus < <(taskset -cp $$ | sed 's/.*: //' | tr ',' '\n' | awk -F- '{ e = NF > 1 ? $2 : $1; for (c = $1; c <= e; c++) print c }')
  n=$((${#cpus[@]} / per))
  [ "$n" -ge 1 ] || n=1
  [ "$n" -le 26 ] || n=26
  [ -z "${CI_JOBS:-}" ] || [ "$CI_JOBS" -ge "$n" ] || n=$CI_JOBS
  for ((i = 0; i < n; i++)); do
    local set=("${cpus[@]:i*per:per}")
    [ "${#set[@]}" -gt 0 ] || set=("${cpus[@]}")
    slot_cpus+=("$(IFS=,; echo "${set[*]}")")
  done
  letters=abcdefghijklmnopqrstuvwxyz

  [ -z "$has_mac" ] || [ -n "$mac_skip" ] || mac_cap=${CI_MAC_JOBS:-$(mac_default_cap)}
  slots="$logs/host.slots"
  rm -f "$slots"
  mkfifo "$slots"
  exec 9<>"$slots"
  printf '%s' "${letters:0:n}" >&9
  mac_lock=${CI_MAC_LOCK:-$HOME/.cache/relift/mac.lock}
  tokens="$logs/mac.tokens"
  rm -f "$tokens"
  mkfifo "$tokens"
  exec 8<>"$tokens"
  subs=()
  mac_holder=
  trap 'stop_all 143' TERM
  trap 'stop_all 130' INT
  trap 'stop_all 129' HUP
  if [ -n "$has_mac" ] && [ -z "$mac_skip" ]; then
    mac_total=0
    for j in "${jobs[@]}"; do
      case $j in mac-*) mac_total=$((mac_total + 1)) ;; esac
    done
    (
      mkdir -p "$(dirname "$mac_lock")"
      exec 7>"$mac_lock"
      flock 7
      for ((i = 0; i < mac_cap; i++)); do echo >&8; done
      while kill -0 "$$" 2>/dev/null && [ "$(find "$logs" -name 'mac-*.status' | wc -l)" -lt "$mac_total" ]; do
        sleep 1
      done
    ) &
    mac_holder=$!
  fi

  for j in "${jobs[@]}"; do
    (
      queued=0
      held=
      slot=
      q=$(date +%s)
      if [ "${j#mac-}" != "$j" ]; then
        if [ -z "$mac_skip" ]; then
          read -r -u 8
          held=1
        fi
      else
        read -r -n 1 -u 9 slot
      fi
      queued=$(($(date +%s) - q))
      s=$(date +%s)
      set +e
      limit=$(limit_minutes "$j")
      set -m
      (
        if [ -n "$slot" ]; then
          i=${letters%%"$slot"*}
          export CI_CPUSET=${slot_cpus[${#i}]}
          taskset -cp "$CI_CPUSET" "$BASHPID" >/dev/null
        fi
        job "$j"
      ) >"$logs/$j.log" 2>&1 &
      pid=$!
      echo "$pid" >"$logs/$j.pgid"
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
      rm -f "$logs/$j.pgid"
      set +m
      set -e
      [ -z "$held" ] || echo >&8
      [ -z "$slot" ] || printf '%s' "$slot" >&9
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
      [ "$queued" -eq 0 ] || note="$note (queued ${queued}s)"
      printf '%-40s %-4s %5ss  %s\n' "$j" "$r" "$(($(date +%s) - s))" "$note" >"$logs/$j.status"
    ) &
    subs+=("$!")
  done
  wait

  fail=0
  for j in "${jobs[@]}"; do
    cat "$logs/$j.status"
    grep -qE ' (ok|SKIP) ' "$logs/$j.status" || fail=1
  done
  exit "$fail"
}

main "$@"; exit $?
