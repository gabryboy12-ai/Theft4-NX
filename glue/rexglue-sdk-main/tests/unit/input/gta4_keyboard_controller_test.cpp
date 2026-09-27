#include <array>
#include <utility>
#include <climits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "gta4_keyboard_controller.h"

namespace gta4::input {

TEST_CASE("Global controller keys produce exactly their corresponding Xbox button",
          "[input][gta4][keyboard][controller]") {
  using rex::ui::VirtualKey;
  using namespace rex::input;
  const auto bindings = KeyboardControllerBindings(GENERATE(false, true));
  const std::array directions{
      std::pair{VirtualKey::kUp, X_INPUT_GAMEPAD_DPAD_UP},
      std::pair{VirtualKey::kDown, X_INPUT_GAMEPAD_DPAD_DOWN},
      std::pair{VirtualKey::kLeft, X_INPUT_GAMEPAD_DPAD_LEFT},
      std::pair{VirtualKey::kRight, X_INPUT_GAMEPAD_DPAD_RIGHT},
      std::pair{VirtualKey::kReturn, X_INPUT_GAMEPAD_A},
      std::pair{VirtualKey::kDelete, X_INPUT_GAMEPAD_B},
      std::pair{VirtualKey::kBack, X_INPUT_GAMEPAD_B},
      std::pair{VirtualKey::kF, X_INPUT_GAMEPAD_Y},
  };
  for (const auto [key, expected] : directions) {
    bool keys[256]{};
    keys[static_cast<size_t>(key)] = true;
    auto state = mnk::BuildNativeControllerCompatibilityGamepad(bindings, keys,
                                                               std::size(keys));
    CHECK(static_cast<uint16_t>(state.buttons) == expected);
    CHECK(state.left_trigger == 0);
    CHECK(state.right_trigger == 0);
    CHECK(static_cast<int16_t>(state.thumb_lx) == 0);
    CHECK(static_cast<int16_t>(state.thumb_ly) == 0);
    CHECK(static_cast<int16_t>(state.thumb_rx) == 0);
    CHECK(static_cast<int16_t>(state.thumb_ry) == 0);
    keys[static_cast<size_t>(key)] = false;
    state = mnk::BuildNativeControllerCompatibilityGamepad(bindings, keys,
                                                          std::size(keys));
    CHECK(static_cast<uint16_t>(state.buttons) == 0);
  }
}

TEST_CASE("Helicopter keys reproduce the requested controller layout",
          "[input][gta4][keyboard][controller][helicopter]") {
  using rex::ui::VirtualKey;
  using namespace rex::input;
  struct Mapping {
    VirtualKey key;
    uint16_t buttons;
    uint8_t left_trigger;
    uint8_t right_trigger;
    int16_t left_x;
    int16_t left_y;
  };
  const Mapping mappings[] = {
      {VirtualKey::kLButton, X_INPUT_GAMEPAD_A, 0, 0, 0, 0},
      {VirtualKey::kShift, X_INPUT_GAMEPAD_X, 0, 0, 0, 0},
      {VirtualKey::kW, 0, 0, UINT8_MAX, 0, 0},
      {VirtualKey::kS, 0, UINT8_MAX, 0, 0, 0},
      {VirtualKey::kNumpad8, 0, 0, 0, 0, INT16_MAX},
      {VirtualKey::kNumpad2, 0, 0, 0, 0, -INT16_MAX},
      {VirtualKey::kA, 0, 0, 0, -INT16_MAX, 0},
      {VirtualKey::kD, 0, 0, 0, INT16_MAX, 0},
      {VirtualKey::kNumpad4, X_INPUT_GAMEPAD_LEFT_SHOULDER, 0, 0, 0, 0},
      {VirtualKey::kNumpad6, X_INPUT_GAMEPAD_RIGHT_SHOULDER, 0, 0, 0, 0},
  };
  for (const Mapping& mapping : mappings) {
    CAPTURE(mapping.key);
    bool keys[256]{};
    keys[static_cast<size_t>(mapping.key)] = true;
    const auto flight = mnk::BuildNativeControllerCompatibilityGamepad(
        KeyboardControllerBindings(true), keys, std::size(keys));
    CHECK(flight.buttons == mapping.buttons);
    CHECK(flight.left_trigger == mapping.left_trigger);
    CHECK(flight.right_trigger == mapping.right_trigger);
    CHECK(flight.thumb_lx == mapping.left_x);
    CHECK(flight.thumb_ly == mapping.left_y);
    CHECK(flight.thumb_rx == 0);
    CHECK(flight.thumb_ry == 0);
    CHECK(IsKeyboardControllerKey(mapping.key, true));
    CHECK_FALSE(IsKeyboardControllerKey(mapping.key, false));

    const auto after_exit = mnk::BuildNativeControllerCompatibilityGamepad(
        KeyboardControllerBindings(false), keys, std::size(keys));
    CHECK(after_exit.buttons == 0);
    CHECK(after_exit.left_trigger == 0);
    CHECK(after_exit.right_trigger == 0);
    CHECK(after_exit.thumb_lx == 0);
    CHECK(after_exit.thumb_ly == 0);
  }
}

TEST_CASE("Flight bindings follow helicopter driver ownership and yield to menus",
          "[input][gta4][keyboard][controller][helicopter]") {
  CHECK(UseHelicopterControllerBindings(true, true, false));
  CHECK_FALSE(UseHelicopterControllerBindings(false, true, false));
  CHECK_FALSE(UseHelicopterControllerBindings(true, false, false));
  CHECK_FALSE(UseHelicopterControllerBindings(false, false, false));
  CHECK_FALSE(UseHelicopterControllerBindings(true, true, true));
  CHECK_FALSE(IsKeyboardControllerKey(rex::ui::VirtualKey::kNone, false));
  CHECK_FALSE(IsKeyboardControllerKey(rex::ui::VirtualKey::kNone, true));
}

TEST_CASE("Other PC controls remain outside the global controller bridge",
          "[input][gta4][keyboard][controller]") {
  using rex::ui::VirtualKey;
  bool keys[256]{};
  for (const auto key : {VirtualKey::kW, VirtualKey::kA, VirtualKey::kS,
                         VirtualKey::kD, VirtualKey::kEscape,
                         VirtualKey::kSpace, VirtualKey::kLButton,
                         VirtualKey::kRButton}) {
    keys[static_cast<size_t>(key)] = true;
  }
  const auto state = rex::input::mnk::BuildNativeControllerCompatibilityGamepad(
      KeyboardControllerBindings(), keys, std::size(keys));
  CHECK(static_cast<uint16_t>(state.buttons) == 0);
  CHECK(state.left_trigger == 0);
  CHECK(state.right_trigger == 0);
}

}  // namespace gta4::input
