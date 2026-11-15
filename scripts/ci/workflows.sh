#!/usr/bin/env bash
# Lint every workflow file.
. "$(dirname "$0")/lib.sh"

dir="$CI_WORK/actionlint-$ACTIONLINT_VERSION"
fetch "https://github.com/rhysd/actionlint/releases/download/v$ACTIONLINT_VERSION/actionlint_${ACTIONLINT_VERSION}_linux_amd64.tar.gz" \
  "$ACTIONLINT_SHA256" "$dir/actionlint.tgz"
[ -x "$dir/actionlint" ] || tar -xzf "$dir/actionlint.tgz" -C "$dir" actionlint
"$dir/actionlint" -shellcheck= -color
