#pragma once

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>

#include <rex/assert.h>
#include <rex/memory.h>
#include <rex/system/xam/live_compatibility.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

namespace rex::kernel::xam::apps::detail {

inline constexpr uint32_t kGta4TitleId = 0x545407F2;
inline constexpr uint32_t kGta4TitleStorageFacility = 3;
inline constexpr std::u16string_view kGta4AchievementStoragePath = u"Prog_ACH";
inline constexpr size_t kGta4AchievementStorageValueCount = 151;
inline constexpr size_t kGta4AchievementStorageBytes = 604;
inline constexpr size_t kGta4AchievementStorageResultBytes = 20;
inline constexpr size_t kGta4AchievementStoragePathCodeUnitsWithNul = 9;
inline constexpr size_t kGta4TitleStorageBuildRequestBytes = 40;
inline constexpr size_t kGta4TitleStorageRuntimeDescriptorBytes = 40;
inline constexpr size_t kGta4TitleStorageRuntimeRequestBytes = 1088;
inline constexpr size_t kGta4TitleStorageRuntimeArgumentsPointerOffset = 4;
inline constexpr size_t kGta4TitleStorageRuntimeResultPointerOffset = 44;
inline constexpr size_t kGta4TitleStorageRuntimeResultSizeOffset = 48;
inline constexpr size_t kGta4TitleStorageRuntimeOverlappedPointerOffset = 72;
inline constexpr size_t kGta4TitleStorageDynamicArgumentsBytes = 516;
inline constexpr size_t kGta4TitleStorageDynamicArgumentStride = 16;
inline constexpr size_t kGta4TitleStorageDynamicArgumentValueOffset = 8;
inline constexpr size_t kGta4TitleStorageDynamicArgumentCountOffset = 512;
inline constexpr uint32_t kGta4TitleStorageDynamicArgumentCount = 6;
inline constexpr uint32_t kGta4TitleStorageDynamicArgumentTypePointer = 4;
inline constexpr X_HRESULT kGta4TitleStorageMissing = 0x8015C004;

struct Gta4TitleStorageBuildRequest {
  rex::be<uint32_t> user_index;
  rex::be<uint32_t> padding;
  rex::be<uint64_t> owner_xuid;
  rex::be<uint32_t> facility;
  rex::be<uint32_t> title_id;
  rex::be<uint32_t> reserved;
  rex::be<uint32_t> item_path_ptr;
  rex::be<uint32_t> server_path_ptr;
  rex::be<uint32_t> server_path_capacity_ptr;
};
static_assert_size(Gta4TitleStorageBuildRequest, kGta4TitleStorageBuildRequestBytes);
static_assert(offsetof(Gta4TitleStorageBuildRequest, owner_xuid) == 8);
static_assert(offsetof(Gta4TitleStorageBuildRequest, facility) == 16);
static_assert(offsetof(Gta4TitleStorageBuildRequest, item_path_ptr) == 28);
static_assert(offsetof(Gta4TitleStorageBuildRequest, server_path_ptr) == 32);
static_assert(offsetof(Gta4TitleStorageBuildRequest, server_path_capacity_ptr) == 36);

struct Gta4TitleStorageRuntimeDescriptor {
  rex::be<uint32_t> request_ptr;
  std::array<uint8_t, 36> reserved;
};
static_assert_size(Gta4TitleStorageRuntimeDescriptor,
                   kGta4TitleStorageRuntimeDescriptorBytes);

struct Gta4TitleStorageResult {
  std::array<uint8_t, kGta4AchievementStorageResultBytes> bytes;
};
static_assert_size(Gta4TitleStorageResult, kGta4AchievementStorageResultBytes);

enum class Gta4TitleStorageOperation : uint32_t {
  kUnknown,
  kDownload,
  kUpload,
};

inline bool IsRuntimeSchemaMessage(uint32_t message) {
  return (message >> 16) == 5;
}

inline bool IsValidGta4AchievementStorageByteCount(uint32_t byte_count) {
  return byte_count == kGta4AchievementStorageBytes;
}

inline X_RESULT Gta4TitleStorageAsyncStartResult() {
  return X_ERROR_IO_PENDING;
}

inline void WriteGta4AchievementStoragePath(rex::be<char16_t>* output) {
  for (size_t index = 0; index < kGta4AchievementStoragePath.size(); ++index) {
    output[index] = kGta4AchievementStoragePath[index];
  }
  output[kGta4AchievementStoragePath.size()] = 0;
}

inline Gta4TitleStorageOperation ClassifyTitleStorageRuntimeRequest(
    const uint8_t* request) {
  const uint32_t result_ptr = memory::load_and_swap<uint32_t>(
      request + kGta4TitleStorageRuntimeResultPointerOffset);
  const uint32_t result_size = memory::load_and_swap<uint32_t>(
      request + kGta4TitleStorageRuntimeResultSizeOffset);
  if (result_ptr && result_size == kGta4AchievementStorageResultBytes) {
    return Gta4TitleStorageOperation::kDownload;
  }
  if (!result_ptr && !result_size) {
    return Gta4TitleStorageOperation::kUpload;
  }
  return Gta4TitleStorageOperation::kUnknown;
}

inline std::optional<std::array<uint32_t, kGta4TitleStorageDynamicArgumentCount>>
ReadTitleStorageDynamicArguments(const uint8_t* metadata) {
  if (memory::load_and_swap<uint32_t>(
          metadata + kGta4TitleStorageDynamicArgumentCountOffset) !=
      kGta4TitleStorageDynamicArgumentCount) {
    return std::nullopt;
  }
  std::array<uint32_t, kGta4TitleStorageDynamicArgumentCount> arguments{};
  for (uint32_t index = 0; index < kGta4TitleStorageDynamicArgumentCount; ++index) {
    const uint8_t* entry = metadata + index * kGta4TitleStorageDynamicArgumentStride;
    if (memory::load_and_swap<uint32_t>(entry) !=
        kGta4TitleStorageDynamicArgumentTypePointer) {
      return std::nullopt;
    }
    const uint64_t value = memory::load_and_swap<uint64_t>(
        entry + kGta4TitleStorageDynamicArgumentValueOffset);
    const uint32_t high = static_cast<uint32_t>(value >> 32);
    if (high != 0 && high != UINT32_MAX) {
      return std::nullopt;
    }
    arguments[index] = static_cast<uint32_t>(value);
  }
  return arguments;
}

inline void CompleteTitleStorageResult(Gta4TitleStorageResult& result,
                                       uint64_t owner_xuid) {
  result.bytes.fill(0);
  memory::store_and_swap<uint32_t>(result.bytes.data(),
                                   kGta4AchievementStorageBytes);
  memory::store_and_swap<uint64_t>(result.bytes.data() + 4, owner_xuid);
  memory::store_and_swap<uint64_t>(result.bytes.data() + 12, 0);
}

inline X_HRESULT DownloadGta4AchievementStorage(
    system::xam::IGta4AchievementStorageService& service, uint64_t local_xuid,
    std::span<uint8_t, kGta4AchievementStorageBytes> destination,
    Gta4TitleStorageResult& result) {
  result.bytes.fill(0);
  const auto fetched = service.FetchGta4AchievementStorage();
  if (fetched.status ==
          system::xam::Gta4AchievementStorageFetchStatus::kNotFound &&
      !fetched.record) {
    return kGta4TitleStorageMissing;
  }
  if (fetched.status !=
          system::xam::Gta4AchievementStorageFetchStatus::kFound ||
      !fetched.record || !local_xuid || fetched.record->xuid != local_xuid) {
    return X_E_FAIL;
  }
  std::ranges::copy(fetched.record->blob, destination.begin());
  CompleteTitleStorageResult(result, fetched.record->xuid);
  return X_E_SUCCESS;
}

inline X_HRESULT UploadGta4AchievementStorage(
    system::xam::IGta4AchievementStorageService& service,
    std::span<const uint8_t> source) {
  if (source.size() != kGta4AchievementStorageBytes) return X_E_INVALIDARG;
  return service.StoreGta4AchievementStorage(source) ? X_E_SUCCESS : X_E_FAIL;
}

}  // namespace rex::kernel::xam::apps::detail
