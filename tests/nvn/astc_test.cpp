// SPDX-License-Identifier: MIT
// Host-only helper for tests/nvn/astc_compare.py: ASTC LDR decode (nvn_astc.cpp) and BC7 mode-6 encode (nvn_bc7.cpp) of
// raw data given on the command line. Usage: astc_test <blocks.bin> W H bw bh srgb <out_rgba.bin> <out_bc7.bin>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "nvn_tex.h"
int main(int argc, char** argv) {
  if (argc < 9) return 2;
  FILE* f = fopen(argv[1], "rb"); if (!f) return 2;
  std::vector<unsigned char> in; unsigned char buf[4096]; size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) in.insert(in.end(), buf, buf + n);
  fclose(f);
  const unsigned W = atoi(argv[2]), H = atoi(argv[3]), bw = atoi(argv[4]), bh = atoi(argv[5]); const bool srgb = atoi(argv[6]) != 0;
  std::vector<unsigned char> rgba((size_t)W * H * 4), bc7(((W + 3) / 4) * ((H + 3) / 4) * 16);
  nvn::AstcDecodeLdr(in.data(), W, H, bw, bh, srgb, rgba.data());
  nvn::EncodeBc7(rgba.data(), W, H, bc7.data());
  f = fopen(argv[7], "wb"); fwrite(rgba.data(), 1, rgba.size(), f); fclose(f);
  f = fopen(argv[8], "wb"); fwrite(bc7.data(), 1, bc7.size(), f); fclose(f);
  return 0;
}
