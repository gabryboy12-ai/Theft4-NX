#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

#include <rex/system/xam/live_compatibility.h>
#include <rex/types.h>

namespace rex::kernel::xam::detail {

inline constexpr uint32_t kXnqosHeaderBytes = 8;
inline constexpr uint32_t kXnqosInfoBytes = 24;
inline constexpr uint32_t kXnqosTitleDataBytes = 12;
inline constexpr uint32_t kMaximumXnqosAllocationBytes = 2312;
inline constexpr uint8_t kQosComplete = 0x01;
inline constexpr uint8_t kQosTargetContacted = 0x02;
inline constexpr uint8_t kQosDataReceived = 0x08;

inline constexpr uint32_t kQosListenEnable = 0x01;
inline constexpr uint32_t kQosListenDisable = 0x02;
inline constexpr uint32_t kQosListenSetData = 0x04;
inline constexpr uint32_t kQosListenSetBitsPerSecond = 0x08;
inline constexpr uint32_t kQosListenRelease = 0x10;
inline constexpr uint32_t kQosListenSupportedFlags =
    kQosListenEnable | kQosListenDisable | kQosListenSetData |
    kQosListenSetBitsPerSecond | kQosListenRelease;

struct QosListenCommand {
  bool release = false;
  rex::system::xam::QosListenerUpdate update;
};

inline std::optional<QosListenCommand> DecodeQosListenCommand(
    uint32_t flags, const uint8_t* data, uint32_t data_size,
    uint32_t bits_per_second) {
  if (!flags || (flags & ~kQosListenSupportedFlags) ||
      ((flags & kQosListenEnable) && (flags & kQosListenDisable)) ||
      ((flags & kQosListenRelease) && flags != kQosListenRelease)) {
    return std::nullopt;
  }
  if (flags & kQosListenRelease) {
    return !data && !data_size && !bits_per_second
               ? std::optional(QosListenCommand{.release = true})
               : std::nullopt;
  }

  QosListenCommand command;
  if (flags & kQosListenEnable) command.update.enabled = true;
  if (flags & kQosListenDisable) command.update.enabled = false;
  if (flags & kQosListenSetData) {
    if (!data || data_size != kXnqosTitleDataBytes) return std::nullopt;
    std::array<uint8_t, rex::system::xam::kQosTitleDataSize> title_data{};
    std::memcpy(title_data.data(), data, title_data.size());
    command.update.title_data = title_data;
  } else if (data || data_size) {
    return std::nullopt;
  }
  if (flags & kQosListenSetBitsPerSecond) {
    command.update.bits_per_second = bits_per_second;
  } else if (bits_per_second) {
    return std::nullopt;
  }
  if (!command.update.enabled && !command.update.title_data &&
      !command.update.bits_per_second) {
    return std::nullopt;
  }
  return command;
}

inline std::optional<uint32_t> QosGuestAllocationSize(
    std::span<const rex::system::xam::QosResult> results) {
  if (results.empty() || results.size() > rex::system::xam::kMaximumQosTargets) {
    return std::nullopt;
  }
  if (std::ranges::any_of(results, [](const auto& result) {
        return result.title_data.has_value() && !result.reachable;
      })) {
    return std::nullopt;
  }
  const uint32_t title_data_count = static_cast<uint32_t>(
      std::ranges::count_if(results, [](const auto& result) {
        return result.title_data.has_value();
      }));
  const uint32_t bytes = kXnqosHeaderBytes +
                         kXnqosInfoBytes * static_cast<uint32_t>(results.size()) +
                         kXnqosTitleDataBytes * title_data_count;
  return bytes <= kMaximumXnqosAllocationBytes ? std::optional(bytes) : std::nullopt;
}

template <typename T>
inline void StoreGuest(std::span<uint8_t> output, uint32_t offset, T value) {
  const rex::be<T> encoded = value;
  std::memcpy(output.data() + offset, &encoded, sizeof(encoded));
}

inline bool WriteQosGuestAllocation(std::span<uint8_t> output, uint32_t guest_address,
                                    std::span<const rex::system::xam::QosResult> results) {
  const auto required = QosGuestAllocationSize(results);
  if (!required || output.size() != *required) return false;
  std::ranges::fill(output, uint8_t{0});
  StoreGuest<uint32_t>(output, 0, static_cast<uint32_t>(results.size()));
  StoreGuest<uint32_t>(output, 4, 0);
  uint32_t data_offset = kXnqosHeaderBytes +
                         kXnqosInfoBytes * static_cast<uint32_t>(results.size());
  for (uint32_t index = 0; index < results.size(); ++index) {
    const uint32_t info = kXnqosHeaderBytes + kXnqosInfoBytes * index;
    const auto& result = results[index];
    output[info] = kQosComplete;
    if (!result.reachable) continue;
    output[info] |= kQosTargetContacted;
    StoreGuest<uint16_t>(output, info + 2, result.probes_xmit);
    StoreGuest<uint16_t>(output, info + 4, result.probes_recv);
    StoreGuest<uint16_t>(output, info + 12, result.rtt_min_milliseconds);
    StoreGuest<uint16_t>(output, info + 14, result.rtt_median_milliseconds);
    if (result.title_data) {
      output[info] |= kQosDataReceived;
      StoreGuest<uint16_t>(output, info + 6, kXnqosTitleDataBytes);
      StoreGuest<uint32_t>(output, info + 8, guest_address + data_offset);
      std::ranges::copy(*result.title_data, output.begin() + data_offset);
      data_offset += kXnqosTitleDataBytes;
    }
  }
  return data_offset == output.size();
}

}  // namespace rex::kernel::xam::detail
