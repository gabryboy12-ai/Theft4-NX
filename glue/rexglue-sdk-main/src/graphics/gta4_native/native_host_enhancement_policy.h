#pragma once

#include <cstdint>

namespace rex::graphics::gta4_native {

constexpr bool IsNativeHostFogOverride(uint64_t hash) {
  // Exact entries in LibertyRecompLib/shader_overrides/manifest.json.
  return hash == 0xEFAACEED3DBD802DULL || hash == 0x1A6669BBAFDC43E7ULL;
}

constexpr bool AllowNativeHostOverride(uint64_t hash, bool host_fog_enabled) {
  return !IsNativeHostFogOverride(hash) || host_fog_enabled;
}

}  // namespace rex::graphics::gta4_native
