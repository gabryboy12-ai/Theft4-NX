/**
 ******************************************************************************
 * @file        community_profile_policy_test.cpp
 * @brief       Tests for community title-profile write acknowledgements.
 ******************************************************************************
 */

#include <array>

#include <catch2/catch_test_macros.hpp>

#include "network/community_profile_policy.h"

TEST_CASE("title-profile store accepts only an exact echoed blob",
          "[live][profile]") {
  const std::array<uint8_t, 8> requested = {0, 0, 0, 1, 0xDE, 0xAD, 0xBE, 0xEF};
  auto changed = requested;
  changed.back() = 0;
  const std::array<uint8_t, 16> extended = {
      0, 0, 0, 1, 0xDE, 0xAD, 0xBE, 0xEF,
      0, 0, 0, 2, 0x12, 0x34, 0x56, 0x78,
  };

  CHECK(LibertyRecomp::Network::detail::IsAcknowledgedTitleProfileBlob(
      requested, requested));
  CHECK_FALSE(LibertyRecomp::Network::detail::IsAcknowledgedTitleProfileBlob(
      requested, changed));
  CHECK_FALSE(LibertyRecomp::Network::detail::IsAcknowledgedTitleProfileBlob(
      requested, extended));
}
