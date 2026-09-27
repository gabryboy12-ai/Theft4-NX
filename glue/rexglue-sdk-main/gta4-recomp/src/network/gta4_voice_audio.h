#pragma once

#include <memory>

namespace rex::system::xam {
class IVoiceAudioDevice;
class IVoiceSampleCodec;
}  // namespace rex::system::xam

namespace gta4::voice {

std::shared_ptr<rex::system::xam::IVoiceAudioDevice> CreateAudioDevice();
std::shared_ptr<rex::system::xam::IVoiceSampleCodec> CreateSampleCodec();

namespace detail {

struct VoiceDeviceLifecycleInputs {
  bool active = false;
  bool permission_granted = false;
  bool playback_device_available = false;
  bool capture_device_available = false;
  bool playback_stream_open = false;
  bool capture_stream_open = false;
  bool playback_stream_invalidated = false;
  bool capture_stream_invalidated = false;
};

struct VoiceDeviceLifecycleActions {
  bool close_playback = false;
  bool close_capture = false;
  bool open_playback = false;
  bool open_capture = false;
};

constexpr VoiceDeviceLifecycleActions EvaluateVoiceDeviceLifecycle(
    const VoiceDeviceLifecycleInputs& inputs) noexcept {
  const bool wants_playback = inputs.active && inputs.playback_device_available;
  const bool wants_capture =
      inputs.active && inputs.permission_granted && inputs.capture_device_available;
  return {
      .close_playback =
          inputs.playback_stream_open && (!wants_playback || inputs.playback_stream_invalidated),
      .close_capture =
          inputs.capture_stream_open && (!wants_capture || inputs.capture_stream_invalidated),
      .open_playback =
          wants_playback && (!inputs.playback_stream_open || inputs.playback_stream_invalidated),
      .open_capture =
          wants_capture && (!inputs.capture_stream_open || inputs.capture_stream_invalidated),
  };
}

}  // namespace detail

}  // namespace gta4::voice
