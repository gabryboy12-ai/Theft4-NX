#include <fstream>
#include <iostream>
#include <vector>

#include "smolv.h"

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: decode_smolv INPUT OUTPUT\n";
    return 2;
  }
  std::ifstream input(argv[1], std::ios::binary);
  std::vector<char> encoded((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
  const size_t decoded_size =
      smolv::GetDecodedBufferSize(encoded.data(), encoded.size());
  if (!decoded_size) {
    std::cerr << "invalid SMOL-V input\n";
    return 1;
  }
  std::vector<char> decoded(decoded_size);
  if (!smolv::Decode(encoded.data(), encoded.size(), decoded.data(), decoded.size())) {
    std::cerr << "SMOL-V decode failed\n";
    return 1;
  }
  std::ofstream output(argv[2], std::ios::binary);
  output.write(decoded.data(), decoded.size());
  return output ? 0 : 1;
}
