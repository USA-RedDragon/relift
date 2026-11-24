#!/bin/bash
# capture_watch.sh PID CAPDIR [MAX] : watches a game run that writes RELIFT_VK_CAPTURE pictures into CAPDIR and kills
# PID (the run's own process, nothing else) when the capture numbered MAX or later exists, when no capture has
# appeared FIRST seconds (default 300) after start, or when the newest capture has not changed for FREEZE seconds
# (default 60). Prints why it stopped. A hung run then ends instead of holding the GPU unnoticed.
PID=$1; CAPDIR=$2; MAX=${3:-}
waited=0; last=""; same=0
while kill -0 "$PID" 2>/dev/null; do
    sleep 10
    f=$(ls "$CAPDIR" 2>/dev/null | tail -1)
    if [ -z "$f" ]; then
        waited=$((waited + 10))
        [ $waited -ge "${FIRST:-300}" ] && { echo "no capture after $waited s"; break; }
        continue
    fi
    n=$((10#$(echo "$f" | cut -d- -f2)))
    [ -n "$MAX" ] && [ $n -ge "$MAX" ] && { echo "max at $f"; break; }
    h=$(md5sum < "$CAPDIR/$f")
    if [ "$h" = "$last" ]; then same=$((same + 10)); else same=0; last=$h; fi
    [ $same -ge "${FREEZE:-60}" ] && { echo "frozen at $f"; break; }
done
kill -KILL "$PID" 2>/dev/null
