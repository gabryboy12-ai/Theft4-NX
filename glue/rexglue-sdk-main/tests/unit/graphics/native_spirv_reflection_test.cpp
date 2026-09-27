/**
 * @file native_spirv_reflection_test.cpp
 * @brief GTA IV native-renderer SPIR-V output-interface reflection tests.
 */

#include <cstdint>
#include <initializer_list>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "graphics/gta4_native/native_spirv_reflection.h"

namespace rex::graphics::gta4_native {
namespace {

void AppendInstruction(std::vector<uint32_t>& module, uint32_t opcode,
                       std::initializer_list<uint32_t> operands) {
  const uint32_t word_count = uint32_t(operands.size()) + 1u;
  module.push_back((word_count << kNativeSpirvWordCountShift) |
                   (opcode & kNativeSpirvOpcodeMask));
  module.insert(module.end(), operands.begin(), operands.end());
}

std::vector<uint32_t> MakeFragmentInterfaceModule(uint32_t second_location) {
  constexpr uint32_t kIdBound = 8;
  constexpr uint32_t kEntryPoint = 1;
  constexpr uint32_t kColor0 = 2;
  constexpr uint32_t kColor1 = 3;
  constexpr uint32_t kBuiltInOutput = 4;
  constexpr uint32_t kNonInterfaceOutput = 5;
  constexpr uint32_t kOutputPointerType = 6;

  std::vector<uint32_t> module = {
      kNativeSpirvMagicNumber, kNativeSpirvVersion, 0, kIdBound, 0};
  AppendInstruction(module, kNativeSpirvOpcodeEntryPoint,
                    {kNativeSpirvExecutionModelFragment, kEntryPoint, 0, kColor0, kColor1,
                     kBuiltInOutput});
  AppendInstruction(module, kNativeSpirvOpcodeDecorate,
                    {kColor0, kNativeSpirvDecorationLocation, 0});
  AppendInstruction(module, kNativeSpirvOpcodeDecorate,
                    {kColor1, kNativeSpirvDecorationLocation, second_location});
  AppendInstruction(module, kNativeSpirvOpcodeDecorate,
                    {kBuiltInOutput, kNativeSpirvDecorationBuiltIn, 0});
  AppendInstruction(module, kNativeSpirvOpcodeDecorate,
                    {kNonInterfaceOutput, kNativeSpirvDecorationLocation, 1});
  AppendInstruction(module, kNativeSpirvOpcodeVariable,
                    {kOutputPointerType, kColor0, kNativeSpirvStorageClassOutput});
  AppendInstruction(module, kNativeSpirvOpcodeVariable,
                    {kOutputPointerType, kColor1, kNativeSpirvStorageClassOutput});
  AppendInstruction(module, kNativeSpirvOpcodeVariable,
                    {kOutputPointerType, kBuiltInOutput, kNativeSpirvStorageClassOutput});
  AppendInstruction(module, kNativeSpirvOpcodeVariable,
                    {kOutputPointerType, kNonInterfaceOutput,
                     kNativeSpirvStorageClassOutput});
  return module;
}

TEST_CASE("GTA IV native SPIR-V reflection uses fragment entry-point outputs",
          "[gta4-native][graphics][spirv]") {
  const std::vector<uint32_t> module = MakeFragmentInterfaceModule(2);
  CHECK(ReflectNativeFragmentColorOutputMask(module) == 0b0101u);
}

TEST_CASE("GTA IV native SPIR-V reflection rejects unrepresentable fragment outputs",
          "[gta4-native][graphics][spirv]") {
  const std::vector<uint32_t> out_of_range =
      MakeFragmentInterfaceModule(kNativeFixedRenderTargetCount);
  CHECK_FALSE(ReflectNativeFragmentColorOutputMask(out_of_range).has_value());

  const std::vector<uint32_t> duplicate = MakeFragmentInterfaceModule(0);
  CHECK_FALSE(ReflectNativeFragmentColorOutputMask(duplicate).has_value());
}

TEST_CASE("GTA IV native SPIR-V reflection rejects malformed modules",
          "[gta4-native][graphics][spirv]") {
  std::vector<uint32_t> malformed = {
      kNativeSpirvMagicNumber, kNativeSpirvVersion, 0, 2, 0, 0};
  CHECK_FALSE(ReflectNativeFragmentColorOutputMask(malformed).has_value());

  std::vector<uint32_t> vertex_only = {
      kNativeSpirvMagicNumber, kNativeSpirvVersion, 0, 2, 0};
  AppendInstruction(vertex_only, kNativeSpirvOpcodeEntryPoint, {0, 1, 0});
  CHECK_FALSE(ReflectNativeFragmentColorOutputMask(vertex_only).has_value());
}

}  // namespace
}  // namespace rex::graphics::gta4_native
