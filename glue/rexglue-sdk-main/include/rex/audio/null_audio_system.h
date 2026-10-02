/**
 * @file        audio/null_audio_system.h
 * @brief       An audio system that keeps the game's audio engine in step without sound
 *
 * For a runtime without audio output (Switch today, the PC build with
 * pc_audio off; 06-game-boot-plan.md sections 19 and 21), modelled on the
 * null GPU: XAudioRegisterRenderDriverClient finds an audio system and
 * succeeds, the guest render callback of each client is called as the real
 * drivers make it be called (a burst of initial credits, then one call per
 * frame played), and the frames the game submits are counted and dropped.
 * A frame is 256 samples per channel at 48 kHz, so one is "played" every
 * 5.333 ms per client. It owns an XmaDecoder as AudioSystem does, because the
 * XMA exports reach the decoder through the audio system.
 *
 * Implements system::IAudioSystem directly: the desktop AudioSystem (its
 * worker, semaphores and drivers) is not built on Switch (REXGLUE_CORE_ONLY).
 */

#pragma once

#include <cstdint>
#include <memory>

#include <rex/cvar.h>
#include <rex/system/interfaces/audio.h>

REXCVAR_DECLARE(bool, nx_null_audio);

namespace rex::runtime {
class FunctionDispatcher;
}

namespace rex::audio {

std::unique_ptr<system::IAudioSystem> CreateNullAudioSystem(
    runtime::FunctionDispatcher* function_dispatcher);

// Counters of the null audio system created last (all zero if none), for logs.
struct NullAudioStats {
  uint32_t clients = 0;          // registered render clients
  uint64_t callbacks = 0;        // guest render callbacks called
  uint64_t frames_submitted = 0; // XAudioSubmitRenderDriverFrame
  uint64_t frames_played = 0;    // frames dropped at the real rate
};
NullAudioStats GetNullAudioStats();

}  // namespace rex::audio
