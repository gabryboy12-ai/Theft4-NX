/**
 * @file        ui/sdl_mouse_motion_policy.h
 * @brief       SDL mouse motion routing for the macOS accelerated pointer bridge.
 */
#pragma once

#include <SDL3/SDL.h>

namespace rex::ui {

// These IDs are private SDL constants in src/events/SDL_mouse_c.h. Keep the
// policy beside the SDL window implementation so it can follow the vendored
// SDL version without exposing private SDL headers through the public API.
constexpr SDL_MouseID kSdlGlobalMouseId = 0;
constexpr SDL_MouseID kSdlDefaultMouseId = 1;

enum class SdlMouseMotionRoute {
  kGeneric,
  kRawMouse,
  kSuppressAcceleratedDuplicate,
};

constexpr SdlMouseMotionRoute ClassifySdlMouseMotion(SDL_MouseID mouse_id,
                                                     bool accelerated_pointer_monitor_installed) {
  if (mouse_id == kSdlGlobalMouseId || mouse_id == SDL_TOUCH_MOUSEID ||
      mouse_id == SDL_PEN_MOUSEID) {
    return SdlMouseMotionRoute::kGeneric;
  }

  // Cocoa emits its NSEvent-backed pointer stream with SDL_DEFAULT_MOUSE_ID.
  // The AppKit monitor sees the same event first and supplies its accelerated
  // delta, so forwarding SDL's copy would count trackpad motion twice.
  if (mouse_id == kSdlDefaultMouseId) {
    return accelerated_pointer_monitor_installed
               ? SdlMouseMotionRoute::kSuppressAcceleratedDuplicate
               : SdlMouseMotionRoute::kGeneric;
  }

  // Vendored SDL assigns the native GCMouse object address as its device ID.
  return SdlMouseMotionRoute::kRawMouse;
}

}  // namespace rex::ui
