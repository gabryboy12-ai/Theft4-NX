// C ABI bridge for validate_shader_preservation.py. Uses the same SMOL-V
// decoder as the shader generator; no Vulkan device or shader compiler needed.
#include <cstddef>

#include "smolv.h"

extern "C" size_t liberty_smolv_decoded_size(const void* data, size_t size) {
  return smolv::GetDecodedBufferSize(data, size);
}

extern "C" int liberty_smolv_decode(const void* data, size_t size,
                                    void* output, size_t output_size) {
  return smolv::Decode(data, size, output, output_size) ? 1 : 0;
}
