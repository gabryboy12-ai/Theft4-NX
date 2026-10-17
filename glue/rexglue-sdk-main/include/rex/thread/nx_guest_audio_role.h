// A guest audio mixer is identified by an audited entry address supplied by
// the game launcher. Names and launch-order thread IDs are not sufficient.
#pragma once

#include <atomic>
#include <cstdint>

namespace rex::thread {

class NxGuestAudioRole {
 public:
  void SetEntry(uint32_t entry) noexcept {
    entry_.store(entry, std::memory_order_release);
  }

  bool Matches(bool is_guest, uint32_t entry) const noexcept {
    if (!is_guest || !entry) return false;
    return entry_.load(std::memory_order_acquire) == entry;
  }

 private:
  std::atomic<uint32_t> entry_{0};
};

}  // namespace rex::thread
