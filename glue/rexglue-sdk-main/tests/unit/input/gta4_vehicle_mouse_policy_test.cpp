#include <catch2/catch_test_macros.hpp>

#include "gta4_vehicle_mouse_policy.h"

namespace gta4::input {

TEST_CASE("Helicopter mouse flies the aircraft until RMB owns free-look",
          "[input][gta4][helicopter]") {
  const auto flight = RouteVehicleMouse(true, false, -80, 120);
  CHECK(flight.helicopter_yaw == -80);
  CHECK(flight.helicopter_pitch == 120);
  CHECK(flight.camera_x == 0);
  CHECK(flight.camera_y == 0);

  const auto camera = RouteVehicleMouse(true, true, -80, 120);
  CHECK(camera.camera_x == -80);
  CHECK(camera.camera_y == 120);
  CHECK(camera.helicopter_yaw == 0);
  CHECK(camera.helicopter_pitch == 0);
}

TEST_CASE("On-foot, car and passenger mouse input cannot fly helicopters",
          "[input][gta4][helicopter]") {
  for (bool free_look : {false, true}) {
    const auto mouse = RouteVehicleMouse(false, free_look, 100, -120);
    CHECK(mouse.camera_x == 100);
    CHECK(mouse.camera_y == -120);
    CHECK(mouse.helicopter_yaw == 0);
    CHECK(mouse.helicopter_pitch == 0);
  }
}

TEST_CASE("Mouse yaw produces opposed button magnitudes from neutral input",
          "[input][gta4][helicopter]") {
  const auto left = MergeVehicleYawButtons(0, 0, -80);
  CHECK(left.left == 80);
  CHECK(left.right == 0);
  const auto right = MergeVehicleYawButtons(0, 0, 120);
  CHECK(right.left == 0);
  CHECK(right.right == 120);
  const auto clamped = MergeVehicleYawButtons(0, 0, -999);
  CHECK(clamped.left == 255);
  CHECK(clamped.right == 0);
}

TEST_CASE("Mouse yaw preserves stronger numpad and controller input",
          "[input][gta4][helicopter]") {
  const auto right = MergeVehicleYawButtons(0, 255, -80);
  CHECK(right.left == 0);
  CHECK(right.right == 255);
  const auto left = MergeVehicleYawButtons(255, 0, 120);
  CHECK(left.left == 255);
  CHECK(left.right == 0);
  const auto balanced = MergeVehicleYawButtons(255, 255, 80);
  CHECK(balanced.left == 0);
  CHECK(balanced.right == 80);
}

}  // namespace gta4::input
