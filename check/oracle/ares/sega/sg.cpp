// SPDX-License-Identifier: MIT
// The SG-1000 driver over the ares sg core (ISC).
#include <sg/sg.hpp>
#include "platform.hpp"
#include <cstring>

namespace S = ares::SG1000;
using namespace orc;

namespace {

ares::Node::System g_root;
FILE* g_ilog = nullptr;
uint64_t g_ilogLeft = 0;

FILE* g_ramLog = nullptr;
uint64_t g_ramFrame = 0, g_ramEdge = 0;
/** Hashes RAM into ramhash.tsv at the first boundary at or after the start of line 192 of each frame. */
void cpuBoundary() {
  while(ares::oracleHooks.msCycles >= g_ramEdge) {
    uint32_t h = 2166136261u;
    for(u32 i = 0; i < 0x400; i++) h = (h ^ (u8)S::cpu.ram.read(i)) * 16777619u;
    fprintf(g_ramLog, "%llu\t%08x\n", (unsigned long long)g_ramFrame, h);
    fflush(g_ramLog);
    g_ramEdge = edgeCycle(++g_ramFrame, 262, 192);
  }
}

/** The picture as the VDP finished it, copied at the frame event (see ms.cpp). */
std::vector<u32> g_shot;
void frameReady() {
  auto px = S::vdp.screen->pixels(0);
  g_shot.assign(px.begin(), px.end());
}

struct SgPlatform : ares::Platform {
  std::shared_ptr<vfs::directory> cartPak;
  auto pak(ares::Node::Object node) -> std::shared_ptr<vfs::directory> override {
    if(node->name() == "SG-1000") return std::make_shared<vfs::directory>();
    return cartPak;
  }
  auto video(ares::Node::Video::Screen, const u32* data, u32 pitch, u32 width, u32 height) -> void override {
    onVideo(data, pitch, width, height);
  }
  auto audio(ares::Node::Audio::Stream stream) -> void override {
    f64 samples[2];
    while(stream->pending()) stream->read(samples);
  }
  auto input(ares::Node::Input::Input node) -> void override {
    if(auto button = std::dynamic_pointer_cast<ares::Core::Input::Button>(node)) {
      std::string n = (const char*)node->name();
      bool v = buttonHeld(n);
      if(n == "1") v = v || buttonHeld("a");
      if(n == "2") v = v || buttonHeld("b");
      button->setValue(v);
    }
  }
} g_plat;

void z80Insn(void*, unsigned pc) {
  if(g_ilogLeft && g_ilog) {
    g_ilogLeft--;
    fprintf(g_ilog, "I\t%llu\t%04x\t%04x\t%04x\t%04x\t%04x\t%04x\t%04x\t%04x\t%02x\n", ares::oracleHooks.msCycles, pc,
            (unsigned)S::cpu.af.word, (unsigned)S::cpu.bc.word, (unsigned)S::cpu.de.word, (unsigned)S::cpu.hl.word,
            (unsigned)S::cpu.ix.word, (unsigned)S::cpu.iy.word, (unsigned)S::cpu.SP, (unsigned)S::cpu.ir.byte.lo);
  }
  uint8_t b[4];
  for(int i = 0; i < 4; i++) b[i] = S::cpu.readMapped((pc + i) & 0xffff);
  bool rom = pc <= 0x7fff || pc < g_romSize;
  recExec(pc, rom ? ROM : RAM, rom ? (int64_t)mirrorAddr(pc, g_romSize) : (pc & 0x3ff), b);
}

void portHook(bool out, unsigned port, unsigned data) {
  if(g_ilogLeft && g_ilog) fprintf(g_ilog, "%c\t%llu\t%02x\t%02x\n", out ? 'O' : 'N', ares::oracleHooks.msCycles, port & 0xff, data & 0xff);
}

void irqHook(int, const char* kind, unsigned, unsigned pc) {
  if(g_ilogLeft && g_ilog) fprintf(g_ilog, "Q\t%llu\t%s\t%04x\n", ares::oracleHooks.msCycles, kind, pc);
  recIrq(kind, S::vdp.vcounter(), 0x38, pc);
}

struct SG : Sys {
  const char* name() override { return "sg"; }
  std::string describe() override { return "{\"core\": \"ares sg\", \"console\": \"SG-1000 (NTSC)\", \"mapper\": \"Linear\", \"bios\": false}"; }
  bool boot() override {
    auto d = std::make_shared<vfs::directory>();
    d->setAttribute("title", nall::string{"oracle"});
    d->setAttribute("region", nall::string{"NTSC"});
    d->setAttribute("board", nall::string{"Linear"});
    d->append("program.rom", std::span<const u8>(g_rom.data(), g_romSize));
    g_plat.cartPak = d;
    ares::platform = &g_plat;
    if(!S::load(g_root, "[Sega] SG-1000 (NTSC)")) { fprintf(stderr, "ares load failed\n"); return false; }
    if(auto p = g_root->find<ares::Node::Port>("Cartridge Slot")) { p->allocate(); p->connect(); }
    if(auto p = g_root->find<ares::Node::Port>("Controller Port 1")) p->allocate("Gamepad");
    ares::oracleHooks.z80Insn = z80Insn;
    ares::oracleHooks.irq = irqHook;
    g_ramLog = fopen((traceDir() + "/ramhash.tsv").c_str(), "w");
    g_ramEdge = edgeCycle(0, 262, 192);
    ares::oracleHooks.cpuBoundary = cpuBoundary;
    ares::oracleHooks.frameReady = frameReady;
    if(const char* n = getenv("ORACLE_ILOG")) {
      g_ilogLeft = strtoull(n, nullptr, 0);
      g_ilog = fopen((traceDir() + "/insn.tsv").c_str(), "w");
      ares::oracleHooks.msPort = portHook;
    }
    g_root->power();
    setRandomButtons({"Up", "Down", "Left", "Right", "1", "2"});
    return true;
  }
  void runFrame() override {
    static const char* names[6] = {"Up", "Down", "Left", "Right", "1", "2"};
    uint32_t mask = 0;
    for(int i = 0; i < 6; i++) {
      bool v = buttonHeld(names[i]);
      if(i == 4) v = v || buttonHeld("a");
      if(i == 5) v = v || buttonHeld("b");
      if(v) mask |= 1u << i;
    }
    recInput(mask);
    g_root->run();
    auto& px = g_shot;
    std::vector<uint16_t> raw(256 * 192);
    for(uint32_t y = 0; y < 192; y++)
      for(uint32_t x = 0; x < 256; x++) raw[y * 256 + x] = (uint16_t)px[(y + 27) % 243 * 284 + (x + 13) % 284];
    recRawFrame(raw.data(), 256, 192);
    uint32_t hr = 2166136261u, hv = 2166136261u, hc = 2166136261u;
    for(u32 i = 0; i < 0x400; i++) hr = (hr ^ (u8)S::cpu.ram.read(i)) * 16777619u;
    for(u32 i = 0; i < 0x4000; i++) hv = (hv ^ (u8)S::vdp.vram.read(i)) * 16777619u;
    char row[256];
    snprintf(row, sizeof row, "%04x\t%04x\t%04x\t%04x\t%04x\t%04x\t%04x\t%04x\t%08x\t%08x\t%08x\t%u\t%u", (unsigned)S::cpu.PC,
             (unsigned)S::cpu.SP, (unsigned)S::cpu.af.word, (unsigned)S::cpu.bc.word, (unsigned)S::cpu.de.word, (unsigned)S::cpu.hl.word,
             (unsigned)S::cpu.ix.word, (unsigned)S::cpu.iy.word, hr, hv, hc, (unsigned)S::vdp.vcounter(), 0u);
    recState(row);
    if(g_ilog) fflush(g_ilog);
  }
};

}  // namespace

namespace orc { Sys* makeSG() { return new SG(); } }
