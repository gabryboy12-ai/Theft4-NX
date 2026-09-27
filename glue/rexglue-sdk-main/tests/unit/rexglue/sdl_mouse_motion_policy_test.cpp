#include <catch2/catch_test_macros.hpp>

#include "ui/sdl_mouse_motion_policy.h"

namespace {

using rex::ui::ClassifySdlMouseMotion;
using rex::ui::kSdlDefaultMouseId;
using rex::ui::kSdlGlobalMouseId;
using rex::ui::SdlMouseMotionRoute;

TEST_CASE("SDL default mouse motion is suppressed only for the AppKit duplicate",
          "[sdl_mouse_motion_policy]") {
  CHECK(ClassifySdlMouseMotion(kSdlDefaultMouseId, true) ==
        SdlMouseMotionRoute::kSuppressAcceleratedDuplicate);
  CHECK(ClassifySdlMouseMotion(kSdlDefaultMouseId, false) == SdlMouseMotionRoute::kGeneric);
}

TEST_CASE("SDL global mouse motion is not classified as a raw device",
          "[sdl_mouse_motion_policy]") {
  CHECK(ClassifySdlMouseMotion(kSdlGlobalMouseId, true) == SdlMouseMotionRoute::kGeneric);
  CHECK(ClassifySdlMouseMotion(kSdlGlobalMouseId, false) == SdlMouseMotionRoute::kGeneric);
}

TEST_CASE("SDL synthetic pointer motion remains generic", "[sdl_mouse_motion_policy]") {
  CHECK(ClassifySdlMouseMotion(SDL_TOUCH_MOUSEID, true) == SdlMouseMotionRoute::kGeneric);
  CHECK(ClassifySdlMouseMotion(SDL_PEN_MOUSEID, true) == SdlMouseMotionRoute::kGeneric);
}

TEST_CASE("SDL device mouse motion remains raw with an AppKit monitor",
          "[sdl_mouse_motion_policy]") {
  constexpr SDL_MouseID kDeviceMouseId = 2;
  CHECK(ClassifySdlMouseMotion(kDeviceMouseId, true) == SdlMouseMotionRoute::kRawMouse);
  CHECK(ClassifySdlMouseMotion(kDeviceMouseId, false) == SdlMouseMotionRoute::kRawMouse);
}

}  // namespace
