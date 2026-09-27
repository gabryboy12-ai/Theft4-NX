#pragma once

#include <rex/input/mnk/controller_compatibility.h>

namespace gta4::input {

// These keys are controller buttons in every game context. Retail code owns
// their phone, menu and gameplay actions. SDL maps Return and keypad Enter to
// kReturn, and the Mac's Delete key to kBack.
constexpr rex::input::mnk::NativeControllerCompatibilityBindings KeyboardControllerBindings(
    bool helicopter_controls = false) {
  using rex::ui::VirtualKey;
  rex::input::mnk::NativeControllerCompatibilityBindings bindings{
      .a = VirtualKey::kReturn,
      .b = VirtualKey::kDelete,
      .b_alias = VirtualKey::kBack,
      .y = VirtualKey::kF,
      .dpad_up = VirtualKey::kUp,
      .dpad_down = VirtualKey::kDown,
      .dpad_left = VirtualKey::kLeft,
      .dpad_right = VirtualKey::kRight,
  };
  if (helicopter_controls) {
    bindings.a_alias = VirtualKey::kLButton;
    // SDL normalizes Left Shift to kShift, as it does for native PC controls.
    bindings.x = VirtualKey::kShift;
    bindings.left_trigger = VirtualKey::kS;
    bindings.right_trigger = VirtualKey::kW;
    bindings.left_shoulder = VirtualKey::kNumpad4;
    bindings.right_shoulder = VirtualKey::kNumpad6;
    bindings.left_stick_up = VirtualKey::kNumpad8;
    bindings.left_stick_down = VirtualKey::kNumpad2;
    bindings.left_stick_left = VirtualKey::kA;
    bindings.left_stick_right = VirtualKey::kD;
  }
  return bindings;
}

constexpr bool UseHelicopterControllerBindings(bool driver, bool helicopter,
                                              bool frontend_active) {
  return driver && helicopter && !frontend_active;
}

constexpr bool IsKeyboardControllerKey(rex::ui::VirtualKey key,
                                       bool helicopter_controls = false) {
  if (key == rex::ui::VirtualKey::kNone) {
    return false;
  }
  const auto bindings = KeyboardControllerBindings(helicopter_controls);
  return key == bindings.a || key == bindings.a_alias ||
         key == bindings.b || key == bindings.b_alias ||
         key == bindings.x || key == bindings.y ||
         key == bindings.dpad_up || key == bindings.dpad_down ||
         key == bindings.dpad_left || key == bindings.dpad_right ||
         key == bindings.left_trigger || key == bindings.right_trigger ||
         key == bindings.left_shoulder || key == bindings.right_shoulder ||
         key == bindings.left_stick_up || key == bindings.left_stick_down ||
         key == bindings.left_stick_left || key == bindings.left_stick_right;
}

}  // namespace gta4::input
