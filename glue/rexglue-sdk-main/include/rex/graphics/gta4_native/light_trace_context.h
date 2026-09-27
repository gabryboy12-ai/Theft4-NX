#ifndef REX_GRAPHICS_GTA4_NATIVE_LIGHT_TRACE_CONTEXT_H_
#define REX_GRAPHICS_GTA4_NATIVE_LIGHT_TRACE_CONTEXT_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

#include <rex/graphics/gta4_native/lighting_semantics.h>

namespace rex::graphics::gta4_native {

// Rendering semantics are always maintained. Environment switches below only
// govern diagnostic output and must never determine state capture or replay.
inline thread_local LightingContext g_native_lighting_context{};
inline thread_local std::vector<LightingContext> g_native_lighting_selector_stack;
inline thread_local std::size_t g_native_lighting_selector_floor = 0;

inline uint64_t AcquireNativeLightingOccurrence() {
  static std::atomic<uint64_t> next{1};
  return next.fetch_add(1, std::memory_order_relaxed);
}

inline const LightingContext& GetNativeLightingContext() {
  return g_native_lighting_context;
}

inline void BeginNativeLightingSelector(const LightingContext& selected) {
  g_native_lighting_selector_stack.push_back(g_native_lighting_context);
  g_native_lighting_context = selected;
}

inline void EndNativeLightingSelector() {
  if (g_native_lighting_selector_stack.size() <= g_native_lighting_selector_floor) {
    return;
  }
  g_native_lighting_context = g_native_lighting_selector_stack.back();
  g_native_lighting_selector_stack.pop_back();
}

class ScopedNativeLightingContext {
 public:
  explicit ScopedNativeLightingContext(const LightingContext& context)
      : previous_(g_native_lighting_context),
        selector_depth_(g_native_lighting_selector_stack.size()),
        previous_floor_(g_native_lighting_selector_floor) {
    g_native_lighting_context = context;
    g_native_lighting_selector_floor = selector_depth_;
  }
  ScopedNativeLightingContext(const ScopedNativeLightingContext&) = delete;
  ScopedNativeLightingContext& operator=(const ScopedNativeLightingContext&) = delete;
  ~ScopedNativeLightingContext() {
    // An executor owns its nested selector scopes, including paths with an
    // optional early return. It cannot close a selector belonging to its caller.
    while (g_native_lighting_selector_stack.size() > selector_depth_) {
      EndNativeLightingSelector();
    }
    g_native_lighting_context = previous_;
    g_native_lighting_selector_floor = previous_floor_;
  }

 private:
  LightingContext previous_{};
  std::size_t selector_depth_ = 0;
  std::size_t previous_floor_ = 0;
};

inline thread_local uint32_t g_native_light_trace_context = 0;
inline thread_local uint32_t g_native_light_trace_technique = 0xFFFFFFFFu;
inline thread_local uint32_t g_native_light_trace_mode = 0;

inline bool ReadNativeLightTraceSetting(const char* name) {
  const char* value = std::getenv(name);
  if (!value || !*value) {
    return false;
  }
  const std::string_view setting(value);
  return setting != "0" && setting != "false" && setting != "off";
}

inline bool IsNativeLightTraceEnabled() {
  static const bool enabled = ReadNativeLightTraceSetting("REX_GTA4_NATIVE_LIGHT_TRACE");
  return enabled;
}

inline bool IsNativeLightTraceDetailEnabled() {
  static const bool enabled = ReadNativeLightTraceSetting("REX_GTA4_NATIVE_LIGHT_TRACE_DETAIL");
  return IsNativeLightTraceEnabled() && enabled;
}

inline bool IsNativeLightTraceVariantEnabled() {
  static const bool enabled = ReadNativeLightTraceSetting("REX_GTA4_NATIVE_LIGHT_TRACE_VARIANTS");
  return IsNativeLightTraceEnabled() && enabled;
}

inline bool IsNativeLightLoopTraceEnabled() {
  static const bool enabled = ReadNativeLightTraceSetting("REX_GTA4_NATIVE_LIGHT_TRACE_LOOPS");
  return IsNativeLightTraceEnabled() && enabled;
}

inline constexpr uint64_t kNativeLightTraceRecordLimit = 65536;
inline std::atomic<uint64_t> g_native_light_trace_dropped_records{0};

inline uint64_t GetNativeLightTraceDroppedRecords() {
  return g_native_light_trace_dropped_records.load(std::memory_order_relaxed);
}

inline uint64_t AcquireNativeLightTraceRecord() {
  if (!IsNativeLightTraceDetailEnabled()) {
    return 0;
  }
  static std::atomic<uint64_t> next_record{1};
  const uint64_t record = next_record.fetch_add(1, std::memory_order_relaxed);
  if (record <= kNativeLightTraceRecordLimit) {
    return record;
  }
  if (g_native_light_trace_dropped_records.fetch_add(1, std::memory_order_relaxed) == 0) {
    std::fprintf(stderr,
                 "gta4-native-light-trace: point=record-limit limit=%llu dropped=1 "
                 "capture-complete=0\n",
                 static_cast<unsigned long long>(kNativeLightTraceRecordLimit));
  }
  return 0;
}

inline uint32_t GetNativeLightTraceContext() {
  return g_native_light_trace_context;
}

inline void SetNativeLightTraceContext(uint32_t light_id) {
  g_native_light_trace_context = light_id;
}

inline uint32_t GetNativeLightTraceTechnique() {
  return g_native_light_trace_technique;
}

inline void SetNativeLightTraceTechnique(uint32_t technique) {
  g_native_light_trace_technique = technique;
}

inline uint32_t GetNativeLightTraceMode() {
  return g_native_light_trace_mode;
}

inline void SetNativeLightTraceMode(uint32_t mode) {
  g_native_light_trace_mode = mode;
}

class ScopedNativeLightTraceContext {
 public:
  explicit ScopedNativeLightTraceContext(uint32_t light_id)
      : previous_(GetNativeLightTraceContext()) {
    SetNativeLightTraceContext(light_id);
  }

  ScopedNativeLightTraceContext(const ScopedNativeLightTraceContext&) = delete;
  ScopedNativeLightTraceContext& operator=(const ScopedNativeLightTraceContext&) = delete;

  ~ScopedNativeLightTraceContext() { SetNativeLightTraceContext(previous_); }

 private:
  uint32_t previous_;
};

}  // namespace rex::graphics::gta4_native

#endif  // REX_GRAPHICS_GTA4_NATIVE_LIGHT_TRACE_CONTEXT_H_
