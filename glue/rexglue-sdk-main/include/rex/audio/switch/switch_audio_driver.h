/**
 ******************************************************************************
 * ReXGlue Runtime                                                            *
 ******************************************************************************
 * Copyright 2026. Released under the BSD license.                            *
 ******************************************************************************
 *
 * Switch audio driver using libnx audren (audio renderer) API.
 * Uses the high-level AudioDriver wrapper from libnx for voice-based PCM
 * output with eight-buffered wave buffers for gapless playback.
 */

#pragma once

#if REX_PLATFORM_NX

#include <atomic>
#include <cstdint>
#include <mutex>

#include <rex/audio/audio_driver.h>
#include <rex/thread.h>

extern "C" {
#include <switch/audio/driver.h>
#include <switch/services/audren.h>
}

namespace rex::audio::nx {

// Since start, for switch-game's heartbeat (run 2's sound came in bursts).
struct NxAudioCounters {
  std::atomic<uint64_t> frames{0};   // frames handed to the renderer
  std::atomic<uint64_t> starved{0};  // frames that found nothing queued: the output had run dry
  std::atomic<uint64_t> dropped{0};  // frames dropped after a 500 ms wait for a buffer
  std::atomic<uint64_t> wait_ns{0};  // waiting for a free buffer (the game was ahead)
};
NxAudioCounters& nx_audio_counters();

class SwitchAudioDriver : public AudioDriver {
 public:
  SwitchAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore);
  ~SwitchAudioDriver() override;

  bool Initialize();
  void SubmitFrame(uint32_t frame_ptr) override;
  void Shutdown();
  uint32_t RecommendedInitialCredits(uint32_t maximum) const override {
    return maximum < kWaveBufCount ? maximum : kWaveBufCount;
  }

 protected:
  rex::thread::Semaphore* semaphore_ = nullptr;

  // libnx audio renderer state
  ::AudioDriver audren_driver_ = {};
  int mempool_id_ = -1;
  bool audren_service_initialized_ = false;
  bool audren_driver_initialized_ = false;
  bool voice_initialized_ = false;
  bool owns_renderer_ = false;
  // Serializes access to audrv* calls (audrvUpdate is NOT thread-safe).
  std::mutex audren_mutex_ = {};

  // Audio format constants matching Xbox 360 XMA output:
  // 6-channel (5.1) 48kHz float, 256 samples per channel per frame.
  // Switch audren expects PCM16, so we convert float BE -> int16 LE stereo.
  static const uint32_t kFrameFrequency = 48000;
  static const uint32_t kFrameChannels = 6;
  static const uint32_t kChannelSamples = 256;
  static const uint32_t kFrameSamples = kFrameChannels * kChannelSamples;
  static const uint32_t kFrameSize = sizeof(float) * kFrameSamples;

  // Output is stereo PCM16 for the Switch speaker/headphone sink.
  static const uint32_t kOutputChannels = 2;
  static const uint32_t kOutputSampleBytes = sizeof(int16_t);
  static const uint32_t kOutputFrameSize =
      kChannelSamples * kOutputChannels * kOutputSampleBytes;

  // Eight-buffer wave buffers for gapless playback.
  static const int kWaveBufCount = 8;
  AudioDriverWaveBuf wavebufs_[kWaveBufCount] = {};
  int16_t* pcm_buffers_[kWaveBufCount] = {};
  int current_buf_ = 0;
  uint64_t slot_wait_timeouts_ = 0;  // frames dropped by WaitForCurrentSlotReusable
  uint64_t update_failures_ = 0;     // audren_mutex_ protects this counter

  // Aligned memory pool registered with audren.
  void* mempool_ptr_ = nullptr;
  size_t mempool_size_ = 0;

  void ConvertAndSubmit(const float* input);

  // True iff `wavebufs_[current_buf_]` is no longer owned by the renderer
  // (state is Done or Free) and may be safely overwritten. Caller holds
  // `audren_mutex_`.
  bool CurrentSlotIsReusable();
  // Block (poll via audrenWaitFrame + audrvUpdate) until the current wave
  // buffer slot is reusable. Caller's unique_lock owns `audren_mutex_` on
  // entry; the function may drop and reacquire it, but returns with it held.
  bool WaitForCurrentSlotReusable(std::unique_lock<std::mutex>& drv_lock);
};

}  // namespace rex::audio::nx

#endif  // REX_PLATFORM_NX
