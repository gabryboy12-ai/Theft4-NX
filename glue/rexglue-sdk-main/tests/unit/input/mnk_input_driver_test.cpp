#include <array>
#include <string>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include <rex/cvar.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/sdl_virtual_key.h>
#include <rex/ui/surface.h>

#include "gta4_keyboard_controller.h"

REXCVAR_DECLARE(bool, mnk_mode);
REXCVAR_DECLARE(bool, mnk_controller_emulation);
REXCVAR_DECLARE(int32_t, mnk_user_index);
REXCVAR_DECLARE(std::string, keybind_start);

namespace rex::input::mnk {
namespace {

using rex::ui::VirtualKey;

class TestAppContext final : public rex::ui::WindowedAppContext {
 public:
  void NotifyUILoopOfPendingFunctions() override {}
  void PlatformQuitFromUIThread() override {}
};

class TestInputWindow final : public rex::ui::Window {
 public:
  explicit TestInputWindow(TestAppContext& context)
      : Window(context, "Keyboard focus test", 640, 480) {}
  ~TestInputWindow() override { EnterDestructor(); }

  void SetFocusForTest(bool focused) {
    WindowDestructionReceiver receiver(this);
    OnFocusUpdate(focused, receiver);
  }

 protected:
  bool OpenImpl() override { return true; }
  void RequestCloseImpl() override {}
  std::unique_ptr<rex::ui::Surface> CreateSurfaceImpl(rex::ui::Surface::TypeFlags) override {
    return {};
  }
  void RequestPaintImpl() override {}
};

struct MnkFixture {
  MnkFixture() {
    saved_bindings = GetNativeControllerCompatibilityBindings();
    REXCVAR_SET(mnk_mode, true);
    REXCVAR_SET(mnk_controller_emulation, false);
    REXCVAR_SET(mnk_user_index, 0);
    REXCVAR_SET(keybind_start, "Escape");
    SetNativeControllerCompatibilityBindings({
        .dpad_up = VirtualKey::kUp,
        .dpad_down = VirtualKey::kDown,
        .dpad_left = VirtualKey::kLeft,
        .dpad_right = VirtualKey::kRight,
        .start = VirtualKey::kEscape,
    });
    driver.set_is_active_callback([this] { return active; });
  }

  ~MnkFixture() {
    SetNativeControllerCompatibilityBindings(saved_bindings);
    REXCVAR_SET(mnk_mode, saved_mode);
    REXCVAR_SET(mnk_controller_emulation, saved_emulation);
    REXCVAR_SET(mnk_user_index, saved_user_index);
    REXCVAR_SET(keybind_start, saved_start);
  }

  void Key(VirtualKey key, bool down, bool repeat = false) {
    rex::ui::KeyEvent event(nullptr, key, 1, repeat, false, false, false, false);
    if (down) {
      driver.OnKeyDown(event);
    } else {
      driver.OnKeyUp(event);
    }
  }

  NativeInputState Native() {
    NativeInputState state;
    REQUIRE(driver.ConsumeNativeState(&state));
    return state;
  }

  X_INPUT_STATE Controller() {
    X_INPUT_STATE state;
    REQUIRE(driver.GetState(0, &state) == X_ERROR_SUCCESS);
    return state;
  }

  bool saved_mode = REXCVAR_GET(mnk_mode);
  bool saved_emulation = REXCVAR_GET(mnk_controller_emulation);
  int32_t saved_user_index = REXCVAR_GET(mnk_user_index);
  std::string saved_start = REXCVAR_GET(keybind_start);
  NativeControllerCompatibilityBindings saved_bindings;
  bool active = true;
  MnkInputDriver driver{nullptr, 0};
};

bool Down(const NativeInputState& state, VirtualKey key) {
  return state.keys[static_cast<size_t>(key)] != 0;
}

bool Pressed(const NativeInputState& state, VirtualKey key) {
  return state.pressed_keys[static_cast<size_t>(key)] != 0;
}

}  // namespace

TEST_CASE_METHOD(MnkFixture, "Short Escape and arrow taps survive the native polling boundary",
                 "[input][mnk][polling]") {
  for (VirtualKey key : {VirtualKey::kEscape, VirtualKey::kUp, VirtualKey::kDown,
                        VirtualKey::kLeft, VirtualKey::kRight}) {
    Key(key, true);
    Key(key, false);
    REQUIRE(driver.GetState(0, nullptr) == X_ERROR_SUCCESS);
    CHECK_FALSE(driver.ConsumeNativeState(nullptr));

    const auto press = Native();
    CHECK(Down(press, key));
    CHECK(Pressed(press, key));

    const auto release = Native();
    CHECK_FALSE(Down(release, key));
    CHECK_FALSE(Pressed(release, key));
  }
}

TEST_CASE("Attaching MnK to an open window inherits its actual focus",
          "[input][mnk][focus]") {
  TestAppContext context;
  TestInputWindow window(context);
  REQUIRE(window.Open());
  bool initially_focused = false;
  SECTION("Window was already focused before input attached") {
    initially_focused = true;
    window.SetFocusForTest(true);
  }
  SECTION("Window has never gained focus") {}

  // Destruction order keeps the window alive while the driver detaches.
  MnkFixture fixture;
  fixture.driver.OnWindowAvailable(&window);
  NativeInputState state;
  CHECK(fixture.driver.ConsumeNativeState(&state) == initially_focused);
  fixture.Key(VirtualKey::kEscape, true);
  if (initially_focused) {
    CHECK(Pressed(fixture.Native(), VirtualKey::kEscape));
  } else {
    CHECK_FALSE(fixture.driver.ConsumeNativeState(&state));
    window.SetFocusForTest(true);
    CHECK_FALSE(Down(fixture.Native(), VirtualKey::kEscape));
    fixture.Key(VirtualKey::kEscape, true);
    CHECK(Pressed(fixture.Native(), VirtualKey::kEscape));
  }
}

TEST_CASE_METHOD(MnkFixture, "Native and controller compatibility polls cannot steal key taps",
                 "[input][mnk][polling]") {
  Key(VirtualKey::kEscape, true);
  Key(VirtualKey::kEscape, false);

  SECTION("Controller consumes first") {
    CHECK((Controller().gamepad.buttons & X_INPUT_GAMEPAD_START) != 0);
    CHECK((Controller().gamepad.buttons & X_INPUT_GAMEPAD_START) == 0);
    CHECK(Down(Native(), VirtualKey::kEscape));
  }
  SECTION("Native consumes first") {
    CHECK(Down(Native(), VirtualKey::kEscape));
    CHECK_FALSE(Down(Native(), VirtualKey::kEscape));
    CHECK((Controller().gamepad.buttons & X_INPUT_GAMEPAD_START) != 0);
    CHECK((Controller().gamepad.buttons & X_INPUT_GAMEPAD_START) == 0);
  }
}

TEST_CASE_METHOD(MnkFixture, "Global face-button keys preserve taps, holds and releases",
                 "[input][mnk][polling][gta4]") {
  SetNativeControllerCompatibilityBindings(gta4::input::KeyboardControllerBindings());
  for (const auto [key, button] : {
           std::pair{VirtualKey::kReturn, X_INPUT_GAMEPAD_A},
           std::pair{VirtualKey::kBack, X_INPUT_GAMEPAD_B},
           std::pair{VirtualKey::kDelete, X_INPUT_GAMEPAD_B},
           std::pair{VirtualKey::kF, X_INPUT_GAMEPAD_Y}}) {
    Key(key, true);
    Key(key, false);
    REQUIRE(driver.GetState(0, nullptr) == X_ERROR_SUCCESS);
    CHECK(Pressed(Native(), key));
    CHECK_FALSE(Down(Native(), key));
    CHECK(Controller().gamepad.buttons == button);
    CHECK(Controller().gamepad.buttons == 0);

    Key(key, true);
    const auto held = Controller();
    CHECK(held.gamepad.buttons == button);
    Key(key, true, true);
    const auto repeated = Controller();
    CHECK(repeated.gamepad.buttons == button);
    CHECK(repeated.packet_number == held.packet_number);
    Key(key, false);
    CHECK(Controller().gamepad.buttons == 0);
  }
}

TEST_CASE_METHOD(MnkFixture, "Leaving helicopter mode releases controller outputs for held keys",
                 "[input][mnk][polling][gta4][helicopter]") {
  SetNativeControllerCompatibilityBindings(gta4::input::KeyboardControllerBindings(true));
  Key(VirtualKey::kW, true);
  Key(VirtualKey::kA, true);
  Key(VirtualKey::kNumpad8, true);
  Key(VirtualKey::kLButton, true);
  Key(VirtualKey::kShift, true);
  const auto flight = Controller();
  CHECK(flight.gamepad.right_trigger == UINT8_MAX);
  CHECK(flight.gamepad.thumb_lx < 0);
  CHECK(flight.gamepad.thumb_ly > 0);
  CHECK((flight.gamepad.buttons & X_INPUT_GAMEPAD_A) != 0);
  CHECK((flight.gamepad.buttons & X_INPUT_GAMEPAD_X) != 0);

  SetNativeControllerCompatibilityBindings(gta4::input::KeyboardControllerBindings(false));
  const auto outside = Controller();
  CHECK(outside.gamepad.right_trigger == 0);
  CHECK(outside.gamepad.thumb_lx == 0);
  CHECK(outside.gamepad.thumb_ly == 0);
  CHECK(outside.gamepad.buttons == 0);
  CHECK(outside.packet_number != flight.packet_number);
  const auto native = Native();
  CHECK(Down(native, VirtualKey::kW));
  CHECK(Down(native, VirtualKey::kA));
  CHECK(Down(native, VirtualKey::kLButton));
  CHECK(Down(native, VirtualKey::kShift));
}

TEST_CASE_METHOD(MnkFixture, "Mac Delete and forward Delete hold one B until both release",
                 "[input][mnk][polling][gta4]") {
  SetNativeControllerCompatibilityBindings(gta4::input::KeyboardControllerBindings());
  Key(VirtualKey::kBack, true);
  CHECK(Controller().gamepad.buttons == X_INPUT_GAMEPAD_B);
  Key(VirtualKey::kDelete, true);
  const auto both = Controller();
  CHECK(both.gamepad.buttons == X_INPUT_GAMEPAD_B);
  Key(VirtualKey::kBack, false);
  const auto remaining = Controller();
  CHECK(remaining.gamepad.buttons == X_INPUT_GAMEPAD_B);
  CHECK(remaining.packet_number == both.packet_number);
  Key(VirtualKey::kDelete, false);
  CHECK(Controller().gamepad.buttons == 0);
}

TEST_CASE_METHOD(MnkFixture, "Held keys and operating system repeats produce one native press",
                 "[input][mnk][polling]") {
  Key(VirtualKey::kUp, true);
  CHECK(Pressed(Native(), VirtualKey::kUp));
  Key(VirtualKey::kUp, true, true);
  const auto repeated = Native();
  CHECK(Down(repeated, VirtualKey::kUp));
  CHECK_FALSE(Pressed(repeated, VirtualKey::kUp));
  Key(VirtualKey::kUp, false);
  CHECK_FALSE(Down(Native(), VirtualKey::kUp));

  Key(VirtualKey::kUp, true);
  Key(VirtualKey::kUp, false);
  CHECK(Pressed(Native(), VirtualKey::kUp));
  Key(VirtualKey::kUp, true);
  Key(VirtualKey::kUp, false);
  CHECK_FALSE(Down(Native(), VirtualKey::kUp));
  CHECK(Pressed(Native(), VirtualKey::kUp));
  CHECK_FALSE(Down(Native(), VirtualKey::kUp));
}

TEST_CASE_METHOD(MnkFixture, "A release and re-press between polls preserve both action edges",
                 "[input][mnk][polling]") {
  Key(VirtualKey::kEscape, true);
  CHECK(Pressed(Native(), VirtualKey::kEscape));
  CHECK((Controller().gamepad.buttons & X_INPUT_GAMEPAD_START) != 0);
  Key(VirtualKey::kEscape, false);
  Key(VirtualKey::kEscape, true);
  CHECK_FALSE(Down(Native(), VirtualKey::kEscape));
  CHECK((Controller().gamepad.buttons & X_INPUT_GAMEPAD_START) == 0);
  CHECK(Pressed(Native(), VirtualKey::kEscape));
  CHECK((Controller().gamepad.buttons & X_INPUT_GAMEPAD_START) != 0);
  const auto held = Native();
  CHECK(Down(held, VirtualKey::kEscape));
  CHECK_FALSE(Pressed(held, VirtualKey::kEscape));
  Key(VirtualKey::kEscape, false);
  CHECK_FALSE(Down(Native(), VirtualKey::kEscape));
  CHECK((Controller().gamepad.buttons & X_INPUT_GAMEPAD_START) == 0);
}

TEST_CASE_METHOD(MnkFixture, "Focus and overlay capture discard held keys and pending presses",
                 "[input][mnk][focus]") {
  Key(VirtualKey::kEscape, true);
  Key(VirtualKey::kUp, true);
  Key(VirtualKey::kUp, false);

  SECTION("Focus loss") {
    rex::ui::UISetupEvent event(nullptr);
    driver.OnLostFocus(event);
    driver.OnGotFocus(event);
  }
  SECTION("Host overlay owns input") {
    active = false;
    CHECK(Controller().gamepad.buttons == 0);
    NativeInputState state;
    CHECK_FALSE(driver.ConsumeNativeState(&state));
    Key(VirtualKey::kDown, true);
    active = true;
  }
  CHECK_FALSE(Down(Native(), VirtualKey::kEscape));
  CHECK_FALSE(Down(Native(), VirtualKey::kUp));
  CHECK_FALSE(Down(Native(), VirtualKey::kDown));
  CHECK(Controller().gamepad.buttons == 0);

  Key(VirtualKey::kEscape, true, true);
  CHECK_FALSE(Down(Native(), VirtualKey::kEscape));
  Key(VirtualKey::kEscape, false);
  Key(VirtualKey::kEscape, true);
  CHECK(Pressed(Native(), VirtualKey::kEscape));
}

TEST_CASE_METHOD(MnkFixture, "Disabled native input discards taps before reactivation",
                 "[input][mnk][focus]") {
  Key(VirtualKey::kEscape, true);
  REXCVAR_SET(mnk_mode, false);
  SECTION("Release arrives while disabled") {
    Key(VirtualKey::kEscape, false);
  }
  SECTION("Native consumer observes disable") {
    NativeInputState state;
    CHECK_FALSE(driver.ConsumeNativeState(&state));
  }
  SECTION("Controller consumer observes disable without a key-up event") {
    X_INPUT_STATE state;
    CHECK(driver.GetState(0, &state) == X_ERROR_DEVICE_NOT_CONNECTED);
  }
  REXCVAR_SET(mnk_mode, true);
  CHECK_FALSE(Down(Native(), VirtualKey::kEscape));
  CHECK(Controller().gamepad.buttons == 0);
}

TEST_CASE_METHOD(MnkFixture, "Inactive pointer events cannot leak motion or scrolling into gameplay",
                 "[input][mnk][focus][mouse]") {
  active = false;
  rex::ui::MouseEvent motion(nullptr, rex::ui::MouseEvent::Button::kNone,
                             0, 0, 0, 0, 30.0f, 40.0f, true);
  rex::ui::MouseEvent wheel(nullptr, rex::ui::MouseEvent::Button::kNone,
                            0, 0, 0, 1);
  driver.OnMouseMove(motion);
  driver.OnMouseWheel(wheel);
  active = true;
  const auto state = Native();
  CHECK_FALSE(state.mouse_has_motion);
  CHECK(state.mouse_dx == 0.0);
  CHECK(state.mouse_dy == 0.0);
  CHECK(state.mouse_wheel == 0);
}

TEST_CASE_METHOD(MnkFixture, "Legacy controller mode retains a short Escape tap",
                 "[input][mnk][polling]") {
  REXCVAR_SET(mnk_controller_emulation, true);
  Key(VirtualKey::kEscape, true);
  Key(VirtualKey::kEscape, false);
  REQUIRE(driver.GetState(0, nullptr) == X_ERROR_SUCCESS);
  CHECK((Controller().gamepad.buttons & X_INPUT_GAMEPAD_START) != 0);
  CHECK((Controller().gamepad.buttons & X_INPUT_GAMEPAD_START) == 0);
  REXCVAR_SET(mnk_controller_emulation, false);
  CHECK_FALSE(Down(Native(), VirtualKey::kEscape));
}

TEST_CASE_METHOD(MnkFixture, "Mouse side buttons reach native actions and release cleanly",
                 "[input][mnk][mouse]") {
  for (auto [button, key] : {
           std::pair{rex::ui::MouseEvent::Button::kX1, VirtualKey::kXButton1},
           std::pair{rex::ui::MouseEvent::Button::kX2, VirtualKey::kXButton2}}) {
    rex::ui::MouseEvent event(nullptr, button, 0, 0);
    driver.OnMouseDown(event);
    driver.OnMouseUp(event);
    CHECK(Pressed(Native(), key));
    CHECK_FALSE(Down(Native(), key));
  }
}

TEST_CASE("PC navigation names and SDL scancodes reach the same virtual keys",
          "[input][mnk][keys]") {
  CHECK(rex::ui::ParseVirtualKey("Esc") == VirtualKey::kEscape);
  CHECK(rex::ui::ParseVirtualKey("Enter") == VirtualKey::kReturn);
  CHECK(rex::ui::ParseVirtualKey("NumpadEnter") == VirtualKey::kReturn);
  CHECK(rex::ui::ParseVirtualKey("Mouse4") == VirtualKey::kXButton1);
  CHECK(rex::ui::ParseVirtualKey("Mouse5") == VirtualKey::kXButton2);
  CHECK(rex::ui::VirtualKeyToString(VirtualKey::kEscape) == "Escape");
  CHECK(rex::ui::VirtualKeyToString(VirtualKey::kReturn) == "Return");
  CHECK(rex::ui::VirtualKeyToString(VirtualKey::kXButton1) == "Mouse4");
  CHECK(rex::ui::VirtualKeyToString(VirtualKey::kXButton2) == "Mouse5");
  CHECK(rex::ui::TranslateSDLScancode(SDL_SCANCODE_ESCAPE) == VirtualKey::kEscape);
  CHECK(rex::ui::TranslateSDLScancode(SDL_SCANCODE_UP) == VirtualKey::kUp);
  CHECK(rex::ui::TranslateSDLScancode(SDL_SCANCODE_DOWN) == VirtualKey::kDown);
  CHECK(rex::ui::TranslateSDLScancode(SDL_SCANCODE_LEFT) == VirtualKey::kLeft);
  CHECK(rex::ui::TranslateSDLScancode(SDL_SCANCODE_RIGHT) == VirtualKey::kRight);
  CHECK(rex::ui::TranslateSDLScancode(SDL_SCANCODE_RETURN) == VirtualKey::kReturn);
  CHECK(rex::ui::TranslateSDLScancode(SDL_SCANCODE_KP_ENTER) == VirtualKey::kReturn);
  CHECK(rex::ui::TranslateSDLScancode(SDL_SCANCODE_BACKSPACE) == VirtualKey::kBack);
  CHECK(rex::ui::TranslateSDLScancode(SDL_SCANCODE_DELETE) == VirtualKey::kDelete);
}

}  // namespace rex::input::mnk
