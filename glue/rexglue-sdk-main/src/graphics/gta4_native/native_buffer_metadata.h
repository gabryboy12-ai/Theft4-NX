#pragma once

#include <cstdint>
#include <optional>

namespace rex::graphics::gta4_native {

struct NativeBufferMetadata {
  uint32_t guest_address = 0;
  uint32_t guest_size = 0;
  bool guest_locked = false;

  constexpr bool HasValidPayload(uint32_t maximum_size) const {
    return guest_address && guest_size && guest_size <= maximum_size &&
           uint64_t(guest_address) + guest_size <= uint64_t(UINT32_MAX) + 1;
  }
};

// The retail vertex constructor sub_82A4A808 and header builder sub_82A554A0
// pack fetch flags into both words. Its lock wrapper sub_82A4A8D0 extracts
// exactly these masks. In particular, the low size bits contain 2, not bytes
// belonging to the allocation. The index constructor sub_82A4A930 and lock
// wrapper sub_82A4A9E0 instead store/load an unmodified pointer and byte count.
constexpr std::optional<NativeBufferMetadata> DecodeNativeBufferMetadata(
    uint32_t resource_flags, uint32_t address_word, uint32_t size_word) {
  NativeBufferMetadata metadata;
  switch (resource_flags & 0xFu) {
    case 1:
      metadata.guest_address = address_word & 0xFFFFFFFCu;
      metadata.guest_size = size_word & 0x03FFFFFCu;
      break;
    case 2:
      metadata.guest_address = address_word;
      metadata.guest_size = size_word;
      break;
    default:
      return std::nullopt;
  }
  // sub_82A4A3C8/sub_82A4A600 maintain this nesting count. A buffer that
  // remains locked cannot promise immutable bytes between draw captures.
  metadata.guest_locked = (resource_flags & 0xF00u) != 0;
  return metadata;
}

}  // namespace rex::graphics::gta4_native
