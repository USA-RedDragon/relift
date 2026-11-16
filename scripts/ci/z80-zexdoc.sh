#!/usr/bin/env bash
# Build Frank Cringle's ZEXDOC Z80 instruction exerciser through relift (its
# code lifted with the Z80 frontend, emitted as C, compiled with the runtime's
# core and Z80 layer) and run it natively: all 67 groups must print OK. The
# exerciser is GPL-2 test data, fetched by hash and never committed.
. "$(dirname "$0")/lib.sh"

zexdoc_sha256=34923a7ed82285d3038b2d54bd64899e12173eebb61f9d07b4fc72e78af2ae8f
com="$CI_WORK/zexdoc.com"

fetch "https://raw.githubusercontent.com/anotherlin/z80emu/master/testfiles/zexdoc.com" "$zexdoc_sha256" "$com"

RELIFT_ZEXDOC="$com" cargo test --locked -p relift --test z80_zexdoc -- --nocapture
