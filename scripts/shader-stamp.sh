#!/usr/bin/env bash
# Prints the shader translator stamp of a relift source tree: the stamp a
# relift built from it writes beside each shader table (table.stamp) and
# hands the programs it links. It is the first 16 hex digits of the SHA-256
# of every translator source, in byte order of path, each as its path, a
# newline and its bytes: everything under gpus/*/src, each gpus/*/Cargo.toml,
# relift/src/shaders.rs and relift/src/shaders/. relift/build.rs computes the
# same value; a test keeps the two equal.
# Usage: shader-stamp.sh [TREE]   (default: the tree this script is in)
set -eu
root=${1:-$(dirname "$0")/..}
cd "$root"
{
  find gpus -type f \( -path 'gpus/*/src/*' -o -path 'gpus/*/Cargo.toml' \)
  echo relift/src/shaders.rs
  find relift/src/shaders -type f
} | LC_ALL=C sort | {
  while IFS= read -r f; do
    printf '%s\n' "$f"
    cat "$f"
  done
} | sha256sum | cut -c1-16
