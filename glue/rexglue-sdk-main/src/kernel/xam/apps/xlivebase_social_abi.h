/**
 ******************************************************************************
 * @file        xlivebase_social_abi.h
 * @brief       Verified GTA IV XLiveBase invite and mute guest layouts.
 ******************************************************************************
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include <rex/memory/utils.h>
#include <rex/system/xam/xsession.h>

namespace rex::kernel::xam::apps::detail {

struct XLiveFriendInfo {
  std::array<uint8_t, 196> bytes{};
};
static_assert(sizeof(XLiveFriendInfo) == 196);

// Verified against generated sub_829EF1E8, sub_829E8A88, and sub_829E8A98.
// The reader rejects the top two state bits, tests bit zero for online state,
// and compares the title ID at byte 36 with the running title.
inline constexpr size_t kFriendXuidOffset = 0;
inline constexpr size_t kFriendGamertagOffset = 8;
inline constexpr size_t kFriendGamertagMaximumBytes = 15;
inline constexpr size_t kFriendStateOffset = 24;
inline constexpr size_t kFriendSessionIdOffset = 28;
inline constexpr size_t kFriendTitleIdOffset = 36;
inline constexpr size_t kFriendUserTimeOffset = 40;
inline constexpr size_t kFriendInviteSessionIdOffset = 48;
inline constexpr size_t kFriendInviteTimeOffset = 56;
inline constexpr size_t kFriendRichPresenceUnitsOffset = 64;
inline constexpr size_t kFriendRichPresenceOffset = 68;
inline constexpr uint32_t kFriendStateOnline = UINT32_C(0x00000001);
inline constexpr uint32_t kFriendStateExcludedMask = UINT32_C(0xC0000000);
static_assert((kFriendStateOnline & kFriendStateExcludedMask) == 0);

inline void EncodeFriendInfo(XLiveFriendInfo& entry, uint64_t xuid,
                             std::string_view player_name, bool online,
                             bool playing_current_title, uint64_t session_id,
                             uint32_t title_id) {
  std::memset(&entry, 0, sizeof(entry));
  rex::memory::store_and_swap<uint64_t>(entry.bytes.data() + kFriendXuidOffset,
                                        xuid);
  const size_t name_bytes =
      std::min(player_name.size(), kFriendGamertagMaximumBytes);
  std::memcpy(entry.bytes.data() + kFriendGamertagOffset, player_name.data(),
              name_bytes);
  rex::memory::store_and_swap<uint32_t>(
      entry.bytes.data() + kFriendStateOffset,
      online ? kFriendStateOnline : UINT32_C(0));
  rex::memory::store_and_swap<uint64_t>(
      entry.bytes.data() + kFriendSessionIdOffset,
      playing_current_title ? session_id : UINT64_C(0));
  rex::memory::store_and_swap<uint32_t>(
      entry.bytes.data() + kFriendTitleIdOffset,
      playing_current_title ? title_id : UINT32_C(0));
}

struct XLiveAcceptedInviteInfo {
  rex::be<uint64_t> recipient_xuid;
  rex::be<uint64_t> sender_xuid;
  rex::be<uint32_t> title_id;
  rex::system::xam::XSESSION_INFO session_info;
  rex::be<uint32_t> flags;
  std::array<uint8_t, 12> reserved{};
};
static_assert(sizeof(XLiveAcceptedInviteInfo) == 96);
static_assert(offsetof(XLiveAcceptedInviteInfo, session_info) == 20);
static_assert(offsetof(XLiveAcceptedInviteInfo, flags) == 80);

struct XLiveMuteQuery {
  std::array<uint8_t, 20> bytes{};
};
static_assert(sizeof(XLiveMuteQuery) == 20);

// Checked by gta4-recomp/tools/derive_social_cache_contract.py and matched to
// generated sub_82A36610: user @0, XUID @8, embedded status @16.
inline constexpr size_t kMuteQueryUserIndexOffset = 0;
inline constexpr size_t kMuteQueryXuidOffset = 8;
inline constexpr size_t kMuteQueryStatusOffset = 16;

inline uint32_t MuteQueryUserIndex(const XLiveMuteQuery& query) {
  return rex::memory::load_and_swap<uint32_t>(query.bytes.data() + kMuteQueryUserIndexOffset);
}

inline uint64_t MuteQueryXuid(const XLiveMuteQuery& query) {
  return rex::memory::load_and_swap<uint64_t>(query.bytes.data() + kMuteQueryXuidOffset);
}

inline void CompleteMuteQuery(XLiveMuteQuery& query, rex::be<uint32_t>& output, uint32_t status,
                              bool muted) {
  output = status == X_ERROR_SUCCESS && muted ? 1 : 0;
  rex::memory::store_and_swap<uint32_t>(query.bytes.data() + kMuteQueryStatusOffset, status);
}

}  // namespace rex::kernel::xam::apps::detail
