#pragma once

// ReXGlue runtime. Released under the BSD license; see LICENSE.
// Match the tile-preserving copy planner while keeping depth words integer.
#include <cstdint>
#include <vector>
#include <rex/graphics/pipeline/shader/spirv_builder.h>

namespace rex::graphics::vulkan::raw_depth {

// Both source and destination have identical guest format and sample count.
// A maintenance8 copy extracts source depth into R32_UINT and stencil into
// R8_UINT. Nothing in this shader interprets a depth word as a float.
// Push constants: signed base difference in its uint32 representation, full
// destination origin x/y, and full source mirror origin x/y. Fragment position
// is relative to the cropped destination scratch; source fetches are relative
// to the cropped source mirror.
inline std::vector<unsigned int> GenerateFragment(uint32_t destination_pitch,
    uint32_t source_pitch, uint32_t tile_width, uint32_t tile_height,
    uint32_t physical_samples, bool sample_rate_shading = true,
    uint32_t fixed_sample = 0) {
  SpirvBuilder b(spv::Spv_1_0, (26u << 16) | 1, nullptr);
  b.addCapability(spv::CapabilityShader);
  b.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
  const auto u = b.makeUintType(32), i = b.makeIntType(32), f = b.makeFloatType(32);
  const auto u4 = b.makeVectorType(u, 4), i2 = b.makeVectorType(i, 2);
  const auto f4 = b.makeVectorType(f, 4);
  std::vector<spv::Id> interfaces;
  const auto position = b.createVariable(spv::NoPrecision, spv::StorageClassInput,
      f4, "gl_FragCoord");
  b.addDecoration(position, spv::DecorationBuiltIn, spv::BuiltInFragCoord);
  interfaces.push_back(position);
  spv::Id sample = spv::NoResult;
  if (physical_samples > 1 && sample_rate_shading) {
    b.addCapability(spv::CapabilitySampleRateShading);
    sample = b.createVariable(spv::NoPrecision, spv::StorageClassInput, i, "gl_SampleID");
    b.addDecoration(sample, spv::DecorationBuiltIn, spv::BuiltInSampleId);
    b.addDecoration(sample, spv::DecorationFlat);
    interfaces.push_back(sample);
  }
  spv::Id outputs[2], textures[2];
  for (uint32_t binding = 0; binding < 2; ++binding) {
    outputs[binding] = b.createVariable(spv::NoPrecision, spv::StorageClassOutput,
        u, binding ? "stencil_byte" : "depth_word");
    b.addDecoration(outputs[binding], spv::DecorationLocation, binding);
    interfaces.push_back(outputs[binding]);
    textures[binding] = b.createVariable(spv::NoPrecision,
        spv::StorageClassUniformConstant,
        b.makeImageType(u, spv::Dim2D, false, false, physical_samples > 1,
                        1, spv::ImageFormatUnknown),
        binding ? "source_stencil" : "source_depth_bits");
    b.addDecoration(textures[binding], spv::DecorationDescriptorSet, 0);
    b.addDecoration(textures[binding], spv::DecorationBinding, binding);
  }
  const auto push_type = b.makeStructType({u, u, u, u, u}, "RawDepthAddress");
  b.addDecoration(push_type, spv::DecorationBlock);
  for (uint32_t member = 0; member < 5; ++member)
    b.addMemberDecoration(push_type, member, spv::DecorationOffset, member * 4);
  const auto push = b.createVariable(spv::NoPrecision, spv::StorageClassPushConstant,
      push_type, "raw_depth_address");
  auto* main = b.makeEntryPoint("main");
  auto* entry = b.addEntryPoint(spv::ExecutionModelFragment, main, "main");
  b.addExecutionMode(main, spv::ExecutionModeOriginUpperLeft);
  for (auto interface : interfaces) entry->addIdOperand(interface);
  auto load_push = [&](uint32_t member) {
    return b.createLoad(b.createAccessChain(spv::StorageClassPushConstant,
        push, {b.makeIntConstant(member)}), spv::NoPrecision);
  };
  const auto xy = b.createLoad(position, spv::NoPrecision);
  const auto x = b.createBinOp(spv::OpIAdd, u, load_push(1),
      b.createUnaryOp(spv::OpConvertFToU, u, b.createCompositeExtract(xy, f, 0)));
  const auto y = b.createBinOp(spv::OpIAdd, u, load_push(2),
      b.createUnaryOp(spv::OpConvertFToU, u, b.createCompositeExtract(xy, f, 1)));
  const auto width = b.makeUintConstant(tile_width), height = b.makeUintConstant(tile_height);
  const auto tile_x = b.createBinOp(spv::OpUDiv, u, x, width);
  const auto tile_y = b.createBinOp(spv::OpUDiv, u, y, height);
  const auto delta = load_push(0);
  // The delta may be negative across the EDRAM boundary. Wrapping after the
  // addition is required independently of either source or destination pitch.
  const auto tile = b.createBinOp(spv::OpBitwiseAnd, u,
      b.createBinOp(spv::OpIAdd, u, delta,
          b.createBinOp(spv::OpIAdd, u, tile_x,
              b.createBinOp(spv::OpIMul, u, tile_y,
                  b.makeUintConstant(destination_pitch)))), b.makeUintConstant(2047));
  const auto source_x = b.createBinOp(spv::OpIAdd, u,
      b.createBinOp(spv::OpIMul, u,
          b.createBinOp(spv::OpUMod, u, tile, b.makeUintConstant(source_pitch)), width),
      b.createBinOp(spv::OpUMod, u, x, width));
  const auto source_y = b.createBinOp(spv::OpIAdd, u,
      b.createBinOp(spv::OpIMul, u,
          b.createBinOp(spv::OpUDiv, u, tile, b.makeUintConstant(source_pitch)), height),
      b.createBinOp(spv::OpUMod, u, y, height));
  const auto coordinates = b.createCompositeConstruct(i2, {
      b.createUnaryOp(spv::OpBitcast, i,
          b.createBinOp(spv::OpISub, u, source_x, load_push(3))),
      b.createUnaryOp(spv::OpBitcast, i,
          b.createBinOp(spv::OpISub, u, source_y, load_push(4)))});
  for (uint32_t binding = 0; binding < 2; ++binding) {
    spv::Builder::TextureParameters parameters{};
    parameters.sampler = b.createLoad(textures[binding], spv::NoPrecision);
    parameters.coords = coordinates;
    if (physical_samples > 1)
      parameters.sample = sample_rate_shading ? b.createLoad(sample, spv::NoPrecision)
                                            : b.makeIntConstant(fixed_sample);
    else parameters.lod = b.makeIntConstant(0);
    auto value = b.createCompositeExtract(b.createTextureCall(spv::NoPrecision,
        u4, false, true, false, false, false, parameters, spv::ImageOperandsMaskNone), u, 0);
    if (binding) value = b.createBinOp(spv::OpBitwiseAnd, u, value, b.makeUintConstant(255));
    b.createStore(value, outputs[binding]);
  }
  b.leaveFunction();
  std::vector<unsigned int> result;
  b.dump(result);
  return result;
}

}  // namespace rex::graphics::vulkan::raw_depth
