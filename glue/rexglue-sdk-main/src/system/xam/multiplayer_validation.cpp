/**
 ******************************************************************************
 * @file        multiplayer_validation.cpp
 * @brief       Correlated runtime evidence for GTA IV extended multiplayer.
 ******************************************************************************
 */

#include <rex/system/xam/multiplayer_validation.h>

#include <rex/logging.h>

namespace rex::system::xam {

void MultiplayerValidationRegistry::UpdateSession(
    std::uint64_t session_id, std::span<const MultiplayerValidationMember> members) {
  std::scoped_lock lock(mutex_);
  if (session_id_ != session_id) {
    peers_ = {};
    session_id_ = session_id;
  }
  for (const MultiplayerValidationMember& member : members) {
    if (member.peer_id >= peers_.size() || member.xuid == 0) continue;
    PeerState& peer = peers_[member.peer_id];
    if (peer.xuid != member.xuid) peer = {};
    peer.xuid = member.xuid;
  }
}

std::optional<MultiplayerValidationRecord> MultiplayerValidationRegistry::Record(
    MultiplayerValidationStage stage, std::uint8_t peer_id, std::uint32_t guest_object) {
  const auto stage_index = static_cast<std::size_t>(stage);
  std::scoped_lock lock(mutex_);
  if (session_id_ == 0 || peer_id >= peers_.size() ||
      stage_index >= static_cast<std::size_t>(MultiplayerValidationStage::kCount)) {
    return std::nullopt;
  }
  PeerState& peer = peers_[peer_id];
  if (peer.xuid == 0 || peer.stages.test(stage_index)) return std::nullopt;
  peer.stages.set(stage_index);
  return MultiplayerValidationRecord{.stage = stage,
                                     .session_id = session_id_,
                                     .xuid = peer.xuid,
                                     .peer_id = peer_id,
                                     .guest_object = guest_object};
}

bool MultiplayerValidationRegistry::HasRecorded(MultiplayerValidationStage stage,
                                                 std::uint8_t peer_id) const {
  const auto stage_index = static_cast<std::size_t>(stage);
  std::scoped_lock lock(mutex_);
  return peer_id < peers_.size() &&
         stage_index < static_cast<std::size_t>(MultiplayerValidationStage::kCount) &&
         peers_[peer_id].stages.test(stage_index);
}

void MultiplayerValidationRegistry::Reset() {
  std::scoped_lock lock(mutex_);
  session_id_ = 0;
  peers_ = {};
}

MultiplayerValidationRegistry& multiplayer_validation_registry() {
  static MultiplayerValidationRegistry registry;
  return registry;
}

std::string_view MultiplayerValidationStageName(MultiplayerValidationStage stage) {
  switch (stage) {
    case MultiplayerValidationStage::kXSessionJoin:
      return "xsession-join";
    case MultiplayerValidationStage::kExtendedPeerAdd:
      return "extended-peer-add";
    case MultiplayerValidationStage::kExtendedPeerPublication:
      return "extended-peer-publication";
    case MultiplayerValidationStage::kPlayerInfoConstruction:
      return "player-info-construction";
    case MultiplayerValidationStage::kParticipantAdd:
      return "participant-add";
    case MultiplayerValidationStage::kFirstExtendedEventSend:
      return "first-extended-event-send";
    case MultiplayerValidationStage::kFirstExtendedEventReceive:
      return "first-extended-event-receive";
    case MultiplayerValidationStage::kFirstExtendedObjectSync:
      return "first-extended-object-sync";
    case MultiplayerValidationStage::kRemoval:
      return "removal";
    case MultiplayerValidationStage::kCount:
      return "invalid";
  }
  return "invalid";
}

void PublishMultiplayerValidationSession(
    std::uint64_t session_id, std::span<const MultiplayerValidationMember> members) {
  multiplayer_validation_registry().UpdateSession(session_id, members);
}

bool PublishMultiplayerValidationStage(MultiplayerValidationStage stage, std::uint8_t peer_id,
                                       std::uint32_t guest_object) {
  const auto record = multiplayer_validation_registry().Record(stage, peer_id, guest_object);
  if (!record) return false;
  REXLOG_INFO(
      "gta4-mp64-validation: stage={} session={:016X} xuid={:016X} peer={} guest={:08X}",
      MultiplayerValidationStageName(record->stage), record->session_id, record->xuid,
      record->peer_id, record->guest_object);
  return true;
}

}  // namespace rex::system::xam
