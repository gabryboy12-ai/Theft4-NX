#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <rex/input/mnk/encoded_action.h>

#include "gta4_input_action_routing.h"

namespace gta4::input {

using rex::ui::VirtualKey;

TEST_CASE("Retail pause ownership follows native state through exit transitions",
          "[input][gta4][keyboard]") {
  CHECK_FALSE(RetailPauseMenuActive(false, 0));
  CHECK(RetailPauseMenuActive(true, 0));
  CHECK(RetailPauseMenuActive(true, 1));
  CHECK_FALSE(RetailPauseMenuActive(true, 2));
  CHECK_FALSE(RetailPauseMenuActive(true, 6));
  KeyboardInterfaceContext menu{.frontend_active = RetailPauseMenuActive(true, 0)};
  CHECK(ShouldInjectKeyboardInterfaceAction(65, VirtualKey::kW, menu));
  CHECK(ShouldInjectKeyboardInterfaceAction(64, VirtualKey::kS, menu));
  CHECK(ShouldInjectKeyboardInterfaceAction(66, VirtualKey::kA, menu));
  CHECK(ShouldInjectKeyboardInterfaceAction(67, VirtualKey::kD, menu));
  KeyboardEscapeState escape;
  CHECK(escape.Update(true, true, menu.frontend_active, false) ==
        KeyboardEscapeRoute::kFrontendBack);
}

TEST_CASE("A persistent phone HUD widget cannot steal Escape without a live phone",
          "[input][gta4][keyboard]") {
  CHECK_FALSE(PhoneOwnsKeyboard(false, false, true));
  CHECK_FALSE(PhoneOwnsKeyboard(false, true, true));
  CHECK_FALSE(PhoneOwnsKeyboard(true, false, false));
  CHECK_FALSE(PhoneOwnsKeyboard(true, true, true));
  CHECK(PhoneOwnsKeyboard(true, false, true));

  KeyboardEscapeState escape;
  CHECK(escape.Update(true, true, false, PhoneOwnsKeyboard(false, false, true)) ==
        KeyboardEscapeRoute::kPause);
  escape.Update(false, false, false, false);
  CHECK(escape.Update(true, true, false, PhoneOwnsKeyboard(true, false, true)) ==
        KeyboardEscapeRoute::kPhoneBack);
}

TEST_CASE("Menu wheel belongs to frontend scrolling and leaves map zoom separate",
          "[input][gta4][keyboard]") {
  CHECK(KeyboardFrontendScroll(1, true, false) < 0);
  CHECK(KeyboardFrontendScroll(-1, true, false) > 0);
  CHECK(KeyboardFrontendScroll(0, true, false) == 0);
  CHECK(KeyboardFrontendScroll(1, true, true) == 0);
  CHECK(KeyboardFrontendScroll(-1, true, true) == 0);
  CHECK(KeyboardFrontendScroll(1, false, false) == 0);
}

TEST_CASE("Global controller keys never duplicate buttons through native injection",
          "[input][gta4][keyboard]") {
  const auto key = GENERATE(VirtualKey::kUp, VirtualKey::kDown,
                            VirtualKey::kLeft, VirtualKey::kRight,
                            VirtualKey::kReturn, VirtualKey::kBack,
                            VirtualKey::kDelete, VirtualKey::kF);
  const bool frontend = GENERATE(false, true);
  const bool phone = GENERATE(false, true);
  const KeyboardInterfaceContext context{.frontend_active = frontend,
                                          .phone_visible = phone};
  for (const uint32_t action : {21u, 22u, 64u, 65u, 66u, 67u, 76u, 77u, 78u,
                                80u, 82u}) {
    CHECK_FALSE(ShouldInjectKeyboardInterfaceAction(action, key, context));
  }
}

TEST_CASE("Flight controller inputs do not duplicate native interface actions",
          "[input][gta4][keyboard][helicopter]") {
  const KeyboardInterfaceContext flight{.phone_visible = true,
                                          .helicopter_controls = true};
  for (const auto key : {VirtualKey::kLButton, VirtualKey::kShift, VirtualKey::kW,
                         VirtualKey::kS, VirtualKey::kA, VirtualKey::kD,
                         VirtualKey::kNumpad2, VirtualKey::kNumpad4,
                         VirtualKey::kNumpad6, VirtualKey::kNumpad8}) {
    for (const uint32_t action : {21u, 22u, 64u, 65u, 66u, 67u, 76u, 77u, 78u,
                                  79u, 80u, 81u, 82u, 83u, 84u}) {
      CHECK_FALSE(ShouldInjectKeyboardInterfaceAction(action, key, flight));
    }
  }
}

TEST_CASE("Frontend fallback retires keyboard state without losing controller input",
          "[input][gta4][keyboard][replay]") {
  using rex::input::mnk::EncodeActionMagnitude;
  using rex::input::mnk::MergeActionMagnitude;
  const auto polarity = GENERATE(uint8_t{0}, uint8_t{255});
  {
    CAPTURE(polarity);
    const uint8_t idle = EncodeActionMagnitude(polarity, 0);
    const uint8_t down = EncodeActionMagnitude(polarity, 255);
    KeyboardActionHistory history;
    KeyboardActionBytes bytes{idle, idle};
    const auto poll = [&](uint64_t epoch, bool replayed, bool keyboard_down) {
      bytes = history.Prepare(epoch, replayed, bytes);
      if (keyboard_down) {
        bytes.current = MergeActionMagnitude(polarity, bytes.current, 255);
      }
      history.Commit(bytes);
    };

    SECTION("A control without any replay receives one complete press and release") {
      poll(1, false, true);
      CHECK(bytes == KeyboardActionBytes{down, idle});
      poll(1, false, true);
      CHECK(bytes == KeyboardActionBytes{down, idle});
      poll(2, false, true);
      CHECK(bytes == KeyboardActionBytes{down, down});
      poll(3, false, false);
      CHECK(bytes == KeyboardActionBytes{idle, down});
      poll(3, false, false);
      CHECK(bytes == KeyboardActionBytes{idle, down});
      poll(4, false, false);
      CHECK(bytes == KeyboardActionBytes{idle, idle});
      poll(5, false, true);
      CHECK(bytes == KeyboardActionBytes{down, idle});
    }

    SECTION("Fallback completes an input whose original press was replayed") {
      poll(1, true, true);
      poll(2, false, false);
      CHECK(bytes == KeyboardActionBytes{idle, down});
      poll(3, false, false);
      CHECK(bytes == KeyboardActionBytes{idle, idle});
    }

    SECTION("A fresh replay remains authoritative on release") {
      poll(1, false, true);
      bytes = {idle, down};
      poll(2, true, false);
      CHECK(bytes == KeyboardActionBytes{idle, down});
      poll(2, false, false);
      CHECK(bytes == KeyboardActionBytes{idle, down});
    }

    SECTION("Releasing the keyboard preserves a held real controller button") {
      bytes = {down, idle};
      poll(1, true, true);
      poll(2, false, false);
      CHECK(bytes == KeyboardActionBytes{down, down});
      bytes = {idle, down};
      poll(3, true, false);
      CHECK(bytes == KeyboardActionBytes{idle, down});
    }

    SECTION("An intervening guest write is preserved even within one epoch") {
      poll(1, false, true);
      bytes = {down, down};
      poll(1, false, true);
      poll(2, false, false);
      CHECK(bytes == KeyboardActionBytes{down, down});
    }
  }
}

TEST_CASE("Escape retains its destination through interface transitions",
          "[input][gta4][keyboard]") {
  KeyboardEscapeState escape;

  SECTION("Gameplay Escape opens pause without becoming Back on the same press") {
    CHECK(escape.Update(true, true, false, false) == KeyboardEscapeRoute::kPause);
    CHECK(escape.Update(true, false, true, false) == KeyboardEscapeRoute::kPause);
    CHECK(escape.Update(false, false, true, false) == KeyboardEscapeRoute::kPause);
    CHECK(escape.Update(true, true, true, false) == KeyboardEscapeRoute::kFrontendBack);
  }

  SECTION("Closing the phone cannot open pause while Escape is held") {
    CHECK(escape.Update(true, true, false, true) == KeyboardEscapeRoute::kPhoneBack);
    CHECK(escape.Update(true, false, false, false) == KeyboardEscapeRoute::kPhoneBack);
    CHECK(escape.Update(false, false, false, false) == KeyboardEscapeRoute::kPhoneBack);
    CHECK(escape.Update(true, true, false, false) == KeyboardEscapeRoute::kPause);
  }

  SECTION("A new physical press between polls selects the current interface") {
    escape.Update(true, true, false, true);
    CHECK(escape.Update(true, true, true, true) == KeyboardEscapeRoute::kFrontendBack);
  }
}

TEST_CASE("Frontend and phone keys follow the PC control contexts",
          "[input][gta4][keyboard]") {
  KeyboardInterfaceContext context;
  const auto routes = [&](uint32_t action, VirtualKey key) {
    return ShouldInjectKeyboardInterfaceAction(action, key, context);
  };

  SECTION("Gameplay arrows use the controller bridge and Escape pauses") {
    CHECK_FALSE(routes(21, VirtualKey::kUp));
    CHECK(routes(76, VirtualKey::kEscape));
    CHECK_FALSE(routes(78, VirtualKey::kEscape));
    CHECK_FALSE(routes(65, VirtualKey::kUp));
    CHECK_FALSE(routes(77, VirtualKey::kReturn));
  }

  SECTION("A visible phone keeps arrows on the controller bridge") {
    context.phone_visible = true;
    context.escape_route = KeyboardEscapeRoute::kPhoneBack;
    CHECK_FALSE(routes(21, VirtualKey::kUp));
    CHECK_FALSE(routes(65, VirtualKey::kUp));
    CHECK_FALSE(routes(64, VirtualKey::kDown));
    CHECK_FALSE(routes(66, VirtualKey::kLeft));
    CHECK_FALSE(routes(67, VirtualKey::kRight));
    CHECK_FALSE(routes(65, VirtualKey::kW));
    CHECK_FALSE(routes(64, VirtualKey::kS));
    CHECK_FALSE(routes(66, VirtualKey::kA));
    CHECK_FALSE(routes(67, VirtualKey::kD));
    CHECK(routes(22, VirtualKey::kEscape));
    CHECK_FALSE(routes(22, VirtualKey::kBack));
    CHECK(routes(78, VirtualKey::kEscape));
    CHECK_FALSE(routes(77, VirtualKey::kReturn));
    CHECK_FALSE(routes(76, VirtualKey::kEscape));
    CHECK_FALSE(routes(77, VirtualKey::kSpace));
    CHECK_FALSE(routes(77, VirtualKey::kLButton));
  }

  SECTION("A menu takes priority over a retained phone render object") {
    context.frontend_active = true;
    context.phone_visible = true;
    context.escape_route = KeyboardEscapeRoute::kFrontendBack;
    CHECK_FALSE(routes(65, VirtualKey::kUp));
    CHECK(routes(65, VirtualKey::kW));
    CHECK(routes(64, VirtualKey::kS));
    CHECK(routes(66, VirtualKey::kA));
    CHECK(routes(67, VirtualKey::kD));
    CHECK(routes(78, VirtualKey::kEscape));
    CHECK(routes(78, VirtualKey::kXButton1));
    CHECK(routes(80, VirtualKey::kSpace));
    CHECK_FALSE(routes(77, VirtualKey::kReturn));
    CHECK(routes(77, VirtualKey::kLButton));
    CHECK_FALSE(routes(77, VirtualKey::kSpace));
    CHECK_FALSE(routes(76, VirtualKey::kEscape));
    CHECK_FALSE(routes(21, VirtualKey::kUp));
    CHECK_FALSE(routes(22, VirtualKey::kEscape));
  }

  SECTION("Dragging the map does not accept menu entries") {
    context.frontend_active = true;
    context.map_active = true;
    CHECK_FALSE(routes(77, VirtualKey::kLButton));
    CHECK_FALSE(routes(77, VirtualKey::kReturn));
  }
}

TEST_CASE("Phone actions use GTA's active gameplay control", "[input][gta4][replay]") {
  const KeyboardActionRoute take_out = ClassifyKeyboardActionRoute(21);
  const KeyboardActionRoute put_away = ClassifyKeyboardActionRoute(22);

  CHECK(take_out == KeyboardActionRoute::kPhoneActiveGameplayControl);
  CHECK(put_away == KeyboardActionRoute::kPhoneActiveGameplayControl);
  CHECK(IsContextAction(take_out));
  CHECK(IsContextAction(put_away));
  CHECK(UsesActiveGameplayControl(take_out));
  CHECK(UsesActiveGameplayControl(put_away));
  CHECK_FALSE(NeedsFrontendConsumerFallback(take_out));
  CHECK_FALSE(NeedsFrontendConsumerFallback(put_away));
}

TEST_CASE("Frontend actions survive replay and retain a consumer fallback",
          "[input][gta4][replay]") {
  const KeyboardActionRoute pause = ClassifyKeyboardActionRoute(76);
  const KeyboardActionRoute accept = ClassifyKeyboardActionRoute(77);
  const KeyboardActionRoute cancel = ClassifyKeyboardActionRoute(78);

  CHECK(pause == KeyboardActionRoute::kFrontendReplayWithConsumerFallback);
  CHECK(accept == KeyboardActionRoute::kFrontendReplayWithConsumerFallback);
  CHECK(cancel == KeyboardActionRoute::kFrontendReplayWithConsumerFallback);
  CHECK(IsContextAction(pause));
  CHECK_FALSE(UsesActiveGameplayControl(pause));
  CHECK(NeedsFrontendConsumerFallback(pause));
}

TEST_CASE("Gameplay actions use only the normal replay path", "[input][gta4][replay]") {
  const KeyboardActionRoute accelerate = ClassifyKeyboardActionRoute(40);

  CHECK(accelerate == KeyboardActionRoute::kGameplayReplay);
  CHECK_FALSE(IsContextAction(accelerate));
  CHECK_FALSE(UsesActiveGameplayControl(accelerate));
  CHECK_FALSE(NeedsFrontendConsumerFallback(accelerate));
}

}  // namespace gta4::input
