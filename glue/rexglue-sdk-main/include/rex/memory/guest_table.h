/**
 * @file        rex/memory/guest_table.h
 * @brief       Guest-to-host address translation table (Nintendo Switch)
 *
 * Guest memory design A (docs/switch-port/03-memory.md): Switch has no
 * aliased views, so every guest address is translated per 16 MiB slice:
 *
 *   host = addr + rex_guest_table[addr >> 24]
 *
 * The table is filled by rex::memory::nx::ReserveGuestArena
 * (core/memory_switch.cpp) and never changes while the arena exists. It is a
 * hidden global array, so generated code reaches it with adrp + add (no load
 * of a pointer) and then loads one entry.
 *
 * Included by the generated init header on NX. Other platforms keep the
 * aliased views and `base + addr`.
 */

#pragma once

#include <cstdint>

#include <rex/platform.h>

#if REX_PLATFORM_NX

extern "C" {
__attribute__((visibility("hidden"))) extern uint64_t rex_guest_table[256];
// Host addresses of guest virtual 0 and guest physical 0 (the arena halves).
__attribute__((visibility("hidden"))) extern uint8_t* rex_guest_virtual_base;
__attribute__((visibility("hidden"))) extern uint8_t* rex_guest_physical_base;
}

namespace rex::memory {

inline uint8_t* GuestToHost(uint32_t guest_address) noexcept {
  return reinterpret_cast<uint8_t*>(uint64_t(guest_address) + rex_guest_table[guest_address >> 24]);
}

// Inverse of GuestToHost for host addresses inside the arena. Physical memory
// is visible through four guest ranges; a physical host address is reported
// in the 0xA0000000 window (canonical choice, documented in 03-memory.md).
inline uint32_t HostToGuest(const void* host_address) noexcept {
  const uintptr_t host = reinterpret_cast<uintptr_t>(host_address);
  const uintptr_t physical = reinterpret_cast<uintptr_t>(rex_guest_physical_base);
  if (host >= physical) {
    return uint32_t(0xA0000000u + uint32_t(host - physical));
  }
  return uint32_t(host - reinterpret_cast<uintptr_t>(rex_guest_virtual_base));
}

}  // namespace rex::memory

#endif  // REX_PLATFORM_NX
