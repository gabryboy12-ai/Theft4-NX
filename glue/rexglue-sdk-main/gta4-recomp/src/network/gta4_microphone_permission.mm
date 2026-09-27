#include "network/gta4_microphone_permission.h"

#import <AVFoundation/AVFoundation.h>

#include <memory>
#include <utility>

namespace gta4::voice {

MicrophonePermissionStatus GetMicrophonePermissionStatus() noexcept {
  @autoreleasepool {
    switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]) {
      case AVAuthorizationStatusNotDetermined:
        return MicrophonePermissionStatus::kNotDetermined;
      case AVAuthorizationStatusAuthorized:
        return MicrophonePermissionStatus::kAuthorized;
      case AVAuthorizationStatusDenied:
        return MicrophonePermissionStatus::kDenied;
      case AVAuthorizationStatusRestricted:
        return MicrophonePermissionStatus::kRestricted;
      default:
        return MicrophonePermissionStatus::kUnknown;
    }
  }
}

void RequestMicrophonePermission(std::function<void()> completion) {
  @autoreleasepool {
    auto shared_completion =
        std::make_shared<std::function<void()>>(std::move(completion));
    [AVCaptureDevice
        requestAccessForMediaType:AVMediaTypeAudio
                  completionHandler:^(BOOL) {
                    if (*shared_completion) (*shared_completion)();
                  }];
  }
}

}  // namespace gta4::voice
