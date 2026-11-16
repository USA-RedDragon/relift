// A container for the known-answer programs: runner.cpp overwrites this
// kernel's code with a program from gen.py. It only has to be long enough
// and to allocate enough registers (the stores keep many values live), and
// to take the same parameters: in at c[0][0x160], out at c[0][0x168].
typedef unsigned int u32;
extern "C" __global__ void sk(const u32* in, u32* out) {
  int i = blockIdx.x * blockDim.x + threadIdx.x;
  u32 x = in[4 * i], y = in[4 * i + 1], z = in[4 * i + 2], w = in[4 * i + 3];
  u32* r = out + 64 * i;
#pragma unroll
  for (int k = 0; k < 64; k++)
    r[k] = (x * (k + 3)) ^ (y + k * 7) ^ (z >> (k & 15)) ^ (w << (k & 7)) ^ (u32)(k * 2654435761u);
}
