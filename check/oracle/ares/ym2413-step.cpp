// SPDX-License-Identifier: MIT
// Part of relift's checker (check/oracle/ares). The ares sources it is built
// with are ISC; this file is MIT and the two never share a binary with relift.
// ym2413-step: drives ares' YM2413 from a script and prints every sample. Usage: ym2413-step < script
// Script: one event per line, "<clock> a <byte>" (address port), "<clock> w <byte>" (data port) or
// "<clock> end"; clocks are CPU clocks and do not decrease. A sample is made every 72 clocks;
// sample k is complete at clock 72k and an event at clock t is applied after every sample
// complete by t. Output per sample: the chip's mix (melody plus twice the rhythm) as an integer,
// which is ares' clock() result times 8192.
#include <ares/ares.hpp>
#include <component/audio/ym2413/ym2413.hpp>

using namespace ares;

struct Chip : YM2413 {
  uint64_t samples = 0;
  Chip() { YM2413::power(); }
  void until(uint64_t t) {
    while((samples + 1) * 72 <= t) {
      f64 v = YM2413::clock();
      printf("%lld\n", (long long)llround(v * 8192.0));
      samples++;
    }
  }
};

int main() {
  Chip chip;
  char line[128];
  while(fgets(line, sizeof line, stdin)) {
    unsigned long long t;
    char kind[8];
    unsigned v = 0;
    if(sscanf(line, "%llu %7s %u", &t, kind, &v) < 2) continue;
    chip.until(t);
    if(kind[0] == 'a') chip.address(v);
    else if(kind[0] == 'w') chip.write(v);
  }
  return 0;
}
