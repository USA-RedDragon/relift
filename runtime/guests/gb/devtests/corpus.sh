#!/bin/sh
# Runs the lockstep check over a handful of corpus ROMs (never committed).
#   corpus.sh <gb_lockstep> [frames]   ROMs from ~/.cache/relift/gb-corpus/roms
set -eu
bin=$1
frames=${2:-300}
r=${CORPUS_ROOT:-$HOME/.cache/relift/gb-corpus/roms}
rc=0
"$bin" --model dmg --frames "$frames" \
    "$r/roms/blargg/cpu_instrs/cpu_instrs.gb" "$r/roms/blargg/instr_timing/instr_timing.gb" \
    "$r/roms/blargg/interrupt_time/interrupt_time.gb" "$r/roms/blargg/oam_bug/oam_bug.gb" \
    "$r/roms/blargg/mem_timing/mem_timing.gb" "$r/roms/blargg/dmg_sound/dmg_sound.gb" \
    "$r/roms/dmg-acid2/dmg-acid2.gb" "$r/roms/mbc3-tester/mbc3-tester.gb" || rc=1
"$bin" --model cgb --frames "$frames" \
    "$r/roms/cgb-acid2/cgb-acid2.gbc" "$r/roms/blargg/cgb_sound/cgb_sound.gb" \
    "$r/roms/blargg/cpu_instrs/cpu_instrs.gb" "$r/roms/mealybug-tearoom-tests/mbc/mbc3_rtc.gb" \
    "$r/roms/daid/speed_switch_timing_div.gbc" "$r/roms/daid/stop_instr.gb" "$r/roms/daid/rom_and_ram.gb" \
    "$r"/build/dma/*.gbc "$r"/build/apu/*.cgb.mooneye.gbc "$r"/build/ppu/*.cgb.png.gbc || rc=1
exit $rc
