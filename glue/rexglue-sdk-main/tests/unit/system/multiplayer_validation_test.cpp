#include <array>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include <rex/system/xam/multiplayer_validation.h>

namespace xam = rex::system::xam;

TEST_CASE("multiplayer validation joins session XUID and peer identity once per stage",
          "[live][session][multiplayer][64-player][validation]") {
  xam::MultiplayerValidationRegistry registry;
  const std::array members = {
      xam::MultiplayerValidationMember{.xuid = 0x100000000000000FULL, .peer_id = 15},
      xam::MultiplayerValidationMember{.xuid = 0x1000000000000010ULL, .peer_id = 16},
      xam::MultiplayerValidationMember{.xuid = 0x100000000000001FULL, .peer_id = 31},
      xam::MultiplayerValidationMember{.xuid = 0x1000000000000020ULL, .peer_id = 32},
      xam::MultiplayerValidationMember{.xuid = 0x100000000000003FULL, .peer_id = 63},
  };
  registry.UpdateSession(0x1122334455667788ULL, members);

  for (const auto& member : members) {
    const auto first = registry.Record(xam::MultiplayerValidationStage::kParticipantAdd,
                                       member.peer_id, 0xA0000000U);
    REQUIRE(first);
    CHECK(first->session_id == 0x1122334455667788ULL);
    CHECK(first->xuid == member.xuid);
    CHECK(first->peer_id == member.peer_id);
    CHECK(first->guest_object == 0xA0000000U);
    CHECK_FALSE(registry.Record(xam::MultiplayerValidationStage::kParticipantAdd,
                                member.peer_id));
  }
}

TEST_CASE("multiplayer validation resets evidence when a peer slot is reused",
          "[live][session][multiplayer][64-player][validation]") {
  xam::MultiplayerValidationRegistry registry;
  const std::array first = {
      xam::MultiplayerValidationMember{.xuid = 0x1000000000000010ULL, .peer_id = 16}};
  registry.UpdateSession(0x1122334455667788ULL, first);
  REQUIRE(registry.Record(xam::MultiplayerValidationStage::kRemoval, 16));
  CHECK(registry.HasRecorded(xam::MultiplayerValidationStage::kRemoval, 16));

  const std::array replacement = {
      xam::MultiplayerValidationMember{.xuid = 0x2000000000000010ULL, .peer_id = 16}};
  registry.UpdateSession(0x1122334455667788ULL, replacement);
  CHECK_FALSE(registry.HasRecorded(xam::MultiplayerValidationStage::kRemoval, 16));
  const auto reused = registry.Record(xam::MultiplayerValidationStage::kRemoval, 16);
  REQUIRE(reused);
  CHECK(reused->xuid == replacement.front().xuid);

  registry.UpdateSession(0x8877665544332211ULL, replacement);
  CHECK_FALSE(registry.HasRecorded(xam::MultiplayerValidationStage::kRemoval, 16));
}
