#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <rex/system/xam/live_compatibility.h>

namespace gta4::input {

// The title owns these policy choices. The transport owns compressed packets,
// but must not infer a channel from network readiness or an empty target list.
struct VoicePolicyState {
  uint64_t session_id = 0;
  bool team_only = false;
  std::optional<uint32_t> focused_peer_id;
  std::vector<uint64_t> manually_muted_xuids;

  void ObserveSession(uint64_t current_session_id,
                      uint64_t previous_session_id) {
    if (session_id == current_session_id) return;
    const bool migrated = session_id && previous_session_id == session_id;
    session_id = current_session_id;
    if (migrated) return;
    team_only = false;
    focused_peer_id.reset();
    manually_muted_xuids.clear();
  }

  void SetManualMute(uint64_t xuid, bool muted) {
    const auto found = std::ranges::find(manually_muted_xuids, xuid);
    if (muted) {
      if (xuid && found == manually_muted_xuids.end()) {
        manually_muted_xuids.push_back(xuid);
      }
    } else if (found != manually_muted_xuids.end()) {
      manually_muted_xuids.erase(found);
    }
  }
};

inline rex::system::xam::VoiceRoute ResolveVoiceRoute(
    const VoicePolicyState& policy,
    std::span<const rex::system::xam::SessionMember> members,
    const std::optional<std::vector<uint64_t>>& team_targets,
    std::span<const uint64_t> relationship_muted_xuids) {
  using rex::system::xam::VoiceChannel;
  using rex::system::xam::VoiceRoute;

  VoiceRoute route{.session_id = policy.session_id};
  if (policy.focused_peer_id) {
    route.channel = VoiceChannel::kPrivate;
    const auto focused = std::ranges::find(
        members, *policy.focused_peer_id,
        &rex::system::xam::SessionMember::multiplayer_peer_id);
    if (focused != members.end()) route.target_xuids.push_back(focused->xuid);
  } else if (policy.team_only) {
    route.channel = VoiceChannel::kTeam;
    if (team_targets) route.target_xuids = *team_targets;
  }

  route.muted_xuids = policy.manually_muted_xuids;
  route.muted_xuids.insert(route.muted_xuids.end(),
                           relationship_muted_xuids.begin(),
                           relationship_muted_xuids.end());
  std::ranges::sort(route.muted_xuids);
  route.muted_xuids.erase(
      std::unique(route.muted_xuids.begin(), route.muted_xuids.end()),
      route.muted_xuids.end());
  return route;
}

}  // namespace gta4::input
