/**
 ******************************************************************************
 * @file        xam_user_identity_policy.h
 * @brief       Scalar XUID metadata policy used by GTA IV's player enumerator.
 ******************************************************************************
 */

#pragma once

#include <cstdint>

namespace rex::kernel::xam::detail {

constexpr uint32_t kUnknownMembershipTier = 0;
constexpr uint32_t kGoldMembershipTier = 6;
constexpr uint32_t kUnknownOnlineCountry = 0;

inline bool IsLocalXuid(uint64_t queried_xuid, uint64_t local_xuid) {
  return queried_xuid != 0 && local_xuid != 0 && queried_xuid == local_xuid;
}

inline uint32_t MembershipTierFromXuid(uint64_t queried_xuid, uint64_t local_xuid) {
  return IsLocalXuid(queried_xuid, local_xuid) ? kGoldMembershipTier
                                               : kUnknownMembershipTier;
}

inline uint32_t OnlineCountryFromXuid(uint64_t queried_xuid, uint64_t local_xuid,
                                      uint32_t configured_country) {
  return IsLocalXuid(queried_xuid, local_xuid)
             ? static_cast<uint8_t>(configured_country)
             : kUnknownOnlineCountry;
}

}  // namespace rex::kernel::xam::detail
