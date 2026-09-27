#include <array>
#include <cstring>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/gta4_native/light_trace_context.h>
#include <rex/graphics/gta4_native/title_commands.h>

namespace rex::graphics::gta4_native {
namespace {

LightingContext LocalStencil() {
  LightingContext context{};
  context.occurrence_id = 11;
  context.view_id = 7;
  context.stage = RenderExecutionStage::kDeferredLighting;
  context.role = LightPassRole::kLocalStencilSetup;
  context.source = LightSourceKind::kPrimaryRecord;
  context.source_function = 0x822B0F08;
  context.record_address = 0x1000;
  context.view_address = 0x2000;
  context.selector_caller = 0x822B13A4;
  context.requested_selector = 3;
  context.requested_mode = 3;
  context.effective_technique = 0x3000;
  context.effective_mode = 3;
  context.pass = 0x4000;
  context.stencil_setup_expected = 1;
  return context;
}

template <typename Command>
void CheckDrawExecutionContext() {
  Command direct{};
  direct.device = 0x1234;
  ApplyDrawLightingContext(direct, LocalStencil());

  // Simulate a previously captured command executing for a different light.
  // Compare fields, never unspecified C++ padding bytes.
  std::array<unsigned char, sizeof(Command)> captured{};
  std::memcpy(captured.data(), &direct, sizeof(direct));
  Command replay{};
  std::memcpy(&replay, captured.data(), sizeof(replay));
  LightingContext next = LocalStencil();
  next.occurrence_id = 19;
  next.view_id = 17;
  next.record_address = 0x5000;
  next.requested_selector = 6;
  next.requested_mode = 0;
  next.effective_mode = 2;
  next.role = LightPassRole::kLocalContribution;
  ApplyDrawLightingContext(replay, next);
  CHECK(replay.lighting == next);
  CHECK(replay.device == direct.device);
  CHECK(replay.header.size == sizeof(Command));
  CHECK(replay.light_trace_id == next.record_address);
  CHECK(replay.light_trace_technique == 6);
  CHECK(replay.light_trace_mode == 2);
  CHECK(direct.lighting == LocalStencil());
}

TEST_CASE("Lighting snapshots cover every deferred selector family without trace IDs",
          "[gta4-native][graphics][lighting]") {
  CHECK_FALSE(RequiresFullLightingConstants({}));
  for (uint32_t selector = 0; selector <= 24; ++selector) {
    LightingContext context{};
    context.requested_selector = selector;
    context.role = DeferredLightPassRole(selector);
    CHECK(RequiresFullLightingConstants(context));
  }
  CHECK(DeferredLightPassRole(2) == LightPassRole::kUnresolvedDeferredEffect);
  CHECK(DeferredLightPassRole(13) == LightPassRole::kShaft);
  CHECK(DeferredLightPassRole(14) == LightPassRole::kCorona);
  CHECK(DeferredLightPassRole(18) == LightPassRole::kAmbientVolume);
  CHECK(DeferredLightPassRole(24) == LightPassRole::kWaterFx);
}

TEST_CASE("Only proved local stencil provenance qualifies for volume policy",
          "[gta4-native][graphics][lighting]") {
  const LightingContext local = LocalStencil();
  REQUIRE(IsLocalStencilSetup(local));
  for (const auto role : {LightPassRole::kGlobalParameterUpload,
                          LightPassRole::kGlobalContribution, LightPassRole::kGlobalStencil,
                          LightPassRole::kLocalContribution, LightPassRole::kShaft,
                          LightPassRole::kCorona, LightPassRole::kNone}) {
    LightingContext other = local;
    other.role = role;
    CHECK_FALSE(IsLocalStencilSetup(other));
  }
  LightingContext unknown = local;
  unknown.source_function = 0;
  CHECK_FALSE(IsLocalStencilSetup(unknown));
  unknown = local;
  unknown.occurrence_id = 0;
  CHECK_FALSE(IsLocalStencilSetup(unknown));
}

TEST_CASE("Draw metadata preserves selected modes and null-handle fallback",
          "[gta4-native][graphics][lighting]") {
  // The original selector runs unchanged. These are the effective values
  // observed at its sub_828C64C8 boundary, not a second host selector.
  struct Selection {
    uint32_t selector;
    uint32_t requested;
    uint32_t effective;
  };
  for (const auto selected : {Selection{0, 7, 0}, Selection{1, 7, 1}, Selection{4, 3, 3},
                              Selection{5, 0, 1}, Selection{6, 0, 2}, Selection{7, 0, 3},
                              Selection{10, 3, 3}, Selection{11, 0, 1},
                              Selection{12, 0, 2}, Selection{22, 0, 10},
                              Selection{2, 3, 3}}) {
    LightingContext context{};
    context.role = DeferredLightPassRole(selected.selector);
    context.requested_selector = selected.selector;
    context.requested_mode = selected.requested;
    context.effective_mode = selected.effective;
    context.effective_technique = 0x3000;
    context.pass = 0x4000;
    DrawPrimitiveCommand command{};
    ApplyDrawLightingContext(command, context);
    CHECK(command.lighting.requested_mode == selected.requested);
    CHECK(command.lighting.effective_mode == selected.effective);
    CHECK(command.lighting.effective_technique == 0x3000);
    CHECK(command.lighting.pass == 0x4000);
    CHECK(command.light_trace_mode == selected.effective);
    CHECK(RequiresFullLightingConstants(command.lighting));
  }
}

TEST_CASE("Local pairs use occurrence and view identity without requiring optional stencil",
          "[gta4-native][graphics][lighting]") {
  LocalLightOccurrenceTracker tracker;
  CHECK_FALSE(tracker.HasPending(0x1000, 0x2000));
  CHECK(tracker.Select(LightPassRole::kLocalContribution, 0x1000, 0x2000, 1) == 1);
  CHECK(tracker.Select(LightPassRole::kLocalStencilSetup, 0x1000, 0x2000, 2) == 2);
  CHECK(tracker.HasPending(0x1000, 0x2000));
  CHECK(tracker.Select(LightPassRole::kLocalContribution, 0x1000, 0x2000, 3) == 2);
  CHECK_FALSE(tracker.HasPending(0x1000, 0x2000));
  CHECK(tracker.Select(LightPassRole::kLocalContribution, 0x1000, 0x2000, 4) == 4);
  CHECK(tracker.Select(LightPassRole::kLocalStencilSetup, 0x1000, 0x2000, 5) == 5);
  CHECK(tracker.Select(LightPassRole::kLocalContribution, 0x1000, 0x3000, 6) == 6);
  CHECK(tracker.Select(LightPassRole::kLocalStencilSetup, 0x1000, 0x2000, 7) == 7);
  CHECK(tracker.Select(LightPassRole::kLocalContribution, 0x4000, 0x2000, 8) == 8);

  const LightingContext setup = LocalStencil();
  LightingContext contribution = setup;
  contribution.role = LightPassRole::kLocalContribution;
  contribution.requested_selector = 4;
  REQUIRE(IsSameLocalLightOccurrence(setup, contribution));
  contribution.occurrence_id = 12;
  CHECK_FALSE(IsSameLocalLightOccurrence(setup, contribution));
  contribution.occurrence_id = setup.occurrence_id;
  contribution.view_id = 8;
  CHECK_FALSE(IsSameLocalLightOccurrence(setup, contribution));
}

TEST_CASE("Nested lighting executors restore selectors without consuming parent scopes",
          "[gta4-native][graphics][lighting]") {
  ScopedNativeLightingContext outer(LightingContext{});
  const LightingContext local = LocalStencil();
  BeginNativeLightingSelector(local);
  REQUIRE(GetNativeLightingContext() == local);
  {
    LightingContext global{};
    global.role = LightPassRole::kGlobalParameterUpload;
    ScopedNativeLightingContext nested(global);
    EndNativeLightingSelector();
    CHECK(GetNativeLightingContext() == global);
    LightingContext shaft{};
    shaft.role = LightPassRole::kShaft;
    BeginNativeLightingSelector(shaft);
    CHECK(GetNativeLightingContext() == shaft);
    // Executor unwinding also closes its own unmatched child selector.
  }
  CHECK(GetNativeLightingContext() == local);
  EndNativeLightingSelector();
  CHECK(GetNativeLightingContext() == LightingContext{});
}

TEST_CASE("All title draw forms carry current execution semantics through replay",
          "[gta4-native][graphics][lighting]") {
  CheckDrawExecutionContext<DrawPrimitiveCommand>();
  CheckDrawExecutionContext<DrawPrimitiveUpCommand>();
  CheckDrawExecutionContext<DrawIndexedPrimitiveCommand>();
}

TEST_CASE("Title clear and optional resolve command values initialize deterministically",
          "[gta4-native][graphics][lighting]") {
  ClearCommand clear;
  CHECK(clear.header.type == CommandType::kClear);
  CHECK(clear.flags == 0);
  CHECK(clear.depth_bits == 0);
  CHECK(clear.stencil == 0);
  for (uint32_t color : clear.color_bits) {
    CHECK(color == 0);
  }
  ResolveCommand resolve;
  CHECK(resolve.header.type == CommandType::kResolve);
  CHECK(resolve.parameters_valid == 0);
  CHECK(resolve.color_format == 0);
  CHECK(resolve.color_exp_bias == 0);
  CHECK(resolve.depth_format == 0);
  CHECK(resolve.source_rectangle_valid == 0);
  CHECK(resolve.destination_point_valid == 0);
  CHECK(resolve.source_rectangle.right == 0);
  CHECK(resolve.destination_point.y == 0);
  for (uint32_t color : resolve.clear_color_bits) {
    CHECK(color == 0);
  }
  RegisterVertexDeclarationCommand declaration;
  for (const auto& element : declaration.elements) {
    CHECK(element.stream == 0);
    CHECK(element.offset == 0);
    CHECK(element.padding == 0);
  }
}

}  // namespace
}  // namespace rex::graphics::gta4_native
