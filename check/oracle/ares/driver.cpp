// n64-oracle: a headless ares (ISC) Nintendo 64 core that runs one unit of
// CPU instructions from a JSON state and prints the resulting state. MIT.
// The core is ares's; see README.md.
#include <n64/n64.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace N = ares::Nintendo64;
using Str = std::string;

static Str hex64(uint64_t v) { char b[24]; snprintf(b, sizeof b, "0x%016llx", (unsigned long long)v); return b; }
static Str hex32(uint32_t v) { char b[16]; snprintf(b, sizeof b, "0x%08x", v); return b; }
static Str fmt(const char* f, ...) {
  char b[4096]; va_list a; va_start(a, f); vsnprintf(b, sizeof b, f, a); va_end(a); return b;
}
static std::vector<uint8_t> readFile(const Str& p) {
  std::vector<uint8_t> d; FILE* f = fopen(p.c_str(), "rb"); if(!f) return d;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); d.resize(n);
  if(fread(d.data(), 1, n, f) != (size_t)n) d.clear();
  fclose(f); return d;
}
static bool writeFile(const Str& p, const Str& s) {
  FILE* f = fopen(p.c_str(), "wb"); if(!f) return false; fwrite(s.data(), 1, s.size(), f); fclose(f); return true;
}
static Str baseName(Str p) {
  auto s = p.find_last_of('/'); if(s != Str::npos) p = p.substr(s + 1);
  auto d = p.find_last_of('.'); if(d != Str::npos) p = p.substr(0, d);
  return p;
}

struct JV {
  enum T { Null, Num, SStr, Arr, Obj, Bool } t = Null;
  double n = 0; uint64_t u = 0; bool isInt = false; Str s; bool b = false;
  std::vector<JV> a; std::vector<Str> ok; std::vector<JV> ov;
  const JV* get(const Str& k) const { for(size_t i = 0; i < ok.size(); i++) if(ok[i] == k) return &ov[i]; return nullptr; }
  uint64_t asU64() const {
    if(t == SStr) return strtoull(s.c_str(), nullptr, 0);
    if(t == Num) return isInt ? u : (uint64_t)(int64_t)n;
    if(t == Bool) return b;
    return 0;
  }
};
struct JP {
  const char* p; const char* e;
  void ws() { while(p < e && (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r')) p++; }
  bool parse(JV& v) {
    ws(); if(p >= e) return false;
    if(*p == '{') {
      p++; v.t = JV::Obj; ws(); if(*p == '}') { p++; return true; }
      for(;;) {
        JV k; ws(); if(!parse(k) || k.t != JV::SStr) return false;
        ws(); if(*p++ != ':') return false;
        JV x; if(!parse(x)) return false;
        v.ok.push_back(k.s); v.ov.push_back(x);
        ws(); if(*p == ',') { p++; continue; } if(*p == '}') { p++; return true; } return false;
      }
    }
    if(*p == '[') {
      p++; v.t = JV::Arr; ws(); if(*p == ']') { p++; return true; }
      for(;;) {
        JV x; if(!parse(x)) return false;
        v.a.push_back(x);
        ws(); if(*p == ',') { p++; continue; } if(*p == ']') { p++; return true; } return false;
      }
    }
    if(*p == '"') {
      p++; v.t = JV::SStr;
      while(p < e && *p != '"') { if(*p == '\\') { p++; char c = *p++; v.s += c == 'n' ? '\n' : c == 't' ? '\t' : c; } else v.s += *p++; }
      p++; return true;
    }
    if(!strncmp(p, "true", 4)) { p += 4; v.t = JV::Bool; v.b = true; return true; }
    if(!strncmp(p, "false", 5)) { p += 5; v.t = JV::Bool; v.b = false; return true; }
    if(!strncmp(p, "null", 4)) { p += 4; return true; }
    char* end; v.t = JV::Num; v.n = strtod(p, &end);
    if(end == p) return false;
    bool isi = true; for(const char* q = p; q < end; q++) if(*q == '.' || *q == 'e' || *q == 'E') isi = false;
    if(isi) { v.isInt = true; v.u = (*p == '-') ? (uint64_t)strtoll(p, nullptr, 10) : strtoull(p, nullptr, 10); }
    p = end; return true;
  }
};

struct Opt {
  Str rom, outDir;
  uint64_t stepInsns = 0;
  std::vector<Str> stateFiles;
} opt;

static std::map<uint32_t, uint64_t> g_excCount;
static std::vector<uint8_t> g_romBE;
static ares::Node::System g_root;

template<uint32_t S> static uint64_t rdr(uint32_t a) { return ((N::Memory::Writable&)N::rdram.ram).read<S>(a); }
template<uint32_t S> static void rdw(uint32_t a, uint64_t v) { ((N::Memory::Writable&)N::rdram.ram).write<S>(a, v); }
static N::CPU& CPU() { return N::cpu; }

struct OraclePlatform : ares::Platform {
  std::shared_ptr<vfs::directory> cartPak;
  auto pak(ares::Node::Object node) -> std::shared_ptr<vfs::directory> override {
    if(node->name() == "Nintendo 64") return std::make_shared<vfs::directory>();
    if(node->name() == "Nintendo 64 Cartridge" || node->name().find("Cartridge")) return cartPak;
    return std::make_shared<vfs::directory>();
  }
} g_platform;

static bool loadRom(const Str& path, uint32_t& entryOut) {
  std::vector<uint8_t> rom;
  if(path.empty()) {
    rom.assign(0x2000, 0);
    rom[0] = 0x80; rom[1] = 0x37; rom[2] = 0x12; rom[3] = 0x40;
    rom[8] = 0x80; rom[9] = 0x00; rom[10] = 0x10; rom[11] = 0x00;
  } else {
    rom = readFile(path);
    if(rom.size() < 0x1000) { fprintf(stderr, "bad rom %s\n", path.c_str()); return false; }
    uint32_t w = rom[0] << 24 | rom[1] << 16 | rom[2] << 8 | rom[3];
    if(w == 0x37804012) for(size_t i = 0; i + 1 < rom.size(); i += 2) std::swap(rom[i], rom[i + 1]);
    else if(w == 0x40123780) for(size_t i = 0; i + 3 < rom.size(); i += 4) { std::swap(rom[i], rom[i + 3]); std::swap(rom[i + 1], rom[i + 2]); }
  }
  entryOut = rom[8] << 24 | rom[9] << 16 | rom[10] << 8 | rom[11];
  static std::vector<uint8_t> keep;
  keep = std::move(rom);
  g_romBE = keep;
  auto d = std::make_shared<vfs::directory>();
  d->setAttribute("title", nall::string{"oracle"});
  d->setAttribute("region", nall::string{"NTSC"});
  d->setAttribute("cic", nall::string{"CIC-NUS-6102"});
  d->append("program.rom", std::span<const u8>(keep.data(), keep.size()));
  g_platform.cartPak = d;
  return true;
}

static void bootSkip() {
  auto& c = CPU();
  for(uint32_t n = 0; n < 4; n++) {
    auto& ch = N::rdram.chips[n];
    if(!ch.present) continue;
    ch.enable = 1; ch.deviceID = n * 2; ch.cci = ch.ccHigh; ch.ccInternal = ch.ccHigh;
  }
  N::rdram.updateMapping();
  N::ri.io.mode = 0x0e; N::ri.io.config = 0x40; N::ri.io.currentLoad = 0x40; N::ri.io.select = 0x14; N::ri.io.refresh = 0x00063634;
  N::ri.io.latency = 1; N::ri.io.currentLoaded = 1;
  N::pi.bsd1.latency = g_romBE[3]; N::pi.bsd1.pulseWidth = g_romBE[2]; N::pi.bsd1.pageSize = g_romBE[1] & 15; N::pi.bsd1.releaseDuration = g_romBE[1] >> 4 & 3;
  for(uint32_t i = 0; i < 0x1000; i += 4) {
    uint32_t w = g_romBE[i] << 24 | g_romBE[i + 1] << 16 | g_romBE[i + 2] << 8 | g_romBE[i + 3];
    N::rsp.dmem.write<N::Word>(i, w);
  }
  N::pif.state = N::PIF::Run; N::pif.io.romLockout = 1; N::pif.io.resetEnabled = 1; N::pif.joyInit();
  N::cic.state = N::CIC::Run;
  uint64_t K = 0xFFFFFFFF00000000ull;
  for(int i = 0; i < 32; i++) c.ipu.r[i].u64 = 0;
  c.ipu.r[11].u64 = K | 0xA4000040;
  c.ipu.r[20].u64 = 1;
  c.ipu.r[22].u64 = N::cic.seed;
  c.ipu.r[29].u64 = K | 0xA4001FF0;
  c.ipu.r[31].u64 = K | 0xA4001554;
  c.setControlRegister(12, 0x34000000);
  c.setControlRegister(13, 0);
  c.scc.count = 0; c.scc.compare = 0;
  c.scc.nmiPending = 0;
  c.pipeline.setPc(K | 0xA4000040);
  c.context.setMode();
}

static bool bootSystem(const Str& romPath, uint32_t& entry) {
  if(!loadRom(romPath, entry)) return false;
  ares::platform = &g_platform;
  N::option("Enable GPU acceleration", "false");
  N::option("Quality", "SD");
  N::option("Deterministic Entropy", "true");
  N::option("Recompiler", "false");
  N::option("Expansion Pak", "true");
  N::option("Homebrew Mode", "false");
  if(!N::load(g_root, nall::string{"[Nintendo] Nintendo 64 (NTSC)"})) { fprintf(stderr, "ares load failed\n"); return false; }
  if(auto port = g_root->find<ares::Node::Port>("Cartridge Slot")) { port->allocate(); port->connect(); }
  g_root->power();
  if(N::system._vulkanNeedsLoad) { N::vulkan.load(N::system.node); N::system._vulkanNeedsLoad = false; }
  bootSkip();
  return true;
}

static const char* kCop0Names[32] = {"index", "random", "entrylo0", "entrylo1", "context", "pagemask", "wired", "r7", "badvaddr", "count",
  "entryhi", "compare", "status", "cause", "epc", "prid", "config", "lladdr", "watchlo", "watchhi", "xcontext", "r21", "r22", "r23", "r24", "r25",
  "perr", "cacheerr", "taglo", "taghi", "errorepc", "r31"};

static Str dumpState(const std::vector<std::pair<uint32_t, uint32_t>>& regions) {
  auto& c = CPU();
  c.flushCount();
  Str j = "{\n  \"gpr\": [";
  for(int i = 0; i < 32; i++) j += (i ? ", \"" : "\"") + hex64(c.ipu.r[i].u64) + "\"";
  j += "],\n  \"hi\": \"" + hex64(c.ipu.hi.u64) + "\", \"lo\": \"" + hex64(c.ipu.lo.u64) + "\", \"pc\": \"" + hex64(c.ipu.pc) + "\",\n  \"cop0\": {";
  bool first = true;
  for(int i = 0; i < 31; i++) {
    if(i == 1 || i == 7 || (i >= 21 && i <= 25)) continue;
    uint64_t v = c.getControlRegister(i);
    if(i == 9) v = (uint64_t)(c.scc.count >> 1) & 0xffffffffull;
    if(i == 11) v = (uint64_t)(c.scc.compare >> 1) & 0xffffffffull;
    j += Str(first ? "" : ", ") + "\"" + kCop0Names[i] + "\": \"" + hex64(v) + "\""; first = false;
  }
  j += "},\n  \"fpr\": [";
  for(int i = 0; i < 32; i++) j += (i ? ", \"" : "\"") + hex64(c.fpu.r[i].u64) + "\"";
  j += "],\n  \"fcr31\": \"" + hex32(c.getControlRegisterFPU(31)) + "\"";
  if(!regions.empty()) {
    j += ",\n  \"mem\": [";
    bool f = true;
    for(auto& r : regions) {
      Str hx; for(uint32_t i = 0; i < r.second; i++) hx += fmt("%02x", (unsigned)rdr<N::Byte>((r.first + i) & 0x7fffff));
      j += Str(f ? "" : ", ") + "{\"addr\": \"" + hex32(r.first) + "\", \"bytes\": \"" + hx + "\"}"; f = false;
    }
    j += "]";
  }
  j += "\n}\n";
  return j;
}

static void flushCaches() {
  auto& c = CPU();
  for(auto& l : c.dcache.lines) { if(l.valid() && l.dirty) l.writeBack(); l.setValid(false); l.dirty = 0; }
  for(auto& l : c.icache.lines) l.setValid(false);
}

static void applyState(const JV& s, std::vector<std::pair<uint32_t, uint32_t>>& regions) {
  auto& c = CPU();
  flushCaches();
  c.flushCount();
  if(auto m = s.get("mem")) for(auto& r : m->a) {
    uint32_t addr = (uint32_t)r.get("addr")->asU64() & 0x1fffffff; Str hx = r.get("bytes")->s;
    uint32_t n = hx.size() / 2;
    for(uint32_t i = 0; i < n; i++) rdw<N::Byte>(addr + i, (uint8_t)strtoul(hx.substr(i * 2, 2).c_str(), nullptr, 16));
    regions.push_back({addr, n});
  }
  if(auto g = s.get("gpr")) for(int i = 0; i < 32 && i < (int)g->a.size(); i++) c.ipu.r[i].u64 = g->a[i].asU64();
  c.ipu.r[0].u64 = 0;
  if(auto v = s.get("hi")) c.ipu.hi.u64 = v->asU64();
  if(auto v = s.get("lo")) c.ipu.lo.u64 = v->asU64();
  if(auto o = s.get("cop0")) {
    auto set = [&](int n, uint64_t v) { c.setControlRegister(n, v); };
    if(auto v = o->get("status")) set(12, v->asU64());
    for(int i = 0; i < 31; i++) {
      if(i == 12 || i == 1 || i == 8 || i == 15 || i == 9) continue;
      if(auto v = o->get(kCop0Names[i])) {
        if(i == 13) { c.scc.cause.interruptPending.bit(0) = v->asU64() >> 8 & 1; c.scc.cause.interruptPending.bit(1) = v->asU64() >> 9 & 1; c.scc.cause.exceptionCode = v->asU64() >> 2 & 31; c.scc.cause.branchDelay = v->asU64() >> 31 & 1; }
        else set(i, v->asU64());
      }
    }
    if(auto v = o->get("badvaddr")) c.scc.badVirtualAddress = v->asU64();
    if(auto v = o->get("count")) c.scc.count = (uint64_t)(v->asU64() & 0xffffffffull) << 1;
  }
  if(auto f = s.get("fpr")) for(int i = 0; i < 32 && i < (int)f->a.size(); i++) c.fpu.r[i].u64 = f->a[i].asU64();
  if(auto v = s.get("fcr31")) c.setControlRegisterFPU(31, (uint32_t)v->asU64());
  N::mi.irq.sp.mask = N::mi.irq.si.mask = N::mi.irq.ai.mask = N::mi.irq.vi.mask = N::mi.irq.pi.mask = N::mi.irq.dp.mask = 0;
  N::mi.poll();
  c.setInterruptPending(N::CPU::Interrupt::Timer, 0);
  uint64_t pc = 0; if(auto v = s.get("pc")) pc = v->asU64();
  c.pipeline.setPc(pc);
  c.context.setMode();
}

static int cmdStep() {
  uint32_t entry = 0;
  if(!bootSystem(opt.rom, entry)) return 2;
  N::oracleHooks.exception = [](u32 code, u64, bool) { g_excCount[code]++; };
  auto& c = CPU();
  if(!opt.rom.empty()) {
    uint64_t entry64 = (uint64_t)(int64_t)(int32_t)entry;
    uint64_t guard = 0;
    while(c.ipu.pc != entry64) {
      if(c.instruction()) c.synchronize();
      if(++guard > 400000000ull) { fprintf(stderr, "IPL3 did not reach entry %s\n", hex64(entry64).c_str()); return 3; }
    }
  }
  auto snap = g_root->serialize();
  bool canSnap = snap.size() > 0;
  int rc = 0;
  for(size_t k = 0; k < opt.stateFiles.size(); k++) {
    auto data = readFile(opt.stateFiles[k]);
    JV root; JP jp{(const char*)data.data(), (const char*)data.data() + data.size()};
    if(data.empty() || !jp.parse(root)) { fprintf(stderr, "bad json %s\n", opt.stateFiles[k].c_str()); rc = 4; continue; }
    if(k > 0 && canSnap) { nall::serializer s2(snap.data(), snap.size()); if(!g_root->unserialize(s2)) fprintf(stderr, "warning: snapshot restore failed\n"); }
    std::vector<std::pair<uint32_t, uint32_t>> regions;
    applyState(root, regions);
    uint64_t pc0 = c.ipu.pc;
    uint64_t maxInsns = opt.stepInsns; if(auto v = root.get("max_insns")) maxInsns = v->asU64();
    g_excCount.clear();
    bool warm = true; if(auto v = root.get("warm_icache")) warm = v->asU64() != 0;
    if(warm) for(uint64_t a = pc0 & ~31ull; a < pc0 + 8; a += 32) c.icacheFillLine(a, (uint32_t)a & 0x1fffffff);
    uint64_t n = 0;
    bool done = false;
    while(!done) {
      if(c.instruction()) c.synchronize();
      n++;
      if(maxInsns) { if(n >= maxInsns) done = true; }
      else if(!c.pipeline.inDelaySlot()) done = true;
      if(n > 1000000) done = true;
    }
    Str out = dumpState(regions);
    out.resize(out.rfind('}'));
    out += fmt(",\n  \"start_pc\": \"%s\", \"insns\": %llu", hex64(pc0).c_str(), (unsigned long long)n);
    out += ",\n  \"exceptions\": {"; bool fst = true;
    for(auto& kv : g_excCount) { out += fmt("%s\"%u\": %llu", fst ? "" : ", ", kv.first, (unsigned long long)kv.second); fst = false; }
    out += "}\n}\n";
    if(opt.stateFiles.size() == 1 && opt.outDir == "-") fputs(out.c_str(), stdout);
    else writeFile((opt.outDir.empty() ? opt.stateFiles[k] + ".out.json" : opt.outDir + "/" + baseName(opt.stateFiles[k]) + ".out.json"), out);
  }
  return rc;
}

static void usage() {
  fprintf(stderr, "n64-oracle step STATE.json [STATE2.json ...] [--rom ROM] [--out DIR|-] [--insns K]\n"
                  "  runs each state for one unit (an instruction, or a branch and its delay slot; K instructions with --insns)\n"
                  "  and writes DIR/<name>.out.json. Without --rom the core starts at once, without running a boot ROM.\n");
}

int main(int argc, char** argv) {
  if(argc < 2 || Str(argv[1]) != "step") { usage(); return 1; }
  for(int i = 2; i < argc; i++) {
    Str a = argv[i];
    auto next = [&]() -> Str { if(i + 1 >= argc) { usage(); exit(1); } return argv[++i]; };
    if(a == "--rom") opt.rom = next();
    else if(a == "--out") opt.outDir = next();
    else if(a == "--insns") opt.stepInsns = strtoull(next().c_str(), nullptr, 0);
    else if(a.rfind("--", 0) == 0) { usage(); return 1; }
    else opt.stateFiles.push_back(a);
  }
  if(opt.stateFiles.empty()) { usage(); return 1; }
  return cmdStep();
}
