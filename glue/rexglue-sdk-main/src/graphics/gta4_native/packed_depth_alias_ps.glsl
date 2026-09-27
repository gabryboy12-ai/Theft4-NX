#version 450

// sub_828D9768 reinterprets a resolved D24FS8 texture as A8R8G8B8.
// Read the resolved snapshot, never the mutable per-light stencil attachment.
layout(set = 0, binding = 0) uniform sampler2D source_depth;
layout(set = 0, binding = 1) uniform usampler2D source_stencil;
layout(push_constant) uniform AliasConstants {
  ivec2 source_origin;
  ivec2 destination_origin;
  uint source_guest_sample_type;
  uint requested_guest_sample_type;
  uint destination_guest_sample_type;
  uint sample_select;
  uint mode;
  uint physical_source_sample_type;
  uint physical_destination_sample_type;
  uint flags;
  uvec2 source_extent;
  uvec2 destination_extent;
} alias_constants;
layout(location = 0) out vec4 output_color;

// src/graphics/xenos.cpp Float32To20e4, truncating native/Xenos default.
// Native depth is already guest [0,1], not Xenos backend's half-depth encoding.
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
  ivec2 pixel = ivec2(gl_FragCoord.xy);
  float depth = texelFetch(source_depth, pixel, 0).r;
  uint stencil = texelFetch(source_stencil, pixel, 0).r & 255u;
  uint packed_depth = alias_constants.mode != 0u ? depth20e4(depth)
      : uint(roundEven(clamp(depth, 0.0, 1.0) * 16777215.0));
  uint packed = (packed_depth << 8u) | stencil;
  vec4 raw = vec4(uvec4(packed, packed >> 8u, packed >> 16u, packed >> 24u) & 255u) / 255.0;
  // Apply the guest fetch swizzle here on every host; the alias sampled view
  // is identity, including portability devices. 0x60A puts stencil in BLUE.
  for (uint channel = 0u; channel < 4u; ++channel) {
    uint component = (alias_constants.flags >> (3u * channel)) & 7u;
    output_color[channel] = component < 4u ? raw[component] : float(component & 1u);
  }
}
