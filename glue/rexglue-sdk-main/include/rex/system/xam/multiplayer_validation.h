/**
 ******************************************************************************
 * @file        multiplayer_validation.h
 * @brief       Correlated runtime evidence for GTA IV extended multiplayer.
 ******************************************************************************
 */

#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>

namespace rex::system::xam {

inline constexpr std::size_t kMultiplayerValidationPeerCapacity = 64;

enum class MultiplayerValidationStage : std::uint8_t {
  kXSessionJoin,
  kExtendedPeerAdd,
  kExtendedPeerPublication,
  kPlayerInfoConstruction,
  kParticipantAdd,
  kFirstExtendedEventSend,
  kFirstExtendedEventReceive,
  kFirstExtendedObjectSync,
  kRemoval,
  kCount,
};

struct MultiplayerValidationMember {
  std::uint64_t xuid = 0;
  std::uint8_t peer_id = 0;
};

struct MultiplayerValidationRecord {
  MultiplayerValidationStage stage = MultiplayerValidationStage::kXSessionJoin;
  std::uint64_t session_id = 0;
  std::uint64_t xuid = 0;
  std::uint8_t peer_id = 0;
  std::uint32_t guest_object = 0;
};

class MultiplayerValidationRegistry {
 public:
  void UpdateSession(std::uint64_t session_id,
                     std::span<const MultiplayerValidationMember> members);
  std::optional<MultiplayerValidationRecord> Record(MultiplayerValidationStage stage,
                                                     std::uint8_t peer_id,
                                                     std::uint32_t guest_object = 0);
  bool HasRecorded(MultiplayerValidationStage stage, std::uint8_t peer_id) const;
  void Reset();

 private:
  struct PeerState {
    std::uint64_t xuid = 0;
    std::bitset<static_cast<std::size_t>(MultiplayerValidationStage::kCount)> stages;
  };

  mutable std::mutex mutex_;
  std::uint64_t session_id_ = 0;
  std::array<PeerState, kMultiplayerValidationPeerCapacity> peers_{};
};

MultiplayerValidationRegistry& multiplayer_validation_registry();
std::string_view MultiplayerValidationStageName(MultiplayerValidationStage stage);
void PublishMultiplayerValidationSession(
    std::uint64_t session_id, std::span<const MultiplayerValidationMember> members);
bool PublishMultiplayerValidationStage(MultiplayerValidationStage stage, std::uint8_t peer_id,
                                       std::uint32_t guest_object = 0);

}  // namespace rex::system::xam
