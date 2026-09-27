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

#include <cstring>
#include <limits>
#include <ranges>

#include <rex/cvar.h>
#include <rex/kernel/xam/private.h>
#include <rex/kernel/xboxkrnl/error.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xam/xsession.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xio.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

#include <rex/kernel/xam/xgi_stats_abi.h>

#include "xam_user_identity_policy.h"

REXCVAR_DEFINE_UINT32(user_language, 1, "Kernel", "User's language ID");
REXCVAR_DECLARE(uint32_t, user_country);

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;

namespace {

bool IsGuestRangeValid(uint32_t address, size_t size) {
  if (!address || !size || size > std::numeric_limits<uint32_t>::max()) return false;
  const uint64_t end = static_cast<uint64_t>(address) + size - 1;
  return end <= std::numeric_limits<uint32_t>::max() &&
         REX_KERNEL_MEMORY()->LookupHeap(address) &&
         REX_KERNEL_MEMORY()->LookupHeap(static_cast<uint32_t>(end));
}

}  // namespace

i32 XamUserGetXUID_entry(u32 user_index, u32 type_mask, mapped_u64 xuid_ptr) {
  assert_true(type_mask == 1 || type_mask == 2 || type_mask == 3 || type_mask == 4 ||
              type_mask == 7);
  if (!xuid_ptr) {
    return X_E_INVALIDARG;
  }
  uint32_t result = X_E_NO_SUCH_USER;
  uint64_t xuid = 0;
  if (user_index < 4) {
    if (user_index == 0) {
      const auto& user_profile = REX_KERNEL_STATE()->user_profile();
      auto type = user_profile->type() & type_mask;
      if (type & (2 | 4)) {
        // maybe online profile?
        xuid = user_profile->xuid();
        result = X_E_SUCCESS;
      } else if (type & 1) {
        // maybe offline profile?
        xuid = user_profile->xuid();
        result = X_E_SUCCESS;
      }
    }
  } else {
    result = X_E_INVALIDARG;
  }
  *xuid_ptr = xuid;
  return result;
}

u32 XamUserGetSigninState_entry(u32 user_index) {
  uint32_t signin_state = 0;
  if (user_index < 4) {
    if (user_index == 0) {
      const auto& user_profile = REX_KERNEL_STATE()->user_profile();
      signin_state = user_profile->signin_state();
    }
  }
  return signin_state;
}

typedef struct {
  rex::be<uint64_t> xuid;
  rex::be<uint32_t> unk08;  // maybe zero?
  rex::be<uint32_t> signin_state;
  rex::be<uint32_t> unk10;  // ?
  rex::be<uint32_t> unk14;  // ?
  char name[16];
} X_USER_SIGNIN_INFO;
static_assert_size(X_USER_SIGNIN_INFO, 40);

i32 XamUserGetSigninInfo_entry(u32 user_index, u32 flags, ppc_ptr_t<X_USER_SIGNIN_INFO> info) {
  if (!info) {
    return X_E_INVALIDARG;
  }

  std::memset(info, 0, sizeof(X_USER_SIGNIN_INFO));
  if (user_index) {
    return X_E_NO_SUCH_USER;
  }

  const auto& user_profile = REX_KERNEL_STATE()->user_profile();
  info->xuid = user_profile->xuid();
  info->signin_state = user_profile->signin_state();
  rex::string::copy_truncating(info->name, user_profile->name(), rex::countof(info->name));
  return X_E_SUCCESS;
}

u32 XamUserGetName_entry(u32 user_index, mapped_string buffer, u32 buffer_len) {
  if (user_index >= 4) {
    return X_E_INVALIDARG;
  }

  if (user_index) {
    return X_E_NO_SUCH_USER;
  }

  const auto& user_profile = REX_KERNEL_STATE()->user_profile();
  const auto& user_name = user_profile->name();
  rex::string::copy_truncating(buffer, user_name, std::min(buffer_len, uint32_t(16)));
  return X_E_SUCCESS;
}

u32 XamUserGetGamerTag_entry(u32 user_index, mapped_wstring buffer, u32 buffer_len) {
  if (user_index >= 4) {
    return X_E_INVALIDARG;
  }

  if (user_index) {
    return X_E_NO_SUCH_USER;
  }

  if (!buffer || buffer_len < 16) {
    return X_E_INVALIDARG;
  }

  const auto& user_profile = REX_KERNEL_STATE()->user_profile();
  auto user_name = rex::string::to_utf16(user_profile->name());
  rex::string::copy_and_swap_truncating(buffer, user_name, std::min(buffer_len, uint32_t(16)));
  return X_E_SUCCESS;
}

typedef struct {
  rex::be<uint32_t> setting_count;
  rex::be<uint32_t> settings_ptr;
} X_USER_READ_PROFILE_SETTINGS;
static_assert_size(X_USER_READ_PROFILE_SETTINGS, 8);

// https://github.com/oukiar/freestyledash/blob/master/Freestyle/Tools/Generic/xboxtools.cpp
uint32_t XamUserReadProfileSettingsEx(uint32_t title_id, uint32_t user_index, uint32_t xuid_count,
                                      be<uint64_t>* xuids, uint32_t setting_count,
                                      be<uint32_t>* setting_ids, uint32_t unk,
                                      be<uint32_t>* buffer_size_ptr, uint8_t* buffer,
                                      XAM_OVERLAPPED* overlapped) {
  if (!xuid_count) {
    assert_null(xuids);
  } else {
    assert_true(xuid_count == 1);
    assert_not_null(xuids);
    // TODO(gibbed): allow proper lookup of arbitrary XUIDs
    const auto& user_profile = REX_KERNEL_STATE()->user_profile();
    assert_true(static_cast<uint64_t>(xuids[0]) == user_profile->xuid());
    // TODO(gibbed): we assert here, but in case a title passes xuid_count > 1
    // until it's implemented for release builds...
    xuid_count = 1;
  }
  assert_zero(unk);  // probably flags

  // must have at least 1 to 32 settings
  if (setting_count < 1 || setting_count > 32) {
    return X_ERROR_INVALID_PARAMETER;
  }

  // buffer size pointer must be valid
  if (!buffer_size_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  // if buffer size is non-zero, buffer pointer must be valid
  auto buffer_size = static_cast<uint32_t>(*buffer_size_ptr);
  if (buffer_size && !buffer) {
    return X_ERROR_INVALID_PARAMETER;
  }

  uint32_t needed_header_size = 0;
  uint32_t needed_data_size = 0;
  for (uint32_t i = 0; i < setting_count; ++i) {
    needed_header_size += sizeof(X_USER_PROFILE_SETTING);
    UserProfile::Setting::Key setting_key;
    setting_key.value = static_cast<uint32_t>(setting_ids[i]);
    switch (static_cast<UserProfile::Setting::Type>(setting_key.type)) {
      case UserProfile::Setting::Type::WSTRING:
      case UserProfile::Setting::Type::BINARY:
        needed_data_size += setting_key.size;
        break;
      default:
        break;
    }
  }
  if (xuids) {
    needed_header_size *= xuid_count;
    needed_data_size *= xuid_count;
  }
  needed_header_size += sizeof(X_USER_READ_PROFILE_SETTINGS);

  uint32_t needed_size = needed_header_size + needed_data_size;
  if (!buffer || buffer_size < needed_size) {
    if (!buffer_size) {
      *buffer_size_ptr = needed_size;
    }
    return X_ERROR_INSUFFICIENT_BUFFER;
  }

  // Title ID = 0 means us.
  // 0xfffe07d1 = profile?

  if (!xuids && user_index) {
    // Only support user 0.
    if (overlapped) {
      REX_KERNEL_STATE()->CompleteOverlappedImmediate(
          REX_KERNEL_MEMORY()->HostToGuestVirtual(overlapped), X_ERROR_NO_SUCH_USER);
      return X_ERROR_IO_PENDING;
    }
    return X_ERROR_NO_SUCH_USER;
  }

  const auto& user_profile = REX_KERNEL_STATE()->user_profile();

  // First call asks for size (fill buffer_size_ptr).
  // Second call asks for buffer contents with that size.

  // TODO(gibbed): setting validity checking without needing a user profile
  // object.
  bool any_missing = false;
  for (uint32_t i = 0; i < setting_count; ++i) {
    auto setting_id = static_cast<uint32_t>(setting_ids[i]);
    if (!user_profile->HasSetting(setting_id)) {
      any_missing = true;
      REXKRNL_ERROR(
          "xeXamUserReadProfileSettingsEx requested unimplemented setting "
          "{:08X}",
          setting_id);
    }
  }
  if (any_missing) {
    // TODO(benvanik): don't fail? most games don't even check!
    if (overlapped) {
      REX_KERNEL_STATE()->CompleteOverlappedImmediate(
          REX_KERNEL_MEMORY()->HostToGuestVirtual(overlapped), X_ERROR_INVALID_PARAMETER);
      return X_ERROR_IO_PENDING;
    }
    return X_ERROR_INVALID_PARAMETER;
  }

  auto out_header = reinterpret_cast<X_USER_READ_PROFILE_SETTINGS*>(buffer);
  auto out_setting = reinterpret_cast<X_USER_PROFILE_SETTING*>(&out_header[1]);
  out_header->setting_count = static_cast<uint32_t>(setting_count);
  out_header->settings_ptr = REX_KERNEL_MEMORY()->HostToGuestVirtual(out_setting);

  UserProfile::SettingByteStream out_stream(REX_KERNEL_MEMORY()->HostToGuestVirtual(buffer), buffer,
                                            buffer_size, needed_header_size);
  for (uint32_t n = 0; n < setting_count; ++n) {
    uint32_t setting_id = setting_ids[n];

    std::memset(out_setting, 0, sizeof(X_USER_PROFILE_SETTING));
    if (xuids) {
      out_setting->xuid = user_profile->xuid();
    } else {
      out_setting->user_index = static_cast<uint32_t>(user_index);
    }
    out_setting->setting_id = setting_id;

    const auto setting = user_profile->AppendSetting(setting_id, &out_setting->data, &out_stream);
    if (!setting) {
      out_setting->from = 0;
    } else {
      out_setting->from = !setting->is_set ? 0 : setting->is_title_specific ? 2 : 1;
    }
    ++out_setting;
  }

  if (overlapped) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(
        REX_KERNEL_MEMORY()->HostToGuestVirtual(overlapped), X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}

u32 XamUserReadProfileSettings_entry(u32 title_id, u32 user_index, u32 xuid_count, mapped_u64 xuids,
                                     u32 setting_count, mapped_u32 setting_ids,
                                     mapped_u32 buffer_size_ptr, mapped_void buffer_ptr,
                                     ppc_ptr_t<XAM_OVERLAPPED> overlapped) {
  return XamUserReadProfileSettingsEx(title_id, user_index, xuid_count, xuids, setting_count,
                                      setting_ids, 0, buffer_size_ptr, buffer_ptr, overlapped);
}

u32 XamUserReadProfileSettingsEx_entry(u32 title_id, u32 user_index, u32 xuid_count,
                                       mapped_u64 xuids, u32 setting_count, mapped_u32 setting_ids,
                                       mapped_u32 buffer_size_ptr, u32 unk_2,
                                       mapped_void buffer_ptr,
                                       ppc_ptr_t<XAM_OVERLAPPED> overlapped) {
  return XamUserReadProfileSettingsEx(title_id, user_index, xuid_count, xuids, setting_count,
                                      setting_ids, unk_2, buffer_size_ptr, buffer_ptr, overlapped);
}

u32 XamUserWriteProfileSettings_entry(u32 title_id, u32 user_index, u32 setting_count,
                                      ppc_ptr_t<X_USER_PROFILE_SETTING> settings,
                                      ppc_ptr_t<XAM_OVERLAPPED> overlapped) {
  if (!setting_count || !settings) {
    return X_ERROR_INVALID_PARAMETER;
  }
  const uint64_t settings_size =
      static_cast<uint64_t>(setting_count) * sizeof(X_USER_PROFILE_SETTING);
  if (settings_size > std::numeric_limits<uint32_t>::max() ||
      !IsGuestRangeValid(settings.guest_address(), static_cast<size_t>(settings_size))) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (user_index) {
    // Only support user 0.
    if (overlapped) {
      REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped.guest_address(),
                                                      X_ERROR_NO_SUCH_USER);
      return X_ERROR_IO_PENDING;
    }
    return X_ERROR_NO_SUCH_USER;
  }

  // Validate and copy guest-owned buffers before mutating the local profile so
  // malformed batches never partially update the durable setting set.
  const auto& user_profile = REX_KERNEL_STATE()->user_profile();
  const uint32_t effective_title_id = title_id ? title_id : REX_KERNEL_STATE()->title_id();
  struct PendingWrite {
    uint32_t setting_id;
    bool is_binary;
    std::vector<uint8_t> bytes;
    std::unique_ptr<UserProfile::Setting> scalar;
  };
  std::vector<PendingWrite> pending_writes;
  pending_writes.reserve(setting_count);

  for (uint32_t n = 0; n < setting_count; ++n) {
    const X_USER_PROFILE_SETTING& setting = settings[n];

    auto setting_type = static_cast<UserProfile::Setting::Type>(setting.data.type);
    if (setting_type == UserProfile::Setting::Type::UNSET) {
      continue;
    }

    REXKRNL_DEBUG(
        "XamUserWriteProfileSettings: setting index [{}]:"
        " from={} setting_id={:08X} data.type={}",
        n, (uint32_t)setting.from, (uint32_t)setting.setting_id, setting.data.type);

    switch (setting_type) {
      case UserProfile::Setting::Type::CONTENT:
      case UserProfile::Setting::Type::BINARY: {
        const uint32_t setting_id = setting.setting_id;
        const size_t binary_size = setting.data.binary.size;
        const bool is_gta4_profile = effective_title_id == UserProfile::kGta4TitleId &&
                                     setting_id == UserProfile::kGta4TitleProfileSettingId;
        if (is_gta4_profile && setting_type != UserProfile::Setting::Type::BINARY) {
          return X_ERROR_INVALID_PARAMETER;
        }
        if (is_gta4_profile &&
            binary_size > UserProfile::kGta4TitleProfileMaximumSize) {
          return X_ERROR_INVALID_PARAMETER;
        }
        std::vector<uint8_t> bytes;
        if (setting.data.binary.ptr) {
          if (binary_size &&
              !IsGuestRangeValid(setting.data.binary.ptr, binary_size)) {
            return X_ERROR_INVALID_PARAMETER;
          }
          bytes.resize(binary_size);
          if (binary_size) {
            const auto* binary_ptr = REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(
                setting.data.binary.ptr);
            std::memcpy(bytes.data(), binary_ptr, binary_size);
          }
        } else {
          // Data pointer was NULL, so just fill with zeroes
          bytes.resize(binary_size, 0);
        }
        if (is_gta4_profile && !UserProfile::ValidateGta4TitleProfileBlob(bytes)) {
          return X_ERROR_INVALID_PARAMETER;
        }
        pending_writes.push_back({.setting_id = setting_id,
                                  .is_binary = true,
                                  .bytes = std::move(bytes)});
      } break;
      case UserProfile::Setting::Type::INT32:
        pending_writes.push_back(
            {.setting_id = setting.setting_id,
             .is_binary = false,
             .scalar = std::make_unique<UserProfile::Int32Setting>(setting.setting_id,
                                                                    setting.data.s32)});
        break;
      case UserProfile::Setting::Type::INT64:
        pending_writes.push_back(
            {.setting_id = setting.setting_id,
             .is_binary = false,
             .scalar = std::make_unique<UserProfile::Int64Setting>(setting.setting_id,
                                                                    setting.data.s64)});
        break;
      case UserProfile::Setting::Type::DOUBLE:
        pending_writes.push_back(
            {.setting_id = setting.setting_id,
             .is_binary = false,
             .scalar = std::make_unique<UserProfile::DoubleSetting>(setting.setting_id,
                                                                     setting.data.f64)});
        break;
      case UserProfile::Setting::Type::FLOAT:
        pending_writes.push_back(
            {.setting_id = setting.setting_id,
             .is_binary = false,
             .scalar = std::make_unique<UserProfile::FloatSetting>(setting.setting_id,
                                                                    setting.data.f32)});
        break;
      case UserProfile::Setting::Type::DATETIME:
        pending_writes.push_back(
            {.setting_id = setting.setting_id,
             .is_binary = false,
             .scalar = std::make_unique<UserProfile::DateTimeSetting>(
                 setting.setting_id, static_cast<int64_t>(setting.data.filetime))});
        break;
      case UserProfile::Setting::Type::WSTRING: {
        const uint32_t byte_count = setting.data.unicode.size;
        const uint32_t string_ptr = setting.data.unicode.ptr;
        if (!byte_count) {
          pending_writes.push_back(
              {.setting_id = setting.setting_id,
               .is_binary = false,
               .scalar = std::make_unique<UserProfile::UnicodeSetting>(setting.setting_id,
                                                                        std::u16string())});
          break;
        }
        if (!string_ptr || byte_count % sizeof(char16_t) != 0 ||
            !IsGuestRangeValid(string_ptr, byte_count)) {
          return X_ERROR_INVALID_PARAMETER;
        }
        const size_t unit_count = byte_count / sizeof(char16_t);
        const auto* source = REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(string_ptr);
        std::u16string value;
        value.reserve(unit_count);
        for (size_t index = 0; index < unit_count; ++index) {
          const char16_t unit = static_cast<char16_t>(memory::load_and_swap<uint16_t>(
              source + index * sizeof(char16_t)));
          if (!unit) break;
          value.push_back(unit);
        }
        pending_writes.push_back(
            {.setting_id = setting.setting_id,
             .is_binary = false,
             .scalar = std::make_unique<UserProfile::UnicodeSetting>(setting.setting_id,
                                                                      value)});
      } break;
      default: {
        REXKRNL_ERROR("XamUserWriteProfileSettings: invalid data type {}", setting_type);
        return X_ERROR_INVALID_PARAMETER;
      }
    };
  }

  for (auto& write : pending_writes) {
    if (write.is_binary) {
      if (!user_profile->WriteGuestBinarySetting(effective_title_id, write.setting_id,
                                                 std::move(write.bytes))) {
        return X_ERROR_INVALID_PARAMETER;
      }
    } else {
      user_profile->AddSetting(std::move(write.scalar));
    }
  }

  if (overlapped) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped.guest_address(), X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}

u32 XamUserCheckPrivilege_entry(u32 user_index, u32 mask, mapped_u32 out_value) {
  if (!out_value) {
    return X_ERROR_INVALID_PARAMETER;
  }
  // checking all users?
  if (user_index != 0xFF) {
    if (user_index >= 4) {
      return X_ERROR_INVALID_PARAMETER;
    }

    if (user_index) {
      return X_ERROR_NO_SUCH_USER;
    }
  }

  const auto* live = REX_KERNEL_STATE()->live_compatibility();
  *out_value = live && live->IsPrivilegeAllowed(mask) ? 1 : 0;
  REXKRNL_DEBUG("XamUserCheckPrivilege(user={}, privilege={}) -> {}", user_index, mask,
                static_cast<uint32_t>(*out_value));
  return X_ERROR_SUCCESS;
}

u32 XamUserContentRestrictionGetFlags_entry(u32 user_index, mapped_u32 out_flags) {
  if (user_index) {
    return X_ERROR_NO_SUCH_USER;
  }

  // No restrictions?
  *out_flags = 0;
  return X_ERROR_SUCCESS;
}

u32 XamUserContentRestrictionGetRating_entry(u32 user_index, u32 unk1, mapped_u32 out_unk2,
                                             mapped_u32 out_unk3) {
  if (user_index) {
    return X_ERROR_NO_SUCH_USER;
  }

  // Some games have special case paths for 3F that differ from the failure
  // path, so my guess is that's 'don't care'.
  *out_unk2 = 0x3F;
  *out_unk3 = 0;
  return X_ERROR_SUCCESS;
}

u32 XamUserContentRestrictionCheckAccess_entry(u32 user_index, u32 unk1, u32 unk2, u32 unk3,
                                               u32 unk4, mapped_u32 out_unk5, u32 overlapped_ptr) {
  *out_unk5 = 1;

  if (overlapped_ptr) {
    // TODO(benvanik): does this need the access arg on it?
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr, X_ERROR_SUCCESS);
  }

  return X_ERROR_SUCCESS;
}

u32 XamUserIsOnlineEnabled_entry(u32 user_index) {
  if (user_index != 0) {
    return 0;
  }
  const auto* live = REX_KERNEL_STATE()->live_compatibility();
  return live && live->signed_in() ? 1 : 0;
}

u32 XamUserGetMembershipTier_entry(u32 user_index) {
  if (user_index >= 4) {
    return X_ERROR_INVALID_PARAMETER;
  }
  if (user_index) {
    return X_ERROR_NO_SUCH_USER;
  }
  return detail::kGoldMembershipTier;
}

u32 XamUserGetMembershipTierFromXUID_entry(u64 xuid) {
  const auto& user_profile = REX_KERNEL_STATE()->user_profile();
  return detail::MembershipTierFromXuid(xuid, user_profile->xuid());
}

u32 XamUserGetOnlineCountryFromXUID_entry(u64 xuid) {
  const auto& user_profile = REX_KERNEL_STATE()->user_profile();
  return detail::OnlineCountryFromXuid(xuid, user_profile->xuid(),
                                       REXCVAR_GET(user_country));
}

u32 XamUserAreUsersFriends_entry(u32 user_index, u32 xuids_ptr, u32 xuid_count, mapped_u32 out_value,
                                 u32 overlapped_ptr) {
  uint32_t are_friends = 0;
  X_RESULT result = X_ERROR_SUCCESS;

  if (user_index >= 4 || !xuids_ptr || !xuid_count || xuid_count > 100 || !out_value) {
    result = X_ERROR_INVALID_PARAMETER;
  } else if (user_index != 0) {
    result = X_ERROR_NO_SUCH_USER;
  } else if (REX_KERNEL_STATE()->user_profile()->signin_state() == 0) {
    result = X_ERROR_NOT_LOGGED_ON;
  } else {
    auto* live = REX_KERNEL_STATE()->live_compatibility();
    auto* social = live ? live->social_service() : nullptr;
    const uint64_t byte_count = static_cast<uint64_t>(xuid_count) * sizeof(rex::be<uint64_t>);
    const uint64_t end = static_cast<uint64_t>(xuids_ptr) + byte_count - 1;
    if (!social || end > std::numeric_limits<uint32_t>::max() ||
        !REX_KERNEL_MEMORY()->LookupHeap(xuids_ptr) ||
        !REX_KERNEL_MEMORY()->LookupHeap(static_cast<uint32_t>(end))) {
      result = social ? X_ERROR_INVALID_PARAMETER : X_ERROR_NOT_LOGGED_ON;
    } else {
      const auto* guest_xuids =
          REX_KERNEL_MEMORY()->TranslateVirtual<const rex::be<uint64_t>*>(xuids_ptr);
      std::vector<uint64_t> xuids;
      xuids.reserve(xuid_count);
      for (uint32_t index = 0; index < xuid_count; ++index) xuids.push_back(guest_xuids[index]);
      const auto relationships = social->AreFriends(xuids);
      if (relationships.size() != xuids.size()) {
        result = X_ERROR_FUNCTION_FAILED;
      } else {
        are_friends = std::ranges::all_of(relationships, [](bool value) { return value; }) ? 1 : 0;
      }
    }
  }

  if (out_value) {
    *out_value = result == X_ERROR_SUCCESS ? are_friends : 0;
  }
  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediateEx(
        overlapped_ptr, result == X_ERROR_SUCCESS ? X_ERROR_SUCCESS : X_ERROR_FUNCTION_FAILED,
        X_HRESULT_FROM_WIN32(result), 0);
    return X_ERROR_IO_PENDING;
  }
  return result;
}

u32 XamShowSigninUI_entry(u32 unk, u32 unk_mask) {
  // Mask values vary. Probably matching user types? Local/remote?

  // To fix game modes that display a 4 profile signin UI (even if playing
  // alone):
  // XN_SYS_SIGNINCHANGED
  REX_KERNEL_STATE()->BroadcastNotification(0x0000000A, 1);
  // Games seem to sit and loop until we trigger this notification:
  // XN_SYS_UI (off)
  REX_KERNEL_STATE()->BroadcastNotification(0x00000009, 0);
  return X_ERROR_SUCCESS;
}

// TODO(gibbed): probably a FILETIME/LARGE_INTEGER, unknown currently
struct X_ACHIEVEMENT_UNLOCK_TIME {
  rex::be<uint32_t> unk_0;
  rex::be<uint32_t> unk_4;
};

struct X_ACHIEVEMENT_DETAILS {
  rex::be<uint32_t> id;
  rex::be<uint32_t> label_ptr;
  rex::be<uint32_t> description_ptr;
  rex::be<uint32_t> unachieved_ptr;
  rex::be<uint32_t> image_id;
  rex::be<uint32_t> gamerscore;
  X_ACHIEVEMENT_UNLOCK_TIME unlock_time;
  rex::be<uint32_t> flags;

  static const size_t kStringBufferSize = 464;
};
static_assert_size(X_ACHIEVEMENT_DETAILS, 36);

class XStaticAchievementEnumerator : public XEnumerator {
 public:
  struct AchievementDetails {
    uint32_t id;
    std::u16string label;
    std::u16string description;
    std::u16string unachieved;
    uint32_t image_id;
    uint32_t gamerscore;
    struct {
      uint32_t unk_0;
      uint32_t unk_4;
    } unlock_time;
    uint32_t flags;
  };

  XStaticAchievementEnumerator(KernelState* kernel_state, size_t items_per_enumerate,
                               uint32_t flags)
      : XEnumerator(kernel_state, items_per_enumerate,
                    sizeof(X_ACHIEVEMENT_DETAILS) +
                        (!!(flags & 7) ? X_ACHIEVEMENT_DETAILS::kStringBufferSize : 0)),
        flags_(flags) {}

  void AppendItem(AchievementDetails item) { items_.push_back(std::move(item)); }

  uint32_t WriteItems(uint32_t buffer_ptr, uint8_t* buffer_data, uint32_t* written_count) override {
    size_t count = std::min(items_.size() - current_item_, items_per_enumerate());
    if (!count) {
      return X_ERROR_NO_MORE_FILES;
    }

    size_t size = count * item_size();

    auto details = reinterpret_cast<X_ACHIEVEMENT_DETAILS*>(buffer_data);
    size_t string_offset = items_per_enumerate() * sizeof(X_ACHIEVEMENT_DETAILS);
    auto string_buffer =
        StringBuffer{buffer_ptr + static_cast<uint32_t>(string_offset), &buffer_data[string_offset],
                     count * X_ACHIEVEMENT_DETAILS::kStringBufferSize};
    for (size_t i = 0, o = current_item_; i < count; ++i, ++current_item_) {
      const auto& item = items_[current_item_];
      details[i].id = item.id;
      details[i].label_ptr = !!(flags_ & 1) ? AppendString(string_buffer, item.label) : 0;
      details[i].description_ptr =
          !!(flags_ & 2) ? AppendString(string_buffer, item.description) : 0;
      details[i].unachieved_ptr = !!(flags_ & 4) ? AppendString(string_buffer, item.unachieved) : 0;
      details[i].image_id = item.image_id;
      details[i].gamerscore = item.gamerscore;
      details[i].unlock_time.unk_0 = item.unlock_time.unk_0;
      details[i].unlock_time.unk_4 = item.unlock_time.unk_4;
      details[i].flags = item.flags;
    }

    if (written_count) {
      *written_count = static_cast<uint32_t>(count);
    }

    return X_ERROR_SUCCESS;
  }

 private:
  struct StringBuffer {
    uint32_t ptr;
    uint8_t* data;
    size_t remaining_bytes;
  };

  uint32_t AppendString(StringBuffer& sb, const std::u16string_view string) {
    size_t count = string.length() + 1;
    size_t size = count * sizeof(char16_t);
    if (size > sb.remaining_bytes) {
      assert_always();
      return 0;
    }
    auto ptr = sb.ptr;
    rex::string::copy_and_swap_truncating(reinterpret_cast<char16_t*>(sb.data), string, count);
    sb.ptr += static_cast<uint32_t>(size);
    sb.data += size;
    sb.remaining_bytes -= size;
    return ptr;
  }

 private:
  uint32_t flags_;
  std::vector<AchievementDetails> items_;
  size_t current_item_ = 0;
};

struct XUSER_STATS_SPEC {
  rex::be<uint32_t> view_id;
  rex::be<uint32_t> column_count;
  std::array<rex::be<uint16_t>, 64> column_ids;
};
static_assert_size(XUSER_STATS_SPEC, 136);

class XStatsEnumerator final : public XEnumerator {
 public:
  XStatsEnumerator(KernelState* kernel_state, std::vector<StatView> views, size_t buffer_size)
      : XEnumerator(kernel_state, 1, buffer_size),
        views_(std::move(views)),
        buffer_size_(buffer_size) {}

  uint32_t WriteItems(uint32_t buffer_ptr, uint8_t* buffer_data,
                      uint32_t* written_count) override {
    if (consumed_) return X_ERROR_NO_MORE_FILES;
    if (!buffer_data) {
      return X_ERROR_INVALID_PARAMETER;
    }
    if (!detail::WriteGtaStatsResults(std::span<uint8_t>(buffer_data, buffer_size_), buffer_ptr,
                                      views_)) {
      REXKRNL_WARN(
          "gta4-stats-op operation=enumerator-read views={} hresult={:08X}",
          views_.size(), X_HRESULT_FROM_WIN32(X_ERROR_INSUFFICIENT_BUFFER));
      return X_ERROR_INSUFFICIENT_BUFFER;
    }
    consumed_ = true;
    if (written_count) *written_count = 1;
    for (const auto& view : views_) {
      REXKRNL_INFO(
          "gta4-stats-op operation=enumerator-read view={:08X} total={} page={} "
          "hresult={:08X}",
          view.id, view.total_rows.value_or(static_cast<uint32_t>(view.rows.size())),
          view.rows.size(), X_E_SUCCESS);
    }
    return X_ERROR_SUCCESS;
  }

 private:
  std::vector<StatView> views_;
  size_t buffer_size_;
  bool consumed_ = false;
};

u32 XamUserCreateStatsEnumerator_entry(u32 title_id, u32 enumerator_type, u64 pivot,
                                       u32 row_count, u32 spec_count, u32 specs_ptr,
                                       mapped_u32 buffer_size_ptr, mapped_u32 handle_ptr) {
  if (title_id && title_id != REX_KERNEL_STATE()->title_id()) return X_ERROR_INVALID_PARAMETER;
  if (enumerator_type > 1 || !pivot || !row_count || row_count > 100 || !spec_count ||
      spec_count > 16 || !specs_ptr || !buffer_size_ptr || !handle_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }
  const uint64_t specs_bytes = static_cast<uint64_t>(spec_count) * sizeof(XUSER_STATS_SPEC);
  const uint64_t specs_end = static_cast<uint64_t>(specs_ptr) + specs_bytes - 1;
  if (specs_end > std::numeric_limits<uint32_t>::max() ||
      !REX_KERNEL_MEMORY()->LookupHeap(specs_ptr) ||
      !REX_KERNEL_MEMORY()->LookupHeap(static_cast<uint32_t>(specs_end))) {
    return X_ERROR_INVALID_PARAMETER;
  }
  auto* live = REX_KERNEL_STATE()->live_compatibility();
  auto* stats = live ? live->stats_service() : nullptr;
  const X_RESULT stats_result = detail::GtaStatsServiceResult(
      live && live->signed_in(), stats != nullptr, stats && stats->ready());
  if (stats_result != X_ERROR_SUCCESS) return stats_result;
  const auto* specs = REX_KERNEL_MEMORY()->TranslateVirtual<const XUSER_STATS_SPEC*>(specs_ptr);
  std::vector<StatView> views;
  views.reserve(spec_count);
  for (uint32_t spec_index = 0; spec_index < spec_count; ++spec_index) {
    const uint32_t view_id = specs[spec_index].view_id;
    const uint32_t column_count = specs[spec_index].column_count;
    if (!view_id || column_count > specs[spec_index].column_ids.size()) {
      return X_ERROR_INVALID_PARAMETER;
    }
    std::vector<uint32_t> columns;
    columns.reserve(column_count);
    for (uint32_t column_index = 0; column_index < column_count; ++column_index) {
      columns.push_back(specs[spec_index].column_ids[column_index]);
    }
    if (enumerator_type == 0) {
      const std::array<uint64_t, 1> xuids = {pivot};
      const std::array<uint32_t, 1> view_ids = {view_id};
      auto read = stats->Read(xuids, view_ids, columns);
      if (read.size() != 1) return X_ERROR_FUNCTION_FAILED;
      views.push_back(std::move(read.front()));
    } else {
      if (pivot > std::numeric_limits<uint32_t>::max()) {
        return X_ERROR_INVALID_PARAMETER;
      }
      auto result = stats->Leaderboard(view_id, columns,
                                       static_cast<uint32_t>(pivot) - 1, row_count, false);
      if (!result.succeeded()) return X_ERROR_FUNCTION_FAILED;
      views.push_back({.id = view_id,
                       .rows = std::move(result.page.rows),
                       .total_rows = result.page.total});
    }
  }

  for (size_t view_index = 0; view_index < views.size(); ++view_index) {
    REXKRNL_INFO(
        "gta4-stats-op operation=enumerator-create type={} view={:08X} attributes={} "
        "total={} page={} hresult={:08X}",
        enumerator_type, views[view_index].id,
        static_cast<uint32_t>(specs[view_index].column_count),
        views[view_index].total_rows.value_or(
            static_cast<uint32_t>(views[view_index].rows.size())),
        views[view_index].rows.size(), X_E_SUCCESS);
  }

  const auto result_size = detail::GtaStatsResultSize(views);
  if (!result_size) return X_ERROR_INSUFFICIENT_BUFFER;
  auto enumerator = object_ref<XStatsEnumerator>(
      new XStatsEnumerator(REX_KERNEL_STATE(), std::move(views), *result_size));
  const X_STATUS status = enumerator->Initialize(0, 0xFB, 0xB0021, 0, 0);
  if (XFAILED(status)) return status;
  *buffer_size_ptr = static_cast<uint32_t>(*result_size);
  *handle_ptr = enumerator->handle();
  return X_ERROR_SUCCESS;
}

u32 XamUserCreateAchievementEnumerator_entry(u32 title_id, u32 user_index, u64 xuid, u32 flags,
                                             u32 offset, u32 count, mapped_u32 buffer_size_ptr,
                                             mapped_u32 handle_ptr) {
  if (!count || !buffer_size_ptr || !handle_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (user_index >= 4) {
    return X_ERROR_INVALID_PARAMETER;
  }

  size_t entry_size = sizeof(X_ACHIEVEMENT_DETAILS);
  if (flags & 7) {
    entry_size += X_ACHIEVEMENT_DETAILS::kStringBufferSize;
  }

  if (buffer_size_ptr) {
    *buffer_size_ptr = static_cast<uint32_t>(entry_size) * count;
  }

  auto e = object_ref<XStaticAchievementEnumerator>(
      new XStaticAchievementEnumerator(REX_KERNEL_STATE(), count, flags));
  auto result = e->Initialize(user_index, 0xFB, 0xB000A, 0xB000B, 0);
  if (XFAILED(result)) {
    return result;
  }

  // ACHIEVED | ACHIEVED_ONLINE flags the game checks to consider an achievement earned.
  constexpr uint32_t kAchievedFlags = 0x00030000;

  auto fill_unlock = [](XStaticAchievementEnumerator::AchievementDetails& item, uint32_t id,
                        const rex::system::KernelState* ks) {
    uint64_t ft = ks->GetAchievementUnlockTime(id);
    if (ft) {
      item.flags |= kAchievedFlags;
      item.unlock_time.unk_0 = static_cast<uint32_t>(ft & 0xFFFF'FFFF);
      item.unlock_time.unk_4 = static_cast<uint32_t>(ft >> 32);
    }
  };

  const auto* ks = REX_KERNEL_STATE();

  // Prefer the runtime store (populated from TOML or XDBF at boot) so that
  // dev-edited labels/descriptions are visible to the game's own queries.
  const auto store = ks->loaded_achievements();
  if (!store.empty()) {
    for (const auto& info : store) {
      auto item = XStaticAchievementEnumerator::AchievementDetails{
          info.id,
          rex::string::to_utf16(info.label),
          rex::string::to_utf16(info.description),
          rex::string::to_utf16(info.unachieved_description),
          info.image_id,
          info.gamerscore,
          {0, 0},
          info.flags};
      fill_unlock(item, info.id, ks);
      e->AppendItem(item);
    }
  } else {
    const util::XdbfGameData db = ks->title_xdbf();
    if (db.is_valid()) {
      const XLanguage language =
          db.GetExistingLanguage(static_cast<XLanguage>(REXCVAR_GET(user_language)));
      for (const util::XdbfAchievementTableEntry& entry : db.GetAchievements()) {
        auto item = XStaticAchievementEnumerator::AchievementDetails{
            entry.id,
            rex::string::to_utf16(db.GetStringTableEntry(language, entry.label_id)),
            rex::string::to_utf16(db.GetStringTableEntry(language, entry.description_id)),
            rex::string::to_utf16(db.GetStringTableEntry(language, entry.unachieved_id)),
            entry.image_id,
            entry.gamerscore,
            {0, 0},
            entry.flags};
        fill_unlock(item, entry.id, ks);
        e->AppendItem(item);
      }
    }
  }

  *handle_ptr = e->handle();
  return X_ERROR_SUCCESS;
}

u32 XamParseGamerTileKey_entry(mapped_u32 key_ptr, mapped_u32 out1_ptr, mapped_u32 out2_ptr,
                               mapped_u32 out3_ptr) {
  *out1_ptr = 0xC0DE0001;
  *out2_ptr = 0xC0DE0002;
  *out3_ptr = 0xC0DE0003;
  return X_ERROR_SUCCESS;
}

u32 XamReadTileToTexture_entry(u32 unknown, u32 title_id, u64 tile_id, u32 user_index,
                               mapped_void buffer_ptr, u32 stride, u32 height, u32 overlapped_ptr) {
  // TODO(gibbed): unknown=0,2,3,9
  if (!tile_id) {
    return X_ERROR_INVALID_PARAMETER;
  }

  size_t size = size_t(stride) * size_t(height);
  std::memset(buffer_ptr, 0xFF, size);

  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr, X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}

u32 XamWriteGamerTile_entry(u32 arg1, u32 arg2, u32 arg3, u32 arg4, u32 arg5, u32 overlapped_ptr) {
  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr, X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}

u32 XamSessionCreateHandle_entry(mapped_u32 handle_ptr) {
  if (!handle_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  auto session = object_ref<XSession>(new XSession(REX_KERNEL_STATE()));
  const X_STATUS status = session->Initialize();
  if (XFAILED(status)) {
    return xboxkrnl::xeRtlNtStatusToDosError(status);
  }
  *handle_ptr = session->handle();
  return X_ERROR_SUCCESS;
}

u32 XamSessionRefObjByHandle_entry(u32 handle, mapped_u32 obj_ptr) {
  if (!obj_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }
  auto session = REX_KERNEL_OBJECTS()->LookupObject<XSession>(handle);
  if (!session) {
    return X_ERROR_INVALID_HANDLE;
  }

  // The guest releases this reference through ObDereferenceObject after the
  // XGI request has consumed the native session object.
  session->RetainHandle();
  *obj_ptr = session->guest_object();
  return X_ERROR_SUCCESS;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamUserGetXUID, rex::kernel::xam::XamUserGetXUID_entry)
REX_EXPORT(__imp__XamUserGetSigninState, rex::kernel::xam::XamUserGetSigninState_entry)
REX_EXPORT(__imp__XamUserGetSigninInfo, rex::kernel::xam::XamUserGetSigninInfo_entry)
REX_EXPORT(__imp__XamUserGetName, rex::kernel::xam::XamUserGetName_entry)
REX_EXPORT(__imp__XamUserGetGamerTag, rex::kernel::xam::XamUserGetGamerTag_entry)
REX_EXPORT(__imp__XamUserReadProfileSettings, rex::kernel::xam::XamUserReadProfileSettings_entry)
REX_EXPORT(__imp__XamUserReadProfileSettingsEx,
           rex::kernel::xam::XamUserReadProfileSettingsEx_entry)
REX_EXPORT(__imp__XamUserWriteProfileSettings, rex::kernel::xam::XamUserWriteProfileSettings_entry)
REX_EXPORT(__imp__XamUserCheckPrivilege, rex::kernel::xam::XamUserCheckPrivilege_entry)
REX_EXPORT(__imp__XamUserContentRestrictionGetFlags,
           rex::kernel::xam::XamUserContentRestrictionGetFlags_entry)
REX_EXPORT(__imp__XamUserContentRestrictionGetRating,
           rex::kernel::xam::XamUserContentRestrictionGetRating_entry)
REX_EXPORT(__imp__XamUserContentRestrictionCheckAccess,
           rex::kernel::xam::XamUserContentRestrictionCheckAccess_entry)
REX_EXPORT(__imp__XamUserIsOnlineEnabled, rex::kernel::xam::XamUserIsOnlineEnabled_entry)
REX_EXPORT(__imp__XamUserGetMembershipTier, rex::kernel::xam::XamUserGetMembershipTier_entry)
REX_EXPORT(__imp__XamUserGetMembershipTierFromXUID,
           rex::kernel::xam::XamUserGetMembershipTierFromXUID_entry)
REX_EXPORT(__imp__XamUserGetOnlineCountryFromXUID,
           rex::kernel::xam::XamUserGetOnlineCountryFromXUID_entry)
REX_EXPORT(__imp__XamUserAreUsersFriends, rex::kernel::xam::XamUserAreUsersFriends_entry)
REX_EXPORT(__imp__XamShowSigninUI, rex::kernel::xam::XamShowSigninUI_entry)
REX_EXPORT(__imp__XamUserCreateAchievementEnumerator,
           rex::kernel::xam::XamUserCreateAchievementEnumerator_entry)
REX_EXPORT(__imp__XamUserCreateStatsEnumerator,
           rex::kernel::xam::XamUserCreateStatsEnumerator_entry)
REX_EXPORT(__imp__XamParseGamerTileKey, rex::kernel::xam::XamParseGamerTileKey_entry)
REX_EXPORT(__imp__XamReadTileToTexture, rex::kernel::xam::XamReadTileToTexture_entry)
REX_EXPORT(__imp__XamWriteGamerTile, rex::kernel::xam::XamWriteGamerTile_entry)
REX_EXPORT(__imp__XamSessionCreateHandle, rex::kernel::xam::XamSessionCreateHandle_entry)
REX_EXPORT(__imp__XamSessionRefObjByHandle, rex::kernel::xam::XamSessionRefObjByHandle_entry)

REX_EXPORT_STUB(__imp__XamUserAddRecentPlayer);
REX_EXPORT_STUB(__imp__XamUserAllowedToPostToSocialNetwork);
REX_EXPORT_STUB(__imp__XamUserCreateAvatarAssetEnumerator);
REX_EXPORT_STUB(__imp__XamUserCreatePlayerEnumerator);
REX_EXPORT_STUB(__imp__XamUserCreateTitlesPlayedEnumerator);
REX_EXPORT_STUB(__imp__XamUserFlushLogonQueue);
REX_EXPORT_STUB(__imp__XamUserGetAge);
REX_EXPORT_STUB(__imp__XamUserGetAgeGroup);
REX_EXPORT_STUB(__imp__XamUserGetCachedUserFlags);
REX_EXPORT_STUB(__imp__XamUserGetDeviceId);
REX_EXPORT_STUB(__imp__XamUserGetIndexFromXUID);
REX_EXPORT_STUB(__imp__XamUserGetOnlineLanguageFromXUID);
REX_EXPORT_STUB(__imp__XamUserGetOnlineXUIDFromOfflineXUID);
REX_EXPORT_STUB(__imp__XamUserGetReportingInfo);
REX_EXPORT_STUB(__imp__XamUserGetRequestedUserIndexMask);
REX_EXPORT_STUB(__imp__XamUserGetSubscriptionType);
REX_EXPORT_STUB(__imp__XamUserGetUserFlags);
REX_EXPORT_STUB(__imp__XamUserGetUserFlagsFromXUID);
REX_EXPORT_STUB(__imp__XamUserGetUserIndexMask);
REX_EXPORT_STUB(__imp__XamUserGetUserTenure);
REX_EXPORT_STUB(__imp__XamUserGetUsersMissingAvatars);
REX_EXPORT_STUB(__imp__XamUserGetXUIDForTFA);
REX_EXPORT_STUB(__imp__XamUserInvalidateProfileSetting);
REX_EXPORT_STUB(__imp__XamUserIsGuest);
REX_EXPORT_STUB(__imp__XamUserIsLogonPreviewModeEnabled);
REX_EXPORT_STUB(__imp__XamUserIsParentalControlled);
REX_EXPORT_STUB(__imp__XamUserIsPartial);
REX_EXPORT_STUB(__imp__XamUserIsPartialProfile);
REX_EXPORT_STUB(__imp__XamUserIsUnsafeProgrammingAllowed);
REX_EXPORT_STUB(__imp__XamUserLockLogonPreviewMode);
REX_EXPORT_STUB(__imp__XamUserLogon);
REX_EXPORT_STUB(__imp__XamUserLogonEx);
REX_EXPORT_STUB(__imp__XamUserLookupDevice);
REX_EXPORT_STUB(__imp__XamUserNuiBind);
REX_EXPORT_STUB(__imp__XamUserNuiEnableBiometric);
REX_EXPORT_STUB(__imp__XamUserNuiGetEnrollmentIndex);
REX_EXPORT_STUB(__imp__XamUserNuiGetUserIndex);
REX_EXPORT_STUB(__imp__XamUserNuiGetUserIndexForBind);
REX_EXPORT_STUB(__imp__XamUserNuiGetUserIndexForSignin);
REX_EXPORT_STUB(__imp__XamUserNuiIsBiometricEnabled);
REX_EXPORT_STUB(__imp__XamUserNuiUnbind);
REX_EXPORT_STUB(__imp__XamUserOverrideBindingCallbacks);
REX_EXPORT_STUB(__imp__XamUserOverrideDeviceBindings);
REX_EXPORT_STUB(__imp__XamUserOverrideGlobalState);
REX_EXPORT_STUB(__imp__XamUserOverrideUserInfo);
REX_EXPORT_STUB(__imp__XamUserPrefetchProfileSettings);
REX_EXPORT_STUB(__imp__XamUserProfileSync);
REX_EXPORT_STUB(__imp__XamUserReadUserPreference);
REX_EXPORT_STUB(__imp__XamUserResetSubscriptionType);
REX_EXPORT_STUB(__imp__XamUserUnlockLogonPreviewMode);
REX_EXPORT_STUB(__imp__XamUserUpdateRecentPlayer);
REX_EXPORT_STUB(__imp__XamUserValidateAvatarManifest);
REX_EXPORT_STUB(__imp__XamUserWriteUserPreference);
REX_EXPORT_STUB(__imp__XamVerifyPasscode);
