#!/bin/sh
# Prints the debugging settings of the C runtime as Markdown, for
# docs/reference/debug-settings.md (a docgen:output exec= marker).
#
#   debug-settings.sh documented    the rows of the runtime README's settings
#                                   table whose variable is a debugging aid
#   debug-settings.sh source        the debugging variables the runtime reads
#                                   that the README table does not list, each
#                                   with the doc comment at its source
#
# Both read only files in the repository, so the output is the same every run.
set -eu

cd "$(dirname "$0")/../.."
readme=runtime/README.md
[ -f "$readme" ] || { echo "debug-settings.sh: $readme is missing" >&2; exit 1; }
[ -d runtime ] || { echo "debug-settings.sh: runtime/ is missing" >&2; exit 1; }

# Names that mark a debugging aid.
pat='GDB|SNAPSHOT|TRACE|LOG|RENDERDOC|CAPTURE|WATCH|PROFILE|DUMP|FLASH|DIFF|PHASES|DEBUG|SCHED|STATS|CHECK|PROBE|INPUT_SCRIPT'

case "${1:-}" in
documented)
    rows=$(grep -E "^\| \`RELIFT_[A-Z0-9_]*(${pat})[A-Z0-9_]*[=\`|\\\\]" "$readme" || true)
    [ -n "$rows" ] || { echo "debug-settings.sh: no rows matched in $readme" >&2; exit 1; }
    echo "| Variable | What it does |"
    echo "|---|---|"
    printf '%s\n' "$rows" | grep -v '^| `RELIFT_\(GPU_MATH\|NET\)' | sed -E 's/^\| (`[^|]*`) \| /| \1 | /'
    ;;
source)
    used=$(git grep -ho 'getenv("RELIFT_[A-Z0-9_]*")' -- runtime ':!*/tests/*' | sed 's/getenv("//;s/")//' | sort -u)
    listed=$(grep -o '^| `RELIFT_[A-Z0-9_]*' "$readme" | sed 's/^| `//' | sort -u)
    names=$(printf '%s\n' "$used" | grep -vxF "$listed" | grep -E "${pat}" || true)
    [ -n "$names" ] || { echo "debug-settings.sh: no source-only variables found" >&2; exit 1; }
    echo "| Variable | Read in | First line of its doc comment |"
    echo "|---|---|---|"
    for n in $names; do
        file=$(git grep -l "getenv(\"$n\")" -- runtime ':!*/tests/*' | sort | head -1)
        strip='s/^[[:space:]]*(\/\*+|\*|\/\/) *(@brief)? *//; s/ *\*\/ *$//; s/\|/\\|/g'
        doc=$(git grep -h -E "^[[:space:]]*(/\*+|\*|//) *(@brief)? *[^ ]* *$n" -- runtime ':!*/tests/*' | head -1 | sed -E "$strip")
        [ -n "$doc" ] || doc=$(git grep -h -E "^[[:space:]]*(/\*|\*|//).*$n" -- runtime ':!*/tests/*' | head -1 | sed -E "$strip")
        [ -n "$doc" ] || doc="(none)"
        echo "| \`$n\` | \`$file\` | $doc |"
    done
    ;;
*)
    echo "usage: debug-settings.sh documented|source" >&2
    exit 2
    ;;
esac
