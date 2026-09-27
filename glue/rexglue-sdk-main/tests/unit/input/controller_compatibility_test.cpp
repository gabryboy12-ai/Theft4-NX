#include <algorithm>
#include <iterator>

#include <catch2/catch_test_macros.hpp>

#include <rex/input/mnk/controller_compatibility.h>

namespace rex::input::mnk {
namespace {

using rex::ui::VirtualKey;

void SetKey(bool (&keys)[256], VirtualKey key, bool down) {
  keys[static_cast<size_t>(key)] = down;
}

NativeControllerCompatibilityBindings LibertyBindings() {
  return {
      .a = VirtualKey::kShift,
      .a_alias = VirtualKey::kSpace,
      .b = VirtualKey::kControl,
      .y = VirtualKey::kF,
      .dpad_up = VirtualKey::kUp,
      .dpad_down = VirtualKey::kT,
      .start = VirtualKey::kEscape,
      .back = VirtualKey::kV,
      .left_shoulder = VirtualKey::kE,
      .right_shoulder = VirtualKey::kQ,
  };
}

}  // namespace

TEST_CASE("Space publishes Cross as Xbox A without publishing Xbox X",
          "[input][mnk][compatibility]") {
  bool keys[256] = {};
  SetKey(keys, VirtualKey::kSpace, true);

  const X_INPUT_GAMEPAD gamepad = BuildNativeControllerCompatibilityGamepad(
      LibertyBindings(), keys, std::size(keys));
  CHECK((static_cast<uint16_t>(gamepad.buttons) & X_INPUT_GAMEPAD_A) != 0);
  CHECK((static_cast<uint16_t>(gamepad.buttons) & X_INPUT_GAMEPAD_X) == 0);
  CHECK(static_cast<int16_t>(gamepad.thumb_lx) == 0);
  CHECK(static_cast<int16_t>(gamepad.thumb_ly) == 0);
  CHECK(static_cast<int16_t>(gamepad.thumb_rx) == 0);
  CHECK(static_cast<int16_t>(gamepad.thumb_ry) == 0);
}

TEST_CASE("Primary controller aliases remain asserted until every source is released",
          "[input][mnk][compatibility]") {
  bool keys[256] = {};
  const auto bindings = LibertyBindings();
  SetKey(keys, VirtualKey::kShift, true);
  SetKey(keys, VirtualKey::kSpace, true);
  CHECK((static_cast<uint16_t>(BuildNativeControllerCompatibilityGamepad(
             bindings, keys, std::size(keys)).buttons) &
         X_INPUT_GAMEPAD_A) != 0);

  SetKey(keys, VirtualKey::kSpace, false);
  CHECK((static_cast<uint16_t>(BuildNativeControllerCompatibilityGamepad(
             bindings, keys, std::size(keys)).buttons) &
         X_INPUT_GAMEPAD_A) != 0);

  SetKey(keys, VirtualKey::kShift, false);
  CHECK((static_cast<uint16_t>(BuildNativeControllerCompatibilityGamepad(
             bindings, keys, std::size(keys)).buttons) &
         X_INPUT_GAMEPAD_A) == 0);
}

TEST_CASE("Configured pause and navigation keys publish retail digital buttons",
          "[input][mnk][compatibility]") {
  bool keys[256] = {};
  SetKey(keys, VirtualKey::kEscape, true);
  SetKey(keys, VirtualKey::kUp, true);
  SetKey(keys, VirtualKey::kT, true);
  SetKey(keys, VirtualKey::kE, true);
  SetKey(keys, VirtualKey::kQ, true);

  const X_INPUT_GAMEPAD gamepad = BuildNativeControllerCompatibilityGamepad(
      LibertyBindings(), keys, std::size(keys));
  const uint16_t buttons = static_cast<uint16_t>(gamepad.buttons);
  CHECK((buttons & X_INPUT_GAMEPAD_START) != 0);
  CHECK((buttons & X_INPUT_GAMEPAD_DPAD_UP) != 0);
  CHECK((buttons & X_INPUT_GAMEPAD_DPAD_DOWN) != 0);
  CHECK((buttons & X_INPUT_GAMEPAD_LEFT_SHOULDER) != 0);
  CHECK((buttons & X_INPUT_GAMEPAD_RIGHT_SHOULDER) != 0);
}

TEST_CASE("Key-bound triggers are digital while unconfigured inputs stay neutral",
          "[input][mnk][compatibility]") {
  bool keys[256] = {};
  NativeControllerCompatibilityBindings bindings;
  bindings.left_trigger = VirtualKey::kZ;
  bindings.right_trigger = VirtualKey::kX;
  SetKey(keys, VirtualKey::kZ, true);

  X_INPUT_GAMEPAD gamepad = BuildNativeControllerCompatibilityGamepad(
      bindings, keys, std::size(keys));
  CHECK(gamepad.left_trigger == 0xFF);
  CHECK(gamepad.right_trigger == 0);
  CHECK(static_cast<uint16_t>(gamepad.buttons) == 0);

  gamepad = BuildNativeControllerCompatibilityGamepad(bindings, keys, 0);
  CHECK(gamepad.left_trigger == 0);
  CHECK(gamepad.right_trigger == 0);
}

TEST_CASE("Key-bound left-stick directions preserve signs and cancel opposing keys",
          "[input][mnk][compatibility][stick]") {
  const NativeControllerCompatibilityBindings bindings{
      .a = VirtualKey::kSpace,
      .left_trigger = VirtualKey::kZ,
      .right_trigger = VirtualKey::kX,
      .left_stick_up = VirtualKey::kNumpad8,
      .left_stick_down = VirtualKey::kNumpad2,
      .left_stick_left = VirtualKey::kA,
      .left_stick_right = VirtualKey::kD,
  };
  struct Directions {
    bool left, right, down, up;
    int16_t x, y;
  };
  constexpr Directions cases[] = {
      {false, false, false, false, 0, 0},
      {false, false, false, true, 0, 32767},
      {false, false, true, false, 0, -32767},
      {false, false, true, true, 0, 0},
      {false, true, false, false, 32767, 0},
      {false, true, false, true, 32767, 32767},
      {false, true, true, false, 32767, -32767},
      {false, true, true, true, 32767, 0},
      {true, false, false, false, -32767, 0},
      {true, false, false, true, -32767, 32767},
      {true, false, true, false, -32767, -32767},
      {true, false, true, true, -32767, 0},
      {true, true, false, false, 0, 0},
      {true, true, false, true, 0, 32767},
      {true, true, true, false, 0, -32767},
      {true, true, true, true, 0, 0},
  };
  for (const auto& expected : cases) {
    CAPTURE(expected.left, expected.right, expected.down, expected.up);
    bool keys[256]{};
    SetKey(keys, VirtualKey::kSpace, true);
    SetKey(keys, VirtualKey::kZ, true);
    SetKey(keys, VirtualKey::kA, expected.left);
    SetKey(keys, VirtualKey::kD, expected.right);
    SetKey(keys, VirtualKey::kNumpad2, expected.down);
    SetKey(keys, VirtualKey::kNumpad8, expected.up);
    const auto gamepad = BuildNativeControllerCompatibilityGamepad(
        bindings, keys, std::size(keys));
    CHECK(static_cast<int16_t>(gamepad.thumb_lx) == expected.x);
    CHECK(static_cast<int16_t>(gamepad.thumb_ly) == expected.y);
    CHECK(static_cast<int16_t>(gamepad.thumb_rx) == 0);
    CHECK(static_cast<int16_t>(gamepad.thumb_ry) == 0);
    CHECK(static_cast<uint16_t>(gamepad.buttons) == X_INPUT_GAMEPAD_A);
    CHECK(gamepad.left_trigger == 255);
    CHECK(gamepad.right_trigger == 0);
  }
}

TEST_CASE("Unbound or unavailable stick keys remain neutral",
          "[input][mnk][compatibility][stick]") {
  bool keys[256]{};
  std::fill(std::begin(keys), std::end(keys), true);
  NativeControllerCompatibilityBindings bindings;
  auto gamepad = BuildNativeControllerCompatibilityGamepad(
      bindings, keys, std::size(keys));
  CHECK(static_cast<int16_t>(gamepad.thumb_lx) == 0);
  CHECK(static_cast<int16_t>(gamepad.thumb_ly) == 0);

  bindings.left_stick_up = VirtualKey::kNumpad8;
  bindings.left_stick_left = VirtualKey::kA;
  gamepad = BuildNativeControllerCompatibilityGamepad(bindings, nullptr, std::size(keys));
  CHECK(static_cast<int16_t>(gamepad.thumb_lx) == 0);
  CHECK(static_cast<int16_t>(gamepad.thumb_ly) == 0);
  gamepad = BuildNativeControllerCompatibilityGamepad(bindings, keys, 0);
  CHECK(static_cast<int16_t>(gamepad.thumb_lx) == 0);
  CHECK(static_cast<int16_t>(gamepad.thumb_ly) == 0);

  bindings.left_stick_up = static_cast<VirtualKey>(std::size(keys));
  bindings.left_stick_left = static_cast<VirtualKey>(std::size(keys));
  gamepad = BuildNativeControllerCompatibilityGamepad(bindings, keys, std::size(keys));
  CHECK(static_cast<int16_t>(gamepad.thumb_lx) == 0);
  CHECK(static_cast<int16_t>(gamepad.thumb_ly) == 0);
}

TEST_CASE("Changing stick bindings retires old held directions",
          "[input][mnk][compatibility][stick]") {
  bool keys[256]{};
  SetKey(keys, VirtualKey::kA, true);
  SetKey(keys, VirtualKey::kNumpad8, true);
  NativeControllerCompatibilityBindings bindings{
      .left_stick_up = VirtualKey::kNumpad8,
      .left_stick_left = VirtualKey::kA,
  };
  auto gamepad = BuildNativeControllerCompatibilityGamepad(bindings, keys, std::size(keys));
  CHECK(static_cast<int16_t>(gamepad.thumb_lx) == -32767);
  CHECK(static_cast<int16_t>(gamepad.thumb_ly) == 32767);

  bindings.left_stick_up = VirtualKey::kNone;
  bindings.left_stick_left = VirtualKey::kD;
  gamepad = BuildNativeControllerCompatibilityGamepad(bindings, keys, std::size(keys));
  CHECK(static_cast<int16_t>(gamepad.thumb_lx) == 0);
  CHECK(static_cast<int16_t>(gamepad.thumb_ly) == 0);
}

}  // namespace rex::input::mnk
