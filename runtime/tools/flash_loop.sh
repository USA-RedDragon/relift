#!/usr/bin/env bash
# flash_loop.sh <tries> <stop present> <run root> <game binary> [VAR=value...]: runs a game from the same start again
# and again until the flash trigger (RELIFT_NVN_FLASH, runtime/guests/switch/nvn/nvn_flash.c) fires or <tries> runs
# pass. Each try runs in <run root>/try-<k>/ with its own copy of $SAVE (a folder copied into the run's save/, if set),
# RELIFT_NVN_FLASH=<try>/flash and the log in <try>/log. A try ends at the first "flash trigger:" line, when the log's
# present count ("<n> presents, ") reaches <stop present>, when the log is silent for $SILENT seconds (default 120),
# or after $LIMIT seconds (default 600). The VAR=value pairs and the caller's environment (RELIFT_INPUT_SCRIPT,
# RELIFT_ROMFS and the like) go to every try. $LAUNCH (for example a preflight wrapper that execs its arguments) is
# put before the binary. A launch that exits with 97 (the preflight refused it, such as for want of memory) is not a
# try: it is started again after $REFUSED_WAIT seconds (default 30), at most $REFUSED_MAX times (default 40) in all.
# Prints one line per try and the try that fired; exit 0 when one fired, 1 when none did.
set -u
[ $# -ge 4 ] || { sed -n '2,10p' "$0"; exit 2; }
tries=$1 stop=$2 root=$3 bin=$4
shift 4
SILENT=${SILENT:-120} LIMIT=${LIMIT:-600}
mkdir -p "$root" || exit 2
for kv in "$@"; do export "$kv"; done
fired= refused=0 k=0
while [ "$k" -lt "$tries" ]; do
    k=$((k + 1))
    run=$root/try-$k
    rm -rf "$run" && mkdir -p "$run/save" || exit 2
    [ -n "${SAVE:-}" ] && cp -a "$SAVE/." "$run/save/"
    export RELIFT_NVN_FLASH=$run/flash${FLASH_THRESHOLD:+:$FLASH_THRESHOLD}
    log=$run/log
    (cd "$run" && exec ${LAUNCH:-} "$bin") >"$log" 2>&1 &
    pid=$!
    start=$(date +%s) last_size=-1 last_change=$start why=
    while kill -0 "$pid" 2>/dev/null; do
        sleep 5
        now=$(date +%s)
        if grep -aq "flash trigger: present" "$log"; then why=fired; break; fi
        n=$(grep -ao '[0-9]\+ presents, ' "$log" | tail -1 | grep -o '[0-9]\+')
        if [ -n "$n" ] && [ "$n" -ge "$stop" ]; then why="reached present $n"; break; fi
        size=$(stat -c %s "$log")
        [ "$size" != "$last_size" ] && { last_size=$size; last_change=$now; }
        [ $((now - last_change)) -ge "$SILENT" ] && { why="log silent for ${SILENT}s"; break; }
        [ $((now - start)) -ge "$LIMIT" ] && { why="ran ${LIMIT}s"; break; }
    done
    [ -z "$why" ] && why="the game ended by itself"
    for c in $(ps --ppid "$pid" -o pid= 2>/dev/null); do kill -KILL "$c" 2>/dev/null; done
    kill -KILL "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
    code=$?
    if [ "$why" = "the game ended by itself" ] && [ "$code" = 97 ]; then
        refused=$((refused + 1))
        [ "$refused" -gt "${REFUSED_MAX:-40}" ] && { echo "the preflight refused $refused launches: $(tail -1 "$log")"; exit 2; }
        echo "try $k: refused by the preflight ($(tail -1 "$log" | cut -c1-160)); again in ${REFUSED_WAIT:-30} s"
        sleep "${REFUSED_WAIT:-30}"
        k=$((k - 1))
        continue
    fi
    echo "try $k: $why ($(($(date +%s) - start)) s) $run"
    if [ "$why" = fired ]; then
        grep -a -m1 "flash trigger: present" "$log" | cut -c1-400
        fired=$run
        break
    fi
done
[ -n "$fired" ] && { echo "fired in $fired"; exit 0; }
echo "no flash in $tries tries"
exit 1
