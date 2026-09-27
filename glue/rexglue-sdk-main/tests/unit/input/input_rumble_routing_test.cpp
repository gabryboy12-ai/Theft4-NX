#include <memory>

#include <catch2/catch_test_macros.hpp>

#include <rex/input/input_system.h>

namespace rex::input {
namespace {

class RumbleResultDriver final : public InputDriver {
 public:
  explicit RumbleResultDriver(X_RESULT set_state_result)
      : InputDriver(nullptr, 0), set_state_result_(set_state_result) {}

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  X_RESULT GetCapabilities(uint32_t, uint32_t, X_INPUT_CAPABILITIES*) override {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetState(uint32_t, X_INPUT_STATE*) override { return X_ERROR_DEVICE_NOT_CONNECTED; }

  X_RESULT SetState(uint32_t, X_INPUT_VIBRATION*) override {
    ++set_state_calls;
    return set_state_result_;
  }

  X_RESULT GetKeystroke(uint32_t, uint32_t, X_INPUT_KEYSTROKE*) override {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t set_state_calls = 0;

 private:
  X_RESULT set_state_result_;
};

}  // namespace

TEST_CASE("Rumble routing preserves a physical driver failure", "[input][rumble]") {
  InputSystem input(nullptr);
  auto failing = std::make_unique<RumbleResultDriver>(X_ERROR_FUNCTION_FAILED);
  RumbleResultDriver* failing_ptr = failing.get();
  auto disconnected = std::make_unique<RumbleResultDriver>(X_ERROR_DEVICE_NOT_CONNECTED);
  RumbleResultDriver* disconnected_ptr = disconnected.get();
  input.AddDriver(std::move(failing));
  input.AddDriver(std::move(disconnected));

  X_INPUT_VIBRATION vibration{};
  CHECK(input.SetState(0, &vibration) == X_ERROR_FUNCTION_FAILED);
  CHECK(failing_ptr->set_state_calls == 1);
  CHECK(disconnected_ptr->set_state_calls == 1);
}

TEST_CASE("Rumble routing accepts a later physical driver success", "[input][rumble]") {
  InputSystem input(nullptr);
  auto failing = std::make_unique<RumbleResultDriver>(X_ERROR_FUNCTION_FAILED);
  auto succeeding = std::make_unique<RumbleResultDriver>(X_ERROR_SUCCESS);
  RumbleResultDriver* succeeding_ptr = succeeding.get();
  input.AddDriver(std::move(failing));
  input.AddDriver(std::move(succeeding));

  X_INPUT_VIBRATION vibration{};
  CHECK(input.SetState(0, &vibration) == X_ERROR_SUCCESS);
  CHECK(succeeding_ptr->set_state_calls == 1);
}

TEST_CASE("Rumble routing reports no connected output driver", "[input][rumble]") {
  InputSystem input(nullptr);
  input.AddDriver(std::make_unique<RumbleResultDriver>(X_ERROR_DEVICE_NOT_CONNECTED));

  X_INPUT_VIBRATION vibration{};
  CHECK(input.SetState(0, &vibration) == X_ERROR_DEVICE_NOT_CONNECTED);
}

}  // namespace rex::input
