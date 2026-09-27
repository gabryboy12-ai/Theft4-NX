#ifndef REX_GRAPHICS_GTA4_NATIVE_NATIVE_BLACK_ANOMALY_H_
#define REX_GRAPHICS_GTA4_NATIVE_NATIVE_BLACK_ANOMALY_H_

#include <cstdint>

namespace rex::graphics::gta4_native {

// Content probes are asynchronous diagnostic readbacks. A policy is attached
// only to color resources whose contract requires produced, sampleable image
// content. Raw depth/stencil maps and intentional initialization/title clears
// must remain unmonitored because a uniform endpoint value is valid there.
enum class NativeBlackAnomalyPolicy : uint8_t {
  kIgnore,
  kReflectionProduced,
  kReflectionSampled,
  kShadowFilterProduced,
};

enum class NativeBlackAnomalyVerdict : uint8_t {
  kNotMonitored,
  kNoSamples,
  kNonfinite,
  kNotUniformBlack,
  kUniformBlack,
};

struct NativeBlackProbeSummary {
  uint32_t sample_count = 0;
  uint32_t exact_black_samples = 0;
  uint32_t nonfinite_samples = 0;
};

constexpr NativeBlackAnomalyVerdict ClassifyNativeBlackAnomaly(
    NativeBlackAnomalyPolicy policy, const NativeBlackProbeSummary& summary) {
  if (policy == NativeBlackAnomalyPolicy::kIgnore) {
    return NativeBlackAnomalyVerdict::kNotMonitored;
  }
  if (!summary.sample_count) {
    return NativeBlackAnomalyVerdict::kNoSamples;
  }
  if (summary.nonfinite_samples) {
    return NativeBlackAnomalyVerdict::kNonfinite;
  }
  return summary.exact_black_samples == summary.sample_count
             ? NativeBlackAnomalyVerdict::kUniformBlack
             : NativeBlackAnomalyVerdict::kNotUniformBlack;
}

constexpr const char* NativeBlackAnomalyPolicyName(NativeBlackAnomalyPolicy policy) {
  switch (policy) {
    case NativeBlackAnomalyPolicy::kIgnore:
      return "ignore";
    case NativeBlackAnomalyPolicy::kReflectionProduced:
      return "reflection-produced";
    case NativeBlackAnomalyPolicy::kReflectionSampled:
      return "reflection-sampled";
    case NativeBlackAnomalyPolicy::kShadowFilterProduced:
      return "shadow-filter-produced";
  }
  return "invalid";
}

}  // namespace rex::graphics::gta4_native

#endif  // REX_GRAPHICS_GTA4_NATIVE_NATIVE_BLACK_ANOMALY_H_
