// SPDX-License-Identifier: MIT
// Part of relift's checker (check/oracle/ares). The ares sources it is built
// with are ISC; this file is MIT and the two never share a binary with relift.
// psg-step: drives ares' SN76489 and the Master System / Game Gear mix of ms/psg/psg.cpp
// from a script and prints every sample. Usage: psg-step [--gg] < script
// Script: one event per line, "<clock> w <byte>" (data port), "<clock> s <byte>" (GG stereo
// port 0x06) or "<clock> end"; clocks are CPU clocks and do not decrease. A sample is made
// every 16 clocks; sample k is complete at clock 16k and an event at clock t is applied after
// every sample complete by t. Output per sample: "att0 att1 att2 att3 left right" (the four
// n4 attenuations ares' clock() returns, then the two stream frames as hexadecimal floats).
#include <ares/ares.hpp>
#include <cmath>
#include <component/audio/sn76489/sn76489.hpp>

using namespace ares;

struct Chip : SN76489 {
  f64 volume[16];
  u8 enable = 0xff;
  bool gg = false;
  uint64_t samples = 0;

  Chip(bool gameGear) : gg(gameGear) {
    SN76489::power();
    for(u32 level : range(15)) volume[level] = pow(2, level * -2.0 / 6.0);
    volume[15] = 0;
  }

  void sample() {
    auto channels = SN76489::clock();
    f64 left = 0.0, right = 0.0;
    if(!gg) {
      f64 output = 0.0;
      output += volume[channels[0]];
      output += volume[channels[1]];
      output += volume[channels[2]];
      output += volume[channels[3]];
      left = right = output / 4.0;
    } else {
      if(enable & 0x10) left += volume[channels[0]];
      if(enable & 0x20) left += volume[channels[1]];
      if(enable & 0x40) left += volume[channels[2]];
      if(enable & 0x80) left += volume[channels[3]];
      if(enable & 0x01) right += volume[channels[0]];
      if(enable & 0x02) right += volume[channels[1]];
      if(enable & 0x04) right += volume[channels[2]];
      if(enable & 0x08) right += volume[channels[3]];
      left /= 4.0;
      right /= 4.0;
    }
    printf("%u %u %u %u %a %a\n", (u32)channels[0], (u32)channels[1], (u32)channels[2], (u32)channels[3], (double)left, (double)right);
    samples++;
  }

  void until(uint64_t t) { while((samples + 1) * 16 <= t) sample(); }
};

int main(int argc, char** argv) {
  Chip chip(argc > 1 && std::string(argv[1]) == "--gg");
  char line[128];
  while(fgets(line, sizeof line, stdin)) {
    unsigned long long t;
    char kind[8];
    unsigned v = 0;
    if(sscanf(line, "%llu %7s %u", &t, kind, &v) < 2) continue;
    chip.until(t);
    if(kind[0] == 'w') chip.write(v);
    else if(kind[0] == 's' && chip.gg) chip.enable = v;
  }
  return 0;
}
