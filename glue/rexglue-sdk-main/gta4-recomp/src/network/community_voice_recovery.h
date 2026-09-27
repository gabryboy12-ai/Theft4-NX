#pragma once

#include <charconv>
#include <cstdint>
#include <string_view>

namespace gta4::network::detail {

inline bool IsCanonicalVoiceCursor(std::string_view cursor) {
  static constexpr std::string_view kPrefix = "cursor_";
  static constexpr std::size_t kMaximumDecimalDigits = 20;
  if (!cursor.starts_with(kPrefix))
    return false;
  cursor.remove_prefix(kPrefix.size());
  if (cursor.empty() || cursor.size() > kMaximumDecimalDigits || cursor.front() == '0') {
    return false;
  }
  std::uint64_t value = 0;
  const auto parsed = std::from_chars(cursor.data(), cursor.data() + cursor.size(), value);
  return parsed.ec == std::errc{} && parsed.ptr == cursor.data() + cursor.size();
}

inline bool IsAuthoritativeVoiceRouteLoss(long http_status) {
  return http_status == 403 || http_status == 404;
}

// Protected by CommunityImportedServices::voice_mutex_.  This gate keeps the
// send and receive workers from racing two replacement-route requests after a
// lease expires or libserver restarts.
struct VoiceRouteRecoveryGate {
  bool configured = false;
  bool in_progress = false;

  void OnConfigured() {
    configured = true;
    in_progress = false;
  }

  void OnClosed() {
    configured = false;
    in_progress = false;
  }

  bool Begin(long http_status, bool observed_route_is_current, bool retry_window_open) {
    if (!configured || in_progress || !observed_route_is_current || !retry_window_open ||
        !IsAuthoritativeVoiceRouteLoss(http_status)) {
      return false;
    }
    in_progress = true;
    return true;
  }

  bool CanCommit(bool observed_route_is_current, bool configured_route_is_current) const {
    return configured && in_progress && observed_route_is_current && configured_route_is_current;
  }

  void Finish() { in_progress = false; }
};

}  // namespace gta4::network::detail
