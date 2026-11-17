// SPDX-License-Identifier: MIT
// sega-oracle: options, the input script and the trace writers. See
// README.md for the files a run writes.
#include "oracle.hpp"
#include <zlib.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <tuple>
#include <unordered_map>

namespace orc {

Opt opt;
uint64_t g_frame = 0;
std::vector<uint8_t> g_rom;
uint32_t g_romSize = 0;

namespace {

struct ExecRec {
  uint32_t count = 0, first = 0, changes = 0;
  uint8_t bytes[4] = {};
  uint8_t space = 0;
  uint32_t pc = 0;
  int64_t off = 0;
  bool written = false;
  uint32_t wpc = 0;
  int64_t src = -1;
};
std::unordered_map<uint64_t, ExecRec> g_exec;
struct Wr { uint32_t pc = 0; bool set = false; int64_t src = -1; };
std::vector<Wr> g_wr;
/** Every byte form an instruction in RAM, cartridge RAM or the open bus ran with. */
struct RamForm { int space; uint32_t pc; int64_t off; uint32_t first; };
std::map<std::tuple<int, uint32_t, int64_t, uint32_t>, RamForm> g_ramForms;
FILE *f_map = nullptr, *f_irq = nullptr, *f_frames = nullptr, *f_raw = nullptr, *f_input = nullptr, *f_state = nullptr;
uint64_t n_irqRows = 0, n_mapper = 0;
struct IrqAgg { uint64_t count = 0, first = 0, last = 0; };
std::map<std::string, IrqAgg> g_irqAgg;
std::string g_trace;

struct Ev { uint64_t frame, hold; std::string name; };
std::vector<Ev> g_ev;
std::vector<std::string> g_randNames;
std::string lower(std::string s) { for(auto& c : s) c = tolower((unsigned char)c); return s; }
uint64_t splitmix(uint64_t x) {
  x += 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return x ^ (x >> 31);
}

void mkdirs(const std::string& p) { std::string c = "mkdir -p '" + p + "'"; if(system(c.c_str())) {} }
FILE* openOut(const std::string& name) {
  FILE* f = fopen((g_trace + "/" + name).c_str(), "wb");
  if(!f) { perror(name.c_str()); exit(2); }
  return f;
}

uint64_t key(int space, uint32_t pc, int64_t off) {
  return (uint64_t)space << 48 | (uint64_t)(pc & 0xffffff) << 24 | (uint64_t)((off + 1) & 0xffffff);
}
const char* kSpace[] = {"rom", "ram", "sram", "bios", "z80ram", "unk", "open"};

void be32(std::vector<uint8_t>& o, uint32_t v) { for(int i = 3; i >= 0; i--) o.push_back(v >> (i * 8)); }
void chunk(std::vector<uint8_t>& o, const char* t, const std::vector<uint8_t>& d) {
  be32(o, d.size());
  size_t s = o.size();
  for(int i = 0; i < 4; i++) o.push_back(t[i]);
  o.insert(o.end(), d.begin(), d.end());
  be32(o, crc32(0, o.data() + s, o.size() - s));
}
bool writePng(const std::string& path, const uint32_t* px, uint32_t w, uint32_t h, size_t pitch) {
  std::vector<uint8_t> raw;
  raw.reserve((size_t)(w * 3 + 1) * h);
  for(uint32_t y = 0; y < h; y++) {
    raw.push_back(0);
    for(uint32_t x = 0; x < w; x++) {
      uint32_t p = px[y * (pitch / 4) + x];
      raw.push_back(p >> 16); raw.push_back(p >> 8); raw.push_back(p);
    }
  }
  uLongf cl = compressBound(raw.size());
  std::vector<uint8_t> comp(cl);
  compress2(comp.data(), &cl, raw.data(), raw.size(), 6);
  comp.resize(cl);
  std::vector<uint8_t> o = {0x89, 'P', 'N', 'G', 13, 10, 26, 10}, ihdr(13, 0);
  for(int i = 0; i < 4; i++) { ihdr[i] = w >> (24 - 8 * i); ihdr[4 + i] = h >> (24 - 8 * i); }
  ihdr[8] = 8; ihdr[9] = 2;
  chunk(o, "IHDR", ihdr); chunk(o, "IDAT", comp); chunk(o, "IEND", {});
  FILE* f = fopen(path.c_str(), "wb");
  if(!f) return false;
  fwrite(o.data(), 1, o.size(), f);
  fclose(f);
  return true;
}

void loadInput(const std::string& path) {
  FILE* f = fopen(path.c_str(), "r");
  if(!f) { fprintf(stderr, "cannot read input script %s\n", path.c_str()); exit(2); }
  char line[256];
  while(fgets(line, sizeof line, f)) {
    if(line[0] == '#' || line[0] == '\n') continue;
    unsigned long long fr = 0, hold = 4;
    char nm[64] = {};
    if(sscanf(line, "%llu %63s %llu", &fr, nm, &hold) >= 2) g_ev.push_back({fr, hold, lower(nm)});
  }
  fclose(f);
}

std::string sha256File(const std::string& path) {
  std::string c = "sha256sum '" + path + "'";
  FILE* p = popen(c.c_str(), "r");
  char b[128] = {};
  if(p) { if(fgets(b, sizeof b, p)) {} pclose(p); }
  return std::string(b).substr(0, 64);
}

uint32_t g_vcrc = 0, g_vw = 0, g_vh = 0, g_vcalls = 0;
void writeFrameRow() {
  fprintf(f_frames, "%llu\t%08x\t%u\t%u\t%u\n", (unsigned long long)g_frame, g_vcrc, g_vw, g_vh, g_vcalls);
  g_vcalls = 0;
}

}  // namespace

bool buttonHeld(const std::string& name0) {
  std::string name = lower(name0);
  for(auto& e : g_ev) if(e.name == name && g_frame >= e.frame && g_frame < e.frame + e.hold) return true;
  if(g_frame >= opt.randFrom && !g_randNames.empty()) {
    uint64_t blk = (g_frame - opt.randFrom) / opt.randBlock;
    for(size_t i = 0; i < g_randNames.size(); i++) if(g_randNames[i] == name) {
      uint64_t r = splitmix((uint64_t)opt.seed * 0x10001ull + blk * 64 + i);
      uint32_t odds = (name == "start" || name == "pause") ? 24 : 3;
      return (r % odds) == 0;
    }
  }
  return false;
}
void setRandomButtons(std::vector<std::string> names) { for(auto& n : names) g_randNames.push_back(lower(n)); }

void recExec(uint32_t pc, int space, int64_t off, const uint8_t b[4]) {
  if(space == RAM || space == SRAM || space == OPEN) {
    uint32_t w = (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
    auto k = std::make_tuple(space, pc, off, w);
    if(!g_ramForms.count(k)) g_ramForms[k] = RamForm{space, pc, off, (uint32_t)g_frame};
  }
  auto& r = g_exec[key(space, pc, off)];
  if(r.count++ == 0) {
    r.first = (uint32_t)g_frame; r.space = space; r.pc = pc; r.off = off; memcpy(r.bytes, b, 4);
    if(space == RAM && (uint64_t)off < g_wr.size()) {
      auto& w = g_wr[off];
      r.written = w.set; r.wpc = w.pc; r.src = w.src;
    }
  } else if((space == RAM || space == SRAM) && memcmp(r.bytes, b, 4)) {
    r.changes++;
    memcpy(r.bytes, b, 4);
  }
}
void recStore(uint32_t off, uint32_t pc, int64_t romSrc) {
  if(g_wr.empty()) g_wr.resize(0x10000);
  if(off >= g_wr.size()) return;
  g_wr[off] = Wr{pc, true, romSrc};
}
void recMapper(uint32_t pc, uint32_t reg, uint32_t val, const std::string& desc) {
  if(!f_map) return;
  fprintf(f_map, "%llu\t%04x\t%04x\t%02x\t%s\n", (unsigned long long)g_frame, pc, reg, val, desc.c_str());
  n_mapper++;
}
void recIrq(const char* kind, uint32_t line, uint32_t vec, uint32_t pc) {
  auto& a = g_irqAgg[std::string("z80\t") + kind];
  if(a.count++ == 0) a.first = g_frame;
  a.last = g_frame;
  if(n_irqRows < 200000) {
    fprintf(f_irq, "%llu\t%u\tz80\t%s\t%06x\t%06x\n", (unsigned long long)g_frame, line, kind, vec, pc);
    n_irqRows++;
  }
}

std::string traceDir() { return g_trace; }
void recRawFrame(const uint16_t* px, uint32_t w, uint32_t h) {
  uint32_t crc = crc32(0, (const Bytef*)px, w * h * 2);
  fprintf(f_raw, "%llu\t%08x\t%u\t%u\n", (unsigned long long)g_frame, crc, w, h);
  if(g_frame >= opt.rawFrom && g_frame <= opt.rawTo) {
    char name[64];
    snprintf(name, sizeof name, "raw/f%06llu.bin", (unsigned long long)g_frame);
    FILE* f = fopen((g_trace + "/" + name).c_str(), "wb");
    if(f) { fwrite(px, 2, (size_t)w * h, f); fclose(f); }
  }
}
void recInput(uint32_t mask) { fprintf(f_input, "%llu\t%02x\n", (unsigned long long)g_frame, mask); }
void recState(const std::string& row) { fprintf(f_state, "%llu\t%s\n", (unsigned long long)g_frame, row.c_str()); }
void onVideo(const uint32_t* data, uint32_t pitch, uint32_t w, uint32_t h) {
  uint32_t crc = 0;
  for(uint32_t y = 0; y < h; y++) crc = crc32(crc, (const Bytef*)((const uint8_t*)data + (size_t)y * pitch), w * 4);
  g_vcrc = crc; g_vw = w; g_vh = h; g_vcalls++;
  if(opt.shot && (g_frame % opt.shot == 0 || g_frame + 1 == opt.frames)) {
    char name[64];
    snprintf(name, sizeof name, "shots/f%06llu.png", (unsigned long long)g_frame);
    writePng(g_trace + "/" + name, data, w, h, pitch);
  }
}

}  // namespace orc

using namespace orc;

static void usage() {
  fprintf(stderr,
          "usage: sega-oracle --rom FILE --id NAME --out DIR [--system auto|ms|gg|sg]\n"
          "       [--mapper auto|sega|codemasters|korea] [--bios] [--no-cart-ram] [--frames N]\n"
          "       [--input SCRIPT] [--rand-from FRAME] [--seed N] [--rand-block N] [--shot N]\n"
          "       [--raw-dump FROM:TO] [--quiet]\n"
          "input script lines: FRAME BUTTON [HOLD_FRAMES] (default hold 4); buttons Up Down Left Right 1 2 A B Start Pause\n"
          "ORACLE_ILOG=N writes the first N instructions to insn.tsv\n");
  exit(1);
}

int main(int argc, char** argv) {
  for(int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> const char* { if(i + 1 >= argc) usage(); return argv[++i]; };
    if(a == "--rom") opt.rom = next();
    else if(a == "--id") opt.id = next();
    else if(a == "--out") opt.outDir = next();
    else if(a == "--system") opt.system = next();
    else if(a == "--mapper") opt.mapper = next();
    else if(a == "--frames") opt.frames = strtoull(next(), nullptr, 0);
    else if(a == "--input") opt.input = next();
    else if(a == "--rand-from") opt.randFrom = strtoull(next(), nullptr, 0);
    else if(a == "--seed") opt.seed = strtoul(next(), nullptr, 0);
    else if(a == "--rand-block") opt.randBlock = strtoul(next(), nullptr, 0);
    else if(a == "--shot") opt.shot = strtoull(next(), nullptr, 0);
    else if(a == "--quiet") opt.quiet = true;
    else if(a == "--raw-dump") {
      unsigned long long x = 0, y = 0;
      if(sscanf(next(), "%llu:%llu", &x, &y) != 2) usage();
      opt.rawFrom = x; opt.rawTo = y;
    }
    else if(a == "--no-cart-ram") opt.cartRam = false;
    else if(a == "--bios") opt.bios = true;
    else usage();
  }
  if(opt.rom.empty() || opt.id.empty() || opt.outDir.empty()) usage();
  if(opt.randBlock == 0) opt.randBlock = 1;
  {
    FILE* f = fopen(opt.rom.c_str(), "rb");
    if(!f) { perror(opt.rom.c_str()); return 2; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_rom.assign(n + 16, 0);
    if(fread(g_rom.data(), 1, n, f) != (size_t)n) return 2;
    fclose(f);
    g_romSize = n;
  }
  std::string sys = opt.system;
  size_t dot = opt.rom.find_last_of('.');
  std::string ext = dot == std::string::npos ? "" : lower(opt.rom.substr(dot));
  if(sys == "auto") sys = ext == ".gg" ? "gg" : (ext == ".sg" || ext == ".sc") ? "sg" : "ms";
  if(sys != "ms" && sys != "gg" && sys != "sg") usage();
  g_trace = opt.outDir + "/" + opt.id;
  mkdirs(g_trace + "/shots");
  mkdirs(g_trace + "/raw");
  if(!opt.input.empty()) loadInput(opt.input);
  f_frames = openOut("frames.tsv");
  f_raw = openOut("frames_raw.tsv");
  f_input = openOut("input.tsv");
  f_state = openOut("state.tsv");
  f_irq = openOut("irq.tsv");
  f_map = openOut("mapper.tsv");
  fputs("frame\tz80_pc\treg\tvalue\tbanks_after\n", f_map);
  fputs("frame\tline\tcpu\tkind\tvector\tpc\n", f_irq);
  fputs("frame\tcrc32\twidth\theight\tvideo_calls\n", f_frames);

  Sys* S = sys == "sg" ? makeSG() : makeMS(sys == "gg");
  if(!S->boot()) { fprintf(stderr, "boot failed\n"); return 3; }
  auto t0 = std::chrono::steady_clock::now();
  for(g_frame = 0; g_frame < opt.frames; g_frame++) {
    S->runFrame();
    writeFrameRow();
    if(!opt.quiet && g_frame % 1000 == 999) {
      double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      fprintf(stderr, "%s: frame %llu  %.1f fps  exec records %zu\n", opt.id.c_str(), (unsigned long long)g_frame + 1, (g_frame + 1) / s, g_exec.size());
    }
  }
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  uint64_t framesRun = g_frame;

  std::vector<const ExecRec*> v;
  v.reserve(g_exec.size());
  for(auto& kv : g_exec) v.push_back(&kv.second);
  std::sort(v.begin(), v.end(), [](const ExecRec* a, const ExecRec* b) {
    if(a->space != b->space) return a->space < b->space;
    if(a->pc != b->pc) return a->pc < b->pc;
    return a->off < b->off;
  });
  FILE* fe = openOut("exec.tsv");
  FILE* fr = openOut("ramcode.tsv");
  fputs("cpu\tpc\tregion\toffset\tfirst_frame\tcount\tbytes\n", fe);
  fputs("cpu\tpc\tregion\toffset\twriter_cpu\twriter_pc\trom_src_offset\tfirst_frame\tcount\tchanges\tbytes\n", fr);
  uint64_t nRom = 0, nRam = 0, nOther = 0;
  for(auto r : v) {
    char hx[16];
    snprintf(hx, sizeof hx, "%02x%02x%02x%02x", r->bytes[0], r->bytes[1], r->bytes[2], r->bytes[3]);
    char off[24];
    if(r->off < 0) strcpy(off, "-"); else snprintf(off, sizeof off, "%llx", (unsigned long long)r->off);
    fprintf(fe, "z80\t%06x\t%s\t%s\t%u\t%u\t%s\n", r->pc, kSpace[r->space], off, r->first, r->count, hx);
    if(r->space == ROM) {
      nRom++;
    } else if(r->space == RAM) {
      nRam++;
      char wp[32], src[24];
      if(!r->written) strcpy(wp, "-\t-"); else snprintf(wp, sizeof wp, "z80\t%06x", r->wpc);
      if(r->src < 0) strcpy(src, "-"); else snprintf(src, sizeof src, "%llx", (unsigned long long)r->src);
      fprintf(fr, "z80\t%06x\t%s\t%s\t%s\t%s\t%u\t%u\t%u\t%s\n", r->pc, kSpace[r->space], off, wp, src, r->first, r->count, r->changes, hx);
    } else {
      nOther++;
    }
  }
  fclose(fr);
  for(auto& kv : g_ramForms) {
    auto& f = kv.second;
    fprintf(fe, "z80\t%06x\t%s\t%llx\t%u\t1\t%08x\n", f.pc, kSpace[f.space], (unsigned long long)f.off, f.first, std::get<3>(kv.first));
  }
  fclose(fe);
  fclose(f_frames);
  fclose(f_raw);
  fclose(f_input);
  fclose(f_state);
  FILE* fs = openOut("irq_summary.tsv");
  fputs("cpu\tkind\tcount\tfirst_frame\tlast_frame\tdetail_rows_truncated\n", fs);
  for(auto& kv : g_irqAgg)
    fprintf(fs, "%s\t%llu\t%llu\t%llu\t%s\n", kv.first.c_str(), (unsigned long long)kv.second.count, (unsigned long long)kv.second.first,
            (unsigned long long)kv.second.last, n_irqRows >= 200000 ? "yes" : "no");
  fclose(fs);
  fclose(f_irq);
  fclose(f_map);

  FILE* fm = openOut("meta.json");
  fprintf(fm, "{\n  \"rom_id\": \"%s\",\n  \"rom_size\": %u,\n  \"rom_sha256\": \"%s\",\n  \"system\": \"%s\",\n  \"ares_commit\": \"0b928930\",\n",
          opt.id.c_str(), g_romSize, sha256File(opt.rom).c_str(), S->name());
  fprintf(fm, "  \"frames\": %llu,\n  \"input_script\": \"%s\",\n  \"random_from_frame\": %lld,\n  \"seed\": %u,\n  \"random_block_frames\": %u,\n",
          (unsigned long long)framesRun, opt.input.empty() ? "" : opt.input.substr(opt.input.find_last_of('/') + 1).c_str(),
          opt.randFrom == UINT64_MAX ? -1LL : (long long)opt.randFrom, opt.seed, opt.randBlock);
  fprintf(fm, "  \"run_seconds\": %.2f,\n  \"exec_records\": %zu,\n  \"exec_rom\": %llu,\n  \"exec_ram\": %llu,\n  \"exec_other\": %llu,\n",
          secs, g_exec.size(), (unsigned long long)nRom, (unsigned long long)nRam, (unsigned long long)nOther);
  fprintf(fm, "  \"mapper_rows\": %llu,\n  \"irq_rows\": %llu,\n  \"system_info\": %s\n}\n", (unsigned long long)n_mapper,
          (unsigned long long)n_irqRows, S->describe().c_str());
  fclose(fm);
  if(!opt.quiet)
    fprintf(stderr, "%s: done, %llu frames in %.1fs, %zu exec records\n", opt.id.c_str(), (unsigned long long)framesRun, secs, g_exec.size());
  return 0;
}
