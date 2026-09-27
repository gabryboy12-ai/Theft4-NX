#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <vector>

#include "network/community_stats_policy.h"

namespace {

using LibertyRecomp::Network::IsExpectedGlobalLeaderboardRank;
using LibertyRecomp::Network::IsConsistentStatWriteTarget;
using LibertyRecomp::Network::Gta4WireStatAttributeIds;

TEST_CASE("community leaderboard requires the requested global rank window") {
  REQUIRE(IsExpectedGlobalLeaderboardRank(0, 0, 1));
  REQUIRE(IsExpectedGlobalLeaderboardRank(0, 1, 2));
  REQUIRE(IsExpectedGlobalLeaderboardRank(40, 0, 41));
  REQUIRE(IsExpectedGlobalLeaderboardRank(40, 2, 43));

  REQUIRE_FALSE(IsExpectedGlobalLeaderboardRank(40, 0, 40));
  REQUIRE_FALSE(IsExpectedGlobalLeaderboardRank(40, 0, 42));
  REQUIRE_FALSE(IsExpectedGlobalLeaderboardRank(40, 2, 44));
}

TEST_CASE("community leaderboard rejects ranks beyond the wire width") {
  const uint32_t maximum = std::numeric_limits<uint32_t>::max();
  REQUIRE(IsExpectedGlobalLeaderboardRank(maximum - 1, 0, maximum));
  REQUIRE_FALSE(IsExpectedGlobalLeaderboardRank(maximum, 0, 1));
}

TEST_CASE("community stat writes preserve the title supplied target XUID") {
  constexpr uint64_t local_xuid = UINT64_C(0x100);
  constexpr uint64_t remote_xuid = UINT64_C(0x200);

  REQUIRE(IsConsistentStatWriteTarget(local_xuid, local_xuid));
  REQUIRE(IsConsistentStatWriteTarget(remote_xuid, remote_xuid));
  REQUIRE_FALSE(IsConsistentStatWriteTarget(0, 0));
  REQUIRE_FALSE(IsConsistentStatWriteTarget(remote_xuid, local_xuid));
}

TEST_CASE("community stats omit only the intrinsic rating attribute from wire columns") {
  const std::vector<uint32_t> requested = {UINT32_C(0xFFFE), UINT32_C(5),
                                           UINT32_C(1), UINT32_C(6)};
  CHECK(Gta4WireStatAttributeIds(requested) ==
        std::vector<uint32_t>{UINT32_C(5), UINT32_C(1), UINT32_C(6)});

  const std::vector<uint32_t> rating_only = {UINT32_C(0xFFFE)};
  CHECK(Gta4WireStatAttributeIds(rating_only).empty());
}

}  // namespace
