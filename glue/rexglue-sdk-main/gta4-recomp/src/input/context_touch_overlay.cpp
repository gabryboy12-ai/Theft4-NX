#include "input/context_touch_overlay.h"

#include <algorithm>

#include <imgui.h>

#include "input/context_touch_controls.h"

namespace gta4::input {

ContextTouchOverlay::ContextTouchOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

void ContextTouchOverlay::OnDraw(ImGuiIO& io) {
  const ContextTouchOverlaySnapshot snapshot = GetContextTouchOverlaySnapshot();
  if (!snapshot.visible) {
    return;
  }

  ImDrawList* draw = ImGui::GetForegroundDrawList();
  constexpr ImU32 kIdleFill = IM_COL32(12, 18, 27, 112);
  constexpr ImU32 kActiveFill = IM_COL32(236, 135, 42, 176);
  constexpr ImU32 kOutline = IM_COL32(255, 255, 255, 184);
  constexpr ImU32 kText = IM_COL32(255, 255, 255, 224);
  constexpr ImU32 kStickKnob = IM_COL32(255, 255, 255, 152);

  const ContextTouchOverlayTransform transform = BuildContextTouchOverlayTransform(
      snapshot.layout.viewport, io.DisplaySize.x, io.DisplaySize.y);
  if (!transform.valid) {
    return;
  }
  const float radius_scale = std::min(transform.scale_x, transform.scale_y);
  const auto logical_point = [&](float guest_x, float guest_y) {
    return ImVec2(transform.offset_x + guest_x * transform.scale_x,
                  transform.offset_y + guest_y * transform.scale_y);
  };

  for (size_t index = 0; index < snapshot.layout.control_count; ++index) {
    const ContextTouchControl& control = snapshot.layout.controls[index];
    if (control.kind == ContextTouchControlKind::kLookSurface) {
      continue;
    }
    const bool active = snapshot.active[index] != 0;
    const ImVec2 center = logical_point(control.center_x, control.center_y);
    const float radius = control.radius * radius_scale;
    draw->AddCircleFilled(center, radius, active ? kActiveFill : kIdleFill, 48);
    draw->AddCircle(center, radius, kOutline, 48, std::max(1.0f, radius * 0.025f));

    if (control.kind == ContextTouchControlKind::kMovementStick) {
      const float x = std::clamp(snapshot.movement_x, -255.0f, 255.0f) / 255.0f;
      const float y = std::clamp(snapshot.movement_y, -255.0f, 255.0f) / 255.0f;
      const ImVec2 knob(center.x + x * radius * 0.55f, center.y + y * radius * 0.55f);
      draw->AddCircleFilled(knob, radius * 0.34f, kStickKnob, 32);
    }

    if (control.label[0]) {
      const ImVec2 text_size = ImGui::CalcTextSize(control.label.data());
      draw->AddText(ImVec2(center.x - text_size.x * 0.5f, center.y - text_size.y * 0.5f), kText,
                    control.label.data());
    }
  }
}

}  // namespace gta4::input
