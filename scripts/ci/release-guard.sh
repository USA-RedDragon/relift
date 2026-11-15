#!/usr/bin/env bash
# Release builds load no code and map no executable memory.
. "$(dirname "$0")/lib.sh"

python3 -I runtime/tools/cross/test_release_guard.py
python3 -I runtime/tools/cross/release_guard.py --sources runtime
