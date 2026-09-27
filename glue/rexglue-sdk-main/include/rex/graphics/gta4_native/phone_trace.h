#pragma once

// Diagnostic transport only. No guest ABI, render state, or command-layout changes.
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <fmt/format.h>
#include <rex/logging.h>

namespace rex::graphics::gta4_native {

inline constexpr uint32_t kPhoneTraceEnvelopeAbi = 0x50484F4E;
inline constexpr uint32_t kPhoneTraceEnvelopeVersion = 1;

inline uint32_t PhoneTraceEnvUnsigned(const char* name, uint32_t fallback,
                                      uint32_t maximum = UINT32_MAX) {
  const char* text = std::getenv(name);
  if (!text || !*text) return fallback;
  char* end = nullptr;
  errno = 0;
  const unsigned long long value = std::strtoull(text, &end, 10);
  if (errno || end == text || *end || *text == '-' || value > maximum) return fallback;
  return uint32_t(value);
}

struct PhoneTraceConfiguration {
  bool enabled = false;
  bool readbacks = false;
  bool lineage = false;
  bool allow_closed = false;
  bool fade = false;  // Opt-in cutscene transition diagnostics.
  uint32_t frames = 4;
  uint32_t command_limit = 65536;
  uint32_t probe_first = UINT32_MAX;  // Otherwise an exact native frame command index.
  uint32_t probe_count = 12;
  std::string arm_file;
  std::string output_directory;
};

inline const PhoneTraceConfiguration& PhoneTraceConfig() {
  static const PhoneTraceConfiguration config = [] {
    PhoneTraceConfiguration c;
    const char* enabled = std::getenv("REX_GTA4_PHONE_TRACE");
    c.enabled = enabled && std::strcmp(enabled, "1") == 0;
    c.readbacks = c.enabled && PhoneTraceEnvUnsigned("REX_GTA4_PHONE_READBACKS", 0, 1);
    c.lineage = c.enabled && PhoneTraceEnvUnsigned("REX_GTA4_PHONE_LINEAGE", 0, 1);
    c.allow_closed = c.enabled && PhoneTraceEnvUnsigned("REX_GTA4_PHONE_ALLOW_CLOSED", 0, 1);
    c.fade = c.enabled && PhoneTraceEnvUnsigned("REX_GTA4_FADE_TRACE", 0, 1);
    c.frames = std::max(2u, PhoneTraceEnvUnsigned("REX_GTA4_PHONE_FRAMES", 4, 16));
    c.command_limit = std::max(1u, PhoneTraceEnvUnsigned("REX_GTA4_PHONE_COMMAND_LIMIT", 65536, 262144));
    c.probe_first = PhoneTraceEnvUnsigned("REX_GTA4_PHONE_PROBE_FIRST", UINT32_MAX);
    c.probe_count = std::clamp(PhoneTraceEnvUnsigned("REX_GTA4_PHONE_PROBE_COUNT", 12, 12), 1u, 12u);
    if (const char* p = std::getenv("REX_GTA4_PHONE_ARM_FILE")) c.arm_file = p;
    if (const char* p = std::getenv("REX_GTA4_PHONE_OUTPUT")) c.output_directory = p;
    return c;
  }();
  return config;
}

enum class PhoneTraceScope : uint32_t {
  kOutside, kWorldBuilder, kPhoneBuilder, kPhoneClear, kPhoneModel,
  kScript2DBuilder, kScript2DPopulation, kScreenFadeBuilder, kScreenColorDraw,
};

struct PhoneGuestState {
  // RT[0..3], depth, declaration, index buffer, viewport[6], API scissor enabled,
  // API scissor[4], logical viewport pointer, PHONE_SCREEN wrapper pointer.
  std::array<uint32_t, 20> words{};
  uint64_t fixed_hash = 0;
  uint64_t constants_hash = 0;
  uint64_t fetch_hash = 0;
  uint64_t booleans = 0;
  bool operator==(const PhoneGuestState&) const = default;
};

struct PhoneTraceContext {
  uint64_t run = 0;
  uint64_t event = 0;
  uint64_t occurrence = 0;
  uint64_t dc_event = 0;
  uint64_t captured_event = 0;
  uint32_t scope = 0;
  uint32_t object = 0;
  uint32_t dc = 0;
  uint32_t dc_token = 0;
  uint32_t caller = 0;
  uint32_t model = 0;
  uint32_t material = 0;
  uint32_t technique = 0;
  uint32_t pass = 0;
  uint32_t mode = UINT32_MAX;
  uint32_t replay = 0;
  uint32_t capture_object = 0;
  uint32_t capture_ordinal = 0;
  uint32_t command_list = 0;
  uint32_t device = 0;
  uint32_t guest_frame = 0;
  uint32_t snapshot_valid = 0;
  PhoneGuestState live{};
  PhoneGuestState applied{};
};

struct alignas(8) PhoneTraceEnvelope {
  uint32_t version = kPhoneTraceEnvelopeVersion;
  uint32_t command_abi = 0;
  uint32_t command_size = 0;
  uint32_t reserved = 0;
  PhoneTraceContext context{};
};
static_assert(std::is_trivially_copyable_v<PhoneTraceEnvelope>);
static_assert(sizeof(PhoneTraceEnvelope) % alignof(uint64_t) == 0);

inline bool UnpackPhoneTraceEnvelope(const void*& command, size_t& size, uint32_t& abi,
                                     PhoneTraceContext& context) {
  if (abi != kPhoneTraceEnvelopeAbi || !command || size < sizeof(PhoneTraceEnvelope)) return false;
  PhoneTraceEnvelope envelope{};
  std::memcpy(&envelope, command, sizeof(envelope));
  if (envelope.version != kPhoneTraceEnvelopeVersion || !envelope.context.run ||
      !envelope.context.event || envelope.command_abi == kPhoneTraceEnvelopeAbi ||
      envelope.command_size != size - sizeof(envelope) || !envelope.command_size) return false;
  context = envelope.context;
  command = static_cast<const uint8_t*>(command) + sizeof(envelope);
  size = envelope.command_size;
  abi = envelope.command_abi;
  return true;
}

inline std::vector<uint8_t> PackPhoneTraceEnvelope(const void* command, size_t size,
                                                   uint32_t abi,
                                                   const PhoneTraceContext& context) {
  PhoneTraceEnvelope envelope{};
  envelope.command_abi = abi;
  envelope.command_size = uint32_t(size);
  envelope.context = context;
  std::vector<uint8_t> bytes(sizeof(envelope) + size);
  std::memcpy(bytes.data(), &envelope, sizeof(envelope));
  std::memcpy(bytes.data() + sizeof(envelope), command, size);
  return bytes;
}

inline void PhoneTraceLog(std::string_view point, std::string_view details) {
  if (!PhoneTraceConfig().enabled) return;
  static std::atomic<uint64_t> records{0};
  const uint64_t index = records.fetch_add(1, std::memory_order_relaxed);
  constexpr uint64_t limit = 524288;
  if (index < limit) {
    REXLOG_INFO("gta4-native-phone: point={} {}", point, details);
  } else if (index == limit) {
    REXLOG_ERROR("gta4-native-phone: point=incomplete reason=record-budget-exhausted limit={}", limit);
  }
}

inline std::string PhoneGuestStateText(const PhoneGuestState& s) {
  std::string words;
  for (uint32_t word : s.words) {
    if (!words.empty()) words += ',';
    words += fmt::format("{:08X}", word);
  }
  return fmt::format("words={} fixed={:016X} constants={:016X} fetch={:016X} booleans={:016X}",
                     words, s.fixed_hash, s.constants_hash, s.fetch_hash, s.booleans);
}

inline std::filesystem::path PhoneTraceOutputDirectory(uint64_t run) {
  const auto& configured = PhoneTraceConfig().output_directory;
  std::error_code error;
  auto parent = configured.empty() ? std::filesystem::temp_directory_path(error)
                                   : std::filesystem::path(configured);
  if (error) return {};
  return parent / fmt::format("liberty-phone-{}", run);
}

struct PhoneProbeIdentity {
  PhoneTraceContext context{};
  uint64_t submit_sequence = 0;
  uint64_t submission = 0;
  uint64_t image = 0;
  uint64_t lifetime = 0;
  uint64_t writer_serial = 0;
  uint32_t epoch = 0;
  uint32_t frame = 0;
  uint32_t command_index = 0;
  uint32_t point = 0;  // 0 before scope preparation, 1 after loads, 2 after command.
  uint32_t attachment = 0;
  uint32_t bytes_per_texel = 0;
  uint64_t ancestor_serial = 0;
  uint64_t resource_generation = 0;
  uint32_t resolve_frame = 0;
  std::string role;
};

}  // namespace rex::graphics::gta4_native
