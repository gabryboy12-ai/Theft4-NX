#pragma once

// In-session A/B switch for measurements on the console.
//
// When SkipGpuWork() is set, the command processor still parses the whole PM4
// stream (registers, constants, fences, interrupts, swaps, occlusion queries)
// but does not hand draws or resolves to the backend. The guest keeps running
// with its frames presented as before (the image freezes on the last resolved
// frame), so the swap rate measures how fast the game's own threads plus PM4
// parsing can go when no GPU emulation work is done.

#include <atomic>
#include <cstdint>

namespace rex::graphics::nx_ab {

inline std::atomic<bool>& SkipGpuWork() {
  static std::atomic<bool> value{false};
  return value;
}

// Draw packets that reached the backend decision point (guest draws, resolves
// included), whether they were executed or skipped. Written only by the GPU
// command processor thread.
inline std::atomic<uint64_t>& GuestDraws() {
  static std::atomic<uint64_t> value{0};
  return value;
}

inline std::atomic<uint64_t>& SkippedDraws() {
  static std::atomic<uint64_t> value{0};
  return value;
}

// XE_SWAP packets (one per guest VdSwap), counted by the command processor.
inline std::atomic<uint64_t>& GuestSwaps() {
  static std::atomic<uint64_t> value{0};
  return value;
}

// Single-writer increment without a load-exclusive/store-exclusive loop
// (Cortex-A57 has no LSE atomics).
inline void BumpSingleWriter(std::atomic<uint64_t>& counter) {
  counter.store(counter.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

// C4: which EDRAM ownership transfers (copies of a render target's previous
// contents into the render target that takes over its EDRAM range) to skip.
// 0 = none (all copied), 1 = transfers between depth and color, 2 = all.
// Skipping leaves whatever the host render target held before, so it is only
// safe for the transfers the game never reads back (it clears or overwrites).
inline std::atomic<int>& OwnershipTransferSkip() {
  static std::atomic<int> value{0};
  return value;
}

inline std::atomic<uint64_t>& OwnershipTransfersSkipped() {
  static std::atomic<uint64_t> value{0};
  return value;
}

// C6: how a depth render target taking over an EDRAM range gets the stencil
// of the previous owner. 0 = shader into a color scratch image, then an image
// copy into the stencil (vulkan_stencil_copy_ownership, the default), 1 = the
// shader once per stencil bit straight into the stencil, 2 = not copied (the
// stencil keeps what the host render target held; for measurement only).
inline std::atomic<int>& StencilTransferMode() {
  static std::atomic<int> value{0};
  return value;
}

inline std::atomic<uint64_t>& StencilTransfersSkipped() {
  static std::atomic<uint64_t> value{0};
  return value;
}

}  // namespace rex::graphics::nx_ab
