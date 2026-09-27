#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/gta4_native/native_black_anomaly.h>

namespace rex::graphics::gta4_native {

TEST_CASE("Unmonitored GPU content never raises a black anomaly") {
  NativeBlackProbeSummary summary{};
  summary.sample_count = 256;
  summary.exact_black_samples = 256;

  CHECK(ClassifyNativeBlackAnomaly(NativeBlackAnomalyPolicy::kIgnore, summary) ==
        NativeBlackAnomalyVerdict::kNotMonitored);
}

TEST_CASE("Monitored GPU content requires valid finite samples") {
  NativeBlackProbeSummary summary{};
  CHECK(ClassifyNativeBlackAnomaly(NativeBlackAnomalyPolicy::kReflectionProduced, summary) ==
        NativeBlackAnomalyVerdict::kNoSamples);

  summary.sample_count = 256;
  summary.exact_black_samples = 256;
  summary.nonfinite_samples = 1;
  CHECK(ClassifyNativeBlackAnomaly(NativeBlackAnomalyPolicy::kReflectionProduced, summary) ==
        NativeBlackAnomalyVerdict::kNonfinite);
}

TEST_CASE("Only an entirely exact-black monitored probe raises the anomaly") {
  NativeBlackProbeSummary summary{};
  summary.sample_count = 256;
  summary.exact_black_samples = 255;
  CHECK(ClassifyNativeBlackAnomaly(NativeBlackAnomalyPolicy::kReflectionSampled, summary) ==
        NativeBlackAnomalyVerdict::kNotUniformBlack);

  summary.exact_black_samples = 256;
  CHECK(ClassifyNativeBlackAnomaly(NativeBlackAnomalyPolicy::kReflectionSampled, summary) ==
        NativeBlackAnomalyVerdict::kUniformBlack);
  CHECK(ClassifyNativeBlackAnomaly(NativeBlackAnomalyPolicy::kShadowFilterProduced, summary) ==
        NativeBlackAnomalyVerdict::kUniformBlack);
}

}  // namespace rex::graphics::gta4_native
