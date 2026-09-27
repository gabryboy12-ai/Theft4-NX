/**
 * @file        rex/input/mnk/mnk_input_driver.h
 * @brief       Keyboard/mouse input driver - maps MnK to Xbox 360 controller.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/input/input_driver.h>
#include <rex/input/mnk/controller_compatibility.h>
#include <rex/input/mnk/pointer_motion.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

namespace rex::input::mnk {

struct NativeInputState {
  std::array<uint8_t, 256> keys{};
  // Press transitions delivered at this sampling boundary. Per-key transition
  // queues preserve short taps and intervening releases between guest polls.
  std::array<uint8_t, 256> pressed_keys{};
  uint32_t user_index = 0;
  double mouse_dx = 0.0;
  double mouse_dy = 0.0;
  int32_t mouse_wheel = 0;
  double mouse_sensitivity = 1.0;
  rex::ui::MouseEvent::MotionSource mouse_source = rex::ui::MouseEvent::MotionSource::kGeneric;
  bool mouse_has_motion = false;
  uint64_t mouse_reset_generation = 0;
  bool invert_mouse_y = false;
  uint64_t last_key_event_sequence = 0;
  uint64_t key_state_generation = 0;
  std::array<uint64_t, 256> key_event_sequences{};
};

// Native gameplay's explicit sampling boundary. A successful call transfers
// ownership of one accumulated key-press/relative-mouse/wheel interval to the caller.
// Connectivity observations must not call this function.
bool ConsumeNativeInputState(NativeInputState* out_state);

class MnkInputDriver final : public InputDriver,
                             public rex::ui::WindowInputListener,
                             public rex::ui::WindowListener {
 public:
  explicit MnkInputDriver(rex::ui::Window* window, size_t window_z_order);
  ~MnkInputDriver() override;

  X_STATUS Setup() override;
  const char* trace_name() const override { return "mnk"; }
  const char* input_trace_name() const override { return "mnk"; }

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke) override;

  void OnWindowAvailable(rex::ui::Window* window) override;

  // WindowInputListener
  void OnKeyDown(rex::ui::KeyEvent& e) override;
  void OnKeyUp(rex::ui::KeyEvent& e) override;
  void OnMouseDown(rex::ui::MouseEvent& e) override;
  void OnMouseUp(rex::ui::MouseEvent& e) override;
  void OnMouseMove(rex::ui::MouseEvent& e) override;
  void OnMouseWheel(rex::ui::MouseEvent& e) override;

  // WindowListener
  void OnClosing(rex::ui::UIEvent& e) override;
  void OnLostFocus(rex::ui::UISetupEvent& e) override;
  void OnGotFocus(rex::ui::UISetupEvent& e) override;

  // This and controller-emulation GetState are mutually exclusive consumers:
  // the mnk_controller_emulation mode selects exactly one sampling owner.
  bool ConsumeNativeState(NativeInputState* out_state);

 private:
  uint32_t UserIndex() const;
  bool IsEnabled() const;
  void CenterCursor();
  void UpdateMouseCapture();
  void ResetPointerMotionLocked();
  void ResetKeyboardStateLocked();
  bool SetKeyState(uint16_t vk, bool down);
  void EnqueueKeystroke(uint16_t vk_pad, bool down);
  void TraceControllerCompatibility(
      const NativeControllerCompatibilityBindings& bindings,
      const X_INPUT_GAMEPAD& gamepad);

  rex::ui::Window* attached_window_ = nullptr;

  std::mutex state_mutex_;
  bool key_down_[256] = {};
  // XInput and native action injection poll independently. Neither consumer
  // may drain a short press before the other has observed it.
  std::array<std::vector<uint8_t>, 256> native_key_transitions_;
  std::array<std::vector<uint8_t>, 256> controller_key_transitions_;

  // Mouse delta tracking
  PointerMotionAccumulator pointer_motion_;
  int32_t mouse_wheel_ = 0;
  int32_t prev_mouse_x_ = 0;
  int32_t prev_mouse_y_ = 0;
  bool mouse_captured_ = false;
  // Cursor visibility to restore on capture release - the window owner may run
  // an auto-hide policy that capture must not permanently override.
  rex::ui::Window::CursorVisibility precapture_cursor_visibility_ =
      rex::ui::Window::CursorVisibility::kVisible;
  bool has_focus_ = true;
  uint64_t mouse_reset_generation_ = 0;
  uint64_t last_key_event_sequence_ = 0;
  uint64_t key_state_generation_ = 0;
  std::array<uint64_t, 256> key_event_sequences_{};
  uint64_t last_traced_snapshot_generation_ = 0;
  std::array<uint8_t, 256> last_traced_snapshot_keys_{};
  bool trace_snapshot_initialized_ = false;
  int32_t last_traced_consume_status_ = -1;
  bool compatibility_trace_configured_ = false;
  bool compatibility_trace_initialized_ = false;
  NativeControllerCompatibilityBindings last_traced_compatibility_bindings_{};
  X_INPUT_GAMEPAD last_traced_compatibility_gamepad_{};

  // Keystroke queue
  std::queue<X_INPUT_KEYSTROKE> keystroke_queue_;

  // Packet number incremented on state change
  uint32_t packet_number_ = 0;
  X_INPUT_GAMEPAD last_emulated_gamepad_{};
};

}  // namespace rex::input::mnk
