#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace LibertyRecomp::Network {

inline constexpr uint32_t kGta4IntrinsicLeaderboardRatingAttribute =
    UINT32_C(0xFFFE);

inline std::vector<uint32_t> Gta4WireStatAttributeIds(
    std::span<const uint32_t> requested_attribute_ids) {
  std::vector<uint32_t> wire_attribute_ids;
  wire_attribute_ids.reserve(requested_attribute_ids.size());
  for (uint32_t attribute_id : requested_attribute_ids) {
    if (attribute_id != kGta4IntrinsicLeaderboardRatingAttribute) {
      wire_attribute_ids.push_back(attribute_id);
    }
  }
  return wire_attribute_ids;
}

inline bool IsExpectedGlobalLeaderboardRank(uint32_t offset,
                                            size_t row_index,
                                            uint32_t rank) {
  const uint64_t expected = static_cast<uint64_t>(offset) +
                            static_cast<uint64_t>(row_index) + 1;
  return expected <= std::numeric_limits<uint32_t>::max() &&
         rank == static_cast<uint32_t>(expected);
}

inline bool IsConsistentStatWriteTarget(uint64_t target_xuid,
                                        uint64_t row_xuid) {
  return target_xuid != 0 && row_xuid == target_xuid;
}

}  // namespace LibertyRecomp::Network
