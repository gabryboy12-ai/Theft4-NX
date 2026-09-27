/**
 ******************************************************************************
 * @file        social_cache.h
 * @brief       Host-side invite acceptance and relationship cache policy.
 ******************************************************************************
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include <rex/system/xam/live_compatibility.h>

namespace rex::system::xam::detail {

// Values are checked by gta4-recomp/tools/derive_social_cache_contract.py.
inline constexpr std::chrono::milliseconds kCommunityInvitePollInterval{2000};
inline constexpr std::chrono::milliseconds kCommunityInviteRetryInterval{1000};
inline constexpr std::chrono::milliseconds kCommunityRelationshipRefreshInterval{30000};
inline constexpr long kCommunitySocialRequestTimeoutMilliseconds = 1000;
inline constexpr size_t kCommunityDeliveredInviteHistoryLimit = 300;
inline constexpr size_t kCommunityInvitationPageSize = 100;
inline constexpr size_t kCommunityRelationshipPageSize = 100;
inline constexpr size_t kCommunityRelationshipCacheEntryLimit = 4096;
inline constexpr size_t kCommunityRelationshipMaximumPages = 41;

inline bool IsLocallyBlockedRelationship(std::string_view relationship) {
  return relationship == "blocked";
}

struct InvitationAcceptAttempt {
  InvitationRecord invitation;
  std::string idempotency;
  std::vector<uint64_t> session_aliases;
};

inline std::string_view Gta4EpisodePresenceName(uint32_t episode_index) {
  switch (episode_index) {
    case 1:
      return "tlad";
    case 2:
      return "tbogt";
    default:
      return "base";
  }
}

class SocialCacheState {
 public:
  bool PendingDescriptorCurrent(const InvitationRecord& invitation) const;
  bool ObservePending(InvitationRecord invitation, std::string candidate_idempotency);
  void RetainPending(std::span<const std::string> invitation_ids);
  bool PresentNextPending();
  std::optional<InvitationAcceptAttempt> PrepareExplicitAcceptance(uint64_t session_id,
                                                                   uint64_t sender_xuid);
  bool PublishExplicitAcceptance(const std::string& invitation_id,
                                 const InvitationRecord& accepted_invitation);
  void AbandonExplicitAcceptance(const std::string& invitation_id);
  std::optional<InvitationRecord> ConsumeAcceptedInvitation();
  bool HasAcceptedInvitation() const;

  void ReplaceRelationships(std::span<const FriendRecord> relationships);
  CachedMuteState QueryMute(uint64_t xuid) const;

  void SetInviteNotificationHandler(std::function<void()> handler);

  size_t delivered_invitation_count() const;
  size_t pending_invitation_count() const;

 private:
  struct PendingInvitation {
    InvitationAcceptAttempt attempt;
    bool presented = false;
    bool accepting = false;
  };

  void RememberDeliveredLocked(const std::string& invitation_id);

  mutable std::mutex mutex_;
  std::map<std::string, PendingInvitation> pending_invitations_;
  std::optional<InvitationRecord> accepted_invitation_;
  std::deque<std::string> delivered_order_;
  std::unordered_set<std::string> delivered_ids_;
  bool relationship_cache_ready_ = false;
  std::unordered_map<uint64_t, bool> muted_relationships_;
  std::function<void()> invite_notification_handler_;
};

}  // namespace rex::system::xam::detail
