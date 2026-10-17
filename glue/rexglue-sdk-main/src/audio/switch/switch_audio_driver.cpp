/**
 ******************************************************************************
 * ReXGlue Runtime                                                            *
 ******************************************************************************
 * Copyright 2026. Released under the BSD license.                            *
 ******************************************************************************
 *
 * Switch audio driver using libnx audio renderer (audren).
 *
 * Architecture:
 *   - Uses the libnx AudioDriver high-level wrapper around audren.
 *   - A single voice outputs stereo PCM16 at 48kHz.
 *   - Double-buffered AudioDriverWaveBuf for gapless playback:
 *     while one buffer plays, the next is filled and queued.
 *   - SubmitFrame receives 6-channel big-endian float from the guest,
 *     down-mixes to stereo, byte-swaps, and converts to int16.
 */

#if REX_PLATFORM_NX

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#include <rex/assert.h>
#include <rex/audio/conversion.h>
#include <rex/audio/flags.h>
#include <rex/audio/switch/switch_audio_driver.h>
#include <rex/logging.h>

extern "C" {
#include <switch/audio/driver.h>
#include <switch/arm/cache.h>
#include <switch/arm/counter.h>
#include <switch/kernel/event.h>
#include <switch/kernel/svc.h>
#include <switch/kernel/thread.h>
#include <switch/services/audren.h>
}

// Audren memory pool buffers must be aligned to 0x1000.
static constexpr size_t kMempoolAlign = AUDREN_MEMPOOL_ALIGNMENT;
// Wave buffer data must be aligned to 0x40.
static constexpr size_t kBufferAlign = AUDREN_BUFFER_ALIGNMENT;

namespace rex::audio::nx {

namespace { std::atomic<bool> renderer_claimed{false}; }

NxAudioCounters& nx_audio_counters() {
  static NxAudioCounters counters;
  return counters;
}

SwitchAudioDriver::SwitchAudioDriver(memory::Memory* memory,
                                     rex::thread::Semaphore* semaphore)
    : AudioDriver(memory), semaphore_(semaphore) {}

SwitchAudioDriver::~SwitchAudioDriver() = default;

bool SwitchAudioDriver::Initialize() {
  // libnx owns one process-wide renderer session. Independent wrappers
  // cannot update it without corrupting shared voice state.
  bool expected = false;
  if (!renderer_claimed.compare_exchange_strong(expected, true)) {
    REXAPU_ERROR("Switch audio: a native render client is already active");
    return false;
  }
  owns_renderer_ = true;
  // Configure audio renderer. Follows the libnx `audren_user` sample: minimal
  // voice/mix/sink counts. 1 voice (stereo PCM16 downmix of the guest 5.1),
  // 1 final mix with 2 mix buffers for stereo output, 1 device sink.
  // NOTE: AudioRendererConfig in vendored libnx has no `revision` field —
  // the renderer revision is negotiated internally by audrenInitialize()
  // (uses the latest supported rev, currently AUDREN_REVISION_6).
  const AudioRendererConfig ar_config = {
      .output_rate     = AudioRendererOutputRate_48kHz,
      // libnx allocates channel slots from this capacity as well: a stereo
      // voice needs two slots even though only voice ID 0 is active.
      .num_voices      = static_cast<int>(kOutputChannels),
      .num_effects     = 0,
      .num_sinks       = 1,
      .num_mix_objs    = 1,
      .num_mix_buffers = static_cast<int>(kOutputChannels),
  };

  // STEP 1: Connect to the audren service. `audrvCreate` cannot be called
  // before this — it assumes the renderer service session exists.
  Result rc = audrenInitialize(&ar_config);
  if (R_FAILED(rc)) {
    REXAPU_ERROR("audrenInitialize failed: 0x{:08X}", rc);
    return false;
  }
  audren_service_initialized_ = true;

  // STEP 2: Create the high-level audio driver wrapper over audren.
  rc = audrvCreate(&audren_driver_, &ar_config, static_cast<int>(kOutputChannels));
  if (R_FAILED(rc)) {
    REXAPU_ERROR("audrvCreate failed: 0x{:08X}", rc);
    return false;
  }
  audren_driver_initialized_ = true;

  // Compute aligned memory pool size for double-buffered PCM16 output.
  size_t per_buf_size = kOutputFrameSize;
  per_buf_size = (per_buf_size + kBufferAlign - 1) & ~(kBufferAlign - 1);

  // Total memory pool must be aligned to kMempoolAlign.
  mempool_size_ = kWaveBufCount * per_buf_size;
  mempool_size_ = (mempool_size_ + kMempoolAlign - 1) & ~(kMempoolAlign - 1);

  // Allocate page-aligned memory for the audio pool.
  mempool_ptr_ = aligned_alloc(kMempoolAlign, mempool_size_);
  if (!mempool_ptr_) {
    REXAPU_ERROR("Failed to allocate audren memory pool ({} bytes)", mempool_size_);
    return false;
  }
  std::memset(mempool_ptr_, 0, mempool_size_);
  armDCacheFlush(mempool_ptr_, mempool_size_);

  // STEP 3 + 4: Register the PCM memory pool with audren and attach it.
  mempool_id_ = audrvMemPoolAdd(&audren_driver_, mempool_ptr_, mempool_size_);
  if (mempool_id_ < 0) {
    REXAPU_ERROR("audrvMemPoolAdd failed");
    return false;
  }
  if (!audrvMemPoolAttach(&audren_driver_, mempool_id_)) {
    REXAPU_ERROR("audrvMemPoolAttach failed");
    return false;
  }

  // Set up pointers into the memory pool for each wave buffer.
  auto* pool_bytes = static_cast<uint8_t*>(mempool_ptr_);
  for (int i = 0; i < kWaveBufCount; i++) {
    pcm_buffers_[i] = reinterpret_cast<int16_t*>(pool_bytes + i * per_buf_size);
    std::memset(&wavebufs_[i], 0, sizeof(AudioDriverWaveBuf));
    wavebufs_[i].data_pcm16 = pcm_buffers_[i];
    wavebufs_[i].size = per_buf_size;
    wavebufs_[i].start_sample_offset = 0;
    wavebufs_[i].end_sample_offset = static_cast<int32_t>(kChannelSamples);
    wavebufs_[i].state = AudioDriverWaveBufState_Done;
  }

  // Initialize a single stereo voice at 48kHz PCM16.
  if (!audrvVoiceInit(&audren_driver_, 0, static_cast<int>(kOutputChannels),
                      PcmFormat_Int16, kFrameFrequency)) {
    REXAPU_ERROR("audrvVoiceInit failed");
    return false;
  }
  voice_initialized_ = true;

  audrvVoiceSetDestinationMix(&audren_driver_, 0, AUDREN_FINAL_MIX_ID);
  audrvVoiceSetMixFactor(&audren_driver_, 0, 1.0f, 0, 0);  // left  -> left
  audrvVoiceSetMixFactor(&audren_driver_, 0, 1.0f, 1, 1);  // right -> right
  audrvVoiceSetVolume(&audren_driver_, 0, 1.0f);

  // STEP 5: Add a device sink for the default audio output.
  const u8 sink_channels[] = {0, 1};
  int sink_id = audrvDeviceSinkAdd(&audren_driver_, AUDREN_DEFAULT_DEVICE_NAME,
                                   static_cast<int>(kOutputChannels), sink_channels);
  if (sink_id < 0) {
    REXAPU_ERROR("audrvDeviceSinkAdd failed");
    return false;
  }

  // STEP 6: Push initial state to the renderer.
  rc = audrvUpdate(&audren_driver_);
  if (R_FAILED(rc)) {
    REXAPU_ERROR("Initial audrvUpdate failed: 0x{:08X}", rc);
    return false;
  }

  // STEP 7: Start the audio renderer.
  rc = audrenStartAudioRenderer();
  if (R_FAILED(rc)) {
    REXAPU_ERROR("audrenStartAudioRenderer failed: 0x{:08X}", rc);
    return false;
  }

  // No priority change here: this runs on the thread that registers the
  // audio client, i.e. the guest thread calling
  // XAudioRegisterRenderDriverClient, which it used to move to 0x28, a
  // cooperative band where a spinning guest holds its core. The Audio Worker
  // that calls SubmitFrame gets 0x2B by name (04-threads-exit.md section 7).

  // Start the voice.
  audrvVoiceStart(&audren_driver_, 0);

  return true;
}

// Returns true if the current-slot wave buffer is no longer owned by the
// renderer (Free/Done) and may be safely overwritten. Caller must hold
// `audren_mutex_` before invoking audrvUpdate.
bool SwitchAudioDriver::CurrentSlotIsReusable() {
  const auto state = wavebufs_[current_buf_].state;
  return state == AudioDriverWaveBufState_Done ||
         state == AudioDriverWaveBufState_Free;
}

// Wait for the current wave-buffer slot to transition to Done/Free before
// returning. Polls audrvUpdate every millisecond (audren renders a 5 ms
// frame). Caller passes the unique_lock owning `audren_mutex_`; this function
// drops and reacquires it around each sleep. On return the caller's lock is
// re-held.
//
// Bounded (docs/switch-port/09-rules.md): it used to wait in audrenWaitFrame()
// with no timeout, so a renderer that stopped consuming left the audio worker
// blocked forever and the game silent, with nothing in the log (the failure
// UnleashedRecomp-NX saw with SDL's audio thread). Now a wait longer than
// 100 ms is logged, and after 500 ms the caller drops the frame (the wave
// buffer is still linked in the voice's queue, so it cannot be reused): the
// worker never hangs. false = the wait timed out.
bool SwitchAudioDriver::WaitForCurrentSlotReusable(
    std::unique_lock<std::mutex>& drv_lock) {
  constexpr uint64_t kSliceNs = 1'000'000;
  constexpr uint64_t kLogAfterNs = 100'000'000;
  constexpr uint64_t kGiveUpAfterNs = 500'000'000;
  uint64_t waited_ns = 0;
  bool logged = false;
  while (!CurrentSlotIsReusable()) {
    if (waited_ns >= kGiveUpAfterNs) {
      ++slot_wait_timeouts_;
      REXAPU_WARN("Switch audio: wave buffer {} still owned by the renderer after {} ms; "
                  "dropping one frame ({} dropped so far)",
                  current_buf_, waited_ns / 1'000'000, slot_wait_timeouts_);
      return false;
    }
    if (!logged && waited_ns >= kLogAfterNs) {
      logged = true;
      REXAPU_WARN("Switch audio: waiting {} ms for wave buffer {} (renderer late or stopped)",
                  waited_ns / 1'000'000, current_buf_);
    }
    drv_lock.unlock();
    svcSleepThread(kSliceNs);
    waited_ns += kSliceNs;
    drv_lock.lock();
    const Result rc = audrvUpdate(&audren_driver_);
    if (R_FAILED(rc)) {
      REXAPU_ERROR("Switch audio wait update failed: 0x{:08X}", rc);
      return false;
    }
  }
  return true;
}

void SwitchAudioDriver::SubmitFrame(uint32_t frame_ptr) {
  // Keep guest samples stable while waiting for the renderer. Fixed stack
  // storage avoids allocating and maintaining a heap queue every 256 samples.
  alignas(16) float frame[kFrameSamples];
  std::memcpy(frame, memory_->TranslateVirtual<float*>(frame_ptr), sizeof(frame));
  bool update_failed = false;
  {
    std::unique_lock<std::mutex> guard(audren_mutex_);
    const Result rc = audrvUpdate(&audren_driver_);
    if (R_SUCCEEDED(rc)) {
      NxAudioCounters& counters = nx_audio_counters();
      // Nothing waiting, queued or playing: the output ran dry before this
      // frame came (the game's mixer was late).
      bool output_busy = false;
      for (const AudioDriverWaveBuf& wavebuf : wavebufs_) {
        output_busy |= wavebuf.state == AudioDriverWaveBufState_Waiting ||
                       wavebuf.state == AudioDriverWaveBufState_Queued ||
                       wavebuf.state == AudioDriverWaveBufState_Playing;
      }
      if (!output_busy && counters.frames.load(std::memory_order_relaxed)) {
        counters.starved.fetch_add(1, std::memory_order_relaxed);
      }
      const u64 wait_start = armGetSystemTick();
      const bool slot_free = WaitForCurrentSlotReusable(guard);
      counters.wait_ns.fetch_add(armTicksToNs(armGetSystemTick() - wait_start),
                                 std::memory_order_relaxed);
      if (slot_free) {
        ConvertAndSubmit(frame);
        counters.frames.fetch_add(1, std::memory_order_relaxed);
      } else {
        counters.dropped.fetch_add(1, std::memory_order_relaxed);
      }
    } else {
      update_failed = true;
      if (++update_failures_ == 1 || !(update_failures_ % 128)) {
        REXAPU_ERROR("Switch audio update failed: 0x{:08X} ({} failures)", rc,
                     update_failures_);
      }
    }
  }
  // A failed service must not turn returned credits into an unbounded busy
  // loop that starves rendering and floods the SD log.
  if (update_failed) svcSleepThread(5'333'333);
  // Return a producer credit even after a bounded renderer timeout so the
  // guest callback can recover instead of permanently running out of credits.
  if (semaphore_) {
    auto released = semaphore_->Release(1, nullptr);
    assert_true(released);
  }
}

// Convert a single guest 5.1 float frame to stereo PCM16 and submit to the
// current wave-buffer slot. Caller MUST hold `audren_mutex_` and MUST have
// verified that the current slot is Done/Free (see WaitForCurrentSlotReusable).
void SwitchAudioDriver::ConvertAndSubmit(const float* input) {
  // Temporary buffer for stereo float conversion.
  float stereo_float[kChannelSamples * kOutputChannels];
  conversion::sequential_6_BE_to_interleaved_2_LE(stereo_float, input, kChannelSamples);

  // Convert float [-1.0, 1.0] to int16 and write into the current PCM buffer.
  int16_t* dest = pcm_buffers_[current_buf_];
  const size_t total_samples = kChannelSamples * kOutputChannels;
  for (size_t i = 0; i < total_samples; i++) {
    float sample = stereo_float[i];
    // Clamp to [-1.0, 1.0] then scale to int16 range.
    sample = std::isfinite(sample) ? std::clamp(sample, -1.0f, 1.0f) : 0.0f;
    dest[i] = static_cast<int16_t>(sample * 32767.0f);
  }

  // Publish CPU writes before the audio renderer reads the PCM memory pool.
  armDCacheFlush(dest, kOutputFrameSize);

  // Prepare and submit the wave buffer.
  wavebufs_[current_buf_].state = AudioDriverWaveBufState_Free;
  wavebufs_[current_buf_].start_sample_offset = 0;
  wavebufs_[current_buf_].end_sample_offset = static_cast<int32_t>(kChannelSamples);
  audrvVoiceAddWaveBuf(&audren_driver_, 0, &wavebufs_[current_buf_]);
  audrvUpdate(&audren_driver_);

  // Flip to the other buffer for next submission.
  current_buf_ = (current_buf_ + 1) % kWaveBufCount;
}

void SwitchAudioDriver::Shutdown() {
  // Teardown order per libnx `audren_user` sample:
  //   audrvVoiceStop -> audrvClose -> audrenStopAudioRenderer -> audrenExit
  // Serialize against the audio worker: anything touching audrv_* needs
  // audren_mutex_.
  {
    std::unique_lock<std::mutex> drv_guard(audren_mutex_);

    if (voice_initialized_) {
      audrvVoiceStop(&audren_driver_, 0);
      audrvVoiceDrop(&audren_driver_, 0);
      voice_initialized_ = false;
    }

    if (audren_driver_initialized_) {
      if (mempool_id_ >= 0) {
        audrvMemPoolDetach(&audren_driver_, mempool_id_);
        audrvMemPoolRemove(&audren_driver_, mempool_id_);
        mempool_id_ = -1;
      }
      audrvClose(&audren_driver_);
      audren_driver_initialized_ = false;
    }

    if (audren_service_initialized_) {
      audrenStopAudioRenderer();
      audrenExit();
      audren_service_initialized_ = false;
    }
  }

  if (mempool_ptr_) {
    free(mempool_ptr_);
    mempool_ptr_ = nullptr;
  }
  if (owns_renderer_) {
    owns_renderer_ = false;
    renderer_claimed.store(false);
  }
}

}  // namespace rex::audio::nx

#endif  // REX_PLATFORM_NX
