/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <rex/kernel/xam/private.h>
#include <rex/kernel/xam/apps/xgi_app.h>
#include <rex/kernel/xboxkrnl/error.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xio.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>
#include <rex/system/xam/xsession.h>
#include <rex/system/xam/arbitration_async.h>

#include "apps/xlivebase_title_storage_abi.h"

#include <cstring>
#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;

namespace {

constexpr uint32_t kXgiAppId = 0xFB;
constexpr uint32_t kXliveBaseAppId = 0xFC;
constexpr uint32_t kMaximumSessionArrayCount = 64;
constexpr uint32_t kMaximumPropertyPayloadSize = 512;
constexpr uint32_t kMaximumGta4AchievementCount = 65;
constexpr size_t kXmsgDynamicArgumentsSize = 516;
constexpr size_t kXmsgDynamicArgumentStride = 16;
constexpr size_t kXmsgDynamicArgumentValueOffset = 8;
constexpr size_t kXmsgDynamicArgumentCountOffset = 512;

struct XGI_STATS_SPEC_SNAPSHOT {
  rex::be<uint32_t> view_id;
  rex::be<uint32_t> column_count;
  std::array<rex::be<uint16_t>, 64> column_ids;
};
static_assert_size(XGI_STATS_SPEC_SNAPSHOT, 136);

struct XGI_STATS_VIEW_SNAPSHOT {
  rex::be<uint32_t> view_id;
  rex::be<uint32_t> property_count;
  rex::be<uint32_t> properties_ptr;
};
static_assert_size(XGI_STATS_VIEW_SNAPSHOT, 12);

struct XGI_STATS_MODIFY_SKILL_SNAPSHOT {
  rex::be<uint32_t> object_ptr;
  rex::be<uint32_t> xuid_count;
  rex::be<uint32_t> xuids_ptr;
  std::array<uint8_t, 12> reserved{};
};
static_assert_size(XGI_STATS_MODIFY_SKILL_SNAPSHOT, 24);

struct XGI_STATS_READ_SNAPSHOT {
  rex::be<uint32_t> title_id;
  rex::be<uint32_t> xuid_count;
  rex::be<uint32_t> xuids_ptr;
  rex::be<uint32_t> spec_count;
  rex::be<uint32_t> specs_ptr;
  rex::be<uint32_t> results_size;
  rex::be<uint32_t> results_ptr;
};
static_assert_size(XGI_STATS_READ_SNAPSHOT, 28);

struct alignas(8) XGI_STATS_WRITE_SNAPSHOT {
  rex::be<uint32_t> object_ptr;
  uint32_t reserved = 0;
  rex::be<uint64_t> xuid;
  rex::be<uint32_t> view_count;
  rex::be<uint32_t> views_ptr;
};
static_assert_size(XGI_STATS_WRITE_SNAPSHOT, 24);

struct XGI_ACHIEVEMENTS_WRITE_SNAPSHOT {
  rex::be<uint32_t> achievement_count;
  rex::be<uint32_t> achievements_ptr;
};
static_assert_size(XGI_ACHIEVEMENTS_WRITE_SNAPSHOT, 8);

struct XGI_ACHIEVEMENT_WRITE_ENTRY_SNAPSHOT {
  rex::be<uint32_t> user_index;
  rex::be<uint32_t> achievement_id;
};
static_assert_size(XGI_ACHIEVEMENT_WRITE_ENTRY_SNAPSHOT, 8);

struct XLIVE_MARKETPLACE_COUNTS_REQUEST_SNAPSHOT {
  rex::be<uint32_t> user_index;
  rex::be<uint32_t> title_id;
  rex::be<uint32_t> content_categories;
  rex::be<uint32_t> result_ptr;
};
static_assert_size(XLIVE_MARKETPLACE_COUNTS_REQUEST_SNAPSHOT, 16);

struct GuestInputSegment {
  uint32_t original_address = 0;
  uint32_t relocated_address = 0;
  size_t size = 0;
};

class AsyncInputSnapshot {
 public:
  explicit AsyncInputSnapshot(memory::Memory* memory) : memory_(memory) {}

  ~AsyncInputSnapshot() {
    for (const auto& segment : segments_) {
      if (segment.relocated_address) {
        memory_->SystemHeapFree(segment.relocated_address);
      }
    }
  }

  bool Capture(uint32_t address, size_t size) {
    if (!size) {
      return true;
    }
    if (!address || size > std::numeric_limits<uint32_t>::max()) {
      valid_ = false;
      return false;
    }
    const uint64_t end = static_cast<uint64_t>(address) + size - 1;
    if (end > std::numeric_limits<uint32_t>::max() || !memory_->LookupHeap(address) ||
        !memory_->LookupHeap(static_cast<uint32_t>(end))) {
      valid_ = false;
      return false;
    }

    for (const auto& segment : segments_) {
      const uint64_t segment_begin = segment.original_address;
      const uint64_t segment_end = segment_begin + segment.size;
      const uint64_t requested_begin = address;
      const uint64_t requested_end = requested_begin + size;
      if (requested_begin >= segment_begin && requested_end <= segment_end) {
        return true;
      }
      if (requested_begin < segment_end && requested_end > segment_begin) {
        valid_ = false;
        return false;
      }
    }

    const uint32_t relocated = memory_->SystemHeapAlloc(static_cast<uint32_t>(size));
    if (!relocated) {
      valid_ = false;
      return false;
    }
    std::memcpy(memory_->TranslateVirtual(relocated), memory_->TranslateVirtual(address), size);
    segments_.push_back(
        {.original_address = address, .relocated_address = relocated, .size = size});
    return true;
  }

  bool RelocatePointer(uint32_t pointer_field_address, uint32_t target_address) {
    const auto field = FindSegment(pointer_field_address, sizeof(rex::be<uint32_t>));
    const auto target = FindSegment(target_address, 1);
    if (!field || !target) {
      valid_ = false;
      return false;
    }
    auto* relocated_field = memory_->TranslateVirtual<uint8_t*>(
        field->segment->relocated_address + field->offset);
    if (memory::load_and_swap<uint32_t>(relocated_field) != target_address) {
      valid_ = false;
      return false;
    }
    memory::store_and_swap<uint32_t>(
        relocated_field, target->segment->relocated_address + target->offset);
    return true;
  }

  bool RelocatePointer64(uint32_t pointer_field_address, uint32_t target_address) {
    const auto field = FindSegment(pointer_field_address, sizeof(rex::be<uint64_t>));
    const auto target = FindSegment(target_address, 1);
    if (!field || !target) {
      valid_ = false;
      return false;
    }
    auto* relocated_field = memory_->TranslateVirtual<uint8_t*>(
        field->segment->relocated_address + field->offset);
    const uint64_t original = memory::load_and_swap<uint64_t>(relocated_field);
    if (static_cast<uint32_t>(original) != target_address) {
      valid_ = false;
      return false;
    }
    const uint32_t relocated_target = target->segment->relocated_address + target->offset;
    const uint64_t sign_extended_target =
        static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(relocated_target)));
    memory::store_and_swap<uint64_t>(relocated_field, sign_extended_target);
    return true;
  }

  std::optional<uint32_t> RelocatedAddress(uint32_t original_address) const {
    const auto match = FindSegment(original_address, 1);
    if (!valid_ || !match) {
      return std::nullopt;
    }
    return match->segment->relocated_address + match->offset;
  }

  void Invalidate() { valid_ = false; }
  bool valid() const { return valid_; }

 private:
  struct SegmentLocation {
    const GuestInputSegment* segment = nullptr;
    uint32_t offset = 0;
  };

  std::optional<SegmentLocation> FindSegment(uint32_t address, size_t size) const {
    if (!size) {
      return std::nullopt;
    }
    const uint64_t requested_begin = address;
    const uint64_t requested_end = requested_begin + size;
    for (const auto& segment : segments_) {
      const uint64_t segment_begin = segment.original_address;
      const uint64_t segment_end = segment_begin + segment.size;
      if (requested_begin >= segment_begin && requested_end <= segment_end) {
        return SegmentLocation{.segment = &segment,
                               .offset = static_cast<uint32_t>(requested_begin - segment_begin)};
      }
    }
    return std::nullopt;
  }

  memory::Memory* memory_;
  bool valid_ = true;
  std::vector<GuestInputSegment> segments_;
};

bool CapturePointedRange(AsyncInputSnapshot& snapshot, uint32_t pointer_field_address,
                         uint32_t target_address, size_t size) {
  if (!size) {
    return true;
  }
  return snapshot.Capture(target_address, size) &&
         snapshot.RelocatePointer(pointer_field_address, target_address);
}

bool CapturePointedRange64(AsyncInputSnapshot& snapshot, uint32_t pointer_field_address,
                           uint32_t target_address, size_t size) {
  if (!size) {
    return true;
  }
  return snapshot.Capture(target_address, size) &&
         snapshot.RelocatePointer64(pointer_field_address, target_address);
}

bool UsesDynamicArgumentsAsBufferLength(uint32_t app, uint32_t message) {
  return app == kXliveBaseAppId &&
         (message == 0x00058020 || message == 0x00058023);
}

size_t XgiRequestSize(uint32_t message) {
  switch (message) {
    case 0x000B0006:
      return sizeof(XGI_XUSER_SET_CONTEXT);
    case 0x000B0007:
      return sizeof(XGI_XUSER_SET_PROPERTY);
    case 0x000B0008:
      return sizeof(XGI_ACHIEVEMENTS_WRITE_SNAPSHOT);
    case 0x000B0010:
      return sizeof(XGI_SESSION_CREATE);
    case 0x000B0011:
    case 0x000B0014:
    case 0x000B0015:
      return sizeof(XGI_SESSION_STATE);
    case 0x000B0012:
    case 0x000B0013:
      return sizeof(XGI_SESSION_MANAGE);
    case 0x000B0016:
      return sizeof(XGI_SESSION_SEARCH);
    case 0x000B0018:
      return sizeof(XGI_SESSION_MODIFY);
    case 0x000B001A:
      return sizeof(XGI_SESSION_ARBITRATION_REGISTER);
    case 0x000B001B:
      return sizeof(XGI_SESSION_SEARCH_BY_ID);
    case 0x000B001C:
      return sizeof(XGI_SESSION_SEARCH_EX);
    case 0x000B001D:
      return sizeof(XGI_SESSION_DETAILS);
    case 0x000B001E:
      return sizeof(XGI_SESSION_MIGRATE);
    case 0x000B0019:
    case 0x000B0020:
      return 8;
    case 0x000B001F:
    case 0x000B0025:
    case 0x000B0026:
      return 24;
    case 0x000B0021:
      return 28;
    default:
      return 0;
  }
}

bool CaptureProperties(AsyncInputSnapshot& snapshot, memory::Memory* memory,
                       uint32_t properties_ptr, uint32_t property_count) {
  if (!property_count) {
    return true;
  }
  if (!properties_ptr || property_count > kMaximumSessionArrayCount) {
    return false;
  }
  const auto* properties = memory->TranslateVirtual<const XUSER_PROPERTY*>(properties_ptr);
  for (uint32_t index = 0; index < property_count; ++index) {
    auto type = properties[index].data.type;
    if (type == XUserDataType::kUnset) {
      type = static_cast<XUserDataType>(
          (static_cast<uint32_t>(properties[index].property_id) >> 28) & 0x0F);
    }
    if (type != XUserDataType::kWString && type != XUserDataType::kBinary) {
      continue;
    }
    const uint32_t size = properties[index].data.value.binary.size;
    const uint32_t pointer = properties[index].data.value.binary.pointer;
    const uint32_t pointer_field_address =
        properties_ptr + static_cast<uint32_t>(index * sizeof(XUSER_PROPERTY)) +
        static_cast<uint32_t>(offsetof(XUSER_PROPERTY, data)) +
        static_cast<uint32_t>(offsetof(XUSER_DATA, value)) +
        static_cast<uint32_t>(offsetof(XUSER_DATA_VALUE, binary.pointer));
    if (size > kMaximumPropertyPayloadSize ||
        !CapturePointedRange(snapshot, pointer_field_address, pointer, size)) {
      return false;
    }
  }
  return true;
}

bool CaptureSearchInputs(AsyncInputSnapshot& snapshot, memory::Memory* memory,
                         uint32_t request_address, const XGI_SESSION_SEARCH& request) {
  const uint32_t context_count = request.context_count;
  const uint32_t property_count = request.property_count;
  if (context_count > kMaximumSessionArrayCount ||
      !CapturePointedRange(
          snapshot,
          request_address + static_cast<uint32_t>(offsetof(XGI_SESSION_SEARCH, contexts_ptr)),
          request.contexts_ptr, static_cast<size_t>(context_count) * sizeof(XUSER_CONTEXT))) {
    return false;
  }
  if (property_count > kMaximumSessionArrayCount ||
      !CapturePointedRange(
          snapshot,
          request_address + static_cast<uint32_t>(offsetof(XGI_SESSION_SEARCH, properties_ptr)),
          request.properties_ptr,
          static_cast<size_t>(property_count) * sizeof(XUSER_PROPERTY))) {
    return false;
  }
  return CaptureProperties(snapshot, memory, request.properties_ptr, property_count);
}

std::shared_ptr<AsyncInputSnapshot> CaptureAsyncInputs(uint32_t app, uint32_t message,
                                                       uint32_t buffer_ptr,
                                                       uint32_t buffer_length) {
  auto* memory = REX_KERNEL_MEMORY();
  auto snapshot = std::make_shared<AsyncInputSnapshot>(memory);
  if (app == kXliveBaseAppId &&
      apps::detail::IsRuntimeSchemaMessage(message) &&
      buffer_length == apps::detail::kGta4TitleStorageRuntimeDescriptorBytes) {
    if (!snapshot->Capture(
            buffer_ptr,
            apps::detail::kGta4TitleStorageRuntimeDescriptorBytes)) {
      snapshot->Invalidate();
      return snapshot;
    }
    const auto* descriptor = memory->TranslateVirtual<
        const apps::detail::Gta4TitleStorageRuntimeDescriptor*>(buffer_ptr);
    bool reserved_zero = true;
    for (uint8_t byte : descriptor->reserved) reserved_zero &= byte == 0;
    if (reserved_zero && descriptor->request_ptr) {
      if (!CapturePointedRange(
              *snapshot,
              buffer_ptr + static_cast<uint32_t>(offsetof(
                               apps::detail::Gta4TitleStorageRuntimeDescriptor, request_ptr)),
              descriptor->request_ptr,
              apps::detail::kGta4TitleStorageRuntimeRequestBytes)) {
        snapshot->Invalidate();
        return snapshot;
      }
      const auto* request =
          memory->TranslateVirtual<const uint8_t*>(descriptor->request_ptr);
      const auto operation =
          apps::detail::ClassifyTitleStorageRuntimeRequest(request);
      const uint32_t metadata_ptr = memory::load_and_swap<uint32_t>(
          request +
          apps::detail::kGta4TitleStorageRuntimeArgumentsPointerOffset);
      if (operation != apps::detail::Gta4TitleStorageOperation::kUnknown &&
          metadata_ptr) {
        if (!CapturePointedRange(
                *snapshot,
                descriptor->request_ptr + static_cast<uint32_t>(
                                              apps::detail::
                                                  kGta4TitleStorageRuntimeArgumentsPointerOffset),
                metadata_ptr,
                apps::detail::kGta4TitleStorageDynamicArgumentsBytes)) {
          snapshot->Invalidate();
          return snapshot;
        }
        const auto* metadata =
            memory->TranslateVirtual<const uint8_t*>(metadata_ptr);
        const auto arguments =
            apps::detail::ReadTitleStorageDynamicArguments(metadata);
        if (!arguments) {
          snapshot->Invalidate();
          return snapshot;
        }
        for (uint32_t index : {0U, 2U, 4U, 5U}) {
          const uint32_t argument_field =
              metadata_ptr +
              index * static_cast<uint32_t>(
                          apps::detail::kGta4TitleStorageDynamicArgumentStride) +
              static_cast<uint32_t>(
                  apps::detail::kGta4TitleStorageDynamicArgumentValueOffset);
          if (!CapturePointedRange64(*snapshot, argument_field, (*arguments)[index],
                                     sizeof(rex::be<uint32_t>))) {
            snapshot->Invalidate();
            return snapshot;
          }
        }
        const uint32_t path_argument_field =
            metadata_ptr +
            static_cast<uint32_t>(
                apps::detail::kGta4TitleStorageDynamicArgumentStride) +
            static_cast<uint32_t>(
                apps::detail::kGta4TitleStorageDynamicArgumentValueOffset);
        if (!CapturePointedRange64(
                *snapshot, path_argument_field, (*arguments)[1],
                apps::detail::kGta4AchievementStoragePathCodeUnitsWithNul *
                    sizeof(rex::be<char16_t>))) {
          snapshot->Invalidate();
          return snapshot;
        }
        if (operation ==
            apps::detail::Gta4TitleStorageOperation::kUpload) {
          const uint32_t byte_count =
              *memory->TranslateVirtual<const rex::be<uint32_t>*>(
                  (*arguments)[2]);
          const uint32_t source_ptr =
              *memory->TranslateVirtual<const rex::be<uint32_t>*>(
                  (*arguments)[5]);
          if (byte_count != apps::detail::kGta4AchievementStorageBytes ||
              source_ptr != (*arguments)[3] ||
              !CapturePointedRange64(
                  *snapshot,
                  metadata_ptr +
                      static_cast<uint32_t>(
                          3U * apps::detail::kGta4TitleStorageDynamicArgumentStride) +
                      static_cast<uint32_t>(
                          apps::detail::kGta4TitleStorageDynamicArgumentValueOffset),
                  source_ptr, apps::detail::kGta4AchievementStorageBytes) ||
              !snapshot->RelocatePointer((*arguments)[5], source_ptr)) {
            snapshot->Invalidate();
          }
        }
        return snapshot;
      }
    }
  }
  if (app == kXliveBaseAppId &&
      (message == 0x00058020 || message == 0x00058023)) {
    const uint32_t expected_count = message == 0x00058020 ? 5 : 2;
    if (!snapshot->Capture(buffer_length, kXmsgDynamicArgumentsSize)) {
      snapshot->Invalidate();
      return snapshot;
    }
    const auto* metadata = memory->TranslateVirtual<const uint8_t*>(buffer_length);
    if (memory::load_and_swap<uint32_t>(metadata + kXmsgDynamicArgumentCountOffset) !=
        expected_count) {
      snapshot->Invalidate();
      return snapshot;
    }
    const uint32_t input_count = message == 0x00058020 ? 3 : 1;
    for (uint32_t index = 0; index < expected_count; ++index) {
      const uint8_t* entry = metadata + index * kXmsgDynamicArgumentStride;
      const uint64_t value =
          memory::load_and_swap<uint64_t>(entry + kXmsgDynamicArgumentValueOffset);
      const uint32_t high = static_cast<uint32_t>(value >> 32);
      if (memory::load_and_swap<uint32_t>(entry) != 4 ||
          (high != 0 && high != UINT32_MAX)) {
        snapshot->Invalidate();
        return snapshot;
      }
      if (index < input_count &&
          !CapturePointedRange64(
              *snapshot,
              buffer_length + index * static_cast<uint32_t>(kXmsgDynamicArgumentStride) +
                  static_cast<uint32_t>(kXmsgDynamicArgumentValueOffset),
              static_cast<uint32_t>(value), sizeof(rex::be<uint32_t>))) {
        snapshot->Invalidate();
        return snapshot;
      }
    }
    return snapshot;
  }
  if (app == kXliveBaseAppId && message == 0x00058009) {
    if (buffer_length != sizeof(XLIVE_MARKETPLACE_COUNTS_REQUEST_SNAPSHOT) ||
        !snapshot->Capture(buffer_ptr,
                           sizeof(XLIVE_MARKETPLACE_COUNTS_REQUEST_SNAPSHOT))) {
      snapshot->Invalidate();
    }
    return snapshot;
  }
  if (app != kXgiAppId) {
    return snapshot;
  }
  const size_t expected_size = app == kXgiAppId ? XgiRequestSize(message) : 0;
  const size_t request_size = expected_size ? expected_size : buffer_length;
  if (!snapshot->Capture(buffer_ptr, request_size) || app != kXgiAppId || !buffer_ptr) {
    return snapshot;
  }

  switch (message) {
    case 0x000B0008: {
      const auto* request = memory->TranslateVirtual<
          const XGI_ACHIEVEMENTS_WRITE_SNAPSHOT*>(buffer_ptr);
      const uint32_t count = request->achievement_count;
      if (!count || count > kMaximumGta4AchievementCount ||
          !CapturePointedRange(
              *snapshot,
              buffer_ptr + static_cast<uint32_t>(
                               offsetof(XGI_ACHIEVEMENTS_WRITE_SNAPSHOT,
                                        achievements_ptr)),
              request->achievements_ptr,
              static_cast<size_t>(count) *
                  sizeof(XGI_ACHIEVEMENT_WRITE_ENTRY_SNAPSHOT))) {
        snapshot->Invalidate();
      }
      break;
    }
    case 0x000B0007: {
      const auto* request = memory->TranslateVirtual<const XGI_XUSER_SET_PROPERTY*>(buffer_ptr);
      if (request->data_size > kMaximumPropertyPayloadSize ||
          !CapturePointedRange(
              *snapshot,
              buffer_ptr +
                  static_cast<uint32_t>(offsetof(XGI_XUSER_SET_PROPERTY, data_ptr)),
              request->data_ptr, request->data_size)) {
        snapshot->Invalidate();
        return snapshot;
      }
      break;
    }
    case 0x000B0012:
    case 0x000B0013: {
      const auto* request = memory->TranslateVirtual<const XGI_SESSION_MANAGE*>(buffer_ptr);
      const uint32_t count = request->count;
      if (count > kMaximumSessionArrayCount) {
        snapshot->Invalidate();
        break;
      }
      if (request->xuids_ptr) {
        if (!CapturePointedRange(
                *snapshot,
                buffer_ptr +
                    static_cast<uint32_t>(offsetof(XGI_SESSION_MANAGE, xuids_ptr)),
                request->xuids_ptr,
                static_cast<size_t>(count) * sizeof(rex::be<uint64_t>))) {
          snapshot->Invalidate();
          break;
        }
      }
      if (request->user_indices_ptr) {
        if (!CapturePointedRange(
                *snapshot,
                buffer_ptr +
                    static_cast<uint32_t>(
                        offsetof(XGI_SESSION_MANAGE, user_indices_ptr)),
                request->user_indices_ptr,
                static_cast<size_t>(count) * sizeof(rex::be<uint32_t>))) {
          snapshot->Invalidate();
          break;
        }
      }
      if (request->private_slots_ptr) {
        if (!CapturePointedRange(
                *snapshot,
                buffer_ptr +
                    static_cast<uint32_t>(
                        offsetof(XGI_SESSION_MANAGE, private_slots_ptr)),
                request->private_slots_ptr,
                static_cast<size_t>(count) * sizeof(rex::be<uint32_t>))) {
          snapshot->Invalidate();
          break;
        }
      }
      break;
    }
    case 0x000B0016: {
      const auto* request = memory->TranslateVirtual<const XGI_SESSION_SEARCH*>(buffer_ptr);
      if (!CaptureSearchInputs(*snapshot, memory, buffer_ptr, *request)) {
        snapshot->Invalidate();
      }
      break;
    }
    case 0x000B001C: {
      const auto* request = memory->TranslateVirtual<const XGI_SESSION_SEARCH_EX*>(buffer_ptr);
      if (!CaptureSearchInputs(*snapshot, memory, buffer_ptr, request->search)) {
        snapshot->Invalidate();
      }
      break;
    }
    case 0x000B001F: {
      const auto* request =
          memory->TranslateVirtual<const XGI_STATS_MODIFY_SKILL_SNAPSHOT*>(buffer_ptr);
      const uint32_t count = request->xuid_count;
      const uint32_t xuids_ptr = request->xuids_ptr;
      if (count > 100 ||
          !CapturePointedRange(
              *snapshot,
              buffer_ptr +
                  static_cast<uint32_t>(offsetof(XGI_STATS_MODIFY_SKILL_SNAPSHOT, xuids_ptr)),
              xuids_ptr, static_cast<size_t>(count) * sizeof(rex::be<uint64_t>))) {
        snapshot->Invalidate();
      }
      break;
    }
    case 0x000B0021: {
      const auto* request =
          memory->TranslateVirtual<const XGI_STATS_READ_SNAPSHOT*>(buffer_ptr);
      const uint32_t xuid_count = request->xuid_count;
      const uint32_t xuids_ptr = request->xuids_ptr;
      const uint32_t spec_count = request->spec_count;
      const uint32_t specs_ptr = request->specs_ptr;
      if (xuid_count > 100 || spec_count > 16 ||
          !CapturePointedRange(
              *snapshot,
              buffer_ptr + static_cast<uint32_t>(offsetof(XGI_STATS_READ_SNAPSHOT, xuids_ptr)),
              xuids_ptr, static_cast<size_t>(xuid_count) * sizeof(rex::be<uint64_t>)) ||
          !CapturePointedRange(
              *snapshot,
              buffer_ptr + static_cast<uint32_t>(offsetof(XGI_STATS_READ_SNAPSHOT, specs_ptr)),
              specs_ptr, static_cast<size_t>(spec_count) * sizeof(XGI_STATS_SPEC_SNAPSHOT))) {
        snapshot->Invalidate();
      }
      break;
    }
    case 0x000B0025: {
      const auto* request =
          memory->TranslateVirtual<const XGI_STATS_WRITE_SNAPSHOT*>(buffer_ptr);
      const uint32_t view_count = request->view_count;
      const uint32_t views_ptr = request->views_ptr;
      if (view_count > 16 ||
          !CapturePointedRange(
              *snapshot,
              buffer_ptr + static_cast<uint32_t>(offsetof(XGI_STATS_WRITE_SNAPSHOT, views_ptr)),
              views_ptr,
              static_cast<size_t>(view_count) * sizeof(XGI_STATS_VIEW_SNAPSHOT))) {
        snapshot->Invalidate();
        break;
      }
      const auto* views =
          memory->TranslateVirtual<const XGI_STATS_VIEW_SNAPSHOT*>(views_ptr);
      for (uint32_t index = 0; index < view_count; ++index) {
        const uint32_t properties_ptr = views[index].properties_ptr;
        const uint32_t property_count = views[index].property_count;
        const uint32_t pointer_field_address =
            views_ptr + static_cast<uint32_t>(index * sizeof(XGI_STATS_VIEW_SNAPSHOT)) +
            static_cast<uint32_t>(offsetof(XGI_STATS_VIEW_SNAPSHOT, properties_ptr));
        if (!CapturePointedRange(
                *snapshot, pointer_field_address, properties_ptr,
                static_cast<size_t>(property_count) * sizeof(XUSER_PROPERTY)) ||
            !CaptureProperties(*snapshot, memory, properties_ptr,
                               views[index].property_count)) {
          snapshot->Invalidate();
          break;
        }
      }
      break;
    }
    default:
      break;
  }
  return snapshot;
}

bool IsCommunityArbitrationRequest(uint32_t app, uint32_t message) {
  if (app != kXgiAppId || message != 0x000B001A) return false;
  auto* live = REX_KERNEL_STATE()->live_compatibility();
  return live && live->config().backend == LiveBackend::kCommunity;
}

void CompleteArbitrationOverlapped(KernelState* kernel_state,
                                   uint32_t overlapped_ptr,
                                   X_RESULT result) {
  const X_HRESULT hresult = X_HRESULT_FROM_WIN32(result);
  kernel_state->CompleteOverlappedEx(overlapped_ptr, hresult, hresult, 0);
}

X_HRESULT StartCommunityArbitrationRequest(uint32_t app, uint32_t message,
                                           uint32_t overlapped_ptr,
                                           uint32_t buffer_ptr,
                                           uint32_t buffer_length) {
  auto* kernel_state = REX_KERNEL_STATE();
  auto* manager = kernel_state->arbitration_async_manager();
  auto input_snapshot =
      CaptureAsyncInputs(app, message, buffer_ptr, buffer_length);
  uint32_t dispatch_buffer_ptr = buffer_ptr;
  bool dispatch_arguments_valid =
      input_snapshot && input_snapshot->valid();
  if (dispatch_arguments_valid && buffer_ptr) {
    const auto relocated = input_snapshot->RelocatedAddress(buffer_ptr);
    if (relocated) {
      dispatch_buffer_ptr = *relocated;
    } else {
      dispatch_arguments_valid = false;
    }
  }

  auto* overlapped = kernel_state->memory()->TranslateVirtual(overlapped_ptr);
  XOverlappedSetResult(overlapped, X_ERROR_IO_PENDING);
  XOverlappedSetContext(overlapped, XThread::GetCurrentThreadHandle());

  if (!manager) {
    kernel_state->CompleteOverlappedEx(overlapped_ptr, X_ERROR_CANCELLED,
                                       X_ERROR_CANCELLED, 0);
    return X_ERROR_IO_PENDING;
  }

  auto operation = manager->Reserve(
      overlapped_ptr, [kernel_state, overlapped_ptr] {
        kernel_state->CompleteOverlappedEx(overlapped_ptr, X_ERROR_CANCELLED,
                                           X_ERROR_CANCELLED, 0);
      });
  if (!operation) {
    if (!manager->Cancel(overlapped_ptr)) {
      kernel_state->CompleteOverlappedEx(overlapped_ptr, X_ERROR_CANCELLED,
                                         X_ERROR_CANCELLED, 0);
    }
    return X_ERROR_IO_PENDING;
  }

  const HostTaskAdmissionResult admission = kernel_state->QueueHostTask(
      [kernel_state, manager, operation = *operation, dispatch_buffer_ptr,
       buffer_length, dispatch_arguments_valid,
       input_snapshot = std::move(input_snapshot)]() mutable {
        if (!manager->IsCurrent(operation)) return;

        apps::detail::PreparedArbitrationRegister prepared;
        X_RESULT preparation = X_ERROR_INVALID_PARAMETER;
        if (dispatch_arguments_valid && input_snapshot &&
            input_snapshot->valid()) {
          preparation = apps::detail::PrepareArbitrationRegister(
              kernel_state, dispatch_buffer_ptr, buffer_length, prepared);
        }
        if (preparation != X_ERROR_SUCCESS) {
          if (manager->TryFinish(operation)) {
            CompleteArbitrationOverlapped(kernel_state,
                                          operation.overlapped_ptr,
                                          preparation);
          }
          return;
        }

        const bool submitted = manager->Submit(
            operation,
            [kernel_state, manager, prepared](
                const ArbitrationAsyncOperation& running) mutable {
              std::optional<SessionRecord> registered;
              if (manager->IsCurrent(running)) {
                auto* live = kernel_state->live_compatibility();
                auto* directory = live ? live->session_directory() : nullptr;
                if (directory) {
                  registered = directory->RegisterArbitration(
                      prepared.context.session_id, prepared.context.nonce,
                      prepared.context.registration_duration_seconds,
                      prepared.context.flags, running.cancellation);
                }
              }
              if (!manager->IsCurrent(running)) return;

              const HostTaskAdmissionResult completion_admission =
                  kernel_state->QueueHostTask(
                      [kernel_state, manager, prepared,
                       registered = std::move(registered), running]() mutable {
                        if (!manager->TryFinish(running)) return;
                        const X_RESULT result =
                            apps::detail::CompleteArbitrationRegister(
                                kernel_state, prepared,
                                std::move(registered));
                        CompleteArbitrationOverlapped(
                            kernel_state, running.overlapped_ptr, result);
                      });
              if (completion_admission !=
                  HostTaskAdmissionResult::kAccepted) {
                manager->Cancel(running);
              }
            });
        if (!submitted) manager->Cancel(operation);
      });
  if (admission != HostTaskAdmissionResult::kAccepted) {
    manager->Cancel(*operation);
  }
  return X_ERROR_IO_PENDING;
}

}  // namespace

u32 XMsgInProcessCall_entry(u32 app, u32 message, u32 arg1, u32 arg2) {
  auto result = REX_KERNEL_STATE()->app_manager()->DispatchMessageSync(app, message, arg1, arg2);
  if (result == X_ERROR_NOT_FOUND) {
    REXKRNL_ERROR("XMsgInProcessCall: app {:08X} undefined", app);
  }
  return result;
}

u32 XMsgSystemProcessCall_entry(u32 app, u32 message, u32 buffer, u32 buffer_length) {
  auto result =
      REX_KERNEL_STATE()->app_manager()->DispatchMessageAsync(app, message, buffer, buffer_length);
  if (result == X_ERROR_NOT_FOUND) {
    REXKRNL_ERROR("XMsgSystemProcessCall: app {:08X} undefined", app);
  }
  return result;
}

struct XMSGSTARTIOREQUEST_UNKNOWNARG {
  be<uint32_t> unk_0;
  be<uint32_t> unk_1;
};

X_HRESULT xeXMsgStartIORequestEx(uint32_t app, uint32_t message, uint32_t overlapped_ptr,
                                 uint32_t buffer_ptr, uint32_t buffer_length,
                                 XMSGSTARTIOREQUEST_UNKNOWNARG* unknown) {
  if (overlapped_ptr && IsCommunityArbitrationRequest(app, message)) {
    const X_HRESULT result = StartCommunityArbitrationRequest(
        app, message, overlapped_ptr, buffer_ptr, buffer_length);
    XThread::SetLastError(0);
    return result;
  }
  if (overlapped_ptr) {
    auto input_snapshot = CaptureAsyncInputs(app, message, buffer_ptr, buffer_length);
    uint32_t dispatch_buffer_ptr = buffer_ptr;
    uint32_t dispatch_buffer_length = buffer_length;
    bool dispatch_arguments_valid = input_snapshot && input_snapshot->valid();
    if (dispatch_arguments_valid && UsesDynamicArgumentsAsBufferLength(app, message)) {
      const auto relocated_length = input_snapshot->RelocatedAddress(buffer_length);
      if (relocated_length) {
        dispatch_buffer_length = *relocated_length;
      } else {
        dispatch_arguments_valid = false;
      }
    } else if (dispatch_arguments_valid && buffer_ptr) {
      const auto relocated_buffer = input_snapshot->RelocatedAddress(buffer_ptr);
      if (relocated_buffer) {
        dispatch_buffer_ptr = *relocated_buffer;
      } else if (app == kXgiAppId ||
                 (app == kXliveBaseAppId &&
                  apps::detail::IsRuntimeSchemaMessage(message) &&
                  buffer_length ==
                      apps::detail::kGta4TitleStorageRuntimeDescriptorBytes)) {
        dispatch_arguments_valid = false;
      }
    }
    REX_KERNEL_STATE()->CompleteOverlappedDeferredEx(
        [app, message, dispatch_buffer_ptr, dispatch_buffer_length,
         dispatch_arguments_valid,
         input_snapshot = std::move(input_snapshot)](
            uint32_t& extended_error, uint32_t& length) -> X_RESULT {
          X_RESULT result = X_E_INVALIDARG;
          if (dispatch_arguments_valid && input_snapshot && input_snapshot->valid()) {
            result = REX_KERNEL_STATE()->app_manager()->DispatchMessageAsync(
                app, message, dispatch_buffer_ptr, dispatch_buffer_length);
          }
          if (result == X_E_NOTFOUND) {
            REXKRNL_ERROR("XMsgStartIORequestEx: app {:08X} undefined", app);
            result = X_E_INVALIDARG;
          }
          extended_error = result;
          length = 0;
          return result;
        },
        overlapped_ptr);
    XThread::SetLastError(0);
    return apps::detail::Gta4TitleStorageAsyncStartResult();
  }

  auto result = REX_KERNEL_STATE()->app_manager()->DispatchMessageAsync(app, message, buffer_ptr,
                                                                        buffer_length);
  if (result == X_E_NOTFOUND) {
    REXKRNL_ERROR("XMsgStartIORequestEx: app {:08X} undefined", app);
    result = X_E_INVALIDARG;
    XThread::SetLastError(X_ERROR_NOT_FOUND);
  } else if (result == X_ERROR_SUCCESS) {
    XThread::SetLastError(0);
  }
  return result;
}

u32 XMsgStartIORequestEx_entry(u32 app, u32 message, ppc_ptr_t<XAM_OVERLAPPED> overlapped_ptr,
                               u32 buffer_ptr, u32 buffer_length,
                               ppc_ptr_t<XMSGSTARTIOREQUEST_UNKNOWNARG> unknown_ptr) {
  return xeXMsgStartIORequestEx(app, message, overlapped_ptr.guest_address(), buffer_ptr,
                                buffer_length, unknown_ptr);
}

u32 XMsgStartIORequest_entry(u32 app, u32 message, ppc_ptr_t<XAM_OVERLAPPED> overlapped_ptr,
                             u32 buffer_ptr, u32 buffer_length) {
  return xeXMsgStartIORequestEx(app, message, overlapped_ptr.guest_address(), buffer_ptr,
                                buffer_length, nullptr);
}

u32 XMsgCancelIORequest_entry(ppc_ptr_t<XAM_OVERLAPPED> overlapped_ptr, u32 wait) {
  if (auto* manager = REX_KERNEL_STATE()->arbitration_async_manager()) {
    manager->Cancel(overlapped_ptr.guest_address());
  }
  X_HANDLE event_handle = XOverlappedGetEvent(overlapped_ptr);
  if (event_handle && wait) {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
    if (ev) {
      ev->Wait(0, 0, true, nullptr);
    }
  }

  return 0;
}

u32 XMsgCompleteIORequest_entry(ppc_ptr_t<XAM_OVERLAPPED> overlapped_ptr, u32 result,
                                u32 extended_error, u32 length) {
  REX_KERNEL_STATE()->CompleteOverlappedImmediateEx(overlapped_ptr.guest_address(), result,
                                                    extended_error, length);
  return X_ERROR_SUCCESS;
}

u32 XamGetOverlappedResult_entry(ppc_ptr_t<XAM_OVERLAPPED> overlapped_ptr, mapped_u32 length_ptr,
                                 u32 unknown) {
  uint32_t result;
  if (overlapped_ptr->result != X_ERROR_IO_PENDING) {
    result = overlapped_ptr->result;
  } else if (!overlapped_ptr->event) {
    result = X_ERROR_IO_INCOMPLETE;
  } else {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(overlapped_ptr->event);
    result = ev->Wait(3, 1, 0, nullptr);
    if (XSUCCEEDED(result)) {
      result = overlapped_ptr->result;
    } else {
      result = xboxkrnl::xeRtlNtStatusToDosError(result);
    }
  }
  if (XSUCCEEDED(result) && length_ptr) {
    *length_ptr = overlapped_ptr->length;
  }
  return result;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XMsgInProcessCall, rex::kernel::xam::XMsgInProcessCall_entry)
REX_EXPORT(__imp__XMsgSystemProcessCall, rex::kernel::xam::XMsgSystemProcessCall_entry)
REX_EXPORT(__imp__XMsgStartIORequestEx, rex::kernel::xam::XMsgStartIORequestEx_entry)
REX_EXPORT(__imp__XMsgStartIORequest, rex::kernel::xam::XMsgStartIORequest_entry)
REX_EXPORT(__imp__XMsgCancelIORequest, rex::kernel::xam::XMsgCancelIORequest_entry)
REX_EXPORT(__imp__XMsgCompleteIORequest, rex::kernel::xam::XMsgCompleteIORequest_entry)
REX_EXPORT(__imp__XamGetOverlappedResult, rex::kernel::xam::XamGetOverlappedResult_entry)

REX_EXPORT_STUB(__imp__XMsgAcquireAsyncMessageFromOverlapped);
REX_EXPORT_STUB(__imp__XMsgProcessRequest);
REX_EXPORT_STUB(__imp__XMsgReleaseAsyncMessageToOverlapped);
