#!/usr/bin/env bash
# Check the generated facts of the docs with docgen, then build the docs site
# with the pinned pandoc. The build fails on a missing page or a broken link.
. "$(dirname "$0")/lib.sh"

docgen=$(sed -n 's/^  docgen: //p' .docgen.yaml)
[ -n "$docgen" ] || { echo ".docgen.yaml has no docgen var" >&2; exit 1; }
go run "github.com/USA-RedDragon/docgen@$docgen" check -diff

dir="$CI_WORK/pandoc-$PANDOC_VERSION"
fetch "https://github.com/jgm/pandoc/releases/download/$PANDOC_VERSION/pandoc-$PANDOC_VERSION-linux-amd64.tar.gz" \
  "$PANDOC_SHA256" "$dir/pandoc.tgz"
[ -x "$dir/pandoc-$PANDOC_VERSION/bin/pandoc" ] || tar -xzf "$dir/pandoc.tgz" -C "$dir"
PATH="$dir/pandoc-$PANDOC_VERSION/bin:$PATH" docs/site/build.sh "${DOCS_OUT:-artifacts/docs-site}"
