/**
 ******************************************************************************
 * @file        community_profile_policy.h
 * @brief       Validation policy for acknowledged title-profile writes.
 ******************************************************************************
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <span>

namespace LibertyRecomp::Network::detail {

inline bool IsAcknowledgedTitleProfileBlob(std::span<const uint8_t> requested,
                                           std::span<const uint8_t> acknowledged) {
  return std::ranges::equal(requested, acknowledged);
}

}  // namespace LibertyRecomp::Network::detail
