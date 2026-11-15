#include <nba/config.hh>
#include <nba/core.hh>
#include <nba/rom/backup/eeprom.hh>
#include <nba/rom/backup/flash.hh>
#include <nba/rom/rom.hh>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "arm/arm7tdmi.hh"
#include "bus/bus.hh"
#include "hw/apu/apu.hh"
#include "hw/dma/dma.hh"
#include "hw/irq/irq.hh"
#include "hw/keypad/keypad.hh"
#include "hw/ppu/ppu.hh"
#include "hw/timer/timer.hh"

namespace {

using namespace nba;
using namespace nba::core;

template <typename Tag, typename Tag::type M>
struct Expose {
  friend typename Tag::type member(Tag) { return M; }
};

struct LatchIrq {
  using type = bool arm::ARM7TDMI::*;
  friend type member(LatchIrq);
};
template struct Expose<LatchIrq, &arm::ARM7TDMI::latch_irq_disable>;

struct Reload16 {
  using type = void (arm::ARM7TDMI::*)();
  friend type member(Reload16);
};
template struct Expose<Reload16, &arm::ARM7TDMI::ReloadPipeline16>;

struct Reload32 {
  using type = void (arm::ARM7TDMI::*)();
  friend type member(Reload32);
};
template struct Expose<Reload32, &arm::ARM7TDMI::ReloadPipeline32>;

constexpr u64 kFnvBasis = 0xcbf29ce484222325ull;
constexpr u64 kFnvPrime = 0x100000001b3ull;

void fnv(u64& h, u8 b) {
  h ^= b;
  h *= kFnvPrime;
}

struct Video : VideoDevice {
  u64 frames = 0;
  std::vector<u16> last = std::vector<u16>(240 * 160, 0);

  void Draw(u32* buffer) override {
    for(int i = 0; i < 240 * 160; i++) {
      u32 p = buffer[i];
      u16 r = (p >> 19) & 31;
      u16 g = (p >> 11) & 31;
      u16 b = (p >> 3) & 31;
      last[i] = u16(r | g << 5 | b << 10);
    }
    frames++;
  }

  auto Hash() const -> u64 {
    u64 h = kFnvBasis;
    for(u16 c : last) {
      fnv(h, u8(c));
      fnv(h, u8(c >> 8));
    }
    return h;
  }
};

struct Audio : StereoResampler<float> {
  Audio() : StereoResampler<float>(nullptr) {}

  u64 samples = 0;
  u64 hash = kFnvBasis;

  void SetSampleRates(float, float) override {}

  void Write(StereoSample<float> const& s) override {
    for(float f : {s.left, s.right}) {
      s32 v = s32(f * 512.0f);
      fnv(hash, u8(v));
      fnv(hash, u8(v >> 8));
    }
    samples++;
  }
};

struct Sram : Backup {
  std::vector<u8> data = std::vector<u8>(0x8000, 0xFF);

  void Reset() override {}
  auto Read(u32 address) -> u8 override { return data[address & 0x7FFF]; }
  void Write(u32 address, u8 value) override { data[address & 0x7FFF] = value; }
  void LoadState(SaveState const&) override {}
  void CopyState(SaveState&) override {}
};

auto ReadFile(std::string const& path) -> std::vector<u8> {
  std::ifstream f(path, std::ios::binary);
  if(!f) {
    throw std::runtime_error("cannot read " + path);
  }
  return {std::istreambuf_iterator<char>(f), {}};
}

struct Machine {
  std::shared_ptr<Config> config;
  std::shared_ptr<Video> video;
  Scheduler scheduler;
  arm::ARM7TDMI cpu;
  IRQ irq;
  DMA dma;
  APU apu;
  PPU ppu;
  Timer timer;
  KeyPad keypad;
  Bus bus;
  Audio* audio = nullptr;
  Sram* sram = nullptr;
  std::vector<u8> rom_bytes;

  Machine(std::shared_ptr<Config> cfg, std::shared_ptr<Video> vid)
      : config(cfg)
      , video(vid)
      , cpu(scheduler, bus)
      , irq(cpu, scheduler)
      , dma(bus, irq, scheduler)
      , apu(scheduler, dma, bus, cfg)
      , ppu(scheduler, irq, dma, cfg)
      , timer(scheduler, irq, apu)
      , keypad(scheduler, irq)
      , bus(scheduler, {cpu, irq, dma, apu, ppu, timer, keypad}) {}

  void Reset(bool skip_bios) {
    scheduler.Reset();
    cpu.Reset();
    irq.Reset();
    dma.Reset();
    timer.Reset();
    apu.Reset();
    ppu.Reset();
    bus.Reset();
    keypad.Reset();
    auto a = std::make_unique<Audio>();
    audio = a.get();
    apu.resampler = std::move(a);
    if(skip_bios) {
      cpu.SwitchMode(arm::MODE_SYS);
      cpu.state.bank[arm::BANK_SVC][arm::BANK_R13] = 0x03007FE0;
      cpu.state.bank[arm::BANK_IRQ][arm::BANK_R13] = 0x03007FA0;
      cpu.state.r13 = 0x03007F00;
      cpu.state.r15 = 0x08000000;
    }
    video->frames = 0;
    std::fill(video->last.begin(), video->last.end(), 0);
  }

  auto Halted() const -> bool {
    return bus.hw.haltcnt != Bus::Hardware::HaltControl::Run;
  }

  void Step(u64 limit) {
    if(!Halted()) {
      cpu.Run();
      return;
    }
    while(scheduler.GetTimestampNow() < limit && !irq.ShouldUnhaltCPU()) {
      if(dma.IsRunning()) {
        dma.Run();
        if(irq.ShouldUnhaltCPU()) {
          continue;
        }
      }
      bus.Step(scheduler.GetRemainingCycleCount());
    }
    if(irq.ShouldUnhaltCPU()) {
      bus.Step(1);
      bus.hw.haltcnt = Bus::Hardware::HaltControl::Run;
    }
  }

  auto Now() const -> u64 { return scheduler.GetTimestampNow(); }

  auto Pc() const -> u32 {
    return cpu.state.r15 - (cpu.state.cpsr.f.thumb ? 4 : 8);
  }

  auto Peek(u32 a) -> u8 {
    switch(a >> 24) {
      case 0x00:
        return a < 0x4000 ? bus.memory.bios[a] : 0;
      case 0x02:
        return bus.memory.wram[a & 0x3FFFF];
      case 0x03:
        return bus.memory.iram[a & 0x7FFF];
      case 0x04:
        return (a & 0xFFFFFF) < 0x400 ? bus.hw.ReadByte(a) : 0;
      case 0x05:
        return ppu.GetPRAM()[a & 0x3FF];
      case 0x06: {
        u32 o = a & 0x1FFFF;
        if(o >= 0x18000) {
          o -= 0x8000;
        }
        return ppu.GetVRAM()[o];
      }
      case 0x07:
        return ppu.GetOAM()[a & 0x3FF];
      case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: {
        u32 o = a & 0x1FFFFFF;
        if(o < rom_bytes.size()) {
          return rom_bytes[o];
        }
        u16 h = u16(o >> 1);
        return (o & 1) ? u8(h >> 8) : u8(h);
      }
      case 0x0E: case 0x0F:
        return sram ? sram->data[a & 0x7FFF] : 0xFF;
      default:
        return 0;
    }
  }

  auto Poke(u32 a, u8 v) -> bool {
    switch(a >> 24) {
      case 0x02: bus.memory.wram[a & 0x3FFFF] = v; return true;
      case 0x03: bus.memory.iram[a & 0x7FFF] = v; return true;
      case 0x05: ppu.GetPRAM()[a & 0x3FF] = v; return true;
      case 0x06: {
        u32 o = a & 0x1FFFF;
        if(o >= 0x18000) {
          o -= 0x8000;
        }
        ppu.GetVRAM()[o] = v;
        return true;
      }
      case 0x07: ppu.GetOAM()[a & 0x3FF] = v; return true;
      case 0x0E: case 0x0F:
        if(sram) {
          sram->data[a & 0x7FFF] = v;
          return true;
        }
        return false;
      default: return false;
    }
  }

  static auto BankOf(u32 mode) -> int {
    switch(mode) {
      case arm::MODE_USR: case arm::MODE_SYS: return arm::BANK_NONE;
      case arm::MODE_FIQ: return arm::BANK_FIQ;
      case arm::MODE_IRQ: return arm::BANK_IRQ;
      case arm::MODE_SVC: return arm::BANK_SVC;
      case arm::MODE_ABT: return arm::BANK_ABT;
      case arm::MODE_UND: return arm::BANK_UND;
      default: return arm::BANK_INVALID;
    }
  }

  auto Banked(int bank, int idx) -> u32 {
    auto cur = BankOf(cpu.state.cpsr.f.mode);
    if(idx < 5) {
      bool fiq = bank == arm::BANK_FIQ;
      bool cur_fiq = cur == arm::BANK_FIQ;
      if(fiq == cur_fiq) {
        return cpu.state.reg[8 + idx];
      }
      return cpu.state.bank[fiq ? arm::BANK_FIQ : arm::BANK_NONE][idx];
    }
    if(bank == cur) {
      return cpu.state.reg[8 + idx];
    }
    return cpu.state.bank[bank][idx];
  }
};

auto Hex(u64 v, int w = 8) -> std::string {
  char b[24];
  std::snprintf(b, sizeof b, "%0*llx", w, (unsigned long long)v);
  return b;
}

auto ParseHex(std::string const& s) -> u64 {
  std::size_t n = 0;
  u64 v = std::stoull(s, &n, 16);
  if(n != s.size()) {
    throw std::runtime_error("bad hex " + s);
  }
  return v;
}

auto ParseDec(std::string const& s) -> u64 {
  std::size_t n = 0;
  u64 v = std::stoull(s, &n, 10);
  if(n != s.size()) {
    throw std::runtime_error("bad number " + s);
  }
  return v;
}

struct Server {
  Machine& m;
  bool skip_bios;
  u16 keys = 0;

  void Regs(std::ostream& o) {
    auto& s = m.cpu.state;
    o << "REGS";
    for(int i = 0; i < 16; i++) {
      o << " r" << i << "=" << Hex(s.reg[i]);
    }
    o << " cpsr=" << Hex(s.cpsr.v);
    static const char* names[] = {"usr", "fiq", "svc", "abt", "irq", "und"};
    for(int b = 1; b < 6; b++) {
      o << " spsr_" << names[b] << "=" << Hex(s.spsr[b].v);
    }
    for(int i = 0; i < 7; i++) {
      o << " r" << 8 + i << "_fiq=" << Hex(m.Banked(arm::BANK_FIQ, i));
    }
    for(int b = 0; b < 6; b++) {
      if(b == arm::BANK_FIQ) {
        continue;
      }
      o << " r13_" << names[b] << "=" << Hex(m.Banked(b, 5));
      o << " r14_" << names[b] << "=" << Hex(m.Banked(b, 6));
    }
    for(int i = 0; i < 5; i++) {
      o << " r" << 8 + i << "_usr=" << Hex(m.Banked(arm::BANK_NONE, i));
    }
    o << " pc=" << Hex(m.Pc()) << " halt=" << (m.Halted() ? 1 : 0) << " cyc=" << m.Now() << "\n";
  }

  void Trace(std::ostream& o, u32 pc, u32 op, bool thumb) {
    auto& s = m.cpu.state;
    o << "T " << m.Now() << " " << Hex(pc) << " " << Hex(op, thumb ? 4 : 8);
    for(int i = 0; i < 16; i++) {
      o << " " << Hex(s.reg[i]);
    }
    o << " " << Hex(s.cpsr.v) << "\n";
  }

  void StepTraced(std::ostream& o, u64 limit) {
    if(m.Halted()) {
      m.Step(limit);
      o << "H " << m.Now() << "\n";
      return;
    }
    auto& s = m.cpu.state;
    bool irq = m.cpu.IRQLine() && !(m.cpu.*member(LatchIrq{}));
    if(irq) {
      o << "I " << m.Now() << " " << Hex(m.Pc()) << "\n";
    }
    bool thumb = s.cpsr.f.thumb;
    u32 pc = m.Pc();
    u32 op = m.cpu.GetFetchedOpcode(0);
    if(irq) {
      thumb = false;
      pc = 0x18;
      op = u32(m.Peek(0x18)) | u32(m.Peek(0x19)) << 8 | u32(m.Peek(0x1A)) << 16 | u32(m.Peek(0x1B)) << 24;
    }
    m.Step(limit);
    Trace(o, pc, op, thumb);
  }

  void SetKeys(u16 mask) {
    keys = mask;
    for(int k = 0; k < int(Key::Count); k++) {
      m.keypad.SetKeyStatus(Key(k), (mask >> k) & 1);
    }
  }

  void SetReg(std::string const& name, u32 v) {
    auto& s = m.cpu.state;
    if(name == "cpsr") {
      auto mode = arm::Mode(v & 0x1F);
      m.cpu.SwitchMode(mode);
      s.cpsr.v = v;
      return;
    }
    if(name.size() > 1 && name[0] == 'r') {
      int i = std::stoi(name.substr(1));
      if(i >= 0 && i < 15) {
        s.reg[i] = v;
        return;
      }
    }
    static const std::pair<const char*, int> spsrs[] = {
      {"spsr_fiq", arm::BANK_FIQ}, {"spsr_svc", arm::BANK_SVC}, {"spsr_abt", arm::BANK_ABT},
      {"spsr_irq", arm::BANK_IRQ}, {"spsr_und", arm::BANK_UND}};
    for(auto& [n, b] : spsrs) {
      if(name == n) {
        s.spsr[b].v = v;
        return;
      }
    }
    throw std::runtime_error("unknown register " + name);
  }

  void Jump(u32 pc, bool thumb) {
    auto& s = m.cpu.state;
    s.cpsr.f.thumb = thumb;
    s.r15 = pc & (thumb ? ~1u : ~3u);
    if(thumb) {
      (m.cpu.*member(Reload16{}))();
    } else {
      (m.cpu.*member(Reload32{}))();
    }
  }

  auto Handle(std::vector<std::string> const& w, std::ostream& o) -> bool {
    auto const& c = w[0];
    auto need = [&](std::size_t n) {
      if(w.size() < n + 1) {
        throw std::runtime_error(c + " needs " + std::to_string(n) + " arguments");
      }
    };
    if(c == "QUIT") {
      o << "OK\n";
      return false;
    } else if(c == "RESET") {
      m.Reset(skip_bios);
      SetKeys(keys);
      o << "OK\n";
    } else if(c == "RUN") {
      need(2);
      u64 n = ParseDec(w[2]);
      if(w[1] == "cycles") {
        u64 limit = m.Now() + n;
        while(m.Now() < limit) {
          m.Step(limit);
        }
      } else if(w[1] == "frames") {
        u64 target = m.video->frames + n;
        u64 limit = m.Now() + (n + 1) * u64(CoreBase::kCyclesPerFrame);
        while(m.video->frames < target && m.Now() < limit) {
          m.Step(limit);
        }
        if(m.video->frames < target) {
          throw std::runtime_error("frame limit reached");
        }
      } else if(w[1] == "until") {
        need(3);
        u32 at = u32(ParseHex(w[3]));
        u64 limit = m.Now() + n;
        while(m.Now() < limit && (m.Halted() || m.Pc() != at)) {
          m.Step(limit);
        }
      } else {
        throw std::runtime_error("RUN cycles|frames|until");
      }
      o << "OK cyc=" << m.Now() << " frames=" << m.video->frames << "\n";
    } else if(c == "STEP") {
      need(1);
      u64 n = ParseDec(w[1]);
      u64 limit = m.Now() + 64 * u64(CoreBase::kCyclesPerFrame);
      for(u64 i = 0; i < n; i++) {
        StepTraced(o, limit);
      }
      o << "OK cyc=" << m.Now() << "\n";
    } else if(c == "KEYS") {
      need(1);
      SetKeys(u16(ParseHex(w[1])));
      o << "OK\n";
    } else if(c == "REGS") {
      Regs(o);
    } else if(c == "SET") {
      for(std::size_t i = 1; i < w.size(); i++) {
        auto eq = w[i].find('=');
        if(eq == std::string::npos) {
          throw std::runtime_error("SET name=hex");
        }
        SetReg(w[i].substr(0, eq), u32(ParseHex(w[i].substr(eq + 1))));
      }
      o << "OK\n";
    } else if(c == "JUMP") {
      need(2);
      Jump(u32(ParseHex(w[1])), w[2] == "1");
      o << "OK cyc=" << m.Now() << "\n";
    } else if(c == "MEM") {
      need(2);
      u32 a = u32(ParseHex(w[1]));
      u64 n = ParseHex(w[2]);
      if(n > 0x2000000) {
        throw std::runtime_error("MEM length too large");
      }
      o << "MEM ";
      for(u64 i = 0; i < n; i++) {
        o << Hex(m.Peek(u32(a + i)), 2);
      }
      o << "\n";
    } else if(c == "POKE") {
      need(2);
      u32 a = u32(ParseHex(w[1]));
      auto const& h = w[2];
      if(h.size() % 2) {
        throw std::runtime_error("POKE needs whole bytes");
      }
      for(std::size_t i = 0; i < h.size() / 2; i++) {
        if(!m.Poke(u32(a + i), u8(ParseHex(h.substr(2 * i, 2))))) {
          throw std::runtime_error("POKE outside writable memory at " + Hex(a + i));
        }
      }
      o << "OK\n";
    } else if(c == "IO") {
      o << "IO ";
      for(u32 a = 0; a < 0x400; a += 2) {
        u16 v = m.bus.hw.ReadHalf(0x04000000 + a);
        o << Hex(v & 0xFF, 2) << Hex(v >> 8, 2);
      }
      o << "\n";
    } else if(c == "IOPOKE") {
      need(2);
      u32 a = u32(ParseHex(w[1]));
      auto const& h = w[2];
      if(h.size() % 2 || a >= 0x400 || a + h.size() / 2 > 0x400) {
        throw std::runtime_error("IOPOKE needs whole bytes inside 0..3FF");
      }
      for(std::size_t i = 0; i < h.size() / 2; i++) {
        m.bus.hw.ppu.Sync();
        m.bus.hw.WriteByte(0x04000000 + u32(a + i), u8(ParseHex(h.substr(2 * i, 2))));
      }
      o << "OK\n";
    } else if(c == "PPUREGS") {
      auto& r = m.bus.hw.ppu.mmio;
      u8 img[0x58] = {};
      auto put16 = [&](int at, u16 v) {
        img[at] = u8(v);
        img[at + 1] = u8(v >> 8);
      };
      auto put32 = [&](int at, u32 v) {
        put16(at, u16(v));
        put16(at + 2, u16(v >> 16));
      };
      put16(0x00, r.dispcnt.ReadHalf());
      put16(0x02, r.greenswap);
      for(int i = 0; i < 4; i++) {
        put16(0x08 + 2 * i, r.bgcnt[i].ReadHalf());
        put16(0x10 + 4 * i, r.bghofs[i]);
        put16(0x12 + 4 * i, r.bgvofs[i]);
      }
      for(int i = 0; i < 2; i++) {
        int b = 0x20 + 0x10 * i;
        put16(b + 0, u16(r.bgpa[i]));
        put16(b + 2, u16(r.bgpb[i]));
        put16(b + 4, u16(r.bgpc[i]));
        put16(b + 6, u16(r.bgpd[i]));
        put32(b + 8, u32(r.bgx[i].initial) & 0x0FFFFFFF);
        put32(b + 12, u32(r.bgy[i].initial) & 0x0FFFFFFF);
        put16(0x40 + 2 * i, r.winh[i].ReadHalf());
        put16(0x44 + 2 * i, r.winv[i].ReadHalf());
      }
      put16(0x48, r.winin.ReadHalf());
      put16(0x4A, r.winout.ReadHalf());
      put16(0x4C, u16((r.mosaic.bg.size_x - 1) | (r.mosaic.bg.size_y - 1) << 4 | (r.mosaic.obj.size_x - 1) << 8 | (r.mosaic.obj.size_y - 1) << 12));
      put16(0x50, r.bldcnt.ReadHalf());
      put16(0x52, u16((r.eva & 31) | (r.evb & 31) << 8));
      put16(0x54, u16(r.evy & 31));
      o << "PPUREGS ";
      for(u8 b : img) {
        o << Hex(b, 2);
      }
      o << "\n";
    } else if(c == "FRAME") {
      o << "FRAME count=" << m.video->frames << " hash=" << Hex(m.video->Hash(), 16) << "\n";
    } else if(c == "PIXELS") {
      o << "PIXELS ";
      for(u16 p : m.video->last) {
        o << Hex(p & 0xFF, 2) << Hex(p >> 8, 2);
      }
      o << "\n";
    } else if(c == "AUDIO") {
      o << "AUDIO samples=" << m.audio->samples << " hash=" << Hex(m.audio->hash, 16) << "\n";
    } else if(c == "SAVE") {
      need(1);
      if(!m.sram) {
        throw std::runtime_error("no SRAM");
      }
      std::ofstream f(w[1], std::ios::binary);
      f.write((char const*)m.sram->data.data(), std::streamsize(m.sram->data.size()));
      if(!f) {
        throw std::runtime_error("cannot write " + w[1]);
      }
      o << "OK\n";
    } else {
      throw std::runtime_error("unknown command " + c);
    }
    return true;
  }

  void Serve() {
    std::string line;
    while(std::getline(std::cin, line)) {
      std::istringstream ss(line);
      std::vector<std::string> w;
      for(std::string t; ss >> t;) {
        w.push_back(t);
      }
      if(w.empty()) {
        continue;
      }
      std::ostringstream o;
      bool more = true;
      try {
        more = Handle(w, o);
      } catch(std::exception const& e) {
        o.str("");
        o << "ERR " << e.what() << "\n";
      }
      std::cout << o.str() << std::flush;
      if(!more) {
        return;
      }
    }
  }
};

auto HasSramTag(std::vector<u8> const& rom) -> bool {
  static const char tag[] = "SRAM_V";
  return std::search(rom.begin(), rom.end(), tag, tag + 6) != rom.end();
}

void Usage() {
  std::cerr << "usage: nba-oracle --rom FILE (--bios FILE | --skip-bios) [--bios FILE] "
               "[--save none|sram|flash64|flash128|eeprom4|eeprom64] [--save-file FILE]\n";
}

} // namespace

int main(int argc, char** argv) {
  std::string bios, rom, save = "detect", save_file;
  bool skip = false;
  for(int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto val = [&]() -> std::string {
      if(i + 1 >= argc) {
        Usage();
        std::exit(2);
      }
      return argv[++i];
    };
    if(a == "--bios") bios = val();
    else if(a == "--rom") rom = val();
    else if(a == "--skip-bios") skip = true;
    else if(a == "--save") save = val();
    else if(a == "--save-file") save_file = val();
    else {
      Usage();
      return 2;
    }
  }
  if(rom.empty() || (bios.empty() && !skip)) {
    Usage();
    return 2;
  }
  try {
    auto config = std::make_shared<Config>();
    auto video = std::make_shared<Video>();
    config->video_dev = video;
    config->skip_bios = skip;
    auto m = std::make_unique<Machine>(config, video);
    if(!bios.empty()) {
      m->bus.Attach(ReadFile(bios));
    }
    auto bytes = ReadFile(rom);
    if(bytes.size() > 0x2000000) {
      throw std::runtime_error("ROM larger than 32 MiB");
    }
    m->rom_bytes = bytes;
    if(save == "detect") {
      save = HasSramTag(bytes) ? "sram" : "none";
    }
    std::unique_ptr<Backup> backup;
    if(save == "sram") {
      auto s = std::make_unique<Sram>();
      if(!save_file.empty()) {
        std::ifstream f(save_file, std::ios::binary);
        if(f) {
          f.read((char*)s->data.data(), std::streamsize(s->data.size()));
        }
      }
      m->sram = s.get();
      backup = std::move(s);
    } else if(save != "none") {
      if(save_file.empty()) {
        throw std::runtime_error("--save " + save + " needs --save-file");
      }
      if(save == "flash64") backup = std::make_unique<FLASH>(save_file, FLASH::SIZE_64K);
      else if(save == "flash128") backup = std::make_unique<FLASH>(save_file, FLASH::SIZE_128K);
      else if(save == "eeprom4") backup = std::make_unique<EEPROM>(save_file, EEPROM::SIZE_4K, m->scheduler);
      else if(save == "eeprom64") backup = std::make_unique<EEPROM>(save_file, EEPROM::SIZE_64K, m->scheduler);
      else throw std::runtime_error("unknown save type " + save);
    }
    m->bus.Attach(ROM{std::move(bytes), std::move(backup), nullptr, 0x01FF'FFFF});
    m->Reset(skip);
    std::cout << "READY nba-oracle 1\n" << std::flush;
    Server{*m, skip}.Serve();
  } catch(std::exception const& e) {
    std::cerr << "nba-oracle: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
