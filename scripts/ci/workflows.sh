#!/usr/bin/env bash
# Lint every workflow file, check that every script under scripts/ that
# starts with #! is executable in git, and check that all.sh stops its jobs
# when it is signaled (all-stops.sh).
. "$(dirname "$0")/lib.sh"

dir="$CI_WORK/actionlint-$ACTIONLINT_VERSION"
fetch "https://github.com/rhysd/actionlint/releases/download/v$ACTIONLINT_VERSION/actionlint_${ACTIONLINT_VERSION}_linux_amd64.tar.gz" \
  "$ACTIONLINT_SHA256" "$dir/actionlint.tgz"
[ -x "$dir/actionlint" ] || tar -xzf "$dir/actionlint.tgz" -C "$dir" actionlint
"$dir/actionlint" -shellcheck= -color

missing=0
while read -r mode _ _ path; do
  if [ "$mode" != 100755 ] && [ "$(head -c2 "$path")" = '#!' ]; then
    echo "error: $path starts with #! but is not executable in git" >&2
    missing=1
  fi
done < <(git ls-files -s -- scripts)
[ "$missing" = 0 ]

scripts/ci/all-stops.sh
