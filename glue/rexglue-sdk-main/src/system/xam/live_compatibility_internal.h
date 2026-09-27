/**
 ******************************************************************************
 * @file        live_compatibility_internal.h
 * @brief       Internal LAN record framing and reassembly contracts.
 ******************************************************************************
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <rex/system/xam/live_compatibility.h>

namespace rex::system::xam::detail {

// Derived with libserver/tools/derive_resource_leases.py. The server session
// lease is four of these intervals, leaving room for transient request loss.
inline constexpr std::chrono::seconds kCommunityHeartbeatInterval{30};
inline bool ShouldHeartbeatActiveSession(LiveBackend backend, uint64_t active_session_id,
                                         uint64_t local_xuid, uint64_t host_xuid) {
  return backend == LiveBackend::kCommunity && active_session_id != 0 && local_xuid != 0 &&
         local_xuid == host_xuid;
}

LiveBackendServices CreateInMemoryOnlineServices(
    uint64_t local_xuid, std::shared_ptr<ISessionDirectory> session_directory);

struct TitleOnlineEndpoint {
  uint32_t ipv4 = 0;
  uint16_t port = 0;
};

TitleOnlineEndpoint SelectTitleOnlineEndpoint(
    LiveBackend backend, uint32_t local_ipv4, uint16_t local_port,
    const std::optional<SessionMember>& local_session_member);

inline constexpr size_t kLanMaximumDatagramSize = 1400;
inline constexpr size_t kLanFrameHeaderSize = 26;
inline constexpr size_t kLanMaximumFragmentPayload = 1374;
inline constexpr size_t kLanMaximumRecordSize = 34367;
inline constexpr uint16_t kLanMaximumFragmentCount = 26;
inline constexpr std::chrono::seconds kLanReassemblyLifetime{2};
inline constexpr uint8_t kLanOperationQuery = 1;
inline constexpr uint8_t kLanOperationAdvertise = 2;
inline constexpr uint8_t kLanOperationDelete = 3;
inline constexpr uint8_t kLanOperationJoin = 4;
inline constexpr uint8_t kLanOperationLeave = 5;
inline constexpr uint8_t kLanOperationMigrate = 6;

static_assert(kLanFrameHeaderSize + kLanMaximumFragmentPayload ==
              kLanMaximumDatagramSize);
static_assert((kLanMaximumRecordSize + kLanMaximumFragmentPayload - 1) /
                  kLanMaximumFragmentPayload ==
              kLanMaximumFragmentCount);

struct DecodedLanPayload {
  uint8_t operation = 0;
  std::vector<uint8_t> bytes;
};

std::vector<uint8_t> SerializeLanSessionRecord(const SessionRecord& session);
std::optional<SessionRecord> DeserializeLanSessionRecord(uint8_t operation,
                                                         std::span<const uint8_t> bytes);
std::vector<std::vector<uint8_t>> FrameLanPayload(uint8_t operation,
                                                  std::span<const uint8_t> payload,
                                                  uint64_t message_id);

class LanPacketReassembler {
 public:
  LanPacketReassembler();
  ~LanPacketReassembler();
  LanPacketReassembler(LanPacketReassembler&&) noexcept;
  LanPacketReassembler& operator=(LanPacketReassembler&&) noexcept;
  LanPacketReassembler(const LanPacketReassembler&) = delete;
  LanPacketReassembler& operator=(const LanPacketReassembler&) = delete;

  std::optional<DecodedLanPayload> Consume(
      uint32_t source_ipv4, uint16_t source_port, std::span<const uint8_t> datagram,
      std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
  void Expire(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

  size_t pending_count() const;
  size_t completed_count() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rex::system::xam::detail
