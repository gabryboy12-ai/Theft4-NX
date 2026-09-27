#version 450

// Native form of the depth/stencil byte contract in PS_depthCopy0. The title
// supplies an already-resolved, untiled depth image; no EDRAM address aliasing.
layout(set = 0, binding = 0) uniform sampler2D source_image;
layout(push_constant) uniform SceneDepthConstants { uint float24; } constants;

// Same guest depth encoding as packed_depth_alias_ps.glsl. Classification is
// based on representable packed depth, not the source material stencil.
uint depth20e4(float depth) {
  if (!(depth > 0.0)) return 0u;
  uint bits = floatBitsToUint(depth);
  if (bits >= 0x3FFFFFF8u) return 0xFFFFFFu;
  if (bits < 0x38800000u) {
    uint shift = min(113u - (bits >> 23u), 24u);
    bits = (0x800000u | (bits & 0x7FFFFFu)) >> shift;
  } else {
    bits += 0xC8000000u;
  }
  return (bits >> 3u) & 0xFFFFFFu;
}

void main() {
  float depth = texelFetch(source_image, ivec2(gl_FragCoord.xy), 0).x;
  uint packed = constants.float24 != 0u ? depth20e4(depth)
      : uint(roundEven(clamp(depth, 0.0, 1.0) * 16777215.0));
  // Load ops supply depth zero and stencil 0x80 for empty scene samples.
  // Late fragment tests replace stencil with 0xFF only for covered samples.
  // Do not request early_fragment_tests: discarded pixels must retain 0x80.
  if (packed == 0u) discard;
  gl_FragDepth = depth;
}
