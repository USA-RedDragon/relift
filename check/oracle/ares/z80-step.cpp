// SPDX-License-Identifier: MIT
// Part of relift's checker (check/oracle/ares). The ares sources it is built
// with are ISC; this file is MIT and the two never share a binary with relift.
// z80-step: runs ONE instruction of ares' Z80 core per SingleStepTests/z80 JSON line, or,
// when the line has an "irq" field (0 NMI, 1 IM 0, 2 IM 1, 3 IM 2; 0xFF on the bus), takes that
// interrupt instead. Usage: z80-step [--check] [--quiet] < tests.jsonl
// Input: one test object per line, or a whole SST file (a JSON array; objects are split on the fly).
#include <ares/ares.hpp>
#include <algorithm>
#include <map>
#include <component/processor/z80/z80.hpp>
#include "json.hpp"

using namespace ares;

struct Port { uint32_t addr, val; bool write; bool used; };

struct Sim : Z80, Z80::Bus {
  uint8_t mem[65536];
  bool memTouched[65536];
  std::vector<Port> ports;
  uint32_t tstates = 0;
  struct Acc { uint32_t t; uint32_t addr; int val; char kind; };
  std::vector<Acc> acc;
  std::vector<std::pair<uint32_t, uint8_t>> wrote;

  Sim() { bus = this; }
  auto step(u32 clocks) -> void override { tstates += clocks; }
  auto synchronizing() const -> bool override { return false; }
  auto read(n16 a) -> n8 override { uint8_t v = mem[a]; acc.push_back({tstates, a, v, 'r'}); return v; }
  auto fetch(n16 a) -> n8 override { uint8_t v = mem[a]; acc.push_back({tstates, a, v, 'f'}); return v; }
  auto write(n16 a, n8 d) -> void override { mem[a] = d; memTouched[a] = true; acc.push_back({tstates, a, d, 'w'}); }
  auto in(n16 a) -> n8 override {
    uint8_t v = 0xff;
    for(auto& p : ports) if(!p.used && !p.write && p.addr == a) { p.used = true; v = p.val; break; }
    acc.push_back({tstates, a, v, 'i'}); return v;
  }
  auto out(n16 a, n8 d) -> void override { acc.push_back({tstates, a, d, 'o'}); outs.push_back({a, d}); }
  std::vector<std::pair<uint32_t, uint8_t>> outs;
};

static const JV* G(const JV& o, const char* k) { auto p = o.get(k); return p; }
static uint64_t U(const JV& o, const char* k) { auto p = o.get(k); return p ? p->asU64() : 0; }

static void setState(Sim& z, const JV& s) {
  z.power(Z80::MOSFET::NMOS);
  z.PC = U(s, "pc"); z.SP = U(s, "sp");
  z.af.byte.hi = U(s, "a"); z.af.byte.lo = U(s, "f");
  z.bc.byte.hi = U(s, "b"); z.bc.byte.lo = U(s, "c");
  z.de.byte.hi = U(s, "d"); z.de.byte.lo = U(s, "e");
  z.hl.byte.hi = U(s, "h"); z.hl.byte.lo = U(s, "l");
  z.ir.byte.hi = U(s, "i"); z.ir.byte.lo = U(s, "r");
  z.EI = U(s, "ei"); z.wz.word = U(s, "wz");
  z.ix.word = U(s, "ix"); z.iy.word = U(s, "iy");
  z.af_.word = U(s, "af_"); z.bc_.word = U(s, "bc_"); z.de_.word = U(s, "de_"); z.hl_.word = U(s, "hl_");
  z.IM = U(s, "im"); z.P = U(s, "p"); z.Q = U(s, "q") != 0;
  z.IFF1 = U(s, "iff1"); z.IFF2 = U(s, "iff2");
  z.HALT = 0;
}

struct St { std::vector<std::pair<Str, uint64_t>> f; };
static void getState(Sim& z, std::vector<std::pair<Str, uint64_t>>& o) {
  auto put = [&](const char* k, uint64_t v) { o.push_back({k, v}); };
  put("a", z.af.byte.hi); put("b", z.bc.byte.hi); put("c", z.bc.byte.lo); put("d", z.de.byte.hi); put("e", z.de.byte.lo);
  put("f", z.af.byte.lo); put("h", z.hl.byte.hi); put("l", z.hl.byte.lo); put("i", z.ir.byte.hi); put("r", z.ir.byte.lo);
  put("af_", z.af_.word); put("bc_", z.bc_.word); put("de_", z.de_.word); put("hl_", z.hl_.word);
  put("ix", z.ix.word); put("iy", z.iy.word); put("pc", z.PC); put("sp", z.SP); put("wz", z.wz.word);
  put("iff1", z.IFF1); put("iff2", z.IFF2); put("im", z.IM); put("ei", z.EI); put("p", z.P);
  put("q", z.Q ? (uint64_t)(u8)z.af.byte.lo : 0ull);
}

static uint64_t g_n = 0, g_pass = 0, g_cpass = 0, g_regpass = 0;
static std::map<Str, uint64_t> g_failByReg;
static std::map<Str, std::pair<uint64_t, uint64_t>> g_byOp;  // name prefix -> (n, pass)

static void runTest(const JV& t, bool check, FILE* out, std::map<Str, uint64_t>& dummy) {
  static Sim z;
  memset(z.mem, 0, sizeof z.mem); memset(z.memTouched, 0, sizeof z.memTouched);
  z.ports.clear(); z.acc.clear(); z.outs.clear(); z.tstates = 0;
  auto ini = G(t, "initial"); if(!ini) return;
  for(auto& r : G(*ini, "ram")->a) z.mem[r.a[0].asU64() & 0xffff] = r.a[1].asU64();
  if(auto pp = G(t, "ports")) for(auto& p : pp->a) z.ports.push_back({(uint32_t)p.a[0].asU64(), (uint32_t)p.a[1].asU64(), p.a[2].s == "w", false});
  setState(z, *ini);
  if(auto ip = G(t, "irq")) { if(ip->asU64() == 0) z.nmi(); else z.irq(0xff); } else z.instruction();
  std::vector<std::pair<Str, uint64_t>> st; getState(z, st);
  std::vector<std::pair<uint32_t, uint8_t>> ram;
  { std::vector<uint32_t> addrs; for(auto& r : G(*ini, "ram")->a) addrs.push_back(r.a[0].asU64() & 0xffff);
    for(auto& a : z.acc) if(a.kind == 'w') addrs.push_back(a.addr);
    std::sort(addrs.begin(), addrs.end()); addrs.erase(std::unique(addrs.begin(), addrs.end()), addrs.end());
    for(auto a : addrs) ram.push_back({a, z.mem[a]}); }

  Str nm = G(t, "name") ? G(t, "name")->s : "";
  // result line
  Str o = "{\"name\":\"" + nm + "\",\"initial\":RAWINI,\"final\":{";
  for(size_t i = 0; i < st.size(); i++) { o += (i ? "," : ""); o += "\"" + st[i].first + "\":" + std::to_string(st[i].second); }
  o += ",\"ram\":[";
  for(size_t i = 0; i < ram.size(); i++) { o += (i ? "," : ""); o += "[" + std::to_string(ram[i].first) + "," + std::to_string(ram[i].second) + "]"; }
  o += "]},\"cycles\":[";
  for(size_t i = 0; i < z.acc.size(); i++) {
    auto& a = z.acc[i]; o += (i ? "," : "");
    o += "[" + std::to_string(a.addr) + "," + std::to_string(a.val) + ",\"" + a.kind + "\"," + std::to_string(a.t) + "]";
  }
  o += "],\"tstates\":" + std::to_string(z.tstates) + ",\"ports\":[";
  { bool first = true; for(auto& a : z.acc) if(a.kind == 'i' || a.kind == 'o') { o += (first ? "" : ","); first = false;
      o += "[" + std::to_string(a.addr) + "," + std::to_string(a.val) + ",\"" + (a.kind == 'i' ? "r" : "w") + "\"]"; } }
  o += "]";

  if(check) {
    auto fin = G(t, "final"); bool ok = true, regok = true;
    std::vector<Str> bad;
    for(auto& kv : st) {
      auto e = fin->get(kv.first); if(!e) continue;
      if(e->asU64() != kv.second) { regok = false; bad.push_back(kv.first); g_failByReg[kv.first]++; }
    }
    // ram compare: every expected (addr,val) must match
    for(auto& r : G(*fin, "ram")->a) { uint32_t a = r.a[0].asU64(); if(z.mem[a] != r.a[1].asU64()) { regok = false; bad.push_back("ram"); g_failByReg["ram"]++; break; } }
    uint64_t sc = G(t, "cycles") ? G(t, "cycles")->a.size() : 0;
    bool cok = (sc == z.tstates);
    ok = regok && cok;
    g_n++; g_regpass += regok; g_cpass += cok; g_pass += ok;
    Str key = nm.substr(0, nm.find(' '));
    auto& b = g_byOp[key]; b.first++; b.second += ok;
    o += ",\"match\":" + Str(ok ? "true" : "false") + ",\"regs_match\":" + (regok ? "true" : "false") + ",\"cycles_match\":" + (cok ? "true" : "false") + ",\"sst_tstates\":" + std::to_string(sc);
    if(!bad.empty()) { o += ",\"bad\":["; for(size_t i = 0; i < bad.size(); i++) o += (i ? "," : "") + Str("\"") + bad[i] + "\""; o += "]"; }
  }
  o += "}\n";
  // splice raw initial: re-serialize cheaply from the original text
  fputs(o.c_str(), out);
}

int main(int argc, char** argv) {
  bool check = false, quiet = false;
  for(int i = 1; i < argc; i++) { if(!strcmp(argv[i], "--check")) check = true; if(!strcmp(argv[i], "--quiet")) quiet = true; }
  Str all; { char buf[1 << 16]; size_t n; while((n = fread(buf, 1, sizeof buf, stdin)) > 0) all.append(buf, n); }
  // split into test objects: either JSON lines or a top-level array
  std::vector<std::pair<size_t, size_t>> spans;
  { size_t i = 0, N = all.size(); int depth = 0; size_t start = 0; bool instr = false;
    for(; i < N; i++) {
      char c = all[i];
      if(instr) { if(c == '\\') i++; else if(c == '"') instr = false; continue; }
      if(c == '"') instr = true;
      else if(c == '{') { if(depth == 0) start = i; depth++; }
      else if(c == '}') { depth--; if(depth == 0) spans.push_back({start, i + 1}); }
    } }
  FILE* out = quiet ? fopen("/dev/null", "w") : stdout;
  std::map<Str, uint64_t> d;
  for(auto& sp : spans) {
    JV t; JP p{all.data() + sp.first, all.data() + sp.second};
    if(!p.parse(t)) { fprintf(stderr, "bad json\n"); continue; }
    // capture raw initial text for echo
    Str o;
    FILE* mem = out;
    // run test, printing with RAWINI replaced by a compact copy of the input's initial
    // (we emit via tmp buffer)
    char* bufp = nullptr; size_t bl = 0; FILE* ms = open_memstream(&bufp, &bl);
    runTest(t, check, ms, d); fclose(ms);
    Str line(bufp, bl); free(bufp);
    size_t k = all.find("\"initial\":", sp.first); size_t ie = k;
    if(k != Str::npos && k < sp.second) { size_t s = k + 10; int dd = 0; size_t j = s; for(; j < sp.second; j++) { if(all[j] == '{') dd++; else if(all[j] == '}') { dd--; if(!dd) { j++; break; } } }
      ie = j; line.replace(line.find("RAWINI"), 6, all.substr(s, ie - s)); }
    fputs(line.c_str(), mem);
  }
  if(check) {
    fprintf(stderr, "z80-step check: %llu tests, all-match %llu (%.2f%%), regs+ram match %llu, tstate count match %llu\n",
      (unsigned long long)g_n, (unsigned long long)g_pass, g_n ? 100.0 * g_pass / g_n : 0.0, (unsigned long long)g_regpass, (unsigned long long)g_cpass);
    for(auto& kv : g_failByReg) fprintf(stderr, "  field %s differs in %llu tests\n", kv.first.c_str(), (unsigned long long)kv.second);
    for(auto& kv : g_byOp) if(kv.second.second != kv.second.first) fprintf(stderr, "  opcode %s: %llu/%llu\n", kv.first.c_str(), (unsigned long long)kv.second.second, (unsigned long long)kv.second.first);
  }
  return 0;
}
