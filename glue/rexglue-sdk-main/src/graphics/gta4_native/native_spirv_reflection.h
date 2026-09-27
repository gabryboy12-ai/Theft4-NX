/**
 * @file native_spirv_reflection.h
 * @brief Minimal validated SPIR-V interface reflection for the GTA IV native renderer.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "native_fixed_function_policy.h"

namespace rex::graphics::gta4_native {

// SPIR-V core ABI values used by the minimal parser. Keeping this helper free
// of SPIRV-Headers makes it available to the lightweight unit-test target.
constexpr uint32_t kNativeSpirvMagicNumber = 0x07230203u;
constexpr uint32_t kNativeSpirvVersion = 0x00010600u;
constexpr uint32_t kNativeSpirvWordCountShift = 16u;
constexpr uint32_t kNativeSpirvOpcodeMask = 0xFFFFu;
constexpr uint32_t kNativeSpirvExecutionModelFragment = 4u;
constexpr uint32_t kNativeSpirvStorageClassOutput = 3u;
constexpr uint32_t kNativeSpirvDecorationBuiltIn = 11u;
constexpr uint32_t kNativeSpirvDecorationLocation = 30u;
constexpr uint32_t kNativeSpirvOpcodeEntryPoint = 15u;
constexpr uint32_t kNativeSpirvOpcodeVariable = 59u;
constexpr uint32_t kNativeSpirvOpcodeDecorate = 71u;

inline std::optional<uint32_t> ReflectNativeFragmentColorOutputMask(
    std::span<const uint32_t> spirv) {
  constexpr size_t kHeaderWordCount = 5;
  if (spirv.size() < kHeaderWordCount || spirv[0] != kNativeSpirvMagicNumber || !spirv[3]) {
    return std::nullopt;
  }

  const uint32_t id_bound = spirv[3];
  std::vector<uint8_t> fragment_interfaces(id_bound);
  std::vector<uint8_t> output_variables(id_bound);
  std::vector<uint8_t> built_ins(id_bound);
  std::vector<uint32_t> locations(id_bound, UINT32_MAX);
  bool fragment_entry_point_found = false;

  size_t cursor = kHeaderWordCount;
  while (cursor < spirv.size()) {
    const uint32_t instruction = spirv[cursor];
    const uint32_t word_count = instruction >> 16;
    const uint32_t opcode = instruction & kNativeSpirvOpcodeMask;
    if (!word_count || word_count > spirv.size() - cursor) {
      return std::nullopt;
    }
    const uint32_t* words = spirv.data() + cursor;
    switch (opcode) {
      case kNativeSpirvOpcodeEntryPoint:
        if (word_count < 4) {
          return std::nullopt;
        }
        if (words[1] == kNativeSpirvExecutionModelFragment) {
          fragment_entry_point_found = true;
          size_t interface_begin = 0;
          for (size_t name_word = 3; name_word < word_count; ++name_word) {
            const uint32_t packed_name = words[name_word];
            if ((packed_name & 0x000000FFu) == 0 || (packed_name & 0x0000FF00u) == 0 ||
                (packed_name & 0x00FF0000u) == 0 || (packed_name & 0xFF000000u) == 0) {
              interface_begin = name_word + 1;
              break;
            }
          }
          if (!interface_begin) {
            return std::nullopt;
          }
          for (size_t interface_word = interface_begin; interface_word < word_count;
               ++interface_word) {
            if (words[interface_word] >= id_bound) {
              return std::nullopt;
            }
            fragment_interfaces[words[interface_word]] = 1;
          }
        }
        break;
      case kNativeSpirvOpcodeVariable:
        if (word_count >= 4 && words[2] < id_bound &&
            words[3] == kNativeSpirvStorageClassOutput) {
          output_variables[words[2]] = 1;
        }
        break;
      case kNativeSpirvOpcodeDecorate:
        if (word_count >= 3 && words[1] < id_bound) {
          const uint32_t decoration = words[2];
          if (decoration == kNativeSpirvDecorationLocation && word_count >= 4) {
            locations[words[1]] = words[3];
          } else if (decoration == kNativeSpirvDecorationBuiltIn && word_count >= 4) {
            built_ins[words[1]] = 1;
          }
        }
        break;
      default:
        break;
    }
    cursor += word_count;
  }

  if (!fragment_entry_point_found) {
    return std::nullopt;
  }

  uint32_t output_mask = 0;
  for (uint32_t id = 0; id < id_bound; ++id) {
    if (!fragment_interfaces[id] || !output_variables[id] || built_ins[id]) {
      continue;
    }
    if (locations[id] == UINT32_MAX || locations[id] >= kNativeFixedRenderTargetCount) {
      return std::nullopt;
    }
    const uint32_t target_bit = 1u << locations[id];
    if (output_mask & target_bit) {
      return std::nullopt;
    }
    output_mask |= target_bit;
  }
  return output_mask;
}

}  // namespace rex::graphics::gta4_native
