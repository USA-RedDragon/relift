// Runs known-answer programs on the CUDA device.
//
//   runner <skeleton.cubin> <jobs.bin> <results.bin>
//
// jobs.bin: u32 job count, then per job: u32 word count, the 16-byte
// instruction words, u32 row count, u32 input words per row, u32 output
// words per row, then the inputs. Each job's words replace the code of
// kernel `sk` in the skeleton (the rest of its text is filled with the
// program's last word, a branch to itself), and one thread runs per row.
// results.bin: per job, the output words of every row.
#include <cuda.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int cur_job = -1;

#define CK(x)                                                         \
  do {                                                                \
    CUresult r_ = (x);                                                \
    if (r_) {                                                         \
      const char* m;                                                  \
      cuGetErrorString(r_, &m);                                       \
      fprintf(stderr, "job %d: %s: %s\n", cur_job, #x, m);            \
      exit(1);                                                        \
    }                                                                 \
  } while (0)

static std::vector<uint8_t> slurp(const char* p) {
  FILE* f = fopen(p, "rb");
  if (!f) {
    perror(p);
    exit(1);
  }
  std::vector<uint8_t> b;
  uint8_t buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + n);
  fclose(f);
  return b;
}

template <class T>
static T rd(const std::vector<uint8_t>& b, size_t off) {
  T v;
  memcpy(&v, b.data() + off, sizeof v);
  return v;
}

int main(int argc, char** argv) {
  if (argc != 4) {
    fprintf(stderr, "usage: runner skeleton.cubin jobs.bin results.bin\n");
    return 2;
  }
  std::vector<uint8_t> cub = slurp(argv[1]);
  std::vector<uint8_t> jobs = slurp(argv[2]);
  // ELF64 section headers: find .text.sk.
  uint64_t shoff = rd<uint64_t>(cub, 0x28);
  uint16_t shentsize = rd<uint16_t>(cub, 0x3a), shnum = rd<uint16_t>(cub, 0x3c), shstrndx = rd<uint16_t>(cub, 0x3e);
  uint64_t stroff = rd<uint64_t>(cub, shoff + shstrndx * shentsize + 0x18);
  uint64_t toff = 0, tsize = 0;
  uint32_t regs = 0;
  for (int i = 0; i < shnum; i++) {
    uint64_t h = shoff + i * shentsize;
    const char* name = (const char*)cub.data() + stroff + rd<uint32_t>(cub, h);
    if (!strcmp(name, ".text.sk")) {
      toff = rd<uint64_t>(cub, h + 0x18);
      tsize = rd<uint64_t>(cub, h + 0x20);
      regs = rd<uint32_t>(cub, h + 0x2c) >> 24;
    }
  }
  if (!tsize || regs < 32) {
    fprintf(stderr, "skeleton: no .text.sk or too few registers (%u)\n", regs);
    return 1;
  }
  CK(cuInit(0));
  CUdevice dev;
  CK(cuDeviceGet(&dev, 0));
  CUcontext cx;
  CK(cuCtxCreate(&cx, NULL, 0, dev));
  FILE* out = fopen(argv[3], "wb");
  if (!out) {
    perror(argv[3]);
    return 1;
  }
  size_t at = 0;
  uint32_t njobs = rd<uint32_t>(jobs, at);
  at += 4;
  for (uint32_t j = 0; j < njobs; j++) {
    cur_job = (int)j;
    uint32_t nw = rd<uint32_t>(jobs, at);
    at += 4;
    if (16ull * nw > tsize) {
      fprintf(stderr, "job %u: %u words do not fit in %llu bytes\n", j, nw, (unsigned long long)tsize);
      return 1;
    }
    std::vector<uint8_t> img = cub;
    for (uint64_t o = 0; o < tsize; o += 16) {
      uint64_t k = o / 16 < nw ? o / 16 : nw - 1;
      memcpy(img.data() + toff + o, jobs.data() + at + 16 * k, 16);
    }
    at += 16ull * nw;
    uint32_t rows = rd<uint32_t>(jobs, at);
    uint32_t nin = rd<uint32_t>(jobs, at + 4), nout = rd<uint32_t>(jobs, at + 8);
    at += 12;
    uint32_t padded = (rows + 127) / 128 * 128;
    std::vector<uint32_t> in((uint64_t)(nin ? nin : 1) * padded, 0), res((uint64_t)nout * padded, 0);
    memcpy(in.data(), jobs.data() + at, 4ull * nin * rows);
    at += 4ull * nin * rows;
    CUmodule m;
    CK(cuModuleLoadData(&m, img.data()));
    CUfunction f;
    CK(cuModuleGetFunction(&f, m, "sk"));
    CUdeviceptr din, dout;
    CK(cuMemAlloc(&din, 4ull * in.size()));
    CK(cuMemAlloc(&dout, 4ull * res.size()));
    CK(cuMemcpyHtoD(din, in.data(), 4ull * in.size()));
    CK(cuMemsetD8(dout, 0, 4ull * res.size()));
    void* args[] = {&din, &dout};
    CK(cuLaunchKernel(f, padded / 128, 1, 1, 128, 1, 1, 0, 0, args, 0));
    CK(cuCtxSynchronize());
    CK(cuMemcpyDtoH(res.data(), dout, 4ull * res.size()));
    CK(cuMemFree(din));
    CK(cuMemFree(dout));
    CK(cuModuleUnload(m));
    fwrite(res.data(), 4, (uint64_t)nout * rows, out);
  }
  fclose(out);
  return 0;
}
