#include <catch2/catch_test_macros.hpp>

#include "../../../gta4-recomp/src/gta4_map_pan_policy.h"
#include "../../../gta4-recomp/src/gta4_touch_policy.h"

#include <array>

namespace touch = gta4::touch;

TEST_CASE("GTA IV direct-manipulation map pan preserves pointer direction",
          "[system][gta4][touch][map]") {
  const gta4::input::MapPanAction left = gta4::input::DirectManipulationMapPan(-255, 0);
  CHECK(left.horizontal < 0);
  CHECK(left.vertical == 0);

  const gta4::input::MapPanAction right = gta4::input::DirectManipulationMapPan(255, 0);
  CHECK(right.horizontal > 0);
  CHECK(right.vertical == 0);

  const gta4::input::MapPanAction up = gta4::input::DirectManipulationMapPan(0, -255);
  CHECK(up.horizontal == 0);
  CHECK(up.vertical < 0);

  const gta4::input::MapPanAction down = gta4::input::DirectManipulationMapPan(0, 255);
  CHECK(down.horizontal == 0);
  CHECK(down.vertical > 0);
}

TEST_CASE("GTA IV frontend touch only hits selectable published rows",
          "[system][gta4][touch]") {
  const std::array rows = {
      touch::FrontendRow{.bounds = {.left = 0.1f, .top = 0.2f, .right = 0.8f, .bottom = 0.3f},
                         .channel = 0,
                         .row = 4,
                         .selectable = true},
      touch::FrontendRow{.bounds = {.left = 0.1f, .top = 0.3f, .right = 0.8f, .bottom = 0.4f},
                         .channel = 0,
                         .row = 5,
                         .selectable = false},
  };

  const auto selected = touch::HitTestFrontendRow(rows, {.x = 0.5f, .y = 0.25f});
  REQUIRE(selected);
  CHECK(selected->row == 4);
  CHECK_FALSE(touch::HitTestFrontendRow(rows, {.x = 0.5f, .y = 0.35f}));
  CHECK_FALSE(touch::HitTestFrontendRow(rows, {.x = 0.9f, .y = 0.25f}));
}

TEST_CASE("GTA IV frontend touch transaction rejects stale pointer and layout generations",
          "[system][gta4][touch]") {
  const touch::FrontendRow row{
      .bounds = {.left = 0.1f, .top = 0.2f, .right = 0.8f, .bottom = 0.3f},
      .channel = 0,
      .row = 7,
      .selectable = true,
  };
  touch::FrontendTransaction transaction;
  REQUIRE(transaction.Begin(11, 3, 9, row));
  CHECK_FALSE(transaction.End(11, 4, 9, row));
  CHECK_FALSE(transaction.active());

  REQUIRE(transaction.Begin(12, 4, 9, row));
  CHECK_FALSE(transaction.End(12, 4, 10, row));
  CHECK_FALSE(transaction.active());

  REQUIRE(transaction.Begin(13, 4, 10, row));
  CHECK(transaction.End(13, 4, 10, row));
}

TEST_CASE("GTA IV map touch distinguishes a waypoint tap from a drag",
          "[system][gta4][touch]") {
  touch::MapGesture gesture;
  CHECK(gesture.Down(1, 6, {.x = 200.0f, .y = 300.0f}, 1280.0f, 720.0f).consumed);
  const touch::MapGestureOutput tap =
      gesture.Up(1, 6, {.x = 205.0f, .y = 304.0f});
  CHECK(tap.consumed);
  CHECK(tap.waypoint);

  CHECK(gesture.Down(2, 6, {.x = 200.0f, .y = 300.0f}, 1280.0f, 720.0f).consumed);
  const touch::MapGestureOutput drag =
      gesture.Move(2, 6, {.x = 260.0f, .y = 330.0f});
  CHECK(drag.consumed);
  CHECK(drag.pan_x == 60.0f);
  CHECK(drag.pan_y == 30.0f);
  CHECK_FALSE(gesture.Up(2, 6, {.x = 260.0f, .y = 330.0f}).waypoint);
}

TEST_CASE("GTA IV normalized minimap taps use normalized slop and reject stale geometry",
          "[system][gta4][touch]") {
  const touch::Rect radar{
      .left = 0.1f, .top = 0.1f, .right = 0.3f, .bottom = 0.3f};
  touch::TapTransaction transaction;
  REQUIRE(transaction.Begin(4, 2, 7, {.x = 0.15f, .y = 0.2f}, 1.0f, 1.0f,
                            radar));
  CHECK(transaction.Move(4, 2, 7, {.x = 0.16f, .y = 0.2f}));
  CHECK(transaction.End(4, 2, 7, {.x = 0.16f, .y = 0.2f}));

  REQUIRE(transaction.Begin(5, 2, 7, {.x = 0.15f, .y = 0.2f}, 1.0f, 1.0f,
                            radar));
  CHECK(transaction.Move(5, 2, 7, {.x = 0.19f, .y = 0.2f}));
  CHECK_FALSE(transaction.End(5, 2, 7, {.x = 0.19f, .y = 0.2f}));

  REQUIRE(transaction.Begin(6, 2, 7, {.x = 0.15f, .y = 0.2f}, 1.0f, 1.0f,
                            radar));
  CHECK_FALSE(transaction.End(6, 2, 8, {.x = 0.15f, .y = 0.2f}));
}

TEST_CASE("GTA IV map pinch emits discrete zoom and suppresses a trailing waypoint",
          "[system][gta4][touch]") {
  touch::MapGesture gesture;
  REQUIRE(gesture.Down(10, 8, {.x = 100.0f, .y = 100.0f}, 1280.0f, 720.0f).consumed);
  REQUIRE(gesture.Down(20, 8, {.x = 200.0f, .y = 100.0f}, 1280.0f, 720.0f).consumed);

  const touch::MapGestureOutput pinch =
      gesture.Move(20, 8, {.x = 260.0f, .y = 100.0f});
  CHECK(pinch.zoom_steps == 1);
  const touch::MapGestureOutput huge_pinch =
      gesture.Move(20, 8, {.x = 2.0e11f, .y = 100.0f});
  CHECK(huge_pinch.zoom_steps == 1);
  CHECK_FALSE(gesture.Up(20, 8, {.x = 260.0f, .y = 100.0f}).waypoint);
  CHECK_FALSE(gesture.Up(10, 8, {.x = 100.0f, .y = 100.0f}).waypoint);
}

TEST_CASE("GTA IV map touch cancels the full transaction on generation change",
          "[system][gta4][touch]") {
  touch::MapGesture gesture;
  REQUIRE(gesture.Down(30, 12, {.x = 300.0f, .y = 300.0f}, 1280.0f, 720.0f).consumed);
  const touch::MapGestureOutput stale =
      gesture.Move(30, 13, {.x = 320.0f, .y = 300.0f});
  CHECK(stale.cancelled);
  CHECK_FALSE(gesture.active());
  CHECK_FALSE(gesture.Up(30, 12, {.x = 320.0f, .y = 300.0f}).consumed);

  REQUIRE(gesture.Down(40, 14, {.x = 100.0f, .y = 100.0f}, 1280.0f, 720.0f).consumed);
  REQUIRE(gesture.Down(50, 14, {.x = 200.0f, .y = 100.0f}, 1280.0f, 720.0f).consumed);
  const touch::MapGestureOutput stale_cancel = gesture.Cancel(40, 15);
  CHECK(stale_cancel.cancelled);
  CHECK_FALSE(gesture.active());
}

TEST_CASE("GTA IV map touch scaling saturates without weakening stronger input",
          "[system][gta4][touch]") {
  CHECK(touch::ScaleMapPan(180.0f, 720.0f) == 255);
  CHECK(touch::ScaleMapPan(-180.0f, 720.0f) == -255);
  CHECK(touch::ScaleMapPan(90.0f, 720.0f) == 128);
  CHECK(touch::ScaleMapPan(128.0f, 1280.0f) == 102);
  CHECK(touch::ScaleMapPan(20.0f, 0.0f) == 0);
}
