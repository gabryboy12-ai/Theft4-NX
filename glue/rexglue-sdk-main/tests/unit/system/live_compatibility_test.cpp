/**
 ******************************************************************************
 * @file        live_compatibility_test.cpp
 * @brief       Tests for the title-facing multiplayer session directory.
 ******************************************************************************
 */

#include <array>
#include <chrono>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/net/socket.h>
#include <rex/platform.h>
#include <rex/logging.h>
#include <rex/system/xam/live_compatibility.h>
#include <rex/system/xsocket.h>
#include <rex/system/xmemory.h>

#include "system/xam/live_compatibility_internal.h"
#include "kernel/xam/xam_qos_internal.h"
#include "system/xam/xsession_internal.h"

#if !REX_PLATFORM_WIN32
#include <cerrno>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace rex::system::xam::detail {
bool IsGta4MigrateFollowerUserIndex(uint32_t user_index);
bool HasUsableSessionJoinDescriptor(const SessionRecord& session);
bool AddSessionMemberWithSlotFallback(SessionRecord& record, SessionMember& member);
std::optional<uint32_t> SessionSearchResultsRequiredSize(
    std::span<const SessionRecord> sessions);
X_RESULT WriteSessionSearchResultsToBuffer(std::span<uint8_t> output,
                                           uint32_t output_guest_address,
                                           std::span<const SessionRecord> sessions);
}  // namespace rex::system::xam::detail

namespace {

using rex::X_RESULT;

using rex::system::xam::InMemorySessionDirectory;
using rex::system::xam::IsValidSessionRecord;
using rex::system::xam::kMaximumArbitrationUsersPerMachine;
using rex::system::xam::kMaximumSessionMembers;
using rex::system::xam::kSessionArbitrationResultsSize;
using rex::system::xam::LanSessionDirectory;
using rex::system::xam::SessionContext;
using rex::system::xam::SessionLifecycleState;
using rex::system::xam::SessionMember;
using rex::system::xam::SessionProperty;
using rex::system::xam::SessionRecord;
using rex::system::xam::XSESSION_REGISTRANT;
using rex::system::xam::XSESSION_REGISTRATION_RESULTS;
namespace live_detail = rex::system::xam::detail;
namespace qos_detail = rex::kernel::xam::detail;

TEST_CASE("GTA IV XNetQosListen flag combinations decode exactly",
          "[live][qos][listen][gta4]") {
  const std::array<uint8_t, rex::system::xam::kQosTitleDataSize> title_data = {
      0x47, 0x54, 0x41, 0x34, 0x51, 0x4F, 0x53, 0x44, 0x41, 0x54, 0x41, 0x00};

  const auto enable_with_data = qos_detail::DecodeQosListenCommand(
      5, title_data.data(), static_cast<uint32_t>(title_data.size()), 0);
  REQUIRE(enable_with_data);
  CHECK_FALSE(enable_with_data->release);
  REQUIRE(enable_with_data->update.enabled);
  CHECK(*enable_with_data->update.enabled);
  REQUIRE(enable_with_data->update.title_data);
  CHECK(*enable_with_data->update.title_data == title_data);
  CHECK_FALSE(enable_with_data->update.bits_per_second);

  const auto set_bandwidth =
      qos_detail::DecodeQosListenCommand(9, nullptr, 0, 16384);
  REQUIRE(set_bandwidth);
  CHECK_FALSE(set_bandwidth->release);
  REQUIRE(set_bandwidth->update.enabled);
  CHECK(*set_bandwidth->update.enabled);
  CHECK_FALSE(set_bandwidth->update.title_data);
  REQUIRE(set_bandwidth->update.bits_per_second);
  CHECK(*set_bandwidth->update.bits_per_second == 16384);

  const auto release = qos_detail::DecodeQosListenCommand(16, nullptr, 0, 0);
  REQUIRE(release);
  CHECK(release->release);
  CHECK_FALSE(release->update.enabled);
  CHECK_FALSE(release->update.title_data);
  CHECK_FALSE(release->update.bits_per_second);

  CHECK_FALSE(qos_detail::DecodeQosListenCommand(5, nullptr, 0, 0));
  CHECK_FALSE(qos_detail::DecodeQosListenCommand(9, nullptr, 0, 0)->release);
  CHECK_FALSE(qos_detail::DecodeQosListenCommand(16, nullptr, 0, 16384));
}

TEST_CASE("Voice headset capability is independent from network readiness",
          "[live][voice][capture]") {
  rex::system::xam::LiveConfig config;
  CHECK_FALSE(live_detail::HostVoiceCaptureAvailable(config));

  config.voice_capture_available = [] { return true; };
  CHECK(live_detail::HostVoiceCaptureAvailable(config));

  config.voice_capture_available = []() -> bool {
    throw std::runtime_error("capture query failed");
  };
  CHECK_FALSE(live_detail::HostVoiceCaptureAvailable(config));
}

TEST_CASE("XNQOS guest allocation uses exact big-endian ABI and contiguous title blobs",
          "[live][qos][abi]") {
  rex::system::xam::QosResult reachable;
  reachable.reachable = true;
  reachable.title_data =
      std::array<uint8_t, rex::system::xam::kQosTitleDataSize>{
          0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
  reachable.probes_xmit = 1;
  reachable.probes_recv = 1;
  reachable.rtt_min_milliseconds = 7;
  reachable.rtt_median_milliseconds = 9;
  rex::system::xam::QosResult contacted_without_data;
  contacted_without_data.reachable = true;
  contacted_without_data.probes_xmit = 1;
  contacted_without_data.probes_recv = 1;
  const std::array results = {reachable, contacted_without_data,
                              rex::system::xam::QosResult{}};
  const auto allocation_size = qos_detail::QosGuestAllocationSize(results);
  REQUIRE(allocation_size == 92);
  std::vector<uint8_t> guest(*allocation_size, 255);
  REQUIRE(qos_detail::WriteQosGuestAllocation(guest, 0x1000, results));

  CHECK(std::ranges::equal(std::span<const uint8_t>(guest).subspan(0, 8),
                           std::array<uint8_t, 8>{0, 0, 0, 3, 0, 0, 0, 0}));
  CHECK(guest[8] == 11);
  CHECK(std::ranges::equal(
      std::span<const uint8_t>(guest).subspan(10, 14),
      std::array<uint8_t, 14>{0, 1, 0, 1, 0, 12, 0, 0, 0x10, 0x50, 0, 7, 0, 9}));
  CHECK(guest[32] == 3);
  CHECK(std::ranges::equal(std::span<const uint8_t>(guest).subspan(33, 23),
                           std::array<uint8_t, 23>{0, 0, 1, 0, 1}));
  CHECK(guest[56] == 1);
  CHECK(std::ranges::equal(std::span<const uint8_t>(guest).subspan(57, 23),
                           std::array<uint8_t, 23>{}));
  CHECK(std::ranges::equal(std::span<const uint8_t>(guest).subspan(80, 12),
                           *reachable.title_data));
  CHECK_FALSE(qos_detail::WriteQosGuestAllocation(
      std::span<uint8_t>(guest).first(91), 0x1000, results));

  auto inconsistent = reachable;
  inconsistent.reachable = false;
  CHECK_FALSE(qos_detail::QosGuestAllocationSize(std::span{&inconsistent, size_t{1}}));
}

rex::memory::Memory& GetLiveTestMemory() {
  static rex::memory::Memory memory;
  static bool initialized = false;
  if (!initialized) {
    rex::InitLogging();
    REQUIRE(memory.Initialize());
    initialized = true;
  }
  return memory;
}

class GuestAllocation {
 public:
  explicit GuestAllocation(rex::memory::Memory& memory) : memory_(memory) {
    heap_ = memory_.LookupHeap(0x10000000);
    if (heap_) {
      heap_->Alloc(
          4096, 4096, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
          rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &address_);
    }
  }

  ~GuestAllocation() {
    if (heap_ && address_) {
      heap_->Release(address_);
    }
  }

  GuestAllocation(const GuestAllocation&) = delete;
  GuestAllocation& operator=(const GuestAllocation&) = delete;

  uint32_t address() const { return address_; }
  rex::memory::BaseHeap* heap() const { return heap_; }

 private:
  rex::memory::Memory& memory_;
  rex::memory::BaseHeap* heap_ = nullptr;
  uint32_t address_ = 0;
};

SessionRecord MakeSession(uint64_t session_id) {
  SessionRecord session;
  session.title_id = 0x545407F2;
  session.media_id = 0x12345678;
  session.title_version = 1;
  session.protocol_version = 1;
  session.session_id = session_id;
  session.max_public_slots = 4;
  session.max_private_slots = 2;
  session.open_public_slots = 4;
  session.open_private_slots = 2;
  session.contexts.push_back({.id = 3, .value = 9});
  session.properties.push_back({.id = 7, .value = {1, 2, 3}});
  return session;
}

SessionMember MakeMember(uint64_t xuid, bool private_slot) {
  SessionMember member{};
  member.xuid = xuid;
  member.private_slot = private_slot;
  return member;
}

TEST_CASE("GTA IV migrate follower sentinel is title-specific", "[live][session][migrate][abi]") {
  CHECK(live_detail::IsGta4MigrateFollowerUserIndex(0x000000FE));
  CHECK_FALSE(live_detail::IsGta4MigrateFollowerUserIndex(UINT32_MAX));
  CHECK_FALSE(live_detail::IsGta4MigrateFollowerUserIndex(0));
}

TEST_CASE("title XNADDR advertises the authoritative community relay endpoint",
          "[live][session][route][xnet]") {
  const uint32_t machine_ipv4 = 0x0100007F;
  const uint16_t machine_port = 3074;
  SessionMember local_member = MakeMember(0xE000000000000038ULL, false);
  local_member.virtual_ipv4 = 0x020012AC;
  local_member.online_port = 41001;

  SECTION("community session advertises the directory route") {
    const auto endpoint = live_detail::SelectTitleOnlineEndpoint(
        rex::system::xam::LiveBackend::kCommunity, machine_ipv4, machine_port,
        local_member);
    CHECK(endpoint.ipv4 == local_member.virtual_ipv4);
    CHECK(endpoint.port == local_member.online_port);
  }

  SECTION("community sign-in before a session advertises the machine endpoint") {
    const auto endpoint = live_detail::SelectTitleOnlineEndpoint(
        rex::system::xam::LiveBackend::kCommunity, machine_ipv4, machine_port,
        std::nullopt);
    CHECK(endpoint.ipv4 == machine_ipv4);
    CHECK(endpoint.port == machine_port);
  }

  SECTION("offline and LAN backends never publish a relay-only route") {
    for (const auto backend : {rex::system::xam::LiveBackend::kOffline,
                               rex::system::xam::LiveBackend::kLan}) {
      const auto endpoint = live_detail::SelectTitleOnlineEndpoint(
          backend, machine_ipv4, machine_port, local_member);
      CHECK(endpoint.ipv4 == machine_ipv4);
      CHECK(endpoint.port == machine_port);
    }
  }

  SECTION("incomplete community routing metadata is never advertised") {
    auto incomplete = local_member;
    incomplete.online_port = 0;
    const auto endpoint = live_detail::SelectTitleOnlineEndpoint(
        rex::system::xam::LiveBackend::kCommunity, machine_ipv4, machine_port,
        incomplete);
    CHECK(endpoint.ipv4 == machine_ipv4);
    CHECK(endpoint.port == machine_port);
  }
}

TEST_CASE("Community discovery records preserve hidden-roster occupancy",
          "[live][session][search][discovery]") {
  auto discovery = MakeSession(0xAE00000000000030ULL);
  discovery.roster_complete = false;
  discovery.declared_public_members = 2;
  discovery.declared_private_members = 1;
  discovery.open_public_slots = 2;
  discovery.open_private_slots = 1;
  REQUIRE(discovery.members.empty());
  CHECK(IsValidSessionRecord(discovery));

  auto ambiguous = discovery;
  ambiguous.members.push_back(MakeMember(0xE000000000000030ULL, false));
  CHECK_FALSE(IsValidSessionRecord(ambiguous));

  auto inconsistent = discovery;
  inconsistent.open_public_slots = 1;
  CHECK_FALSE(IsValidSessionRecord(inconsistent));
}

TEST_CASE("Peer create requires the complete host join descriptor",
          "[live][session][join][descriptor]") {
  auto session = MakeSession(0xAE00000000000034ULL);
  session.nonce = 0xABCD000000000034ULL;
  session.host_xuid = 0xE000000000000034ULL;
  session.host_machine_id = 0xD000000000000034ULL;
  session.host_ipv4 = 0x0100007F;
  session.host_port = 3074;
  session.host_peer_id = "host-peer";
  session.exchange_key.fill(0x11);
  session.host_ethernet_address.fill(0x22);
  CHECK(live_detail::HasUsableSessionJoinDescriptor(session));

  auto missing_nonce = session;
  missing_nonce.nonce = 0;
  CHECK_FALSE(live_detail::HasUsableSessionJoinDescriptor(missing_nonce));

  auto missing_route = session;
  missing_route.host_ipv4 = 0;
  CHECK_FALSE(live_detail::HasUsableSessionJoinDescriptor(missing_route));

  auto missing_key = session;
  missing_key.exchange_key.fill(0);
  CHECK_FALSE(live_detail::HasUsableSessionJoinDescriptor(missing_key));
}

TEST_CASE("XSession slot fallback propagates the authoritative slot class",
          "[live][session][join][slots]") {
  SECTION("public request falls back to private") {
    auto session = MakeSession(0xAE00000000000031ULL);
    session.max_public_slots = 0;
    session.open_public_slots = 0;
    session.max_private_slots = 1;
    session.open_private_slots = 1;
    SessionMember member = MakeMember(0xE000000000000031ULL, false);
    REQUIRE(live_detail::AddSessionMemberWithSlotFallback(session, member));
    CHECK(member.private_slot);
    REQUIRE(session.members.size() == 1);
    CHECK(session.members.front().private_slot);
  }

  SECTION("private request falls back to public") {
    auto session = MakeSession(0xAE00000000000032ULL);
    session.max_public_slots = 1;
    session.open_public_slots = 1;
    session.max_private_slots = 0;
    session.open_private_slots = 0;
    SessionMember member = MakeMember(0xE000000000000032ULL, true);
    REQUIRE(live_detail::AddSessionMemberWithSlotFallback(session, member));
    CHECK_FALSE(member.private_slot);
    REQUIRE(session.members.size() == 1);
    CHECK_FALSE(session.members.front().private_slot);
  }

  SECTION("existing public host moves to its requested private slot") {
    auto session = MakeSession(0xAE00000000000035ULL);
    session.max_public_slots = 1;
    session.open_public_slots = 0;
    session.max_private_slots = 1;
    session.open_private_slots = 1;
    session.members.push_back(MakeMember(0xE000000000000035ULL, false));

    SessionMember member = MakeMember(0xE000000000000035ULL, true);
    member.machine_id = 0xD000000000000035ULL;
    member.virtual_ipv4 = 0x0100007F;
    member.online_port = 3074;
    REQUIRE(live_detail::AddSessionMemberWithSlotFallback(session, member));
    CHECK(member.private_slot);
    CHECK(session.members.front().private_slot);
    CHECK(session.open_public_slots == 1);
    CHECK(session.open_private_slots == 0);
    CHECK(session.members.front().machine_id == member.machine_id);
    CHECK(session.members.front().virtual_ipv4 == member.virtual_ipv4);
    CHECK(session.members.front().online_port == member.online_port);
  }

  SECTION("same-class local join refreshes transport without replacing directory route") {
    auto session = MakeSession(0xAE00000000000037ULL);
    SessionMember existing = MakeMember(0xE000000000000037ULL, false);
    existing.machine_id = 0xD000000000000037ULL;
    existing.virtual_ipv4 = 0x0100007F;
    existing.online_port = 3074;
    existing.peer_id = "directory-peer";
    existing.multiplayer_peer_id = 7;
    session.members.push_back(existing);
    session.open_public_slots = 0;

    SessionMember member = MakeMember(existing.xuid, false);
    member.machine_id = 0xD000000000000038ULL;
    member.virtual_ipv4 = 0x0200007F;
    member.online_port = 3075;
    REQUIRE(live_detail::AddSessionMemberWithSlotFallback(session, member));

    CHECK(member.machine_id == 0xD000000000000038ULL);
    CHECK(member.virtual_ipv4 == 0x0200007F);
    CHECK(member.online_port == 3075);
    CHECK(member.peer_id == "directory-peer");
    CHECK(member.multiplayer_peer_id == 7);
    CHECK(session.members.front() == member);
    CHECK(session.open_public_slots == 0);
  }

  SECTION("existing member stays in its old class when the requested class is full") {
    auto session = MakeSession(0xAE00000000000036ULL);
    session.max_public_slots = 1;
    session.open_public_slots = 0;
    session.max_private_slots = 0;
    session.open_private_slots = 0;
    session.members.push_back(MakeMember(0xE000000000000036ULL, false));

    SessionMember member = MakeMember(0xE000000000000036ULL, true);
    REQUIRE(live_detail::AddSessionMemberWithSlotFallback(session, member));
    CHECK_FALSE(member.private_slot);
    CHECK_FALSE(session.members.front().private_slot);
    CHECK(session.open_public_slots == 0);
    CHECK(session.open_private_slots == 0);
  }
}

TEST_CASE("XSession search results own all pointed-to data inside the caller buffer",
          "[live][session][search][abi]") {
  auto session = MakeSession(0xAE00000000000033ULL);
  session.properties = {{.id = 0x60000001, .value = {0x11, 0x22, 0x33}}};
  const std::array sessions = {session};
  const auto required = live_detail::SessionSearchResultsRequiredSize(sessions);
  REQUIRE(required == 139);

  std::vector<uint8_t> output(140, 0xCD);
  REQUIRE(live_detail::WriteSessionSearchResultsToBuffer(
              std::span<uint8_t>(output).first(139), 0x1000, sessions) == X_ERROR_SUCCESS);
  CHECK(output[139] == 0xCD);

  const auto* header =
      reinterpret_cast<const rex::system::xam::XSESSION_SEARCHRESULT_HEADER*>(output.data());
  REQUIRE(static_cast<uint32_t>(header->search_results_count) == 1);
  CHECK(static_cast<uint32_t>(header->search_results_ptr) == 0x1008);
  const auto* result = reinterpret_cast<const rex::system::xam::XSESSION_SEARCHRESULT*>(
      output.data() + sizeof(*header));
  CHECK(static_cast<uint32_t>(result->contexts_count) == 1);
  CHECK(static_cast<uint32_t>(result->contexts_ptr) == 0x1064);
  CHECK(static_cast<uint32_t>(result->properties_count) == 1);
  CHECK(static_cast<uint32_t>(result->properties_ptr) == 0x1070);

  const auto* property =
      reinterpret_cast<const rex::system::xam::XUSER_PROPERTY*>(output.data() + 112);
  CHECK(static_cast<uint32_t>(property->data.value.binary.size) == 3);
  CHECK(static_cast<uint32_t>(property->data.value.binary.pointer) == 0x1088);
  CHECK(std::ranges::equal(std::span<const uint8_t>(output).subspan(136, 3),
                           std::array<uint8_t, 3>{0x11, 0x22, 0x33}));

  CHECK(live_detail::WriteSessionSearchResultsToBuffer(
            std::span<uint8_t>(output).first(138), 0x1000, sessions) ==
        X_ERROR_INSUFFICIENT_BUFFER);
}

TEST_CASE("XSession search results align string payloads after odd binary data",
          "[live][session][search][abi][alignment]") {
  auto session = MakeSession(0xAE00000000000034ULL);
  session.properties = {
      {.id = 0x60000001, .value = {0x11, 0x22, 0x33}},
      {.id = 0x40000002, .value = {0x00, 0x41, 0x00, 0x00}},
  };
  const std::array sessions = {session};
  const auto required = live_detail::SessionSearchResultsRequiredSize(sessions);
  REQUIRE(required == 168);

  std::vector<uint8_t> output(*required, 0xCD);
  REQUIRE(live_detail::WriteSessionSearchResultsToBuffer(
              output, 0x1000, sessions) == X_ERROR_SUCCESS);
  const auto* properties = reinterpret_cast<const rex::system::xam::XUSER_PROPERTY*>(
      output.data() + 112);
  CHECK(static_cast<uint32_t>(properties[0].data.value.binary.pointer) == 0x10A0);
  CHECK(static_cast<uint32_t>(properties[1].data.value.unicode.pointer) == 0x10A4);
  CHECK((static_cast<uint32_t>(properties[1].data.value.unicode.pointer) & 1U) == 0);
  CHECK(std::ranges::equal(std::span<const uint8_t>(output).subspan(164, 4),
                           std::array<uint8_t, 4>{0x00, 0x41, 0x00, 0x00}));
}

SessionRecord MakeMaximumLanSession(uint64_t session_id) {
  auto session = MakeSession(session_id);
  session.max_public_slots = kMaximumSessionMembers;
  session.max_private_slots = 0;
  session.open_public_slots = 0;
  session.open_private_slots = 0;
  session.contexts.clear();
  session.properties.clear();
  session.qos_listener_enabled = true;
  session.qos_title_data = std::array<uint8_t, rex::system::xam::kQosTitleDataSize>{};
  for (uint32_t index = 0; index < 64; ++index) {
    session.contexts.push_back({.id = 0x1000U + index, .value = 0x2000U + index});
    session.properties.push_back(
        {.id = 0x3000U + index, .value = std::vector<uint8_t>(512, static_cast<uint8_t>(index))});
    session.members.push_back(MakeMember(0xE000000000007000ULL + index, false));
  }
  return session;
}

#if !REX_PLATFORM_WIN32
uint16_t ReserveUdpPort() {
  const int handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (handle < 0) {
    return 0;
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = 0;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    close(handle);
    return 0;
  }

  socklen_t address_size = sizeof(address);
  if (getsockname(handle, reinterpret_cast<sockaddr*>(&address), &address_size) != 0) {
    close(handle);
    return 0;
  }
  close(handle);
  return ntohs(address.sin_port);
}
#endif

}  // namespace

TEST_CASE("session directory treats the search procedure as query-only", "[live][session]") {
  InMemorySessionDirectory directory;
  REQUIRE(directory.Create(MakeSession(0xAE00000000000001ULL)));
  CHECK_FALSE(directory.Create(MakeSession(0xAE00000000000001ULL)));

  const std::array required_contexts = {SessionContext{.id = 3, .value = 9}};
  const std::array required_properties = {SessionProperty{.id = 7, .value = {1, 2, 3}}};
  auto matches =
      directory.Search(0x545407F2, 0x12345678, 1, 1, 0, required_contexts, required_properties, 8);
  REQUIRE(matches.size() == 1);
  CHECK(matches.front().session_id == 0xAE00000000000001ULL);

  // XSessionCreate has no procedure field. A nonzero XSessionSearchEx procedure
  // selects a title query schema and must not be compared with session state.
  matches =
      directory.Search(0x545407F2, 0x12345678, 1, 1, 1, required_contexts, required_properties, 8);
  REQUIRE(matches.size() == 1);
  CHECK(matches.front().session_id == 0xAE00000000000001ULL);

  CHECK(directory.Search(0x545407F3, 0x12345678, 1, 1, 0, required_contexts, required_properties, 8)
            .empty());
  CHECK(directory.Search(0x545407F2, 0x12345678, 1, 2, 0, required_contexts, required_properties, 8)
            .empty());

  const std::array wrong_contexts = {SessionContext{.id = 3, .value = 10}};
  CHECK(directory.Search(0x545407F2, 0x12345678, 1, 1, 0, wrong_contexts, required_properties, 8)
            .empty());
}

TEST_CASE("community heartbeat policy is host-only and active", "[live][session][heartbeat]") {
  using rex::system::xam::LiveBackend;
  CHECK(live_detail::ShouldHeartbeatActiveSession(LiveBackend::kCommunity, 1, 2, 2));
  CHECK_FALSE(live_detail::ShouldHeartbeatActiveSession(LiveBackend::kCommunity, 0, 2, 2));
  CHECK_FALSE(live_detail::ShouldHeartbeatActiveSession(LiveBackend::kCommunity, 1, 2, 3));
  CHECK_FALSE(live_detail::ShouldHeartbeatActiveSession(LiveBackend::kCommunity, 1, 0, 0));
  CHECK_FALSE(live_detail::ShouldHeartbeatActiveSession(LiveBackend::kLan, 1, 2, 2));
  CHECK_FALSE(live_detail::ShouldHeartbeatActiveSession(LiveBackend::kOffline, 1, 2, 2));
}

TEST_CASE("LIVE identity sign-in is independent from backend service health",
          "[live][signin]") {
  using rex::system::xam::LiveBackend;
  using rex::system::xam::LiveIdentity;
  using rex::system::xam::IsLiveIdentitySignedIn;

  LiveIdentity missing_identity;
  LiveIdentity identity;
  identity.xuid = 1;

  CHECK_FALSE(IsLiveIdentitySignedIn(LiveBackend::kOffline, identity));
  CHECK(IsLiveIdentitySignedIn(LiveBackend::kLan, identity));
  CHECK(IsLiveIdentitySignedIn(LiveBackend::kCommunity, identity));
  CHECK_FALSE(IsLiveIdentitySignedIn(LiveBackend::kCommunity, missing_identity));
}

TEST_CASE("Community availability recovers with bounded retry timing",
          "[live][community][reconnect]") {
  using rex::system::xam::LiveBackend;
  using rex::system::xam::LiveState;
  using rex::system::xam::detail::CommunityReconnectDelay;
  using rex::system::xam::detail::ResolveLiveServiceState;

  CHECK(CommunityReconnectDelay(0) == std::chrono::milliseconds(1000));
  CHECK(CommunityReconnectDelay(1) == std::chrono::milliseconds(2000));
  CHECK(CommunityReconnectDelay(4) == std::chrono::milliseconds(16000));
  CHECK(CommunityReconnectDelay(5) == std::chrono::milliseconds(30000));
  CHECK(CommunityReconnectDelay(100) == std::chrono::milliseconds(30000));

  CHECK(ResolveLiveServiceState(LiveBackend::kCommunity, LiveState::kError, false) ==
        LiveState::kError);
  CHECK(ResolveLiveServiceState(LiveBackend::kCommunity, LiveState::kError, true) ==
        LiveState::kAvailable);
  CHECK(ResolveLiveServiceState(LiveBackend::kCommunity, LiveState::kAvailable, false) ==
        LiveState::kError);
  CHECK(ResolveLiveServiceState(LiveBackend::kOffline, LiveState::kError, true) ==
        LiveState::kOffline);
}

TEST_CASE("session member operations preserve slot accounting", "[live][session]") {
  InMemorySessionDirectory directory;
  const uint64_t session_id = 0xAE00000000000002ULL;
  REQUIRE(directory.Create(MakeSession(session_id)));

  REQUIRE(directory.Join(session_id, MakeMember(11, false)));
  REQUIRE(directory.Join(session_id, MakeMember(12, true)));
  REQUIRE(directory.Join(session_id, MakeMember(11, false)));

  auto joined = directory.Get(session_id);
  REQUIRE(joined);
  CHECK(joined->members.size() == 2);
  CHECK(joined->open_public_slots == 3);
  CHECK(joined->open_private_slots == 1);

  REQUIRE(directory.Leave(session_id, 11));
  REQUIRE(directory.Leave(session_id, 12));
  auto left = directory.Get(session_id);
  REQUIRE(left);
  CHECK(left->members.empty());
  CHECK(left->open_public_slots == left->max_public_slots);
  CHECK(left->open_private_slots == left->max_private_slots);
}

TEST_CASE("session directory accepts every extended multiplayer boundary",
          "[live][session][capacity]") {
  InMemorySessionDirectory directory;
  auto session = MakeSession(0xAE00000000000020ULL);
  session.max_public_slots = kMaximumSessionMembers;
  session.max_private_slots = 0;
  session.open_public_slots = kMaximumSessionMembers;
  session.open_private_slots = 0;
  REQUIRE(directory.Create(session));

  for (uint32_t index = 0; index < kMaximumSessionMembers; ++index) {
    REQUIRE(directory.Join(session.session_id, MakeMember(0xE000000000001000ULL + index, false)));
    const auto record = directory.Get(session.session_id);
    REQUIRE(record);
    if (index == 16 || index == 32 || index == 63) {
      CHECK(record->members.size() == static_cast<size_t>(index + 1));
    }
  }

  const auto full = directory.Get(session.session_id);
  REQUIRE(full);
  CHECK(full->members.size() == kMaximumSessionMembers);
  CHECK(full->open_public_slots == 0);
  CHECK_FALSE(directory.Join(session.session_id, MakeMember(0xE000000000002000ULL, false)));
}

TEST_CASE("session directory rejects a combined slot count above capacity",
          "[live][session][capacity]") {
  InMemorySessionDirectory directory;
  auto session = MakeSession(0xAE00000000000021ULL);
  session.max_public_slots = kMaximumSessionMembers;
  session.open_public_slots = kMaximumSessionMembers;
  session.max_private_slots = 1;
  session.open_private_slots = 1;
  CHECK_FALSE(directory.Create(session));
}

TEST_CASE("session record validator rejects malformed extended rosters",
          "[live][session][capacity]") {
  auto session = MakeSession(0xAE00000000000022ULL);
  session.max_public_slots = kMaximumSessionMembers;
  session.max_private_slots = 0;
  session.open_public_slots = 0;
  session.open_private_slots = 0;
  for (uint32_t index = 0; index < kMaximumSessionMembers; ++index) {
    session.members.push_back(MakeMember(0xE000000000003000ULL + index, false));
  }
  REQUIRE(IsValidSessionRecord(session));

  session.members.push_back(MakeMember(0xE000000000004000ULL, false));
  CHECK_FALSE(IsValidSessionRecord(session));
  session.members.pop_back();

  session.members.back().xuid = session.members.front().xuid;
  CHECK_FALSE(IsValidSessionRecord(session));
}

TEST_CASE("XSessionGetDetails honors pointer-to-size ABI and full roster bounds",
          "[live][session][details]") {
  auto& memory = GetLiveTestMemory();
  GuestAllocation size_allocation(memory);
  GuestAllocation output_allocation(memory);
  REQUIRE(size_allocation.address());
  REQUIRE(output_allocation.address());

  auto session = MakeMaximumLanSession(0xAE00000000000023ULL);
  rex::system::xam::XSESSION_LOCAL_DETAILS details_template{};
  details_template.flags = 0x55AA;
  details_template.state = static_cast<uint32_t>(rex::system::xam::XSessionState::kInGame);
  auto* declared_size = memory.TranslateVirtual<rex::be<uint32_t>*>(size_allocation.address());
  *declared_size = 0x480;

  const uint64_t local_xuid = session.members[7].xuid;
  REQUIRE(live_detail::WriteSessionDetailsToGuest(&memory, size_allocation.address(),
                                                  output_allocation.address(), details_template,
                                                  session.members, local_xuid) == X_ERROR_SUCCESS);
  CHECK(static_cast<uint32_t>(*declared_size) == 0x480);

  const auto* output = memory.TranslateVirtual<const rex::system::xam::XSESSION_LOCAL_DETAILS*>(
      output_allocation.address());
  CHECK(static_cast<uint32_t>(output->actual_member_count) == kMaximumSessionMembers);
  CHECK(static_cast<uint32_t>(output->returned_member_count) == kMaximumSessionMembers);
  CHECK(static_cast<uint32_t>(output->session_members_ptr) == output_allocation.address() + 0x80);
  CHECK(static_cast<uint32_t>(output->flags) == 0x55AA);

  const auto* members = reinterpret_cast<const rex::system::xam::XSESSION_MEMBER*>(output + 1);
  CHECK(static_cast<uint64_t>(members[0].online_xuid) == session.members[0].xuid);
  CHECK(static_cast<uint32_t>(members[0].user_index) == UINT32_MAX);
  CHECK(static_cast<uint32_t>(members[7].user_index) == 0);
  CHECK(static_cast<uint64_t>(members[63].online_xuid) == session.members[63].xuid);
}

TEST_CASE("XSessionGetDetails returns only complete members that fit", "[live][session][details]") {
  auto& memory = GetLiveTestMemory();
  GuestAllocation size_allocation(memory);
  GuestAllocation output_allocation(memory);
  REQUIRE(size_allocation.address());
  REQUIRE(output_allocation.address());

  auto session = MakeMaximumLanSession(0xAE00000000000024ULL);
  rex::system::xam::XSESSION_LOCAL_DETAILS details_template{};
  auto* declared_size = memory.TranslateVirtual<rex::be<uint32_t>*>(size_allocation.address());
  *declared_size = 0xBF;
  std::memset(memory.TranslateVirtual(output_allocation.address()), 0xCD, 4096);

  REQUIRE(live_detail::WriteSessionDetailsToGuest(&memory, size_allocation.address(),
                                                  output_allocation.address(), details_template,
                                                  session.members, 0) == X_ERROR_SUCCESS);
  CHECK(static_cast<uint32_t>(*declared_size) == 0x480);
  const auto* output = memory.TranslateVirtual<const rex::system::xam::XSESSION_LOCAL_DETAILS*>(
      output_allocation.address());
  CHECK(static_cast<uint32_t>(output->actual_member_count) == kMaximumSessionMembers);
  CHECK(static_cast<uint32_t>(output->returned_member_count) == 3);
  CHECK(static_cast<uint32_t>(output->session_members_ptr) == output_allocation.address() + 0x80);
  CHECK(memory.TranslateVirtual<const uint8_t*>(output_allocation.address())[0xB0] == 0xCD);

  *declared_size = 0x80;
  std::memset(memory.TranslateVirtual(output_allocation.address()), 0xAB, 4096);
  REQUIRE(live_detail::WriteSessionDetailsToGuest(&memory, size_allocation.address(),
                                                  output_allocation.address(), details_template,
                                                  session.members, 0) == X_ERROR_SUCCESS);
  CHECK(static_cast<uint32_t>(*declared_size) == 0x480);
  output = memory.TranslateVirtual<const rex::system::xam::XSESSION_LOCAL_DETAILS*>(
      output_allocation.address());
  CHECK(static_cast<uint32_t>(output->actual_member_count) == kMaximumSessionMembers);
  CHECK(static_cast<uint32_t>(output->returned_member_count) == 0);
  CHECK(static_cast<uint32_t>(output->session_members_ptr) == 0);
  CHECK(memory.TranslateVirtual<const uint8_t*>(output_allocation.address())[0x80] == 0xAB);
}

TEST_CASE("XSessionGetDetails rejects malformed guest spans without writing past them",
          "[live][session][details][safety]") {
  auto& memory = GetLiveTestMemory();
  auto session = MakeMaximumLanSession(0xAE00000000000025ULL);
  rex::system::xam::XSESSION_LOCAL_DETAILS details_template{};

  SECTION("undersized and null outputs negotiate the complete required size") {
    GuestAllocation size_allocation(memory);
    GuestAllocation output_allocation(memory);
    REQUIRE(size_allocation.address());
    REQUIRE(output_allocation.address());
    auto* declared_size = memory.TranslateVirtual<rex::be<uint32_t>*>(size_allocation.address());
    *declared_size = 0x7F;
    std::memset(memory.TranslateVirtual(output_allocation.address()), 0xEF, 4096);
    CHECK(live_detail::WriteSessionDetailsToGuest(
              &memory, size_allocation.address(), output_allocation.address(), details_template,
              session.members, 0) == X_ERROR_INSUFFICIENT_BUFFER);
    CHECK(static_cast<uint32_t>(*declared_size) == 0x480);
    CHECK(memory.TranslateVirtual<const uint8_t*>(output_allocation.address())[0] == 0xEF);

    *declared_size = 0x480;
    CHECK(live_detail::WriteSessionDetailsToGuest(&memory, size_allocation.address(), 0,
                                                  details_template, session.members,
                                                  0) == X_ERROR_INSUFFICIENT_BUFFER);
    CHECK(static_cast<uint32_t>(*declared_size) == 0x480);
  }

  SECTION("invalid or read-only size pointers are rejected") {
    GuestAllocation size_allocation(memory);
    GuestAllocation output_allocation(memory);
    REQUIRE(size_allocation.address());
    REQUIRE(output_allocation.address());
    CHECK(live_detail::WriteSessionDetailsToGuest(&memory, 0, output_allocation.address(),
                                                  details_template, session.members,
                                                  0) == X_ERROR_INVALID_PARAMETER);
    CHECK(live_detail::WriteSessionDetailsToGuest(&memory, 0x7F000000, output_allocation.address(),
                                                  details_template, session.members,
                                                  0) == X_ERROR_INVALID_PARAMETER);

    *memory.TranslateVirtual<rex::be<uint32_t>*>(size_allocation.address()) = 0x480;
    REQUIRE(size_allocation.heap()->Protect(size_allocation.address(), 4096,
                                            rex::memory::kMemoryProtectRead));
    CHECK(live_detail::WriteSessionDetailsToGuest(&memory, size_allocation.address(),
                                                  output_allocation.address(), details_template,
                                                  session.members, 0) == X_ERROR_INVALID_PARAMETER);
  }

  SECTION("a declared buffer cannot cross its committed allocation") {
    GuestAllocation size_allocation(memory);
    GuestAllocation output_allocation(memory);
    REQUIRE(size_allocation.address());
    REQUIRE(output_allocation.address());
    *memory.TranslateVirtual<rex::be<uint32_t>*>(size_allocation.address()) = 0x80;
    CHECK(live_detail::WriteSessionDetailsToGuest(
              &memory, size_allocation.address(), output_allocation.address() + 0xFC0,
              details_template, session.members, 0) == X_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("LAN framing covers the exact maximum 64-member metadata record",
          "[live][lan][fragmentation]") {
  const auto session = MakeMaximumLanSession(0xAE00000000000026ULL);
  REQUIRE(IsValidSessionRecord(session));

  const auto payload = live_detail::SerializeLanSessionRecord(session);
  REQUIRE(payload.size() == live_detail::kLanMaximumRecordSize);
  const auto frames =
      live_detail::FrameLanPayload(live_detail::kLanOperationAdvertise, payload, 0x1234);
  REQUIRE(frames.size() == live_detail::kLanMaximumFragmentCount);
  for (size_t index = 0; index < frames.size(); ++index) {
    CHECK(frames[index].size() <= live_detail::kLanMaximumDatagramSize);
    if (index + 1 < frames.size()) {
      CHECK(frames[index].size() == live_detail::kLanMaximumDatagramSize);
    }
  }
  CHECK(frames.back().size() == 43);

  live_detail::LanPacketReassembler reassembler;
  std::optional<live_detail::DecodedLanPayload> complete;
  const auto now = std::chrono::steady_clock::time_point{};
  for (size_t index = frames.size(); index > 0; --index) {
    complete = reassembler.Consume(0x01020304, 3074, frames[index - 1], now);
    if (index > 1) {
      CHECK_FALSE(complete);
    }
  }
  REQUIRE(complete);
  CHECK(complete->operation == live_detail::kLanOperationAdvertise);
  CHECK(complete->bytes == payload);
  CHECK(reassembler.pending_count() == 0);
  CHECK(reassembler.completed_count() == 1);

  const auto decoded =
      live_detail::DeserializeLanSessionRecord(complete->operation, complete->bytes);
  REQUIRE(decoded);
  CHECK(decoded->session_id == session.session_id);
  CHECK(decoded->contexts == session.contexts);
  CHECK(decoded->properties == session.properties);
  CHECK(decoded->members == session.members);
  CHECK(decoded->qos_listener_enabled == session.qos_listener_enabled);
  CHECK(decoded->qos_title_data == session.qos_title_data);

  // A replay of a completed logical packet is suppressed rather than applied twice.
  for (const auto& frame : frames) {
    CHECK_FALSE(reassembler.Consume(0x01020304, 3074, frame, now));
  }
  CHECK(reassembler.pending_count() == 0);
}

TEST_CASE("LAN framing enforces datagram boundaries and canonical fragment counts",
          "[live][lan][fragmentation][boundary]") {
  const std::vector<uint8_t> exact(live_detail::kLanMaximumFragmentPayload, 0x11);
  const auto exact_frames =
      live_detail::FrameLanPayload(live_detail::kLanOperationAdvertise, exact, 1);
  REQUIRE(exact_frames.size() == 1);
  CHECK(exact_frames.front().size() == live_detail::kLanMaximumDatagramSize);

  const std::vector<uint8_t> over(live_detail::kLanMaximumFragmentPayload + 1, 0x22);
  const auto over_frames =
      live_detail::FrameLanPayload(live_detail::kLanOperationAdvertise, over, 2);
  REQUIRE(over_frames.size() == 2);
  CHECK(over_frames.front().size() == live_detail::kLanMaximumDatagramSize);
  CHECK(over_frames.back().size() == live_detail::kLanFrameHeaderSize + 1);

  const std::vector<uint8_t> too_large(live_detail::kLanMaximumRecordSize + 1, 0x33);
  CHECK(live_detail::FrameLanPayload(live_detail::kLanOperationAdvertise, too_large, 3).empty());
  CHECK(live_detail::FrameLanPayload(live_detail::kLanOperationQuery, exact, 4).empty());
  CHECK(live_detail::FrameLanPayload(live_detail::kLanOperationQuery, {}, 0).empty());
}

TEST_CASE("LAN reassembly rejects malformed conflicts and expires incomplete messages",
          "[live][lan][fragmentation][safety]") {
  const std::vector<uint8_t> payload(live_detail::kLanMaximumFragmentPayload + 1, 0x44);
  const auto frames =
      live_detail::FrameLanPayload(live_detail::kLanOperationAdvertise, payload, 0x7777);
  REQUIRE(frames.size() == 2);
  const auto start = std::chrono::steady_clock::time_point{};

  live_detail::LanPacketReassembler reassembler;
  CHECK_FALSE(reassembler.Consume(0x05060708, 3074, frames[0], start));
  CHECK(reassembler.pending_count() == 1);
  CHECK_FALSE(reassembler.Consume(0x05060708, 3074, frames[0], start));
  CHECK(reassembler.pending_count() == 1);

  auto conflicting = frames[0];
  conflicting.back() ^= 0xFF;
  CHECK_FALSE(reassembler.Consume(0x05060708, 3074, conflicting, start));
  CHECK(reassembler.pending_count() == 0);

  CHECK_FALSE(reassembler.Consume(0x05060708, 3074, frames[0], start));
  reassembler.Expire(start + live_detail::kLanReassemblyLifetime);
  CHECK(reassembler.pending_count() == 0);

  auto bad_version = frames[0];
  bad_version[8] ^= 0xFF;
  CHECK_FALSE(reassembler.Consume(0x05060708, 3074, bad_version, start));
  CHECK(reassembler.pending_count() == 0);

  auto truncated = frames[0];
  truncated.pop_back();
  CHECK_FALSE(reassembler.Consume(0x05060708, 3074, truncated, start));
  CHECK(reassembler.pending_count() == 0);

  std::vector<uint8_t> oversized(live_detail::kLanMaximumDatagramSize + 1, 0);
  CHECK_FALSE(reassembler.Consume(0x05060708, 3074, oversized, start));
  CHECK(reassembler.pending_count() == 0);
}

TEST_CASE("session lifecycle supports modify migrate and delete", "[live][session]") {
  InMemorySessionDirectory directory;
  const uint64_t old_id = 0xAE00000000000003ULL;
  const uint64_t new_id = 0xAE00000000000004ULL;
  auto session = MakeSession(old_id);
  REQUIRE(directory.Create(session));

  session.flags = 5;
  session.lifecycle_state = SessionLifecycleState::kInGame;
  session.open_public_slots = 3;
  session.members.push_back(MakeMember(33, false));
  REQUIRE(directory.Modify(session));
  REQUIRE(directory.Heartbeat(session));
  REQUIRE(directory.Get(old_id));
  CHECK(directory.Get(old_id)->flags == 5);
  CHECK(directory.Get(old_id)->lifecycle_state == SessionLifecycleState::kInGame);

  auto replacement = session;
  replacement.session_id = new_id;
  replacement.host_xuid = 44;
  REQUIRE(directory.Migrate(old_id, replacement));
  CHECK_FALSE(directory.Get(old_id));
  REQUIRE(directory.ResolveMigration(old_id));
  CHECK(directory.ResolveMigration(old_id)->session_id == new_id);
  REQUIRE(directory.Get(new_id));
  CHECK(directory.Get(new_id)->host_xuid == 44);
  CHECK(directory.Get(new_id)->previous_session_id == old_id);

  REQUIRE(directory.Delete(new_id));
  CHECK_FALSE(directory.Get(new_id));
  CHECK_FALSE(directory.Delete(new_id));
}

#if !REX_PLATFORM_WIN32
TEST_CASE("LAN directory synchronizes authoritative session mutations", "[live][lan]") {
  const uint16_t discovery_port = ReserveUdpPort();
  REQUIRE(discovery_port != 0);

  LanSessionDirectory host(discovery_port);
  LanSessionDirectory client(discovery_port);
  REQUIRE(host.ready());
  REQUIRE(client.ready());

  const uint64_t old_id = 0xAE00000000000011ULL;
  const uint64_t new_id = 0xAE00000000000012ULL;
  auto session = MakeSession(old_id);
  REQUIRE(host.Create(session));

  auto matches = client.Search(session.title_id, session.media_id, session.title_version,
                               session.protocol_version, 0, {}, {}, 1);
  REQUIRE(matches.size() == 1);
  CHECK(matches.front().session_id == old_id);

  const SessionMember member = MakeMember(0xE000000000000021ULL, false);
  REQUIRE(client.Join(old_id, member));
  REQUIRE(host.Get(old_id));
  CHECK(host.Get(old_id)->members == std::vector{member});

  REQUIRE(client.Leave(old_id, member.xuid));
  REQUIRE(host.Get(old_id));
  CHECK(host.Get(old_id)->members.empty());

  auto migrated = *client.Get(old_id);
  migrated.session_id = new_id;
  migrated.host_xuid = member.xuid;
  REQUIRE(client.Migrate(old_id, migrated));
  CHECK_FALSE(client.Get(old_id));
  REQUIRE(client.ResolveMigration(old_id));
  CHECK(client.ResolveMigration(old_id)->session_id == new_id);
  REQUIRE(client.Get(new_id));

  auto migrated_matches = host.Search(session.title_id, session.media_id, session.title_version,
                                      session.protocol_version, 0, {}, {}, 1);
  REQUIRE(migrated_matches.size() == 1);
  CHECK(migrated_matches.front().session_id == new_id);
  CHECK_FALSE(host.Get(old_id));
  REQUIRE(host.ResolveMigration(old_id));
  CHECK(host.ResolveMigration(old_id)->session_id == new_id);

  REQUIRE(client.Delete(new_id));
  CHECK_FALSE(client.Get(new_id));
  CHECK(host.Search(session.title_id, session.media_id, session.title_version,
                    session.protocol_version, 0, {}, {}, 1)
            .empty());

  auto extended = MakeMaximumLanSession(0xAE00000000000013ULL);
  REQUIRE(host.Create(extended));

  const auto extended_matches =
      client.Search(extended.title_id, extended.media_id, extended.title_version,
                    extended.protocol_version, 0, {}, {}, 1);
  REQUIRE(extended_matches.size() == 1);
  CHECK(extended_matches.front().members.size() == kMaximumSessionMembers);
  CHECK(extended_matches.front().members == extended.members);
  CHECK(extended_matches.front().contexts == extended.contexts);
  CHECK(extended_matches.front().properties == extended.properties);
}

TEST_CASE("UDP wrapper preserves Xbox network byte order", "[live][socket]") {
  rex::system::XSocket receiver(nullptr);
  REQUIRE(XSUCCEEDED(receiver.Initialize(rex::system::XSocket::X_AF_INET,
                                         rex::system::XSocket::X_SOCK_DGRAM,
                                         rex::system::XSocket::X_IPPROTO_UDP)));

  rex::system::N_XSOCKADDR_IN bind_address{};
  bind_address.sin_family = AF_INET;
  bind_address.sin_port = 0;
  bind_address.sin_addr = INADDR_LOOPBACK;
  REQUIRE(XSUCCEEDED(receiver.Bind(&bind_address, sizeof(bind_address))));

  sockaddr_in receiver_address{};
  socklen_t receiver_address_size = sizeof(receiver_address);
  REQUIRE(getsockname(static_cast<int>(receiver.native_handle()),
                      reinterpret_cast<sockaddr*>(&receiver_address), &receiver_address_size) == 0);

  timeval timeout{.tv_sec = 1, .tv_usec = 0};
  REQUIRE(setsockopt(static_cast<int>(receiver.native_handle()), SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof(timeout)) == 0);

  rex::system::XSocket sender(nullptr);
  REQUIRE(XSUCCEEDED(sender.Initialize(rex::system::XSocket::X_AF_INET,
                                       rex::system::XSocket::X_SOCK_DGRAM,
                                       rex::system::XSocket::X_IPPROTO_UDP)));

  rex::system::N_XSOCKADDR_IN destination{};
  destination.sin_family = AF_INET;
  destination.sin_port = ntohs(receiver_address.sin_port);
  destination.sin_addr = ntohl(receiver_address.sin_addr.s_addr);
  std::array<uint8_t, 4> payload = {0x47, 0x54, 0x41, 0x34};
  REQUIRE(sender.SendTo(payload.data(), static_cast<uint32_t>(payload.size()), 0, &destination,
                        sizeof(destination)) == static_cast<int>(payload.size()));

  sockaddr_in sender_address{};
  socklen_t sender_address_size = sizeof(sender_address);
  REQUIRE(getsockname(static_cast<int>(sender.native_handle()),
                      reinterpret_cast<sockaddr*>(&sender_address), &sender_address_size) == 0);

  std::array<uint8_t, 4> received{};
  rex::system::N_XSOCKADDR_IN source{};
  uint32_t source_size = sizeof(source);
  REQUIRE(receiver.RecvFrom(received.data(), static_cast<uint32_t>(received.size()), 0, &source,
                            &source_size) == static_cast<int>(received.size()));
  CHECK(received == payload);
  CHECK(static_cast<uint32_t>(source.sin_addr) == ntohl(receiver_address.sin_addr.s_addr));
  CHECK(static_cast<uint16_t>(source.sin_port) == ntohs(sender_address.sin_port));
}

TEST_CASE("Xbox getsockname reports and retains the host-selected UDP port",
          "[live][socket][getsockname][relay]") {
  rex::system::XSocket socket(nullptr);
  REQUIRE(XSUCCEEDED(socket.Initialize(rex::system::XSocket::X_AF_INET,
                                       rex::system::XSocket::X_SOCK_DGRAM,
                                       rex::system::XSocket::X_IPPROTO_UDP)));

  rex::system::N_XSOCKADDR_IN bind_address{};
  bind_address.sin_family = AF_INET;
  bind_address.sin_port = 0;
  bind_address.sin_addr = INADDR_LOOPBACK;
  REQUIRE(XSUCCEEDED(socket.Bind(&bind_address, sizeof(bind_address))));

  rex::system::N_XSOCKADDR local_address{};
  int local_address_size = sizeof(local_address);
  REQUIRE(socket.GetSockName(&local_address, &local_address_size) == 0);
  REQUIRE(local_address_size == sizeof(rex::system::N_XSOCKADDR_IN));
  const auto& local_ipv4 =
      reinterpret_cast<const rex::system::N_XSOCKADDR_IN&>(local_address);
  CHECK(local_ipv4.sin_family == AF_INET);
  CHECK(static_cast<uint16_t>(local_ipv4.sin_port) != 0);
  CHECK(socket.bound_port() == static_cast<uint16_t>(local_ipv4.sin_port));
  CHECK(static_cast<uint32_t>(local_ipv4.sin_addr) == INADDR_LOOPBACK);
}

TEST_CASE("POSIX socket failures preserve WinSock polling semantics", "[live][socket]") {
  errno = EAGAIN;
  CHECK(rex::net::socket_last_error() == 10035);
  errno = ECONNREFUSED;
  CHECK(rex::net::socket_last_error() == 10061);

  rex::system::XSocket receiver(nullptr);
  REQUIRE(XSUCCEEDED(receiver.Initialize(rex::system::XSocket::X_AF_INET,
                                         rex::system::XSocket::X_SOCK_DGRAM,
                                         rex::system::XSocket::X_IPPROTO_UDP)));
  unsigned long nonblocking = 1;
  REQUIRE(ioctl(static_cast<int>(receiver.native_handle()), FIONBIO, &nonblocking) == 0);

  std::array<uint8_t, 4> received{};
  rex::system::N_XSOCKADDR_IN source{};
  source.sin_family = AF_INET;
  source.sin_port = 77;
  source.sin_addr = 0x10203040;
  const auto original_source = source;
  uint32_t source_size = sizeof(source);
  REQUIRE(receiver.RecvFrom(received.data(), static_cast<uint32_t>(received.size()), 0, &source,
                            &source_size) == -1);
  CHECK(rex::net::socket_last_error() == 10035);
  CHECK(source.sin_family == original_source.sin_family);
  CHECK(source.sin_port == original_source.sin_port);
  CHECK(source.sin_addr == original_source.sin_addr);
  CHECK(source_size == sizeof(source));

  REQUIRE(XSUCCEEDED(receiver.Close()));
  CHECK(XSUCCEEDED(receiver.Close()));
}

TEST_CASE("peer route addresses use network byte order", "[live][socket]") {
  in_addr address{};
  REQUIRE(inet_pton(AF_INET, "10.67.4.9", &address) == 1);

  rex::system::N_XSOCKADDR_IN guest_destination{};
  guest_destination.sin_addr = ntohl(address.s_addr);
  const uint32_t route_key = htonl(static_cast<uint32_t>(guest_destination.sin_addr));
  CHECK(route_key == address.s_addr);

  rex::system::N_XSOCKADDR_IN guest_source{};
  guest_source.sin_addr = ntohl(route_key);
  CHECK(static_cast<uint32_t>(guest_source.sin_addr) == ntohl(address.s_addr));
}
#endif

TEST_CASE("arbitration registration marshals machine-grouped roster",
          "[live][session][arbitration]") {
  constexpr uint32_t guest_address = 0x10000000;
  alignas(8) std::array<uint8_t, kSessionArbitrationResultsSize> output{};

  std::array<SessionMember, 3> members{};
  members[0].xuid = 0xE000000000008001ULL;
  members[0].machine_id = 0xA000000000001001ULL;
  members[1].xuid = 0xE000000000008002ULL;
  members[1].machine_id = 0xA000000000001001ULL;
  members[2].xuid = 0xE000000000008003ULL;
  members[2].machine_id = 0xA000000000001002ULL;

  REQUIRE(live_detail::WriteSessionArbitrationResultsToBuffer(output, guest_address, members) ==
          X_ERROR_SUCCESS);

  const auto* header = reinterpret_cast<const XSESSION_REGISTRATION_RESULTS*>(output.data());
  REQUIRE(static_cast<uint32_t>(header->registrant_count) == 2);
  REQUIRE(static_cast<uint32_t>(header->registrants_ptr) == guest_address + sizeof(*header));
  const auto* registrants =
      reinterpret_cast<const XSESSION_REGISTRANT*>(output.data() + sizeof(*header));

  const uint32_t users_offset =
      sizeof(*header) + kMaximumSessionMembers * sizeof(XSESSION_REGISTRANT);
  const uint32_t first_users_ptr = guest_address + users_offset;
  CHECK(static_cast<uint64_t>(registrants[0].machine_id) == members[0].machine_id);
  CHECK(static_cast<uint32_t>(registrants[0].trustworthy) == 1);
  REQUIRE(static_cast<uint32_t>(registrants[0].user_count) == 2);
  REQUIRE(static_cast<uint32_t>(registrants[0].users_ptr) == first_users_ptr);
  const auto* first_users =
      reinterpret_cast<const rex::be<uint64_t>*>(output.data() + users_offset);
  CHECK(static_cast<uint64_t>(first_users[0]) == members[0].xuid);
  CHECK(static_cast<uint64_t>(first_users[1]) == members[1].xuid);
  CHECK(static_cast<uint64_t>(first_users[2]) == 0);
  CHECK(static_cast<uint64_t>(first_users[3]) == 0);

  const uint32_t second_users_ptr =
      first_users_ptr + kMaximumArbitrationUsersPerMachine * sizeof(rex::be<uint64_t>);
  CHECK(static_cast<uint64_t>(registrants[1].machine_id) == members[2].machine_id);
  CHECK(static_cast<uint32_t>(registrants[1].trustworthy) == 1);
  REQUIRE(static_cast<uint32_t>(registrants[1].user_count) == 1);
  REQUIRE(static_cast<uint32_t>(registrants[1].users_ptr) == second_users_ptr);
  const auto* second_users = reinterpret_cast<const rex::be<uint64_t>*>(
      output.data() + users_offset +
      kMaximumArbitrationUsersPerMachine * sizeof(rex::be<uint64_t>));
  CHECK(static_cast<uint64_t>(second_users[0]) == members[2].xuid);
  CHECK(static_cast<uint64_t>(second_users[1]) == 0);
}

TEST_CASE("arbitration registration rejects uncertifiable or oversized rosters",
          "[live][session][arbitration]") {
  constexpr uint32_t guest_address = 0x10000000;
  alignas(8) std::array<uint8_t, kSessionArbitrationResultsSize> output{};

  SessionMember valid{.xuid = 0xE000000000009001ULL, .machine_id = 0xA000000000002001ULL};
  std::memset(output.data(), 0xA5, output.size());
  CHECK(live_detail::WriteSessionArbitrationResultsToBuffer(
            std::span<uint8_t>(output.data(), output.size() - 1), guest_address,
            std::span<const SessionMember>(&valid, 1)) == X_ERROR_INSUFFICIENT_BUFFER);
  CHECK(output[0] == 0xA5);

  SessionMember missing_machine{.xuid = 0xE000000000009002ULL};
  CHECK(live_detail::WriteSessionArbitrationResultsToBuffer(
            output, guest_address, std::span<const SessionMember>(&missing_machine, 1)) ==
        X_ERROR_INVALID_PARAMETER);
  CHECK(output[0] == 0xA5);

  std::array<SessionMember, 5> too_many_local_users{};
  const std::array<uint64_t, 5> xuids = {0xE000000000009010ULL, 0xE000000000009011ULL,
                                         0xE000000000009012ULL, 0xE000000000009013ULL,
                                         0xE000000000009014ULL};
  for (size_t index = 0; index < too_many_local_users.size(); ++index) {
    too_many_local_users[index].xuid = xuids[index];
    too_many_local_users[index].machine_id = 0xA000000000002002ULL;
  }
  CHECK(live_detail::WriteSessionArbitrationResultsToBuffer(
            output, guest_address, too_many_local_users) == X_ERROR_INVALID_PARAMETER);
  CHECK(output[0] == 0xA5);
}
