/**
 ******************************************************************************
 * @file        xam_user_identity_policy_test.cpp
 * @brief       Tests for GTA IV's XUID-backed player metadata imports.
 ******************************************************************************
 */

#include <catch2/catch_test_macros.hpp>

#include "kernel/xam/xam_user_identity_policy.h"

namespace identity_policy = rex::kernel::xam::detail;

TEST_CASE("XUID membership metadata is restricted to the local identity",
          "[xam][user][identity]") {
  constexpr uint64_t local_xuid = 0xE000000000000123ULL;
  constexpr uint64_t remote_xuid = 0xE000000000000456ULL;

  CHECK(identity_policy::MembershipTierFromXuid(local_xuid, local_xuid) ==
        identity_policy::kGoldMembershipTier);
  CHECK(identity_policy::MembershipTierFromXuid(remote_xuid, local_xuid) ==
        identity_policy::kUnknownMembershipTier);
  CHECK(identity_policy::MembershipTierFromXuid(0, local_xuid) ==
        identity_policy::kUnknownMembershipTier);
}

TEST_CASE("XUID country metadata uses the configured Xbox wire byte",
          "[xam][user][identity]") {
  constexpr uint64_t local_xuid = 0xE000000000000123ULL;
  constexpr uint64_t remote_xuid = 0xE000000000000456ULL;

  CHECK(identity_policy::OnlineCountryFromXuid(local_xuid, local_xuid, 103) == 103);
  CHECK(identity_policy::OnlineCountryFromXuid(local_xuid, local_xuid, 359) == 103);
  CHECK(identity_policy::OnlineCountryFromXuid(remote_xuid, local_xuid, 103) ==
        identity_policy::kUnknownOnlineCountry);
  CHECK(identity_policy::OnlineCountryFromXuid(0, local_xuid, 103) ==
        identity_policy::kUnknownOnlineCountry);
}
