#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>

#include "graphics/gta4_native/native_pipeline_lookup_memo.h"

namespace {
namespace gta4 = rex::graphics::gta4_native;
struct FixedState {
  uint32_t depth_function = 3;
  std::array<uint32_t, 4> blend_controls{};
  float alpha_reference = 0.0f;
  bool depth_clamp = false;
  bool operator==(const FixedState&) const = default;
};
using Memo = gta4::NativePipelineLookupMemo<FixedState, 4, uint64_t>;
using Context = Memo::Context;
}  // namespace

TEST_CASE("pipeline memo reuses only a completed request on the original snapshot",
          "[gta4-native][pipeline-memo]") {
  Memo memo;
  FixedState fixed;
  Context context;
  context.lifetime = 1;
  context.pipeline_layout = 100;
  const int snapshot = 1;
  const int copied_snapshot = 2;
  REQUIRE(memo.Find(&snapshot, fixed, context) == 0);
  memo.Store(&snapshot, fixed, context, 500);
  REQUIRE(memo.Find(&snapshot, fixed, context) == 500);

  Memo copied_memo = memo;
  REQUIRE(copied_memo.Find(&copied_snapshot, fixed, context) == 0);
  REQUIRE(memo.Find(nullptr, fixed, context) == 0);

  SECTION("diagnostic fixed state changes must return to full pipeline validation") {
    fixed.depth_function = 7;
    REQUIRE(memo.Find(&snapshot, fixed, context) == 0);
    fixed.depth_function = 3;
    fixed.blend_controls[3] = 1;
    REQUIRE(memo.Find(&snapshot, fixed, context) == 0);
    fixed.blend_controls[3] = 0;
    fixed.depth_clamp = true;
    REQUIRE(memo.Find(&snapshot, fixed, context) == 0);
    fixed.depth_clamp = false;
    fixed.alpha_reference = 1.0f;
    REQUIRE(memo.Find(&snapshot, fixed, context) == 0);
  }
  SECTION("NaNs cannot accidentally compare equal during validation reuse") {
    fixed.alpha_reference = std::numeric_limits<float>::quiet_NaN();
    memo.Store(&snapshot, fixed, context, 500);
    REQUIRE(memo.Find(&snapshot, fixed, context) == 0);
  }
  SECTION("async prewarm misses remain eligible for later completion") {
    Memo pending;
    pending.Store(&snapshot, fixed, context, 0);
    REQUIRE(pending.Find(&snapshot, fixed, context) == 0);
    pending.Store(&snapshot, fixed, context, 501);
    REQUIRE(pending.Find(&snapshot, fixed, context) == 501);
  }
  SECTION("all renderer target and primitive inputs participate") {
    using Mutation = void (*)(Context&);
    const Mutation mutations[] = {
        [](Context& value) { value.lifetime = 2; },
        [](Context& value) { value.pipeline_layout = 200; },
        [](Context& value) { value.color_formats[0] = 1; },
        [](Context& value) { value.color_formats[3] = 1; },
        [](Context& value) { value.depth_format = 1; },
        [](Context& value) { value.color_attachment_mask = 1; },
        [](Context& value) { value.color_write_mask = 1; },
        [](Context& value) { value.samples = 4; },
        [](Context& value) { value.guest_samples = 4; },
        [](Context& value) { value.width = 640; },
        [](Context& value) { value.height = 480; },
        [](Context& value) { value.primitive_type = 1; },
        [](Context& value) { value.user_pointer_stride = 32; },
        [](Context& value) { value.descriptor_backend = 1; },
        [](Context& value) { value.shader_override_mode = 1; },
        [](Context& value) { value.depth_stencil_attachment_active = true; },
        [](Context& value) { value.uses_presenter = true; },
        [](Context& value) { value.primitive_restart_enable = true; },
        [](Context& value) { value.host_fog = true; },
    };
    for (const Mutation mutate : mutations) {
      Context changed = context;
      mutate(changed);
      REQUIRE(memo.Find(&snapshot, fixed, changed) == 0);
    }
  }
}

TEST_CASE("pipeline lifetime invalidation cannot resurrect recycled Vulkan handles",
          "[gta4-native][pipeline-memo]") {
  Memo memo;
  FixedState fixed;
  const int snapshot = 1;
  gta4::NativePipelineLookupLifetime lifetime;
  Context context;
  context.lifetime = lifetime.epoch();
  context.pipeline_layout = 100;
  memo.Store(&snapshot, fixed, context, 500);
  REQUIRE(memo.Find(&snapshot, fixed, context) == 500);
  lifetime.Invalidate();
  context.lifetime = lifetime.epoch();
  REQUIRE(memo.Find(&snapshot, fixed, context) == 0);
  memo.Store(&snapshot, fixed, context, 500);
  REQUIRE(memo.Find(&snapshot, fixed, context) == 500);

  gta4::NativePipelineLookupLifetime exhausted(std::numeric_limits<uint64_t>::max());
  exhausted.Invalidate();
  REQUIRE(exhausted.epoch() == 0);
  exhausted.Invalidate();
  REQUIRE(exhausted.epoch() == 0);
  context.lifetime = exhausted.epoch();
  memo.Store(&snapshot, fixed, context, 500);
  REQUIRE(memo.Find(&snapshot, fixed, context) == 0);
}
