#pragma once

#include <functional>

namespace gta4::voice {

enum class MicrophonePermissionStatus {
  kNotDetermined,
  kAuthorized,
  kDenied,
  kRestricted,
  kUnknown,
};

// Both operations are nonblocking. The completion callback is invoked after
// macOS has completed a newly-started permission request; callers must query
// the status again because authorization can also change in System Settings.
MicrophonePermissionStatus GetMicrophonePermissionStatus() noexcept;
void RequestMicrophonePermission(std::function<void()> completion);

}  // namespace gta4::voice
