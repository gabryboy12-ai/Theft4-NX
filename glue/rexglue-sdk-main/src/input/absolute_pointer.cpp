#include <rex/input/absolute_pointer.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>

#if REX_PLATFORM_ANDROID
#include <jni.h>
#include <SDL3/SDL_system.h>
#endif

REXCVAR_DEFINE_STRING(touch_controls, "auto", "Input/Touch", "Touch controls: auto, on, or off")
    .allowed({"auto", "on", "off"});

namespace rex::input {

namespace {

TouchControlsMode GetConfiguredTouchControlsMode() {
  const std::string& mode = REXCVAR_GET(touch_controls);
  if (mode == "on") {
    return TouchControlsMode::kOn;
  }
  if (mode == "off") {
    return TouchControlsMode::kOff;
  }
  return TouchControlsMode::kAuto;
}

const char* TouchControlsModeName(TouchControlsMode mode) {
  switch (mode) {
    case TouchControlsMode::kAuto:
      return "auto";
    case TouchControlsMode::kOn:
      return "on";
    case TouchControlsMode::kOff:
      return "off";
  }
  return "auto";
}

#if REX_PLATFORM_ANDROID
bool QueryAndroidPhysicalKeyboard(bool& present_out) {
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  if (!env) {
    return false;
  }

  jclass input_device_class = env->FindClass("android/view/InputDevice");
  if (!input_device_class) {
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
    }
    return false;
  }

  jmethodID get_device_ids = env->GetStaticMethodID(input_device_class, "getDeviceIds", "()[I");
  jmethodID get_device =
      env->GetStaticMethodID(input_device_class, "getDevice", "(I)Landroid/view/InputDevice;");
  jmethodID get_keyboard_type = env->GetMethodID(input_device_class, "getKeyboardType", "()I");
  jmethodID is_virtual = env->GetMethodID(input_device_class, "isVirtual", "()Z");
  jfieldID keyboard_type_alphabetic =
      env->GetStaticFieldID(input_device_class, "KEYBOARD_TYPE_ALPHABETIC", "I");
  if (!get_device_ids || !get_device || !get_keyboard_type || !is_virtual ||
      !keyboard_type_alphabetic) {
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
    }
    env->DeleteLocalRef(input_device_class);
    return false;
  }

  const jint alphabetic_type = env->GetStaticIntField(input_device_class, keyboard_type_alphabetic);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    env->DeleteLocalRef(input_device_class);
    return false;
  }
  jintArray device_ids =
      static_cast<jintArray>(env->CallStaticObjectMethod(input_device_class, get_device_ids));
  if (env->ExceptionCheck() || !device_ids) {
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
    }
    env->DeleteLocalRef(input_device_class);
    return false;
  }

  present_out = false;
  const jsize device_count = env->GetArrayLength(device_ids);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    env->DeleteLocalRef(device_ids);
    env->DeleteLocalRef(input_device_class);
    return false;
  }
  jint* ids = env->GetIntArrayElements(device_ids, nullptr);
  if (!ids) {
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
    }
    env->DeleteLocalRef(device_ids);
    env->DeleteLocalRef(input_device_class);
    return false;
  }
  for (jsize i = 0; i < device_count; ++i) {
    jobject device = env->CallStaticObjectMethod(input_device_class, get_device, ids[i]);
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      if (device) {
        env->DeleteLocalRef(device);
      }
      continue;
    }
    if (!device) {
      continue;
    }
    const jboolean virtual_result = env->CallBooleanMethod(device, is_virtual);
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      env->DeleteLocalRef(device);
      continue;
    }
    const bool device_is_virtual = virtual_result == JNI_TRUE;
    const jint keyboard_type = env->CallIntMethod(device, get_keyboard_type);
    const bool call_failed = env->ExceptionCheck();
    if (call_failed) {
      env->ExceptionClear();
    }
    env->DeleteLocalRef(device);
    if (!call_failed && !device_is_virtual && keyboard_type == alphabetic_type) {
      present_out = true;
      break;
    }
  }
  env->ReleaseIntArrayElements(device_ids, ids, JNI_ABORT);
  env->DeleteLocalRef(device_ids);
  env->DeleteLocalRef(input_device_class);
  return true;
}
#endif

}  // namespace

bool ShouldEnableTouchControls(TouchControlsMode mode, bool controller_connected,
                               bool physical_keyboard_connected, bool focused) noexcept {
  if (!focused || mode == TouchControlsMode::kOff) {
    return false;
  }
  if (mode == TouchControlsMode::kOn) {
    return true;
  }
  return !controller_connected && !physical_keyboard_connected;
}

AbsolutePointerService::AbsolutePointerService(size_t max_queued_moves)
    : max_queued_moves_(max_queued_moves) {}

size_t AbsolutePointerService::SourcePointerKeyHash::operator()(
    const SourcePointerKey& key) const noexcept {
  const size_t device_hash = std::hash<uint64_t>{}(key.device_id);
  const size_t pointer_hash = std::hash<uint64_t>{}(key.pointer_id);
  return device_hash ^ (pointer_hash << 1);
}

bool AbsolutePointerService::PresentationGeometryEqualsLocked(
    const rex::ui::GuestOutputTransform& transform, int32_t safe_area_x, int32_t safe_area_y,
    int32_t safe_area_width, int32_t safe_area_height) const {
  return transform_.revision == transform.revision &&
         transform_.surface_width == transform.surface_width &&
         transform_.surface_height == transform.surface_height &&
         transform_.host_render_target_width == transform.host_render_target_width &&
         transform_.host_render_target_height == transform.host_render_target_height &&
         transform_.output_x == transform.output_x && transform_.output_y == transform.output_y &&
         transform_.output_width == transform.output_width &&
         transform_.output_height == transform.output_height &&
         transform_.guest_width == transform.guest_width &&
         transform_.guest_height == transform.guest_height && safe_area_x_ == safe_area_x &&
         safe_area_y_ == safe_area_y && safe_area_width_ == safe_area_width &&
         safe_area_height_ == safe_area_height;
}

void AbsolutePointerService::UpdatePresentation(const rex::ui::GuestOutputTransform& transform,
                                                int32_t safe_area_x, int32_t safe_area_y,
                                                int32_t safe_area_width, int32_t safe_area_height,
                                                uint64_t timestamp_ns) {
  std::lock_guard lock(mutex_);
  if (PresentationGeometryEqualsLocked(transform, safe_area_x, safe_area_y, safe_area_width,
                                       safe_area_height)) {
    return;
  }
  ResetLocked(timestamp_ns);
  transform_ = transform;
  safe_area_x_ = safe_area_x;
  safe_area_y_ = safe_area_y;
  safe_area_width_ = std::max(safe_area_width, int32_t(0));
  safe_area_height_ = std::max(safe_area_height, int32_t(0));
}

void AbsolutePointerService::SetFocused(bool focused, uint64_t timestamp_ns) {
  std::lock_guard lock(mutex_);
  if (focused_ == focused) {
    return;
  }
  ResetLocked(timestamp_ns);
  focused_ = focused;
}

void AbsolutePointerService::MapPhysicalToGuestLocked(float physical_x, float physical_y,
                                                      float& guest_x, float& guest_y) const {
  const double host_x = double(physical_x) * double(transform_.host_render_target_width) /
                        double(transform_.surface_width);
  const double host_y = double(physical_y) * double(transform_.host_render_target_height) /
                        double(transform_.surface_height);
  guest_x = float((host_x - double(transform_.output_x)) * double(transform_.guest_width) /
                  double(transform_.output_width));
  guest_y = float((host_y - double(transform_.output_y)) * double(transform_.guest_height) /
                  double(transform_.output_height));
}

AbsolutePointerEvent AbsolutePointerService::MakeEventLocked(AbsolutePointerPhase phase,
                                                             const ActivePointer& pointer,
                                                             uint64_t timestamp_ns) {
  AbsolutePointerEvent event;
  event.sequence = next_sequence_++;
  event.generation = generation_;
  event.pointer_id = pointer.pointer_id;
  event.timestamp_ns = timestamp_ns;
  event.x = pointer.x;
  event.y = pointer.y;
  event.output_width = float(transform_.guest_width);
  event.output_height = float(transform_.guest_height);
  event.pressure = pointer.pressure;
  event.phase = phase;
  return event;
}

void AbsolutePointerService::QueueEventLocked(const AbsolutePointerEvent& event) {
  if (event.phase != AbsolutePointerPhase::kMove) {
    events_.push_back(event);
    return;
  }
  if (!max_queued_moves_) {
    return;
  }

  for (auto it = events_.rbegin(); it != events_.rend(); ++it) {
    if (it->phase == AbsolutePointerPhase::kMove && it->pointer_id == event.pointer_id &&
        it->generation == event.generation) {
      events_.erase(std::next(it).base());
      --queued_move_count_;
      break;
    }
  }
  if (queued_move_count_ >= max_queued_moves_) {
    const auto oldest_move = std::find_if(events_.begin(), events_.end(), [](const auto& queued) {
      return queued.phase == AbsolutePointerPhase::kMove;
    });
    if (oldest_move != events_.end()) {
      events_.erase(oldest_move);
      --queued_move_count_;
    }
  }
  events_.push_back(event);
  ++queued_move_count_;
}

void AbsolutePointerService::ResetLocked(uint64_t timestamp_ns) {
  for (const auto& [source, pointer] : active_pointers_) {
    (void)source;
    QueueEventLocked(MakeEventLocked(AbsolutePointerPhase::kCancel, pointer, timestamp_ns));
  }
  active_pointers_.clear();
  ++generation_;
}

void AbsolutePointerService::SubmitPointer(uint64_t source_device_id, uint64_t source_pointer_id,
                                           AbsolutePointerPhase phase, float physical_x,
                                           float physical_y, float pressure,
                                           uint64_t timestamp_ns) {
  if (!std::isfinite(physical_x) || !std::isfinite(physical_y)) {
    return;
  }
  if (!std::isfinite(pressure)) {
    pressure = 0.0f;
  }

  std::lock_guard lock(mutex_);
  if (!focused_ || !transform_.IsValid()) {
    return;
  }

  const SourcePointerKey source{source_device_id, source_pointer_id};
  auto active = active_pointers_.find(source);
  if (phase == AbsolutePointerPhase::kDown) {
    if (active != active_pointers_.end()) {
      ResetLocked(timestamp_ns);
    }
    ActivePointer pointer;
    pointer.pointer_id = next_pointer_id_++;
    MapPhysicalToGuestLocked(physical_x, physical_y, pointer.x, pointer.y);
    pointer.pressure = pressure;
    active_pointers_.emplace(source, pointer);
    QueueEventLocked(MakeEventLocked(phase, pointer, timestamp_ns));
    return;
  }
  if (active == active_pointers_.end()) {
    return;
  }

  MapPhysicalToGuestLocked(physical_x, physical_y, active->second.x, active->second.y);
  active->second.pressure = pressure;
  QueueEventLocked(MakeEventLocked(phase, active->second, timestamp_ns));
  if (phase == AbsolutePointerPhase::kUp || phase == AbsolutePointerPhase::kCancel) {
    active_pointers_.erase(active);
  }
}

void AbsolutePointerService::CancelAll(uint64_t timestamp_ns) {
  std::lock_guard lock(mutex_);
  ResetLocked(timestamp_ns);
}

bool AbsolutePointerService::TryDequeue(AbsolutePointerEvent* out_event) noexcept {
  if (!out_event) {
    return false;
  }
  std::lock_guard lock(mutex_);
  if (events_.empty()) {
    return false;
  }
  *out_event = events_.front();
  if (events_.front().phase == AbsolutePointerPhase::kMove) {
    --queued_move_count_;
  }
  events_.pop_front();
  return true;
}

TouchPresentationState AbsolutePointerService::BuildPresentationStateLocked() const {
  TouchPresentationState state;
  state.generation = generation_;
  state.output_width = float(transform_.guest_width);
  state.output_height = float(transform_.guest_height);
  state.valid = transform_.IsValid();
  state.focused = focused_;
  if (!state.valid) {
    return state;
  }

  state.physical_output_x = float(double(transform_.output_x) * double(transform_.surface_width) /
                                  double(transform_.host_render_target_width));
  state.physical_output_y = float(double(transform_.output_y) * double(transform_.surface_height) /
                                  double(transform_.host_render_target_height));
  state.physical_output_width =
      float(double(transform_.output_width) * double(transform_.surface_width) /
            double(transform_.host_render_target_width));
  state.physical_output_height =
      float(double(transform_.output_height) * double(transform_.surface_height) /
            double(transform_.host_render_target_height));
  state.physical_surface_width = float(transform_.surface_width);
  state.physical_surface_height = float(transform_.surface_height);

  float safe_left;
  float safe_top;
  float safe_right;
  float safe_bottom;
  const int64_t safe_right_physical = int64_t(safe_area_x_) + int64_t(safe_area_width_);
  const int64_t safe_bottom_physical = int64_t(safe_area_y_) + int64_t(safe_area_height_);
  MapPhysicalToGuestLocked(float(safe_area_x_), float(safe_area_y_), safe_left, safe_top);
  MapPhysicalToGuestLocked(float(safe_right_physical), float(safe_bottom_physical), safe_right,
                           safe_bottom);
  safe_left = std::clamp(safe_left, 0.0f, state.output_width);
  safe_top = std::clamp(safe_top, 0.0f, state.output_height);
  safe_right = std::clamp(safe_right, 0.0f, state.output_width);
  safe_bottom = std::clamp(safe_bottom, 0.0f, state.output_height);

  const double int32_max = double(std::numeric_limits<int32_t>::max());
  const int32_t left = int32_t(std::min(std::ceil(double(safe_left)), int32_max));
  const int32_t top = int32_t(std::min(std::ceil(double(safe_top)), int32_max));
  const int32_t right = int32_t(std::min(std::floor(double(safe_right)), int32_max));
  const int32_t bottom = int32_t(std::min(std::floor(double(safe_bottom)), int32_max));
  state.safe_area_x = left;
  state.safe_area_y = top;
  state.safe_area_width = std::max(right - left, int32_t(0));
  state.safe_area_height = std::max(bottom - top, int32_t(0));
  return state;
}

bool AbsolutePointerService::GetPresentationState(
    TouchPresentationState* out_state) const noexcept {
  if (!out_state) {
    return false;
  }
  std::lock_guard lock(mutex_);
  *out_state = BuildPresentationStateLocked();
  return out_state->valid;
}

void AbsolutePointerService::ReplacePhysicalKeyboards(const std::vector<uint64_t>& device_ids,
                                                      uint64_t timestamp_ns) {
  std::unordered_set<uint64_t> replacement;
  for (uint64_t device_id : device_ids) {
    if (device_id) {
      replacement.insert(device_id);
    }
  }
  std::lock_guard lock(mutex_);
  if (physical_keyboards_ == replacement) {
    return;
  }
  ResetLocked(timestamp_ns);
  physical_keyboards_ = std::move(replacement);
}

void AbsolutePointerService::AddPhysicalKeyboard(uint64_t device_id, uint64_t timestamp_ns) {
  if (!device_id) {
    return;
  }
  std::lock_guard lock(mutex_);
  if (physical_keyboards_.insert(device_id).second) {
    ResetLocked(timestamp_ns);
  }
}

void AbsolutePointerService::RemovePhysicalKeyboard(uint64_t device_id, uint64_t timestamp_ns) {
  std::lock_guard lock(mutex_);
  if (physical_keyboards_.erase(device_id)) {
    ResetLocked(timestamp_ns);
  }
}

void AbsolutePointerService::ReplaceGameControllers(const std::vector<uint64_t>& device_ids,
                                                    uint64_t timestamp_ns) {
  std::unordered_set<uint64_t> replacement;
  for (uint64_t device_id : device_ids) {
    if (device_id) {
      replacement.insert(device_id);
    }
  }
  std::lock_guard lock(mutex_);
  if (game_controllers_ == replacement) {
    return;
  }
  ResetLocked(timestamp_ns);
  game_controllers_ = std::move(replacement);
}

void AbsolutePointerService::AddGameController(uint64_t device_id, uint64_t timestamp_ns) {
  if (!device_id) {
    return;
  }
  std::lock_guard lock(mutex_);
  if (game_controllers_.insert(device_id).second) {
    ResetLocked(timestamp_ns);
  }
}

void AbsolutePointerService::RemoveGameController(uint64_t device_id, uint64_t timestamp_ns) {
  std::lock_guard lock(mutex_);
  if (game_controllers_.erase(device_id)) {
    ResetLocked(timestamp_ns);
  }
}

void AbsolutePointerService::SetAndroidPhysicalKeyboardPresence(bool present, bool query_succeeded,
                                                                uint64_t timestamp_ns) {
  std::lock_guard lock(mutex_);
  if (android_keyboard_presence_valid_ == query_succeeded &&
      android_physical_keyboard_present_ == (query_succeeded && present)) {
    return;
  }
  ResetLocked(timestamp_ns);
  android_keyboard_presence_valid_ = query_succeeded;
  android_physical_keyboard_present_ = query_succeeded && present;
}

bool AbsolutePointerService::HasPhysicalKeyboard() const noexcept {
  std::lock_guard lock(mutex_);
#if REX_PLATFORM_ANDROID
  if (android_keyboard_presence_valid_) {
    return android_physical_keyboard_present_;
  }
#endif
  return !physical_keyboards_.empty();
}

bool AbsolutePointerService::HasGameController() const noexcept {
  std::lock_guard lock(mutex_);
  return !game_controllers_.empty();
}

bool AbsolutePointerService::TouchControlsActive(TouchControlsMode mode) noexcept {
  std::lock_guard lock(mutex_);
#if REX_PLATFORM_ANDROID
  const size_t keyboard_count = android_keyboard_presence_valid_
                                    ? size_t(android_physical_keyboard_present_)
                                    : physical_keyboards_.size();
#else
  const size_t keyboard_count = physical_keyboards_.size();
#endif
  const size_t controller_count = game_controllers_.size();
  const bool active =
      ShouldEnableTouchControls(mode, controller_count != 0, keyboard_count != 0, focused_);
  if (policy_log_initialized_ && logged_touch_active_ != active && !active_pointers_.empty()) {
    // A policy transition must not allow a gesture that began while disabled
    // to reappear as an ownerless Move after controls are enabled (or leave a
    // title-owned pointer latched when controls become disabled).
    ResetLocked(0);
  }
  if (!policy_log_initialized_ || logged_policy_mode_ != mode ||
      logged_keyboard_count_ != keyboard_count || logged_controller_count_ != controller_count ||
      logged_focus_ != focused_ || logged_touch_active_ != active) {
    REXLOG_INFO(
        "Touch input policy: mode={} focus={} physical_keyboards={} keyboard_present={} "
        "controllers={} controller_present={} active={}",
        TouchControlsModeName(mode), focused_, keyboard_count, keyboard_count != 0,
        controller_count, controller_count != 0, active);
    policy_log_initialized_ = true;
    logged_policy_mode_ = mode;
    logged_keyboard_count_ = keyboard_count;
    logged_controller_count_ = controller_count;
    logged_focus_ = focused_;
    logged_touch_active_ = active;
  }
  return active;
}

size_t AbsolutePointerService::queued_move_count() const noexcept {
  std::lock_guard lock(mutex_);
  return queued_move_count_;
}

AbsolutePointerService& GetAbsolutePointerService() noexcept {
  static AbsolutePointerService service;
  return service;
}

void RefreshAndroidPhysicalKeyboardPresence(uint64_t timestamp_ns) noexcept {
#if REX_PLATFORM_ANDROID
  bool present = false;
  const bool succeeded = QueryAndroidPhysicalKeyboard(present);
  GetAbsolutePointerService().SetAndroidPhysicalKeyboardPresence(present, succeeded, timestamp_ns);
  static std::atomic<bool> failure_logged{false};
  if (!succeeded && !failure_logged.exchange(true)) {
    REXLOG_WARN(
        "Touch input: Android physical-keyboard inventory query failed; "
        "falling back to SDL keyboard inventory");
  }
#endif
}

bool TryDequeueAbsolutePointerEvent(AbsolutePointerEvent* out_event) noexcept {
  return GetAbsolutePointerService().TryDequeue(out_event);
}

bool TouchControlsActive() noexcept {
  return GetAbsolutePointerService().TouchControlsActive(GetConfiguredTouchControlsMode());
}

bool GetTouchPresentationState(TouchPresentationState* out_state) noexcept {
  return GetAbsolutePointerService().GetPresentationState(out_state);
}

}  // namespace rex::input
