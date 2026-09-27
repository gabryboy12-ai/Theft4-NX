#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

#include "/Users/Ozordi/Downloads/LibertyRecomp/tools/XenosRecomp/thirdparty/smol-v/source/smolv.h"

int main(int argc, char** argv) {
  if (argc < 3 || (argc % 2) == 0) {
    std::fprintf(stderr, "usage: %s input.smolv output.spv [input.smolv output.spv ...]\n", argv[0]);
    return 2;
  }
  for (int argument = 1; argument < argc; argument += 2) {
    std::ifstream input(argv[argument], std::ios::binary);
    std::vector<uint8_t> smolv((std::istreambuf_iterator<char>(input)), {});
    const size_t decoded_size = smolv::GetDecodedBufferSize(smolv.data(), smolv.size());
    if (!decoded_size) {
      std::fprintf(stderr, "invalid SMOL-V input: %s\n", argv[argument]);
      return 3;
    }
    std::vector<uint8_t> spirv(decoded_size);
    if (!smolv::Decode(smolv.data(), smolv.size(), spirv.data(), spirv.size())) {
      std::fprintf(stderr, "SMOL-V decode failed: %s\n", argv[argument]);
      return 4;
    }
    std::ofstream output(argv[argument + 1], std::ios::binary);
    output.write(reinterpret_cast<const char*>(spirv.data()), spirv.size());
    if (!output) {
      return 5;
    }
  }
  return 0;
}
