/**
 ******************************************************************************
 * @file        live_compatibility.h
 * @brief       Title-facing Xbox Live compatibility services.
 *
 * This is a community service abstraction. It never authenticates with or
 * connects to Microsoft's Xbox Live service.
 ******************************************************************************
 */

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

namespace rex::system::xam {

namespace detail {
class LanPacketReassembler;
}

// GTA IV's extended multiplayer compatibility profile uses one authoritative
// capacity across discovery, XSession, and the community directory. Keeping
// this public prevents the three transports from silently drifting apart.
inline constexpr uint32_t kMaximumSessionMembers = 64;

struct LiveConfig;
struct LiveIdentity;
class ISessionDirectory;
class ArbitrationCancellation;
class IPeerDatagramTransport;
class ISocialService;
class IStatsService;
class IAchievementService;
class IEntitlementService;
class ITitleProfileService;
class IGta4AchievementStorageService;
class IVoicePacketTransport;
class IVoiceAudioDevice;
class IVoiceSampleCodec;
class IVoiceSampleCodecState;
class ITextChatTransport;
class IQosService;

inline constexpr size_t kQosTitleDataSize = 12;
inline constexpr uint32_t kMaximumQosTargets = 64;

struct LiveBackendServices {
  std::shared_ptr<ISessionDirectory> session_directory;
  std::shared_ptr<IPeerDatagramTransport> peer_transport;
  std::shared_ptr<ISocialService> social_service;
  std::shared_ptr<IStatsService> stats_service;
  std::shared_ptr<IAchievementService> achievement_service;
  std::shared_ptr<IEntitlementService> entitlement_service;
  std::shared_ptr<ITitleProfileService> title_profile_service;
  std::shared_ptr<IGta4AchievementStorageService> gta4_achievement_storage_service;
  std::shared_ptr<IVoicePacketTransport> voice_transport;
  std::shared_ptr<ITextChatTransport> text_chat_transport;
  std::shared_ptr<IQosService> qos_service;
};

using LiveBackendFactory =
    std::function<LiveBackendServices(const LiveConfig&, const LiveIdentity&)>;

enum class LiveBackend : uint32_t {
  kOffline,
  kLan,
  kCommunity,
};

enum class LiveState : uint32_t {
  kOffline,
  kConnecting,
  kAvailable,
  kError,
};

namespace detail {

// Derived and checked by gta4-recomp/tools/derive_community_reconnect_contract.py.
inline constexpr std::array<std::chrono::milliseconds, 6> kCommunityReconnectDelays = {
    std::chrono::milliseconds(1000), std::chrono::milliseconds(2000),
    std::chrono::milliseconds(4000), std::chrono::milliseconds(8000),
    std::chrono::milliseconds(16000), std::chrono::milliseconds(30000)};

inline std::chrono::milliseconds CommunityReconnectDelay(size_t failed_attempts) {
  const size_t index =
      std::min(failed_attempts, kCommunityReconnectDelays.size() - 1);
  return kCommunityReconnectDelays[index];
}

inline LiveState ResolveLiveServiceState(LiveBackend backend, LiveState initialized_state,
                                         bool services_ready) {
  if (backend == LiveBackend::kOffline) {
    return LiveState::kOffline;
  }
  if (backend == LiveBackend::kCommunity && initialized_state != LiveState::kOffline) {
    return services_ready ? LiveState::kAvailable : LiveState::kError;
  }
  if (initialized_state == LiveState::kAvailable && !services_ready) {
    return LiveState::kError;
  }
  return initialized_state;
}

}  // namespace detail

enum class RelayPolicy : uint32_t {
  kAuto,
  kDirectOnly,
  kRelayOnly,
};

struct LiveConfig {
  LiveBackend backend = LiveBackend::kOffline;
  RelayPolicy relay_policy = RelayPolicy::kAuto;
  uint32_t session_protocol_version = 1;
  uint16_t lan_discovery_port = 3074;
  std::string community_url;
  std::string player_name = "Player";
  LiveBackendFactory community_backend_factory;
  // Host audio capture is independent from the network packet transport.
  // Frontends that implement a real, permission-checked capture path may
  // publish its current availability here. The default is deliberately
  // unavailable; a connected voice relay is not evidence of a microphone.
  std::function<bool()> voice_capture_available;
  // XamVoice's capture/playback device is deliberately separate from the
  // community packet relay. GTA performs its own talker encoding above this
  // boundary; device packets must never be published as network packets.
  std::shared_ptr<IVoiceAudioDevice> voice_audio_device;
  // The device format is title-specific. GTA IV supplies its exact generated
  // stateful sample codec rather than duplicating that algorithm in Xam.
  std::shared_ptr<IVoiceSampleCodec> voice_sample_codec;
};

namespace detail {

inline bool HostVoiceCaptureAvailable(const LiveConfig& config) noexcept {
  if (!config.voice_capture_available) return false;
  try {
    return config.voice_capture_available();
  } catch (...) {
    return false;
  }
}

}  // namespace detail

struct LiveIdentity {
  uint64_t xuid = 0;
  uint64_t machine_id = 0;
  std::array<uint8_t, 6> ethernet_address{};
  std::array<uint8_t, 32> install_secret{};
  std::string player_name;
};

class IVoiceAudioDevice {
 public:
  virtual ~IVoiceAudioDevice() = default;
  virtual bool Open() = 0;
  virtual void Close() = 0;
  virtual bool playback_available() const noexcept = 0;
  virtual bool capture_available() const noexcept = 0;
  virtual size_t Capture(std::span<int16_t> samples,
                         std::chrono::milliseconds timeout) = 0;
  virtual bool Play(std::span<const int16_t> samples) = 0;
  virtual std::string last_error() const = 0;
};

class IVoiceSampleCodecState {
 public:
  virtual ~IVoiceSampleCodecState() = default;
  virtual bool Encode(int16_t sample, uint8_t& encoded) = 0;
  virtual bool Decode(uint8_t encoded, int16_t& sample) = 0;
};

class IVoiceSampleCodec {
 public:
  virtual ~IVoiceSampleCodec() = default;
  virtual std::unique_ptr<IVoiceSampleCodecState> CreateState() = 0;
};

inline bool IsLiveIdentitySignedIn(LiveBackend backend, const LiveIdentity& identity) {
  return backend != LiveBackend::kOffline && identity.xuid != 0;
}

struct SessionContext {
  uint32_t id = 0;
  uint32_t value = 0;

  bool operator==(const SessionContext&) const = default;
};

struct SessionProperty {
  uint32_t id = 0;
  std::vector<uint8_t> value;

  bool operator==(const SessionProperty&) const = default;
};

struct SessionMember {
  uint64_t xuid = 0;
  bool private_slot = false;
  uint64_t machine_id = 0;
  uint32_t virtual_ipv4 = 0;  // Network byte order, matching in_addr::s_addr.
  uint16_t online_port = 0;
  std::string peer_id;
  // Authoritative numeric route slot assigned by the community directory.
  // kMaximumSessionMembers means the route has not been assigned yet.
  uint32_t multiplayer_peer_id = kMaximumSessionMembers;

  bool operator==(const SessionMember&) const = default;
};

enum class SessionLifecycleState : uint32_t {
  kLobby,
  kRegistration,
  kInGame,
  kReporting,
  kDeleted,
};

struct SessionRecord {
  uint32_t title_id = 0;
  uint32_t media_id = 0;
  uint32_t title_version = 0;
  uint32_t protocol_version = 1;
  uint64_t session_id = 0;
  uint64_t previous_session_id = 0;
  std::array<uint8_t, 16> exchange_key{};
  uint64_t nonce = 0;
  uint32_t flags = 0;
  SessionLifecycleState lifecycle_state = SessionLifecycleState::kLobby;
  uint32_t max_public_slots = 0;
  uint32_t max_private_slots = 0;
  uint32_t open_public_slots = 0;
  uint32_t open_private_slots = 0;
  uint64_t host_xuid = 0;
  uint64_t host_machine_id = 0;
  uint32_t host_ipv4 = 0;  // Network byte order, matching in_addr::s_addr.
  uint16_t host_port = 0;  // Host byte order.
  std::array<uint8_t, 6> host_ethernet_address{};
  std::string host_peer_id;
  std::vector<SessionContext> contexts;
  std::vector<SessionProperty> properties;
  // Community search results intentionally omit member identities while
  // retaining authoritative open-slot counts and the host join descriptor.
  // Mutating/hosted/LAN records remain complete.
  bool roster_complete = true;
  uint32_t declared_public_members = 0;
  uint32_t declared_private_members = 0;
  std::vector<SessionMember> members;
  bool qos_listener_enabled = false;
  std::optional<std::array<uint8_t, kQosTitleDataSize>> qos_title_data;
  std::chrono::steady_clock::time_point last_seen = std::chrono::steady_clock::now();
};

struct QosTarget {
  uint64_t session_id = 0;
  std::array<uint8_t, 16> exchange_key{};
};

struct QosResult {
  bool reachable = false;
  std::optional<std::array<uint8_t, kQosTitleDataSize>> title_data;
  uint16_t probes_xmit = 0;
  uint16_t probes_recv = 0;
  uint16_t rtt_min_milliseconds = 0;
  uint16_t rtt_median_milliseconds = 0;
};

struct QosListenerUpdate {
  std::optional<bool> enabled;
  std::optional<std::array<uint8_t, kQosTitleDataSize>> title_data;
  std::optional<uint32_t> bits_per_second;
};

class IQosService {
 public:
  virtual ~IQosService() = default;
  virtual bool ready() const = 0;
  virtual std::string last_error() const = 0;
  virtual bool UpdateListener(uint64_t session_id,
                              std::span<const uint8_t, 16> exchange_key,
                              const QosListenerUpdate& update) = 0;
  virtual bool Close(uint64_t session_id, std::span<const uint8_t, 16> exchange_key) = 0;
  virtual std::vector<QosResult> Lookup(std::span<const QosTarget> targets) = 0;
};

enum class FriendPresence : uint32_t {
  kOffline,
  kOnline,
  kPlayingTitle,
};

struct FriendRecord {
  uint64_t xuid = 0;
  std::string player_name;
  FriendPresence presence = FriendPresence::kOffline;
  uint64_t session_id = 0;
  bool blocked = false;
};

enum class SocialServiceStatus : uint32_t {
  kSuccess,
  kInvalidRequest,
  kUnavailable,
  kRejected,
  kInvalidResponse,
};

struct FriendEnumerationResult {
  SocialServiceStatus status = SocialServiceStatus::kInvalidResponse;
  std::vector<FriendRecord> friends;

  [[nodiscard]] bool succeeded() const {
    return status == SocialServiceStatus::kSuccess;
  }
};

struct InvitationRecord {
  std::string id;
  uint64_t sender_xuid = 0;
  uint64_t recipient_xuid = 0;
  uint64_t session_id = 0;
  std::vector<uint8_t> custom_data;
  std::optional<SessionRecord> session;
  int64_t revision = 0;
};

enum class StatValueType : uint32_t {
  kInt32,
  kInt64,
  kDouble,
  kUnicode,
  kBinary,
  // XUSER_DATA_TYPE_UNSET is a real title-facing read value. GTA IV still
  // consumes its 24-byte column slot, but deliberately copies no payload.
  // It must never be accepted as an XSessionWriteStats property value.
  kUnset,
};

enum class StatColumnIdKind : uint32_t {
  // Title-facing stats reads use the retail 16-bit leaderboard attribute ID.
  kAttribute,
  // XSessionWriteStats supplies a complete 32-bit XUSER_PROPERTY ID.
  kProperty,
};

using StatPayload =
    std::variant<int32_t, int64_t, double, std::u16string, std::vector<uint8_t>>;

struct StatColumn {
  uint32_t id = 0;
  StatColumnIdKind id_kind = StatColumnIdKind::kAttribute;
  StatValueType type = StatValueType::kInt32;
  StatPayload value = int32_t{};
};

struct StatRow {
  uint64_t xuid = 0;
  uint32_t rank = 0;
  int64_t rating = 0;
  std::string player_name;
  std::vector<StatColumn> columns;
};

struct StatView {
  uint32_t id = 0;
  std::vector<StatRow> rows;
  // Leaderboard pages carry the global row count. Direct reads leave this
  // unset and use the number of rows returned by that read.
  std::optional<uint32_t> total_rows;
};

struct LeaderboardPage {
  uint32_t total = 0;
  std::vector<StatRow> rows;
};

enum class StatsServiceStatus : uint32_t {
  kSuccess,
  kInvalidRequest,
  kUnavailable,
  kRejected,
  kInvalidResponse,
};

struct LeaderboardResult {
  StatsServiceStatus status = StatsServiceStatus::kInvalidResponse;
  LeaderboardPage page;

  [[nodiscard]] bool succeeded() const { return status == StatsServiceStatus::kSuccess; }
};

enum class VoiceChannel : uint32_t {
  kAll,
  kTeam,
  kPrivate,
};

struct VoiceRoute {
  uint64_t session_id = 0;
  VoiceChannel channel = VoiceChannel::kAll;
  std::vector<uint64_t> target_xuids;
  std::vector<uint64_t> muted_xuids;

  bool operator==(const VoiceRoute&) const = default;
};

enum class VoiceSessionTransition : uint32_t {
  kNewSession,
  kMembershipChanged,
  kMigration,
};

struct VoicePacket {
  uint64_t source_xuid = 0;
  uint64_t session_id = 0;
  uint32_t sequence = 0;
  std::vector<uint8_t> payload;
};

enum class CachedMuteState : uint32_t {
  kMissing,
  kNotMuted,
  kMuted,
};

class ISocialService {
 public:
  virtual ~ISocialService() = default;
  virtual bool ready() const = 0;
  virtual std::string last_error() const = 0;
  virtual std::vector<bool> AreFriends(std::span<const uint64_t> xuids) = 0;
  virtual FriendEnumerationResult EnumerateFriends(uint32_t maximum_results) = 0;
  virtual bool SendInvitations(uint64_t session_id, std::span<const uint64_t> recipients,
                               std::span<const uint8_t> custom_data) = 0;
  // The title first receives an Xbox notification and copies the invite into
  // its own unaccepted-invite list.  Server acknowledgement belongs to the
  // later, explicit NETWORK_ACCEPT_INVITE action.
  virtual std::optional<InvitationRecord> AcceptInvitation(uint64_t session_id,
                                                            uint64_t sender_xuid) = 0;
  virtual std::optional<InvitationRecord> AcceptedInvitation() = 0;
  virtual bool HasAcceptedInvitation() const = 0;
  virtual CachedMuteState QueryMute(uint64_t xuid) const = 0;
  virtual void SetInviteNotificationHandler(std::function<void()> handler) = 0;
  virtual void Shutdown() = 0;
};

class IStatsService {
 public:
  virtual ~IStatsService() = default;
  virtual bool ready() const = 0;
  virtual std::string last_error() const = 0;
  virtual bool ResetView(uint32_t view_id) = 0;
  virtual bool ModifySkill(uint64_t session_id, std::span<const uint64_t> xuids) = 0;
  virtual bool Write(uint64_t session_id, uint64_t xuid,
                     std::span<const StatView> views) = 0;
  virtual bool Flush(uint64_t session_id) = 0;
  virtual std::vector<StatView> Read(std::span<const uint64_t> xuids,
                                     std::span<const uint32_t> view_ids,
                                     std::span<const uint32_t> attribute_ids) = 0;
  virtual LeaderboardResult Leaderboard(uint32_t view_id,
                                        std::span<const uint32_t> attribute_ids,
                                        uint32_t offset, uint32_t maximum_results,
                                        bool friends_only) = 0;
};

class IAchievementService {
 public:
  virtual ~IAchievementService() = default;
  virtual bool ready() const = 0;
  virtual std::string last_error() const = 0;
  virtual std::optional<std::vector<uint32_t>> FetchUnlockedAchievements() = 0;
  virtual bool MergeUnlockedAchievements(std::span<const uint32_t> achievement_ids) = 0;
};

class IEntitlementService {
 public:
  virtual ~IEntitlementService() = default;
  virtual bool ready() const = 0;
  virtual std::string last_error() const = 0;
  virtual std::optional<std::vector<std::string>> FetchEpisodePackages() = 0;
  // Called after the authenticated community connection is re-established.
  // The owner must clear the handler before destroying captured state.
  virtual void SetConnectionRestoredHandler(std::function<void()> handler) = 0;
};

// The profile service is scoped to the LiveIdentity passed to the backend
// factory. Blobs remain title-owned and opaque at this boundary.
struct TitleProfileRecord {
  uint32_t title_id = 0;
  uint64_t xuid = 0;
  int64_t revision = 0;
  std::vector<uint8_t> blob;
};

enum class TitleProfileFetchStatus : uint32_t {
  kFound,
  kNotFound,
  kError,
};

struct TitleProfileFetchResult {
  TitleProfileFetchStatus status = TitleProfileFetchStatus::kError;
  std::optional<TitleProfileRecord> record;
};

enum class TitleProfileStoreStatus : uint32_t {
  kUpdated,
  kConflict,
  kError,
};

struct TitleProfileStoreResult {
  TitleProfileStoreStatus status = TitleProfileStoreStatus::kError;
  int64_t revision = 0;
  std::optional<TitleProfileRecord> conflict_record;
};

class ITitleProfileService {
 public:
  virtual ~ITitleProfileService() = default;
  virtual bool ready() const = 0;
  virtual std::string last_error() const = 0;
  virtual TitleProfileFetchResult Fetch(uint32_t title_id) = 0;
  virtual TitleProfileStoreResult Store(uint32_t title_id, int64_t expected_revision,
                                        std::span<const uint8_t> blob) = 0;
};

inline constexpr uint32_t kGta4AchievementStorageTitleId = 0x545407F2;
inline constexpr uint32_t kGta4AchievementStorageFacility = 3;
inline constexpr std::string_view kGta4AchievementStoragePath = "Prog_ACH";
inline constexpr std::string_view kGta4AchievementStorageEndpoint =
    "/api/v3/storage/0x545407F2/3/Prog_ACH";
inline constexpr size_t kGta4AchievementStorageBlobBytes = 604;

struct Gta4AchievementStorageRecord {
  uint64_t xuid = 0;
  std::array<uint8_t, kGta4AchievementStorageBlobBytes> blob{};
};

enum class Gta4AchievementStorageFetchStatus : uint32_t {
  kFound,
  kNotFound,
  kError,
};

struct Gta4AchievementStorageFetchResult {
  Gta4AchievementStorageFetchStatus status =
      Gta4AchievementStorageFetchStatus::kError;
  std::optional<Gta4AchievementStorageRecord> record;
};

class IGta4AchievementStorageService {
 public:
  virtual ~IGta4AchievementStorageService() = default;
  virtual bool ready() const = 0;
  virtual std::string last_error() const = 0;
  virtual Gta4AchievementStorageFetchResult FetchGta4AchievementStorage() = 0;
  virtual bool StoreGta4AchievementStorage(
      std::span<const uint8_t> blob) = 0;
};

class IVoicePacketTransport {
 public:
  virtual ~IVoicePacketTransport() = default;
  virtual bool ready() const = 0;
  virtual std::string last_error() const = 0;
  virtual bool Configure(const VoiceRoute& route) = 0;
  virtual bool Send(uint32_t sequence, std::span<const uint8_t> payload) = 0;
  virtual std::optional<VoicePacket> Receive(uint32_t maximum_payload_size) = 0;
  virtual void Close() = 0;
};

enum class TextChatChannel : uint32_t {
  kAll,
  kTeam,
};

struct TextChatMessage {
  uint64_t source_xuid = 0;
  uint64_t session_id = 0;
  uint32_t sequence = 0;
  TextChatChannel channel = TextChatChannel::kAll;
  std::string player_name;
  std::string text;
};

// PC text chat is host-owned because the Xbox title contains voice routing,
// but no U/Y text-message native. Implementations must keep Send nonblocking;
// keybind callbacks run while the global bind registry mutex is held.
class ITextChatTransport {
 public:
  virtual ~ITextChatTransport() = default;
  virtual bool ready() const = 0;
  virtual std::string last_error() const = 0;
  virtual bool Configure(uint64_t session_id) = 0;
  virtual bool Send(TextChatChannel channel, std::span<const uint64_t> target_xuids,
                    uint32_t sequence, std::string_view text) = 0;
  virtual std::vector<TextChatMessage> ReceiveMessages(uint32_t maximum_messages) = 0;
  virtual void CloseTextChat() = 0;
};

// Validates every transport-visible invariant, including the shared 64-member
// cap and public/private slot accounting. Community backends must apply this
// after decoding untrusted directory responses.
bool IsValidSessionRecord(const SessionRecord& session);

enum class SessionLookupState {
  kFound,
  kAbsent,
  kUnavailable,
};

struct SessionLookupResult {
  SessionLookupState state = SessionLookupState::kUnavailable;
  std::optional<SessionRecord> session;
};

struct PeerDatagram {
  uint32_t source_ipv4 = 0;  // Network byte order, matching in_addr::s_addr.
  uint16_t source_port = 0;  // Host byte order.
  std::vector<uint8_t> payload;
};

class IPeerDatagramTransport {
 public:
  virtual ~IPeerDatagramTransport() = default;

  virtual bool ready() const = 0;
  virtual std::string last_error() const = 0;
  virtual void RegisterRoute(uint32_t virtual_ipv4, const SessionRecord& session) = 0;
  virtual void UnregisterRoute(uint32_t virtual_ipv4) = 0;
  virtual bool Send(uint32_t destination_ipv4, uint16_t destination_port, uint16_t source_port,
                    std::span<const uint8_t> payload) = 0;
  virtual bool HasPending(uint16_t local_port) = 0;
  virtual std::optional<PeerDatagram> Receive(uint16_t local_port,
                                              uint32_t maximum_payload_size) = 0;
};

class ISessionDirectory {
 public:
  virtual ~ISessionDirectory() = default;

  virtual bool ready() const = 0;
  virtual std::string last_error() const = 0;
  virtual bool Create(const SessionRecord& session) = 0;
  virtual bool Heartbeat(const SessionRecord& session) = 0;
  virtual std::vector<SessionRecord> Search(uint32_t title_id, uint32_t media_id,
                                            uint32_t title_version, uint32_t protocol_version,
                                            uint32_t procedure_index,
                                            std::span<const SessionContext> contexts,
                                            std::span<const SessionProperty> properties,
                                            uint32_t maximum_results) = 0;
  virtual std::optional<SessionRecord> Get(uint64_t session_id) = 0;
  // Get's optional result cannot distinguish an authoritative 404 from a
  // transport or decode failure. LeaveLocal needs that distinction after a
  // host has already kicked this machine from a private session.
  virtual SessionLookupResult Lookup(uint64_t session_id) {
    auto session = Get(session_id);
    if (session) {
      return {.state = SessionLookupState::kFound,
              .session = std::move(session)};
    }
    return {.state = ready() ? SessionLookupState::kAbsent
                             : SessionLookupState::kUnavailable,
            .session = std::nullopt};
  }
  // Resolves the title's previous XNKID after host migration without making
  // the replacement publicly discoverable. Community directories may map
  // the old ID server-side; local directories retain the alias in the
  // replacement record itself.
  virtual std::optional<SessionRecord> ResolveMigration(uint64_t previous_session_id) {
    auto replacement = Get(previous_session_id);
    if (!replacement || replacement->session_id == previous_session_id ||
        replacement->previous_session_id != previous_session_id) {
      return std::nullopt;
    }
    return replacement;
  }
  virtual bool Modify(const SessionRecord& session) = 0;
  virtual bool Join(uint64_t session_id, const SessionMember& member) = 0;
  virtual bool Leave(uint64_t session_id, uint64_t xuid) = 0;
  virtual std::optional<SessionRecord> Migrate(
      uint64_t session_id, const SessionRecord& replacement) = 0;
  virtual bool Delete(uint64_t session_id) = 0;
  // Atomically freezes and returns the authoritative ranked registrant roster.
  // Community backends override this; local directories retain the existing
  // title-owned registration path by returning no value.
  virtual std::optional<SessionRecord> RegisterArbitration(
      uint64_t session_id, uint64_t nonce, uint32_t duration_seconds,
      uint32_t flags,
      const std::shared_ptr<ArbitrationCancellation>& cancellation = {}) {
    return std::nullopt;
  }
  // A follower resolves host migration through ResolveMigration rather than Migrate, so
  // the title-facing session owner must explicitly republish the replacement
  // session as its current presence. Directories without remote presence are
  // intentionally no-ops.
  virtual void PublishLocalSessionPresence(uint64_t session_id) {}
};

class InMemorySessionDirectory final : public ISessionDirectory {
 public:
  bool ready() const override { return true; }
  std::string last_error() const override { return {}; }
  bool Create(const SessionRecord& session) override;
  bool Heartbeat(const SessionRecord& session) override;
  std::vector<SessionRecord> Search(uint32_t title_id, uint32_t media_id, uint32_t title_version,
                                    uint32_t protocol_version, uint32_t procedure_index,
                                    std::span<const SessionContext> contexts,
                                    std::span<const SessionProperty> properties,
                                    uint32_t maximum_results) override;
  std::optional<SessionRecord> Get(uint64_t session_id) override;
  std::optional<SessionRecord> ResolveMigration(uint64_t previous_session_id) override;
  bool Modify(const SessionRecord& session) override;
  bool Join(uint64_t session_id, const SessionMember& member) override;
  bool Leave(uint64_t session_id, uint64_t xuid) override;
  std::optional<SessionRecord> Migrate(
      uint64_t session_id, const SessionRecord& replacement) override;
  bool Delete(uint64_t session_id) override;

 private:
  std::mutex mutex_;
  std::unordered_map<uint64_t, SessionRecord> sessions_;
};

class LanSessionDirectory final : public ISessionDirectory {
 public:
  explicit LanSessionDirectory(uint16_t discovery_port);
  ~LanSessionDirectory() override;

  bool ready() const override { return ready_.load(std::memory_order_acquire); }
  std::string last_error() const override;
  bool Create(const SessionRecord& session) override;
  bool Heartbeat(const SessionRecord& session) override;
  std::vector<SessionRecord> Search(uint32_t title_id, uint32_t media_id, uint32_t title_version,
                                    uint32_t protocol_version, uint32_t procedure_index,
                                    std::span<const SessionContext> contexts,
                                    std::span<const SessionProperty> properties,
                                    uint32_t maximum_results) override;
  std::optional<SessionRecord> Get(uint64_t session_id) override;
  std::optional<SessionRecord> ResolveMigration(uint64_t previous_session_id) override;
  bool Modify(const SessionRecord& session) override;
  bool Join(uint64_t session_id, const SessionMember& member) override;
  bool Leave(uint64_t session_id, uint64_t xuid) override;
  std::optional<SessionRecord> Migrate(
      uint64_t session_id, const SessionRecord& replacement) override;
  bool Delete(uint64_t session_id) override;

 private:
  void WorkerMain();
  bool OpenSocket();
  void CloseSocket();
  void SetError(std::string message);
  bool SendQuery();
  bool SendPayload(uint8_t operation, std::span<const uint8_t> payload);
  bool SendRecord(const SessionRecord& session, uint8_t operation);
  void ReceivePacket();
  void ExpireRecords();

  uint16_t discovery_port_;
  std::atomic<bool> running_{true};
  std::atomic<bool> ready_{false};
  std::thread worker_;
  mutable std::mutex mutex_;
  std::condition_variable search_condition_;
  std::unordered_map<uint64_t, SessionRecord> hosted_sessions_;
  std::unordered_map<uint64_t, SessionRecord> discovered_sessions_;
  std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> deletion_tombstones_;
  std::unique_ptr<detail::LanPacketReassembler> reassembler_;
  std::atomic<uint64_t> next_message_id_{1};
  std::string last_error_;
  intptr_t socket_ = -1;
};

class LiveCompatibilityRuntime {
 public:
  LiveCompatibilityRuntime(const LiveConfig& config, const std::filesystem::path& user_data_root);
  ~LiveCompatibilityRuntime();

  LiveState state() const;
  // Xbox user sign-in is an identity property, not a health check for the
  // selected session directory. A transient Community outage must not turn a
  // valid emulated LIVE profile into a local-only profile.
  bool signed_in() const {
    return IsLiveIdentitySignedIn(config_.backend, identity_);
  }
  bool available() const { return state() == LiveState::kAvailable; }
  const LiveConfig& config() const { return config_; }
  const LiveIdentity& identity() const { return identity_; }
  ISessionDirectory* session_directory() const { return session_directory_.get(); }
  IPeerDatagramTransport* peer_transport() const { return peer_transport_.get(); }
  ISocialService* social_service() const { return social_service_.get(); }
  IStatsService* stats_service() const { return stats_service_.get(); }
  IAchievementService* achievement_service() const { return achievement_service_.get(); }
  IEntitlementService* entitlement_service() const { return entitlement_service_.get(); }
  ITitleProfileService* title_profile_service() const { return title_profile_service_.get(); }
  IGta4AchievementStorageService* gta4_achievement_storage_service() const {
    return gta4_achievement_storage_service_.get();
  }
  IVoicePacketTransport* voice_transport() const { return voice_transport_.get(); }
  IVoiceAudioDevice* voice_audio_device() const { return config_.voice_audio_device.get(); }
  IVoiceSampleCodec* voice_sample_codec() const { return config_.voice_sample_codec.get(); }
  bool voice_capture_available() const noexcept;
  bool ConfigureVoiceSession(uint64_t session_id, VoiceSessionTransition transition);
  bool ConfigureVoiceRoute(VoiceRoute route);
  void CloseVoiceSession(uint64_t session_id);
  std::optional<VoiceRoute> configured_voice_route() const;
  ITextChatTransport* text_chat_transport() const { return text_chat_transport_.get(); }
  IQosService* qos_service() const { return qos_service_.get(); }
  uint64_t active_session_id() const {
    return active_session_id_.load(std::memory_order_acquire);
  }
  void SetActiveSession(uint64_t session_id) {
    active_session_id_.store(session_id, std::memory_order_release);
  }
  void ClearActiveSession(uint64_t session_id) {
    uint64_t expected = session_id;
    active_session_id_.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
  }

  uint32_t local_ipv4() const { return local_ipv4_; }
  uint16_t online_port() const { return online_port_.load(std::memory_order_acquire); }
  void ObserveBoundPort(uint16_t port);

  bool SetUserContext(uint32_t id, uint32_t value);
  std::optional<uint32_t> GetUserContext(uint32_t id) const;
  std::vector<SessionContext> user_contexts() const;
  bool SetUserProperty(uint32_t id, std::span<const uint8_t> value);
  std::vector<SessionProperty> user_properties() const;

  uint64_t GenerateSessionId();
  uint64_t GenerateNonce();
  void GenerateExchangeKey(std::span<uint8_t, 16> key);
  void FillRandomBytes(std::span<uint8_t> output);
  bool IsPrivilegeAllowed(uint32_t privilege) const;

  void RegisterRoute(uint32_t ipv4, const SessionRecord& session);
  std::optional<SessionRecord> FindRoute(uint32_t ipv4) const;
  std::optional<SessionRecord> FindSessionRoute(uint64_t session_id) const;
  std::optional<SessionMember> FindLocalSessionMember(uint64_t session_id) const;
  void UnregisterRoute(uint32_t ipv4);
  void RegisterKey(uint64_t session_id, std::span<const uint8_t, 16> key);
  bool IsKeyRegistered(uint64_t session_id) const;
  std::optional<std::array<uint8_t, 16>> RegisteredKey(uint64_t session_id) const;
  void UnregisterKey(uint64_t session_id);
  bool SendPeerDatagram(uint32_t destination_ipv4, uint16_t destination_port, uint16_t source_port,
                        std::span<const uint8_t> payload);
  bool HasPendingPeerDatagram(uint16_t local_port);
  std::optional<PeerDatagram> ReceivePeerDatagram(uint16_t local_port,
                                                  uint32_t maximum_payload_size);
  void SetInviteNotificationHandler(std::function<void()> handler);
  void Shutdown();

 private:
  bool LoadOrCreateIdentity(const std::filesystem::path& user_data_root);
  bool ServicesReady() const;
  void HeartbeatWorkerMain();
  static uint32_t DiscoverLocalIpv4();
  static uint64_t RandomU64();
  static void FillRandom(std::span<uint8_t> output);

  LiveConfig config_;
  std::atomic<LiveState> state_{LiveState::kOffline};
  LiveIdentity identity_;
  std::shared_ptr<ISessionDirectory> session_directory_;
  std::shared_ptr<IPeerDatagramTransport> peer_transport_;
  std::shared_ptr<ISocialService> social_service_;
  std::shared_ptr<IStatsService> stats_service_;
  std::shared_ptr<IAchievementService> achievement_service_;
  std::shared_ptr<IEntitlementService> entitlement_service_;
  std::shared_ptr<ITitleProfileService> title_profile_service_;
  std::shared_ptr<IGta4AchievementStorageService> gta4_achievement_storage_service_;
  std::shared_ptr<IVoicePacketTransport> voice_transport_;
  std::shared_ptr<ITextChatTransport> text_chat_transport_;
  std::shared_ptr<IQosService> qos_service_;
  std::atomic<uint64_t> active_session_id_{0};
  mutable std::mutex voice_route_mutex_;
  std::optional<VoiceRoute> configured_voice_route_;
  uint32_t local_ipv4_ = 0;
  std::atomic<uint16_t> online_port_{3074};

  std::mutex heartbeat_mutex_;
  std::condition_variable heartbeat_condition_;
  bool heartbeat_stopping_ = false;
  bool community_recovery_enabled_ = false;
  std::thread heartbeat_worker_;
  std::once_flag shutdown_once_;

  mutable std::mutex user_data_mutex_;
  std::unordered_map<uint32_t, uint32_t> user_contexts_;
  std::unordered_map<uint32_t, std::vector<uint8_t>> user_properties_;

  mutable std::mutex route_mutex_;
  std::unordered_map<uint32_t, SessionRecord> routes_;
  std::unordered_map<uint64_t, std::array<uint8_t, 16>> registered_keys_;
};

}  // namespace rex::system::xam
