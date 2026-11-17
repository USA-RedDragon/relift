// SPDX-License-Identifier: MIT
// sega-oracle: a headless ares (ISC) Master System, Game Gear and SG-1000
// that runs a cartridge for a number of frames and writes trace files. The
// ares sources it is built with are ISC; this file is MIT. See README.md.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace orc {

/** The command line. */
struct Opt {
  std::string rom, id, outDir, input, system = "auto", mapper = "auto";
  uint64_t frames = 600, shot = 0, randFrom = UINT64_MAX;
  uint32_t seed = 1, randBlock = 6;
  bool quiet = false;
  uint64_t rawFrom = UINT64_MAX, rawTo = 0;
  bool cartRam = true;
  bool bios = false;
};
extern Opt opt;
/** The index of the frame being run. */
extern uint64_t g_frame;
/** The image in file order, padded with 16 zero bytes past g_romSize. */
extern std::vector<uint8_t> g_rom;
extern uint32_t g_romSize;

/** Where the bytes of an executed instruction came from. */
enum Space { ROM = 0, RAM = 1, SRAM = 2, UNK = 5, OPEN = 6 };

void recExec(uint32_t pc, int space, int64_t off, const uint8_t bytes[4]);
void recMapper(uint32_t pc, uint32_t reg, uint32_t val, const std::string& desc);
void recIrq(const char* kind, uint32_t line, uint32_t vector, uint32_t pc);
void recStore(uint32_t off, uint32_t pc, int64_t romSrc);

/** Whether the input script holds the named button in the current frame. */
bool buttonHeld(const std::string& name);
void setRandomButtons(std::vector<std::string> names);

void recRawFrame(const uint16_t* px, uint32_t w, uint32_t h);
void recInput(uint32_t mask);
void recState(const std::string& row);
std::string traceDir();
void onVideo(const uint32_t* data, uint32_t pitch, uint32_t w, uint32_t h);

struct Sys {
  virtual ~Sys() {}
  virtual bool boot() = 0;
  virtual void runFrame() = 0;
  virtual const char* name() = 0;
  /** A JSON object for meta.json. */
  virtual std::string describe() = 0;
};
Sys* makeMS(bool gg);
Sys* makeSG();

/** nall's mirror(): the offset an address past the end of a ROM of any size reads. */
inline uint32_t mirrorAddr(uint32_t addr, uint32_t size) {
  if(!size) return 0;
  uint32_t base = 0, mask = 1u << 31;
  while(addr >= size) {
    while(!(addr & mask)) mask >>= 1;
    addr -= mask;
    if(size > mask) { size -= mask; base += mask; }
    mask >>= 1;
  }
  return base + addr;
}

/**
 * The CPU T-state of the start of VDP line `line` of frame k, rounded up, as
 * relift's runtime maps VDP to CPU time (CPU 3579545 Hz, VDP 10738636 Hz, 684
 * VDP clocks a line), so both sides hash RAM at the same instant.
 */
inline uint64_t edgeCycle(uint64_t k, uint64_t lines, uint64_t line) {
  const unsigned __int128 second = 0x7FFFFFFFFFFFFFFFull;
  const unsigned __int128 cpuStep = second / 3579545u, vdpStep = second / 10738636u;
  unsigned __int128 p = (unsigned __int128)(k * lines + line) * 684u;
  return (uint64_t)((p * vdpStep + cpuStep - 1) / cpuStep);
}

}  // namespace orc
