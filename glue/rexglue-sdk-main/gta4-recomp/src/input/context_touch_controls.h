#pragma once

#include <array>
#include <cstdint>

#include "input/context_touch_layout.h"

namespace gta4::input {

struct ContextTouchOverlaySnapshot {
  ContextTouchLayout layout{};
  std::array<uint8_t, ContextTouchLayout::kMaximumControls> active{};
  float movement_x = 0.0f;
  float movement_y = 0.0f;
  bool visible = false;
};

void InitializeContextTouchControls() noexcept;
void ShutdownContextTouchControls() noexcept;
ContextTouchOverlaySnapshot GetContextTouchOverlaySnapshot() noexcept;

void ObserveTouchScriptQuery(TouchScriptQueryKind kind, uint32_t action, uint64_t epoch) noexcept;
void ObserveTouchParachuteState(uint32_t state, uint64_t epoch) noexcept;
uint32_t GetTouchScriptQueryValue(TouchScriptQueryKind kind, uint32_t action,
                                  uint64_t epoch) noexcept;
bool GetTouchScriptAnalogueSticks(uint64_t epoch, int32_t* horizontal, int32_t* vertical) noexcept;
bool MergeTouchScriptQueryResult(uint8_t* base, uint32_t call_context, TouchScriptQueryKind kind,
                                 uint64_t epoch) noexcept;
bool MergeTouchScriptAnalogueStickResults(uint8_t* base, uint32_t call_context,
                                          uint64_t epoch) noexcept;

}  // namespace gta4::input
