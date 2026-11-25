#!/bin/sh
# Prints the files a diagnostic snapshot can hold as a Markdown table, for
# docs/reference/snapshot.md (a docgen:output exec= marker). Each row is a
# call of rl_snapshot_file or out_file with a literal name in the runtime: the
# name, the layer that writes it (the folder of the source file) and the
# description the snapshot's own index.txt gives.
set -eu

cd "$(dirname "$0")/../.."
[ -f runtime/core/snapshot.c ] || { echo "snapshot-files.sh: runtime/core/snapshot.c is missing" >&2; exit 1; }

rows=$(git grep -E '(rl_snapshot_file|out_file)\("[^"]+", "' -- runtime ':!*/tests/*' | sort || true)
[ -n "$rows" ] || { echo "snapshot-files.sh: no snapshot files found" >&2; exit 1; }

echo "| File | Written by | What index.txt says it is |"
echo "|---|---|---|"
printf '%s\n' "$rows" | sed -E 's/^([^:]+):[[:space:]]*[^"]*(rl_snapshot_file|out_file)\("([^"]+)", "([^"]*)".*$/\1|\3|\4/' |
    while IFS='|' read -r file name what; do
        case "$file" in
        runtime/core/*) layer="core" ;;
        runtime/guests/switch/nvn/*) layer="Switch 2 graphics layer" ;;
        runtime/guests/switch/*) layer="Switch layer" ;;
        *) layer="$file" ;;
        esac
        echo "| \`$name\` | $layer | $what |"
    done
