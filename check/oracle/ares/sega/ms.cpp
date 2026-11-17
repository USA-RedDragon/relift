// SPDX-License-Identifier: MIT
// The Master System II and Game Gear driver over the ares ms core (ISC).
#include <ms/ms.hpp>
#include "platform.hpp"
#include <cstring>

namespace M = ares::MasterSystem;
using namespace orc;

namespace {

OraclePlatform g_plat;
ares::Node::System g_root;
bool g_gg = false;
/** Copier header bytes at the start of the file. */
uint32_t g_hdr = 0;
/** The pc of the instruction in flight. */
uint32_t g_last = 0;
std::string g_board;
/** The ROM source of the store an LDI, LDD, LDIR or LDDR is about to do. */
struct Ld { bool valid = false; uint32_t dst = 0; int64_t src = -1; } g_ld;
FILE* g_ilog = nullptr;
uint64_t g_ilogLeft = 0;

FILE* g_ramLog = nullptr;
uint64_t g_ramFrame = 0, g_ramEdge = 0;
/** Hashes work RAM into ramhash.tsv at the first boundary at or after the start of line 240 of each frame. */
void cpuBoundary() {
  while(ares::oracleHooks.msCycles >= g_ramEdge) {
    uint32_t h = 2166136261u;
    for(u32 i = 0; i < 0x2000; i++) h = (h ^ (u8)M::cpu.ram.read(i)) * 16777619u;
    fprintf(g_ramLog, "%llu\t%08x\n", (unsigned long long)g_ramFrame, h);
    fflush(g_ramLog);
    g_ramEdge = edgeCycle(++g_ramFrame, 262, 240);
  }
}

/**
 * The picture as the VDP finished it: ares refreshes its screen buffer on
 * another thread after the frame, so it is copied at the frame event, with
 * the lines the VDP drew in this frame (others are blank).
 */
std::vector<u32> g_shot;
bool g_drawn[320], g_drawnSnap[320];
void lineDrawn(unsigned line, bool drawn) { if(line < 320) g_drawn[line] = drawn; }
void frameReady() {
  auto px = M::vdp.screen->pixels(0);
  g_shot.assign(px.begin(), px.end());
  memcpy(g_drawnSnap, g_drawn, sizeof g_drawn);
  memset(g_drawn, 0, sizeof g_drawn);
}

M::Board::Interface* board() { return M::cartridge.board.get(); }

/** Whether a fetch at addr reads the open bus, which the Master System II and Game Gear pull up to 0xFF. */
bool openBus(uint32_t addr) {
  auto& bus = M::cpu.bus;
  auto* b = board();
  bool cartRamHigh = b && b->oracleOffset(addr) == -1;
  if(g_gg) {
    if(addr >= 0xc000) return !bus.ramEnable && !cartRamHigh;
    return addr < 0x400 && bus.biosEnable && !M::bios;
  }
  if(addr >= 0xc000) return !bus.ramEnable && !(bus.cartridgeEnable && cartRamHigh);
  if(bus.biosEnable && M::bios) return false;
  return !(bus.cartridgeEnable && b);
}

/** The space and file offset (copier header included) of a CPU address. */
void translate(uint32_t addr, int& space, int64_t& off) {
  auto* b = board();
  if(openBus(addr)) { space = OPEN; off = addr; return; }
  if(addr >= 0xc000) {
    if(b && b->oracleOffset(addr) == -1 && !(M::cpu.bus.ramEnable && !M::cpu.bus.cartridgeEnable)) { space = SRAM; off = addr & 0x3fff; return; }
    space = RAM; off = addr & 0x1fff; return;
  }
  if(!g_gg && M::bios && M::cpu.bus.biosEnable) {
    uint32_t o = addr <= 0x3ff ? addr : (uint32_t)M::bios.romBank[addr >> 14] << 14 | (addr & 0x3fff);
    space = ROM; off = (o & M::bios.rom.mask()) + g_hdr; return;
  }
  int64_t o = b ? b->oracleOffset(addr) : -2;
  if(o == -2) { space = UNK; off = addr; return; }
  if(o == -1) { space = SRAM; off = addr & 0x3fff; return; }
  uint32_t sz = g_romSize - g_hdr;
  if((uint64_t)o >= sz) o = mirrorAddr((uint32_t)o, sz);
  space = ROM; off = o + g_hdr;
}

void recordAt(unsigned pc, uint8_t* b) {
  int space; int64_t off;
  translate(pc, space, off);
  for(int i = 0; i < 4; i++) {
    uint32_t a = (pc + i) & 0xffff;
    if(space == OPEN || openBus(a)) b[i] = 0xff;
    else if(a >= 0xc000 && board() && board()->oracleOffset(a) == -1 && !(M::cpu.bus.ramEnable && !M::cpu.bus.cartridgeEnable)) b[i] = board()->read(a, 0);
    else if(a >= 0xc000) b[i] = M::cpu.ram.read(a & 0x1fff);
    else if(space == ROM) { int s2; int64_t o2; translate(a, s2, o2); b[i] = s2 == ROM ? g_rom[o2] : 0; }
    else if(space == SRAM && board()) b[i] = board()->read(a, 0);
    else b[i] = 0;
  }
  recExec(pc, space, space == UNK ? -1 : off, b);
}

/**
 * ares runs a run of DD and FD prefixes followed by DD, FD or ED as one
 * instruction; on the chip each such prefix is a 4 T-state instruction of
 * its own, so each prefix byte is recorded as an entry.
 */
void z80Insn(void*, unsigned pc) {
  if(g_ilogLeft && g_ilog) {
    g_ilogLeft--;
    fprintf(g_ilog, "I\t%llu\t%04x\t%04x\t%04x\t%04x\t%04x\t%04x\t%04x\t%04x\t%02x\n", ares::oracleHooks.msCycles, pc,
            (unsigned)M::cpu.af.word, (unsigned)M::cpu.bc.word, (unsigned)M::cpu.de.word, (unsigned)M::cpu.hl.word,
            (unsigned)M::cpu.ix.word, (unsigned)M::cpu.iy.word, (unsigned)M::cpu.SP, (unsigned)M::cpu.ir.byte.lo);
  }
  g_last = pc;
  g_ld.valid = false;
  uint8_t b[4];
  recordAt(pc, b);
  uint8_t c[4];
  memcpy(c, b, 4);
  for(unsigned p = pc; (c[0] == 0xdd || c[0] == 0xfd) && (c[1] == 0xdd || c[1] == 0xfd || c[1] == 0xed); p++) recordAt((p + 1) & 0xffff, c);
  if(b[0] == 0xed && (b[1] == 0xa0 || b[1] == 0xa8 || b[1] == 0xb0 || b[1] == 0xb8)) {
    int s2; int64_t o2;
    translate(M::cpu.hl.word, s2, o2);
    g_ld.valid = true; g_ld.dst = M::cpu.de.word; g_ld.src = s2 == ROM ? o2 : -1;
  }
}

void msWrite(unsigned address, unsigned data) {
  auto* b = board();
  if(b && b->oracleIsMapperReg(address)) recMapper(g_last, address, data, (const char*)b->oracleBanks());
  if(address >= 0xc000 && M::cpu.bus.ramEnable) recStore(address & 0x1fff, g_last, (g_ld.valid && g_ld.dst == address) ? g_ld.src : -1);
}

void portHook(bool out, unsigned port, unsigned data) {
  if(g_ilogLeft && g_ilog) fprintf(g_ilog, "%c\t%llu\t%02x\t%02x\n", out ? 'O' : 'N', ares::oracleHooks.msCycles, port & 0xff, data & 0xff);
}

void irqHook(int, const char* kind, unsigned, unsigned pc) {
  if(g_ilogLeft && g_ilog) fprintf(g_ilog, "Q\t%llu\t%s\t%04x\n", ares::oracleHooks.msCycles, kind, pc);
  uint32_t vec = kind[0] == 'n' ? 0x66 : (M::cpu.IM == 2 ? ((uint32_t)M::cpu.ir.byte.hi << 8 | 0xff) : 0x38);
  recIrq(kind, M::vdp.io.vcounter, vec, pc);
}

struct MS : Sys {
  bool gg;
  explicit MS(bool gg) : gg(gg) {}
  const char* name() override { return gg ? "gg" : "ms"; }
  std::string describe() override {
    return std::string("{\"core\": \"ares ms\", \"console\": \"") + (gg ? "Game Gear (NTSC-U)" : "Master System II (NTSC-U)") +
           "\", \"mapper\": \"" + g_board + "\", \"copier_header_bytes\": " + std::to_string(g_hdr) +
           ", \"bios\": " + (opt.bios ? "true" : "false") + "}";
  }
  bool boot() override {
    g_gg = gg;
    if(g_romSize > 0x4000 && g_romSize % 0x4000 == 0x200) g_hdr = 0x200;
    g_board = opt.mapper == "codemasters" ? "Codemasters" : opt.mapper == "korea" ? "Korea" : "Sega";
    if(opt.mapper == "auto" && g_romSize - g_hdr >= 0x8000) {
      const uint8_t* r = g_rom.data() + g_hdr;
      uint32_t sum = (r[0x7fe6] | r[0x7fe7] << 8), cmp = (r[0x7fe8] | r[0x7fe9] << 8);
      if(((sum + cmp) & 0xffff) == 0 && sum != 0) {
        uint32_t calc = 0;
        for(uint32_t i = 0; i < 0x8000; i += 2) if(i < 0x7fe0 || i >= 0x7ff0) calc += r[i] | r[i + 1] << 8;
        if((calc & 0xffff) == sum) g_board = "Codemasters";
      }
    }
    auto d = std::make_shared<vfs::directory>();
    d->setAttribute("title", nall::string{"oracle"});
    d->setAttribute("region", nall::string{"NTSC-J, NTSC-U, PAL"});
    d->setAttribute("board", nall::string{g_board.c_str()});
    d->setAttribute("ms", false);
    if(opt.bios) {
      auto sd = std::make_shared<vfs::directory>();
      sd->append("bios.rom", std::span<const u8>(g_rom.data() + g_hdr, g_romSize - g_hdr));
      g_plat.sysPak = sd;
    } else {
      d->append("program.rom", std::span<const u8>(g_rom.data() + g_hdr, g_romSize - g_hdr));
    }
    static std::vector<u8> sram;
    if(opt.cartRam) {
      sram.assign(g_board == "Codemasters" ? 0x2000 : 0x8000, 0);
      d->append("save.ram", std::span<const u8>(sram.data(), sram.size()));
    }
    g_plat.cartPak = d;
    g_plat.pauseIsStart = !gg;
    ares::platform = &g_plat;
    nall::string cfg = gg ? "[Sega] Game Gear (NTSC-U)" : "[Sega] Master System II (NTSC-U)";
    if(!M::load(g_root, cfg)) { fprintf(stderr, "ares load failed\n"); return false; }
    if(!opt.bios) if(auto p = g_root->find<ares::Node::Port>("Cartridge Slot")) { p->allocate(); p->connect(); }
    if(!gg) if(auto p = g_root->find<ares::Node::Port>("Controller Port 1")) p->allocate("Gamepad");
    ares::oracleHooks.z80Insn = z80Insn;
    ares::oracleHooks.msWrite = msWrite;
    ares::oracleHooks.irq = irqHook;
    g_ramLog = fopen((traceDir() + "/ramhash.tsv").c_str(), "w");
    g_ramEdge = edgeCycle(0, 262, 240);
    ares::oracleHooks.cpuBoundary = cpuBoundary;
    ares::oracleHooks.frameReady = frameReady;
    ares::oracleHooks.lineDrawn = lineDrawn;
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
    static const char* names[8] = {"Up", "Down", "Left", "Right", "1", "2", "Start", "Reset"};
    uint32_t mask = 0;
    for(int i = 0; i < 8; i++) {
      bool v = buttonHeld(names[i]);
      if(i == 4) v = v || buttonHeld("a");
      if(i == 5) v = v || buttonHeld("b");
      if(i == 6) v = v || buttonHeld("pause");
      if(v) mask |= 1u << i;
    }
    recInput(mask);
    g_root->run();
    auto& px = g_shot;
    uint32_t x0 = 13, y0 = 27, w = 256, h = M::vdp.vlines();
    if(g_gg) { x0 = 61; y0 = 51; w = 160; h = 144; }
    std::vector<uint16_t> raw((size_t)w * h);
    uint32_t line0 = g_gg ? 24 : 0;
    for(uint32_t y = 0; y < h; y++)
      for(uint32_t x = 0; x < w; x++)
        raw[(size_t)y * w + x] = g_drawnSnap[line0 + y] ? (uint16_t)px[((y0 + y) % 243) * 284 + x0 + x] : 0;
    recRawFrame(raw.data(), w, h);
    uint32_t hr = 2166136261u, hv = 2166136261u, hc = 2166136261u;
    for(u32 i = 0; i < 0x2000; i++) hr = (hr ^ (u8)M::cpu.ram[i]) * 16777619u;
    for(u32 i = 0; i < 0x4000; i++) hv = (hv ^ (u8)M::vdp.vram[i]) * 16777619u;
    for(u32 i = 0; i < 32; i++) {
      hc = (hc ^ (u8)M::vdp.cram[i]) * 16777619u;
      hc = (hc ^ (u8)(M::vdp.cram[i] >> 8)) * 16777619u;
    }
    char row[256];
    snprintf(row, sizeof row, "%04x\t%04x\t%04x\t%04x\t%04x\t%04x\t%04x\t%04x\t%08x\t%08x\t%08x\t%u\t%u", (unsigned)M::cpu.PC,
             (unsigned)M::cpu.SP, (unsigned)M::cpu.af.word, (unsigned)M::cpu.bc.word, (unsigned)M::cpu.de.word, (unsigned)M::cpu.hl.word,
             (unsigned)M::cpu.ix.word, (unsigned)M::cpu.iy.word, hr, hv, hc, (unsigned)M::vdp.io.vcounter, (unsigned)M::vdp.io.hcounter);
    recState(row);
    if(g_ilog) fflush(g_ilog);
  }
};

}  // namespace

namespace orc { Sys* makeMS(bool gg) { return new MS(gg); } }
