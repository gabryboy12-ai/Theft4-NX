/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/kernel/xam/apps/xlivebase_app.h>
#include <rex/logging.h>
#include <rex/system/xam/xsession.h>
#include <rex/system/xenumerator.h>
#include <rex/thread.h>

#include "xlivebase_social_abi.h"
#include "xlivebase_title_storage_abi.h"

#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <ranges>
#include <string_view>
#include <vector>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;
namespace apps {
using namespace rex::system;

namespace {

struct XCONTENT_MARKETPLACE_COUNTS_REQUEST {
  rex::be<uint32_t> user_index;
  rex::be<uint32_t> title_id;
  rex::be<uint32_t> content_categories;
  rex::be<uint32_t> result_ptr;
};
static_assert_size(XCONTENT_MARKETPLACE_COUNTS_REQUEST, 0x10);

struct XCONTENT_MARKETPLACE_COUNTS_RESULT {
  rex::be<uint32_t> new_offers;
  rex::be<uint32_t> total_offers;
};
static_assert_size(XCONTENT_MARKETPLACE_COUNTS_RESULT, 0x8);

struct XMSG_DYNAMIC_ARGUMENTS {
  std::array<uint8_t, 516> bytes;
};
static_assert_size(XMSG_DYNAMIC_ARGUMENTS, 516);

bool IsGuestRangeValid(memory::Memory* memory, uint32_t address, size_t size) {
  if (!address || !size || size > std::numeric_limits<uint32_t>::max()) return false;
  const uint64_t end = static_cast<uint64_t>(address) + size - 1;
  return end <= std::numeric_limits<uint32_t>::max() && memory->LookupHeap(address) &&
         memory->LookupHeap(static_cast<uint32_t>(end));
}

std::optional<std::vector<uint32_t>> DynamicArguments(memory::Memory* memory,
                                                      uint32_t metadata_ptr,
                                                      uint32_t expected_count) {
  if (!IsGuestRangeValid(memory, metadata_ptr, sizeof(XMSG_DYNAMIC_ARGUMENTS))) {
    return std::nullopt;
  }
  const auto* metadata =
      memory->TranslateVirtual<const XMSG_DYNAMIC_ARGUMENTS*>(metadata_ptr);
  if (memory::load_and_swap<uint32_t>(metadata->bytes.data() + 512) != expected_count ||
      expected_count > 32) {
    return std::nullopt;
  }
  std::vector<uint32_t> arguments;
  arguments.reserve(expected_count);
  for (uint32_t index = 0; index < expected_count; ++index) {
    const uint8_t* entry = metadata->bytes.data() + index * 16;
    const uint64_t value = memory::load_and_swap<uint64_t>(entry + 8);
    const uint32_t high = static_cast<uint32_t>(value >> 32);
    if (memory::load_and_swap<uint32_t>(entry) != 4 || (high != 0 && high != UINT32_MAX)) {
      return std::nullopt;
    }
    arguments.push_back(static_cast<uint32_t>(value));
  }
  return arguments;
}

std::optional<uint32_t> DynamicScalar(memory::Memory* memory, uint32_t address) {
  if (!IsGuestRangeValid(memory, address, sizeof(rex::be<uint32_t>))) return std::nullopt;
  return *memory->TranslateVirtual<const rex::be<uint32_t>*>(address);
}

bool GuestUtf16Equals(memory::Memory* memory, uint32_t address,
                      std::u16string_view expected) {
  const size_t code_units = expected.size() + 1;
  const size_t bytes = code_units * sizeof(rex::be<char16_t>);
  if (!IsGuestRangeValid(memory, address, bytes)) return false;
  const auto* guest = memory->TranslateVirtual<const rex::be<char16_t>*>(address);
  for (size_t index = 0; index < expected.size(); ++index) {
    if (guest[index] != expected[index]) return false;
  }
  return guest[expected.size()] == 0;
}

std::optional<X_HRESULT> DispatchGta4TitleStorageRuntime(
    system::KernelState* kernel_state, memory::Memory* memory, uint32_t message,
    uint32_t buffer_ptr, uint32_t buffer_length) {
  if (!detail::IsRuntimeSchemaMessage(message) ||
      buffer_length != detail::kGta4TitleStorageRuntimeDescriptorBytes ||
      !IsGuestRangeValid(memory, buffer_ptr,
                         detail::kGta4TitleStorageRuntimeDescriptorBytes)) {
    return std::nullopt;
  }
  const auto* descriptor =
      memory->TranslateVirtual<const detail::Gta4TitleStorageRuntimeDescriptor*>(buffer_ptr);
  if (std::ranges::any_of(descriptor->reserved,
                          [](uint8_t byte) { return byte != 0; }) ||
      !IsGuestRangeValid(memory, descriptor->request_ptr,
                         detail::kGta4TitleStorageRuntimeRequestBytes)) {
    return std::nullopt;
  }
  const auto* request = memory->TranslateVirtual<const uint8_t*>(descriptor->request_ptr);
  const uint32_t metadata_ptr = memory::load_and_swap<uint32_t>(
      request + detail::kGta4TitleStorageRuntimeArgumentsPointerOffset);
  if (!IsGuestRangeValid(memory, metadata_ptr,
                         detail::kGta4TitleStorageDynamicArgumentsBytes)) {
    return std::nullopt;
  }
  const auto* metadata = memory->TranslateVirtual<const uint8_t*>(metadata_ptr);
  const auto arguments = detail::ReadTitleStorageDynamicArguments(metadata);
  if (!arguments) return std::nullopt;

  const auto operation = detail::ClassifyTitleStorageRuntimeRequest(request);
  if (operation == detail::Gta4TitleStorageOperation::kUnknown) return std::nullopt;

  const auto user_index = DynamicScalar(memory, (*arguments)[0]);
  const auto byte_count = DynamicScalar(memory, (*arguments)[2]);
  const auto path_code_units = DynamicScalar(memory, (*arguments)[4]);
  const auto data_pointer = DynamicScalar(memory, (*arguments)[5]);
  if (!GuestUtf16Equals(memory, (*arguments)[1],
                        detail::kGta4AchievementStoragePath)) {
    return std::nullopt;
  }
  if (!user_index || !byte_count || !path_code_units || !data_pointer ||
      *user_index != 0 ||
      !detail::IsValidGta4AchievementStorageByteCount(*byte_count) ||
      *path_code_units != detail::kGta4AchievementStoragePathCodeUnitsWithNul ||
      *data_pointer != (*arguments)[3] ||
      !IsGuestRangeValid(memory, (*arguments)[3],
                         detail::kGta4AchievementStorageBytes)) {
    return X_E_INVALIDARG;
  }

  auto* live = kernel_state->live_compatibility();
  auto* service = live ? live->gta4_achievement_storage_service() : nullptr;
  if (!service || !live->identity().xuid) {
    return X_HRESULT_FROM_WIN32(X_ERROR_NOT_LOGGED_ON);
  }

  if (operation == detail::Gta4TitleStorageOperation::kUpload) {
    const auto* source = memory->TranslateVirtual<const uint8_t*>((*arguments)[3]);
    return detail::UploadGta4AchievementStorage(
        *service, std::span<const uint8_t>(
                      source, detail::kGta4AchievementStorageBytes));
  }

  const uint32_t result_ptr = memory::load_and_swap<uint32_t>(
      request + detail::kGta4TitleStorageRuntimeResultPointerOffset);
  if (!IsGuestRangeValid(memory, result_ptr,
                         detail::kGta4AchievementStorageResultBytes)) {
    return X_E_INVALIDARG;
  }
  auto* result = memory->TranslateVirtual<detail::Gta4TitleStorageResult*>(result_ptr);
  auto* destination = memory->TranslateVirtual<uint8_t*>((*arguments)[3]);
  return detail::DownloadGta4AchievementStorage(
      *service, live->identity().xuid,
      std::span<uint8_t, detail::kGta4AchievementStorageBytes>(
          destination, detail::kGta4AchievementStorageBytes),
      *result);
}

}  // namespace

XLiveBaseApp::XLiveBaseApp(KernelState* kernel_state) : App(kernel_state, 0xFC) {}

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XLiveBaseApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                            uint32_t buffer_length) {
  if (const auto storage_result = DispatchGta4TitleStorageRuntime(
          kernel_state_, memory_, message, buffer_ptr, buffer_length)) {
    return *storage_result;
  }
  // NOTE: buffer_length may be zero or valid.
  auto buffer = buffer_ptr ? memory_->TranslateVirtual(buffer_ptr) : nullptr;
  switch (message) {
    case 0x00058004: {
      // Called on startup, seems to just return a bool in the buffer.
      if (!buffer || (buffer_length && buffer_length != 4)) {
        return X_E_INVALIDARG;
      }
      REXKRNL_DEBUG("XLiveBaseGetLogonId({:08X})", buffer_ptr);
      const auto* live = kernel_state_->live_compatibility();
      memory::store_and_swap<uint32_t>(buffer + 0, live && live->signed_in() ? 1 : 0);
      return X_E_SUCCESS;
    }
    case 0x00058006: {
      if (!buffer || (buffer_length && buffer_length != 4)) {
        return X_E_INVALIDARG;
      }
      REXKRNL_DEBUG("XLiveBaseGetNatType({:08X})", buffer_ptr);
      const auto* live = kernel_state_->live_compatibility();
      memory::store_and_swap<uint32_t>(buffer + 0,
                                       live && live->available() ? 1 : 0);
      return X_E_SUCCESS;
    }
    case 0x00058007: {
      // Occurs if title calls XOnlineGetServiceInfo, expects dwServiceId
      // and pServiceInfo. pServiceInfo should contain pointer to
      // XONLINE_SERVICE_INFO structure.
      REXKRNL_DEBUG("CXLiveLogon::GetServiceInfo({:08X}, {:08X})", buffer_ptr, buffer_length);
      return kernel_state_->live_compatibility() &&
                     kernel_state_->live_compatibility()->available()
                 ? X_E_SUCCESS
                 : 0x80151802;  // ERROR_CONNECTION_INVALID
    }
    case 0x00058009: {
      if (!buffer || (buffer_length &&
                      buffer_length != sizeof(XCONTENT_MARKETPLACE_COUNTS_REQUEST))) {
        return X_E_INVALIDARG;
      }
      const auto& request =
          *reinterpret_cast<const XCONTENT_MARKETPLACE_COUNTS_REQUEST*>(buffer);
      if (request.user_index != 0 || !request.result_ptr) {
        return X_E_INVALIDARG;
      }
      auto* result = memory_->TranslateVirtual<XCONTENT_MARKETPLACE_COUNTS_RESULT*>(
          request.result_ptr);
      std::memset(result, 0, sizeof(*result));
      REXKRNL_DEBUG("XContentGetMarketplaceCounts(title={:08X}, categories={:08X}) -> 0",
                    static_cast<uint32_t>(request.title_id),
                    static_cast<uint32_t>(request.content_categories));
      return X_E_SUCCESS;
    }
    case 0x0005800E: {
      if (!IsGuestRangeValid(memory_, buffer_ptr, sizeof(detail::XLiveMuteQuery)) ||
          !IsGuestRangeValid(memory_, buffer_length, sizeof(rex::be<uint32_t>))) {
        return X_E_INVALIDARG;
      }
      auto* query = memory_->TranslateVirtual<detail::XLiveMuteQuery*>(buffer_ptr);
      auto* muted = memory_->TranslateVirtual<rex::be<uint32_t>*>(buffer_length);
      detail::CompleteMuteQuery(*query, *muted, X_ERROR_INVALID_PARAMETER, false);

      const uint32_t user_index = detail::MuteQueryUserIndex(*query);
      const uint64_t target_xuid = detail::MuteQueryXuid(*query);
      uint32_t status = X_ERROR_SUCCESS;
      bool is_muted = false;
      if (user_index != 0) {
        status = X_ERROR_NO_SUCH_USER;
      } else if (!target_xuid) {
        status = X_ERROR_INVALID_PARAMETER;
      } else {
        auto* live = kernel_state_->live_compatibility();
        auto* social = live ? live->social_service() : nullptr;
        if (!social) {
          status = X_ERROR_NOT_LOGGED_ON;
        } else {
          switch (social->QueryMute(target_xuid)) {
            case CachedMuteState::kMissing:
              status = X_ERROR_NOT_FOUND;
              break;
            case CachedMuteState::kNotMuted:
              break;
            case CachedMuteState::kMuted:
              is_muted = true;
              break;
          }
        }
      }
      detail::CompleteMuteQuery(*query, *muted, status, is_muted);
      REXKRNL_DEBUG("CXLiveFriends::IsMuted(user={}, xuid={:016X}) -> {}, status={}",
                    user_index, target_xuid, static_cast<uint32_t>(*muted), status);
      return X_E_SUCCESS;
    }
    case 0x00058020: {
      const auto arguments = DynamicArguments(memory_, buffer_length, 5);
      if (!arguments) return X_E_INVALIDARG;
      const auto user_index = DynamicScalar(memory_, (*arguments)[0]);
      const auto starting_index = DynamicScalar(memory_, (*arguments)[1]);
      const auto maximum_results = DynamicScalar(memory_, (*arguments)[2]);
      const uint32_t buffer_size_ptr = (*arguments)[3];
      const uint32_t handle_ptr = (*arguments)[4];
      if (!user_index || !starting_index || !maximum_results || *user_index != 0 ||
          !*maximum_results || *starting_index > 100 ||
          *maximum_results > 100 - *starting_index ||
          !IsGuestRangeValid(memory_, buffer_size_ptr, sizeof(rex::be<uint32_t>)) ||
          !IsGuestRangeValid(memory_, handle_ptr, sizeof(rex::be<uint32_t>))) {
        return X_E_INVALIDARG;
      }
      auto* live = kernel_state_->live_compatibility();
      auto* social = live ? live->social_service() : nullptr;
      if (!social) return X_HRESULT_FROM_WIN32(X_ERROR_NOT_LOGGED_ON);
      auto enumeration =
          social->EnumerateFriends(*starting_index + *maximum_results);
      if (!enumeration.succeeded()) {
        REXKRNL_WARN(
            "CXLiveFriends::Enumerate(user={}, start={}, count={}) failed: {}",
            *user_index, *starting_index, *maximum_results,
            static_cast<uint32_t>(enumeration.status));
        return enumeration.status == SocialServiceStatus::kInvalidRequest
                   ? X_E_INVALIDARG
                   : X_E_FAIL;
      }
      auto friends = std::move(enumeration.friends);
      if (friends.size() > *starting_index) {
        friends.erase(friends.begin(), friends.begin() + *starting_index);
      } else {
        friends.clear();
      }
      if (friends.size() > *maximum_results) friends.resize(*maximum_results);

      auto enumerator = object_ref<XStaticEnumerator<detail::XLiveFriendInfo>>(
          new XStaticEnumerator<detail::XLiveFriendInfo>(kernel_state_,
                                                         *maximum_results));
      for (const auto& friend_record : friends) {
        if (!friend_record.xuid || friend_record.blocked) continue;
        auto* entry = enumerator->AppendItem();
        detail::EncodeFriendInfo(
            *entry, friend_record.xuid, friend_record.player_name,
            friend_record.presence != FriendPresence::kOffline,
            friend_record.presence == FriendPresence::kPlayingTitle,
            friend_record.session_id,
            kernel_state_->title_id());
      }
      const X_STATUS status = enumerator->Initialize(*user_index, 0xFC, 0x00058020, 0, 0);
      if (XFAILED(status)) return X_HRESULT_FROM_WIN32(status);
      *memory_->TranslateVirtual<rex::be<uint32_t>*>(buffer_size_ptr) =
          *maximum_results * sizeof(detail::XLiveFriendInfo);
      *memory_->TranslateVirtual<rex::be<uint32_t>*>(handle_ptr) = enumerator->handle();
      REXKRNL_DEBUG("CXLiveFriends::Enumerate(user={}, start={}, count={}) -> {} items",
                    *user_index, *starting_index, *maximum_results, friends.size());
      return X_E_SUCCESS;
    }
    case 0x00058023: {
      const auto arguments = DynamicArguments(memory_, buffer_length, 2);
      if (!arguments) return X_E_INVALIDARG;
      const auto user_index = DynamicScalar(memory_, (*arguments)[0]);
      const uint32_t result_ptr = (*arguments)[1];
      if (!user_index || *user_index != 0 ||
          !IsGuestRangeValid(memory_, result_ptr,
                             sizeof(detail::XLiveAcceptedInviteInfo))) {
        return X_E_INVALIDARG;
      }
      auto* result = memory_->TranslateVirtual<detail::XLiveAcceptedInviteInfo*>(result_ptr);
      std::memset(result, 0, sizeof(*result));
      auto* live = kernel_state_->live_compatibility();
      auto* social = live ? live->social_service() : nullptr;
      if (!social) return X_HRESULT_FROM_WIN32(X_ERROR_NOT_LOGGED_ON);
      auto invitation = social->AcceptedInvitation();
      if (!invitation || !invitation->session) return X_E_NOTFOUND;
      result->recipient_xuid = invitation->recipient_xuid;
      result->sender_xuid = invitation->sender_xuid;
      result->title_id = invitation->session->title_id;
      SessionRecordToGuestInfo(*invitation->session, result->session_info);
      result->flags = 1;
      REXKRNL_DEBUG("CXLiveMessaging::XMessageGameInviteGetAcceptedInfo(user={}) -> {:016X}",
                    *user_index, invitation->session_id);
      return X_E_SUCCESS;
    }
    case 0x00058035: {
      if (!IsGuestRangeValid(memory_, buffer_ptr,
                             sizeof(detail::Gta4TitleStorageBuildRequest)) ||
          (buffer_length &&
           buffer_length != sizeof(detail::Gta4TitleStorageBuildRequest))) {
        return X_E_INVALIDARG;
      }
      const auto& request =
          *memory_->TranslateVirtual<const detail::Gta4TitleStorageBuildRequest*>(
              buffer_ptr);
      if (request.user_index != 0 || request.owner_xuid != 0 ||
          request.facility != detail::kGta4TitleStorageFacility ||
          (request.title_id != 0 && request.title_id != detail::kGta4TitleId) ||
          kernel_state_->title_id() != detail::kGta4TitleId ||
          request.reserved != 0 ||
          !GuestUtf16Equals(memory_, request.item_path_ptr,
                            detail::kGta4AchievementStoragePath) ||
          !IsGuestRangeValid(memory_, request.server_path_capacity_ptr,
                             sizeof(rex::be<uint32_t>))) {
        return X_E_INVALIDARG;
      }
      auto* capacity = memory_->TranslateVirtual<rex::be<uint32_t>*>(
          request.server_path_capacity_ptr);
      const uint32_t supplied_capacity = *capacity;
      *capacity = detail::kGta4AchievementStoragePathCodeUnitsWithNul;
      if (supplied_capacity <
          detail::kGta4AchievementStoragePathCodeUnitsWithNul) {
        return X_HRESULT_FROM_WIN32(X_ERROR_INSUFFICIENT_BUFFER);
      }
      if (!IsGuestRangeValid(
              memory_, request.server_path_ptr,
              detail::kGta4AchievementStoragePathCodeUnitsWithNul *
                  sizeof(rex::be<char16_t>))) {
        return X_E_INVALIDARG;
      }
      auto* output = memory_->TranslateVirtual<rex::be<char16_t>*>(
          request.server_path_ptr);
      detail::WriteGta4AchievementStoragePath(output);
      REXKRNL_DEBUG("XStorageBuildServerPath(user=0, title={:08X}, path=Prog_ACH)",
                    detail::kGta4TitleId);
      return X_E_SUCCESS;
    }
    case 0x00058046: {
      // Required to be successful for 4D530910 to detect signed-in profile
      // Doesn't seem to set anything in the given buffer, probably only takes
      // input
      REXKRNL_DEBUG("XLiveBaseUnk58046({:08X}, {:08X}) unimplemented", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00058037: {
      REXKRNL_DEBUG("XPresenceInitialize({:08X}, {:08X})", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
  }
  REXKRNL_ERROR(
      "Unimplemented XLIVEBASE message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
