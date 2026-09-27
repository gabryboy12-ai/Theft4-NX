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

#include <rex/kernel/xam/apps/xgi_app.h>
#include <rex/logging.h>
#include <rex/system/xam/xsession.h>
#include <rex/thread.h>

#include <rex/kernel/xam/xgi_stats_abi.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <ranges>
#include <vector>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;
namespace apps {
using namespace rex::system;

namespace {

constexpr uint32_t kFirstAchievementId = 1;
constexpr uint32_t kLastAchievementId = 65;
constexpr uint32_t kGta4SessionCreateReportedLength = 0x10;
constexpr X_HRESULT kUnsupportedHresult = X_HRESULT_FROM_WIN32(0x32);
constexpr uint32_t kMaximumStatsViews = 16;
constexpr uint32_t kMaximumStatsColumns = 64;
constexpr uint32_t kMaximumStatsXuids = 100;

struct XGI_ACHIEVEMENTS_WRITE {
  rex::be<uint32_t> achievement_count;
  rex::be<uint32_t> achievements_ptr;
};
static_assert_size(XGI_ACHIEVEMENTS_WRITE, 8);

struct XGI_ACHIEVEMENT_WRITE_ENTRY {
  rex::be<uint32_t> user_index;
  rex::be<uint32_t> achievement_id;
};
static_assert_size(XGI_ACHIEVEMENT_WRITE_ENTRY, 8);

struct XGI_STATS_VIEW {
  rex::be<uint32_t> view_id;
  rex::be<uint32_t> property_count;
  rex::be<uint32_t> properties_ptr;
};
static_assert_size(XGI_STATS_VIEW, 12);

struct XGI_STATS_SPEC {
  rex::be<uint32_t> view_id;
  rex::be<uint32_t> column_count;
  std::array<rex::be<uint16_t>, 64> column_ids;
};
static_assert_size(XGI_STATS_SPEC, 136);

bool IsGuestRangeValid(memory::Memory* memory, uint32_t address, size_t size);

bool IsGuestRangeValid(memory::Memory* memory, uint32_t address, size_t size) {
  if (!address || !size || size > std::numeric_limits<uint32_t>::max())
    return false;
  const uint64_t end = static_cast<uint64_t>(address) + size - 1;
  return end <= std::numeric_limits<uint32_t>::max() && memory->LookupHeap(address) &&
         memory->LookupHeap(static_cast<uint32_t>(end));
}

std::optional<StatColumn> StatColumnFromProperty(memory::Memory* memory,
                                                 const XUSER_PROPERTY& property) {
  StatColumn column{.id = property.property_id,
                    .id_kind = StatColumnIdKind::kProperty};
  if (!column.id)
    return std::nullopt;
  switch (property.data.type) {
    case XUserDataType::kInt32:
      column.type = StatValueType::kInt32;
      column.value = static_cast<int32_t>(property.data.value.s32);
      break;
    case XUserDataType::kInt64:
    case XUserDataType::kDateTime:
      column.type = StatValueType::kInt64;
      column.value = static_cast<int64_t>(property.data.value.s64);
      break;
    case XUserDataType::kDouble:
      column.type = StatValueType::kDouble;
      column.value = static_cast<double>(property.data.value.f64);
      if (!std::isfinite(std::get<double>(column.value)))
        return std::nullopt;
      break;
    case XUserDataType::kFloat:
      column.type = StatValueType::kDouble;
      column.value = static_cast<double>(static_cast<float>(property.data.value.f32));
      if (!std::isfinite(std::get<double>(column.value)))
        return std::nullopt;
      break;
    case XUserDataType::kWString: {
      const uint32_t size = property.data.value.unicode.size;
      const uint32_t pointer = property.data.value.unicode.pointer;
      if (!size || size > 512 || (size % sizeof(char16_t)) != 0 ||
          !IsGuestRangeValid(memory, pointer, size)) {
        return std::nullopt;
      }
      const auto* input = memory->TranslateVirtual<const rex::be<char16_t>*>(pointer);
      std::u16string value;
      value.reserve(size / sizeof(char16_t));
      for (uint32_t index = 0; index < size / sizeof(char16_t) && input[index]; ++index) {
        value.push_back(input[index]);
      }
      column.type = StatValueType::kUnicode;
      column.value = std::move(value);
      break;
    }
    case XUserDataType::kBinary: {
      const uint32_t size = property.data.value.binary.size;
      const uint32_t pointer = property.data.value.binary.pointer;
      if (size > 512 || (size && !IsGuestRangeValid(memory, pointer, size)))
        return std::nullopt;
      const uint8_t* input = size ? memory->TranslateVirtual<const uint8_t*>(pointer) : nullptr;
      column.type = StatValueType::kBinary;
      column.value = size ? std::vector<uint8_t>(input, input + size) : std::vector<uint8_t>{};
      break;
    }
    default:
      return std::nullopt;
  }
  return column;
}

std::optional<std::vector<StatView>> StatsViewsFromGuest(memory::Memory* memory, uint32_t views_ptr,
                                                         uint32_t view_count, uint64_t xuid) {
  if (!view_count || view_count > kMaximumStatsViews ||
      !IsGuestRangeValid(memory, views_ptr,
                         static_cast<size_t>(view_count) * sizeof(XGI_STATS_VIEW))) {
    return std::nullopt;
  }
  const auto* guest_views = memory->TranslateVirtual<const XGI_STATS_VIEW*>(views_ptr);
  std::vector<StatView> views;
  views.reserve(view_count);
  for (uint32_t view_index = 0; view_index < view_count; ++view_index) {
    const uint32_t property_count = guest_views[view_index].property_count;
    const uint32_t properties_ptr = guest_views[view_index].properties_ptr;
    if (!guest_views[view_index].view_id || property_count > kMaximumStatsColumns ||
        (property_count &&
         !IsGuestRangeValid(memory, properties_ptr,
                            static_cast<size_t>(property_count) * sizeof(XUSER_PROPERTY)))) {
      return std::nullopt;
    }
    StatRow row{.xuid = xuid};
    const auto* properties =
        property_count ? memory->TranslateVirtual<const XUSER_PROPERTY*>(properties_ptr) : nullptr;
    for (uint32_t property_index = 0; property_index < property_count; ++property_index) {
      auto column = StatColumnFromProperty(memory, properties[property_index]);
      if (!column)
        return std::nullopt;
      row.columns.push_back(std::move(*column));
    }
    views.push_back({.id = guest_views[view_index].view_id,
                     .rows = std::vector<StatRow>{std::move(row)},
                     .total_rows = std::nullopt});
  }
  return views;
}

std::atomic<AchievementUnlockCallback> g_achievement_unlock_callback{nullptr};

void DispatchAchievementUnlock(uint32_t xbox_id) {
  AchievementUnlockCallback callback =
      g_achievement_unlock_callback.load(std::memory_order_acquire);
  if (callback) {
    callback(xbox_id);
  }
}

X_HRESULT SessionResult(X_RESULT result) {
  return X_HRESULT_FROM_WIN32(result);
}

X_HRESULT StatsServiceResult(const LiveCompatibilityRuntime* live,
                             const IStatsService* stats) {
  return SessionResult(::rex::kernel::xam::detail::GtaStatsServiceResult(
      live && live->signed_in(), stats != nullptr, stats && stats->ready()));
}

object_ref<XSession> LookupSession(KernelState* kernel_state, memory::Memory* memory,
                                   uint32_t object_ptr) {
  if (!object_ptr) {
    return nullptr;
  }
  const auto* guest_session = memory->TranslateVirtual<const X_KSESSION*>(object_ptr);
  if (!guest_session) {
    return nullptr;
  }
  return kernel_state->object_table()->LookupObject<XSession>(guest_session->handle);
}

}  // namespace

void SetAchievementUnlockCallback(AchievementUnlockCallback callback) {
  g_achievement_unlock_callback.store(callback, std::memory_order_release);
}

namespace detail {

X_RESULT PrepareArbitrationRegister(KernelState* kernel_state,
                                    uint32_t buffer_ptr,
                                    uint32_t buffer_length,
                                    PreparedArbitrationRegister& prepared) {
  if (!kernel_state || !buffer_ptr ||
      buffer_length != sizeof(XGI_SESSION_ARBITRATION_REGISTER)) {
    return X_ERROR_INVALID_PARAMETER;
  }
  auto* memory = kernel_state->memory();
  const auto& request = *memory->TranslateVirtual<
      const XGI_SESSION_ARBITRATION_REGISTER*>(buffer_ptr);
  auto session = LookupSession(kernel_state, memory, request.object_ptr);
  if (!session) return X_ERROR_INVALID_HANDLE;

  prepared.session_handle = session->handle();
  return session->PrepareArbitrationRegister(request, prepared.context);
}

X_RESULT CompleteArbitrationRegister(
    KernelState* kernel_state, const PreparedArbitrationRegister& prepared,
    std::optional<SessionRecord> registered) {
  if (!kernel_state || !prepared.session_handle) {
    return X_ERROR_INVALID_HANDLE;
  }
  auto session = kernel_state->object_table()->LookupObject<XSession>(
      prepared.session_handle);
  if (!session) return X_ERROR_INVALID_HANDLE;
  return session->CompleteArbitrationRegister(prepared.context,
                                              std::move(registered));
}

}  // namespace detail

XgiApp::XgiApp(KernelState* kernel_state) : App(kernel_state, 0xFB) {}

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XgiApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                      uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = buffer_ptr ? memory_->TranslateVirtual(buffer_ptr) : nullptr;
  switch (message) {
    case 0x000B0006: {
      if (!buffer || (buffer_length && buffer_length != sizeof(XGI_XUSER_SET_CONTEXT))) {
        return X_E_INVALIDARG;
      }
      const auto& request = *reinterpret_cast<const XGI_XUSER_SET_CONTEXT*>(buffer);
      const uint32_t user_index = request.user_index;
      const uint32_t context_id = request.context.context_id;
      const uint32_t context_value = request.context.value;
      REXKRNL_DEBUG("XGIUserSetContextEx({:08X}, {:08X}, {:08X})", user_index, context_id,
                    context_value);
      if (user_index != 0 || !kernel_state_->live_compatibility()) {
        return X_E_NO_SUCH_USER;
      }
      return kernel_state_->live_compatibility()->SetUserContext(context_id, context_value)
                 ? X_E_SUCCESS
                 : X_E_INVALIDARG;
    }
    case 0x000B0007: {
      if (!buffer || (buffer_length && buffer_length != sizeof(XGI_XUSER_SET_PROPERTY))) {
        return X_E_INVALIDARG;
      }
      const auto& request = *reinterpret_cast<const XGI_XUSER_SET_PROPERTY*>(buffer);
      const uint32_t user_index = request.user_index;
      const uint32_t property_id = request.property_id;
      const uint32_t value_size = request.data_size;
      const uint32_t value_ptr = request.data_ptr;
      REXKRNL_DEBUG("XGIUserSetPropertyEx({:08X}, {:08X}, {}, {:08X})", user_index, property_id,
                    value_size, value_ptr);
      if (user_index != 0 || !kernel_state_->live_compatibility()) {
        return X_E_NO_SUCH_USER;
      }
      if (value_size && !value_ptr) {
        return X_E_INVALIDARG;
      }
      std::span<const uint8_t> value;
      if (value_size) {
        value = {memory_->TranslateVirtual<const uint8_t*>(value_ptr), value_size};
      }
      return kernel_state_->live_compatibility()->SetUserProperty(property_id, value)
                 ? X_E_SUCCESS
                 : X_E_INVALIDARG;
    }
    case 0x000B0008: {
      if (!buffer || buffer_length != sizeof(XGI_ACHIEVEMENTS_WRITE)) {
        REXKRNL_WARN("XGIUserWriteAchievements invalid buffer ({:08X}, {})", buffer_ptr,
                     buffer_length);
        return X_E_INVALIDARG;
      }

      const auto& request = *reinterpret_cast<const XGI_ACHIEVEMENTS_WRITE*>(buffer);
      const uint32_t raw0 = request.achievement_count;
      const uint32_t raw4 = request.achievements_ptr;
      REXKRNL_INFO("XGIUserWriteAchievements called: buf_len={} raw[0]={:08X} raw[4]={:08X}",
                   buffer_length, raw0, raw4);

      uint32_t achievement_count = raw0;
      uint32_t achievements_ptr = raw4;

      if (!achievements_ptr || !achievement_count) {
        REXKRNL_WARN("XGIUserWriteAchievements: invalid count={} ptr={:08X}", achievement_count,
                     achievements_ptr);
        return X_E_INVALIDARG;
      }

      if (achievement_count > kLastAchievementId) {
        REXKRNL_WARN(
            "XGIUserWriteAchievements: count={} exceeds GTA IV achievement count {}",
            achievement_count, kLastAchievementId);
        return X_E_INVALIDARG;
      }

      const uint64_t span_end64 =
          static_cast<uint64_t>(achievements_ptr) +
          (static_cast<uint64_t>(achievement_count) * sizeof(XGI_ACHIEVEMENT_WRITE_ENTRY)) - 1;
      if (span_end64 > std::numeric_limits<uint32_t>::max() ||
          !memory_->LookupHeap(achievements_ptr) ||
          !memory_->LookupHeap(static_cast<uint32_t>(span_end64))) {
        REXKRNL_WARN("XGIUserWriteAchievements: ptr {:08X} OOB", achievements_ptr);
        return X_E_INVALIDARG;
      }

      const auto* entries =
          memory_->TranslateVirtual<const XGI_ACHIEVEMENT_WRITE_ENTRY*>(achievements_ptr);
      std::vector<uint32_t> achievement_ids;
      achievement_ids.reserve(achievement_count);
      for (uint32_t i = 0; i < achievement_count; ++i) {
        const uint32_t user_index = entries[i].user_index;
        const uint32_t id = entries[i].achievement_id;
        if (user_index != 0) {
          REXKRNL_WARN("XGIUserWriteAchievements: unsupported user={} index={}", user_index, i);
          return X_E_NO_SUCH_USER;
        }
        if (id < kFirstAchievementId || id > kLastAchievementId) {
          REXKRNL_WARN("XGIUserWriteAchievements: invalid id={} user={} index={}", id,
                       user_index, i);
          return X_E_INVALIDARG;
        }
        achievement_ids.push_back(id);
      }
      for (const uint32_t id : achievement_ids) {
        REXKRNL_INFO("XGIUserWriteAchievements: id={}", id);
        kernel_state_->UnlockAchievement(id);
        DispatchAchievementUnlock(id);
      }
      return X_E_SUCCESS;
    }
    case 0x000B0010: {
      // GTA IV reports 0x10 here despite passing the complete 0x1C-byte
      // request. The generated call site stores all seven fields before the
      // call, so validate and snapshot the actual ABI structure.
      if (!buffer || (buffer_length && buffer_length != sizeof(XGI_SESSION_CREATE) &&
                      buffer_length != kGta4SessionCreateReportedLength)) {
        return X_E_INVALIDARG;
      }
      const auto& request = *reinterpret_cast<const XGI_SESSION_CREATE*>(buffer);
      auto session = LookupSession(kernel_state_, memory_, request.object_ptr);
      if (!session) {
        return SessionResult(X_ERROR_INVALID_HANDLE);
      }
      return SessionResult(session->Create(request));
    }
    case 0x000B0011: {
      if (!buffer || (buffer_length && buffer_length != sizeof(XGI_SESSION_STATE))) {
        return X_E_INVALIDARG;
      }
      const auto& request = *reinterpret_cast<const XGI_SESSION_STATE*>(buffer);
      auto session = LookupSession(kernel_state_, memory_, request.object_ptr);
      if (!session) {
        return SessionResult(X_ERROR_INVALID_HANDLE);
      }
      return SessionResult(session->Delete(request));
    }
    case 0x000B0012: {
      if (!buffer || (buffer_length && buffer_length != sizeof(XGI_SESSION_MANAGE))) {
        return X_E_INVALIDARG;
      }
      const auto& request = *reinterpret_cast<const XGI_SESSION_MANAGE*>(buffer);
      auto session = LookupSession(kernel_state_, memory_, request.object_ptr);
      if (!session) {
        return SessionResult(X_ERROR_INVALID_HANDLE);
      }
      return SessionResult(session->Join(request));
    }
    case 0x000B0013: {
      if (!buffer || (buffer_length && buffer_length != sizeof(XGI_SESSION_MANAGE))) {
        return X_E_INVALIDARG;
      }
      const auto& request = *reinterpret_cast<const XGI_SESSION_MANAGE*>(buffer);
      auto session = LookupSession(kernel_state_, memory_, request.object_ptr);
      if (!session) {
        return SessionResult(X_ERROR_INVALID_HANDLE);
      }
      return SessionResult(session->Leave(request));
    }
    case 0x000B0014: {
      if (!buffer || (buffer_length && buffer_length != sizeof(XGI_SESSION_STATE))) {
        return X_E_INVALIDARG;
      }
      const auto& request = *reinterpret_cast<const XGI_SESSION_STATE*>(buffer);
      auto session = LookupSession(kernel_state_, memory_, request.object_ptr);
      if (!session) {
        return SessionResult(X_ERROR_INVALID_HANDLE);
      }
      return SessionResult(session->Start(request));
    }
    case 0x000B0015: {
      if (!buffer || (buffer_length && buffer_length != sizeof(XGI_SESSION_STATE))) {
        return X_E_INVALIDARG;
      }
      const auto& request = *reinterpret_cast<const XGI_SESSION_STATE*>(buffer);
      auto session = LookupSession(kernel_state_, memory_, request.object_ptr);
      if (!session) {
        return SessionResult(X_ERROR_INVALID_HANDLE);
      }
      return SessionResult(session->End(request));
    }
    case 0x000B0016: {
      if (!buffer || (buffer_length && buffer_length != sizeof(XGI_SESSION_SEARCH))) {
        return X_E_INVALIDARG;
      }
      auto& request = *reinterpret_cast<XGI_SESSION_SEARCH*>(buffer);
      return SessionResult(XSession::Search(kernel_state_, request));
    }
    case 0x000B0018: {
      if (!buffer || (buffer_length && buffer_length != sizeof(XGI_SESSION_MODIFY))) {
        return X_E_INVALIDARG;
      }
      const auto& request = *reinterpret_cast<const XGI_SESSION_MODIFY*>(buffer);
      auto session = LookupSession(kernel_state_, memory_, request.object_ptr);
      if (!session) {
        return SessionResult(X_ERROR_INVALID_HANDLE);
      }
      return SessionResult(session->Modify(request));
    }
    case 0x000B001C: {
      if (!buffer || (buffer_length && buffer_length != sizeof(XGI_SESSION_SEARCH_EX))) {
        return X_E_INVALIDARG;
      }
      auto& request = *reinterpret_cast<XGI_SESSION_SEARCH_EX*>(buffer);
      return SessionResult(XSession::Search(kernel_state_, request.search));
    }
    case 0x000B001D: {
      if (!buffer || (buffer_length && buffer_length != sizeof(XGI_SESSION_DETAILS))) {
        return X_E_INVALIDARG;
      }
      const auto& request = *reinterpret_cast<const XGI_SESSION_DETAILS*>(buffer);
      auto session = LookupSession(kernel_state_, memory_, request.object_ptr);
      if (!session) {
        return SessionResult(X_ERROR_INVALID_HANDLE);
      }
      return SessionResult(session->GetDetails(request));
    }
    case 0x000B001E: {
      if (!buffer || (buffer_length && buffer_length != sizeof(XGI_SESSION_MIGRATE))) {
        return X_E_INVALIDARG;
      }
      const auto& request = *reinterpret_cast<const XGI_SESSION_MIGRATE*>(buffer);
      auto session = LookupSession(kernel_state_, memory_, request.object_ptr);
      if (!session) {
        return SessionResult(X_ERROR_INVALID_HANDLE);
      }
      return SessionResult(session->Migrate(request));
    }
    case 0x000B0019: {
      if (!buffer || (buffer_length && buffer_length != 8)) {
        return X_E_INVALIDARG;
      }

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t session_info_ptr = memory::load_and_swap<uint32_t>(buffer + 4);

      auto* live = kernel_state_->live_compatibility();
      auto* social = live ? live->social_service() : nullptr;
      if (user_index != 0)
        return X_HRESULT_FROM_WIN32(X_ERROR_NO_SUCH_USER);
      if (!social || !IsGuestRangeValid(memory_, session_info_ptr, sizeof(XSESSION_INFO))) {
        return X_E_INVALIDARG;
      }
      auto invitation = social->AcceptedInvitation();
      if (!invitation || !invitation->session)
        return X_E_NOTFOUND;
      auto* session_info = memory_->TranslateVirtual<XSESSION_INFO*>(session_info_ptr);
      SessionRecordToGuestInfo(*invitation->session, *session_info);
      REXKRNL_DEBUG("XSessionGetInvitationData({}, {:08X}) -> {:016X}", user_index,
                    session_info_ptr, invitation->session_id);
      return X_E_SUCCESS;
    }
    case 0x000B001A: {
      if (!buffer || (buffer_length && buffer_length != sizeof(XGI_SESSION_ARBITRATION_REGISTER))) {
        return X_E_INVALIDARG;
      }
      const auto& request = *reinterpret_cast<const XGI_SESSION_ARBITRATION_REGISTER*>(buffer);
      auto session = LookupSession(kernel_state_, memory_, request.object_ptr);
      if (!session) {
        return SessionResult(X_ERROR_INVALID_HANDLE);
      }
      REXKRNL_DEBUG("XSessionArbitrationRegister({:08X}, {:08X}, {:016X}, {}, {:08X}, {:08X})",
                    static_cast<uint32_t>(request.object_ptr), static_cast<uint32_t>(request.flags),
                    static_cast<uint64_t>(request.nonce),
                    static_cast<uint32_t>(request.registration_duration_seconds),
                    static_cast<uint32_t>(request.results_buffer_size),
                    static_cast<uint32_t>(request.results_ptr));
      return SessionResult(session->ArbitrationRegister(request));
    }
    case 0x000B001B: {
      if (!buffer || (buffer_length && buffer_length != sizeof(XGI_SESSION_SEARCH_BY_ID))) {
        return X_E_INVALIDARG;
      }
      auto& request = *reinterpret_cast<XGI_SESSION_SEARCH_BY_ID*>(buffer);
      return SessionResult(XSession::SearchById(kernel_state_, request));
    }
    case 0x000B001F: {
      if (!buffer || (buffer_length && buffer_length != 24)) {
        return X_E_INVALIDARG;
      }

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t array_count = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t xuid_array_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      auto session = LookupSession(kernel_state_, memory_, obj_ptr);
      auto* live = kernel_state_->live_compatibility();
      auto* stats = live ? live->stats_service() : nullptr;
      if (!session)
        return SessionResult(X_ERROR_INVALID_HANDLE);
      if (!array_count || array_count > kMaximumStatsXuids ||
          !IsGuestRangeValid(memory_, xuid_array_ptr,
                             static_cast<size_t>(array_count) * sizeof(rex::be<uint64_t>))) {
        return X_E_INVALIDARG;
      }
      const X_HRESULT stats_status = StatsServiceResult(live, stats);
      if (XFAILED(stats_status)) return stats_status;
      const auto* guest_xuids = memory_->TranslateVirtual<const rex::be<uint64_t>*>(xuid_array_ptr);
      std::vector<uint64_t> xuids;
      xuids.reserve(array_count);
      for (uint32_t index = 0; index < array_count; ++index)
        xuids.push_back(guest_xuids[index]);
      return stats->ModifySkill(session->record().session_id, xuids) ? X_E_SUCCESS : X_E_FAIL;
    }
    case 0x000B0020: {
      if (!buffer || (buffer_length && buffer_length != 8)) {
        return X_E_INVALIDARG;
      }

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t view_id = memory::load_and_swap<uint32_t>(buffer + 4);

      auto* live = kernel_state_->live_compatibility();
      auto* stats = live ? live->stats_service() : nullptr;
      if (user_index != 0 || !view_id)
        return X_E_INVALIDARG;
      const X_HRESULT stats_status = StatsServiceResult(live, stats);
      if (XFAILED(stats_status)) return stats_status;
      REXKRNL_DEBUG("XUserResetStatsView({:08X}, {})", user_index, view_id);
      return stats->ResetView(view_id) ? X_E_SUCCESS : X_E_FAIL;
    }
    case 0x000B0021: {
      if (!buffer || (buffer_length && buffer_length != 28)) {
        return X_E_INVALIDARG;
      }

      uint32_t title_id = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t xuids_count = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t xuids_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t specs_count = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t specs_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t results_size = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t results_ptr = memory::load_and_swap<uint32_t>(buffer + 24);

      REXKRNL_DEBUG("XUserReadStats({}, {}, {:08X}, {}, {:08X}, {}, {:08X})", title_id, xuids_count,
                    xuids_ptr, specs_count, specs_ptr, results_size, results_ptr);
      auto* live = kernel_state_->live_compatibility();
      auto* stats = live ? live->stats_service() : nullptr;
      if ((title_id && title_id != kernel_state_->title_id()) || !xuids_count ||
          xuids_count > kMaximumStatsXuids || !specs_count || specs_count > kMaximumStatsViews ||
          !results_size || !results_ptr ||
          results_size > ::rex::kernel::xam::detail::kMaximumGtaStatsResultBytes ||
          !IsGuestRangeValid(memory_, xuids_ptr,
                             static_cast<size_t>(xuids_count) * sizeof(rex::be<uint64_t>)) ||
          !IsGuestRangeValid(memory_, specs_ptr,
                             static_cast<size_t>(specs_count) * sizeof(XGI_STATS_SPEC)) ||
          !IsGuestRangeValid(memory_, results_ptr, results_size)) {
        return X_E_INVALIDARG;
      }
      const X_HRESULT stats_status = StatsServiceResult(live, stats);
      if (XFAILED(stats_status)) return stats_status;

      const auto* guest_xuids = memory_->TranslateVirtual<const rex::be<uint64_t>*>(xuids_ptr);
      std::vector<uint64_t> xuids;
      xuids.reserve(xuids_count);
      for (uint32_t index = 0; index < xuids_count; ++index) {
        if (!guest_xuids[index])
          return X_E_INVALIDARG;
        xuids.push_back(guest_xuids[index]);
      }
      const auto* guest_specs = memory_->TranslateVirtual<const XGI_STATS_SPEC*>(specs_ptr);
      std::vector<StatView> views;
      views.reserve(specs_count);
      for (uint32_t spec_index = 0; spec_index < specs_count; ++spec_index) {
        const uint32_t view_id = guest_specs[spec_index].view_id;
        const uint32_t column_count = guest_specs[spec_index].column_count;
        if (!view_id || column_count > kMaximumStatsColumns)
          return X_E_INVALIDARG;
        std::vector<uint32_t> columns;
        columns.reserve(column_count);
        for (uint32_t column_index = 0; column_index < column_count; ++column_index) {
          const uint32_t column_id = guest_specs[spec_index].column_ids[column_index];
          if (!column_id || std::ranges::find(columns, column_id) != columns.end()) {
            return X_E_INVALIDARG;
          }
          columns.push_back(column_id);
        }
        const std::array<uint32_t, 1> view_ids = {view_id};
        auto result = stats->Read(xuids, view_ids, columns);
        if (result.size() != 1 || result.front().id != view_id)
          return X_E_FAIL;
        views.push_back(std::move(result.front()));
      }
      const auto required_size = ::rex::kernel::xam::detail::GtaStatsResultSize(views);
      if (!required_size || results_size < *required_size) {
        return X_HRESULT_FROM_WIN32(X_ERROR_INSUFFICIENT_BUFFER);
      }
      auto* output = memory_->TranslateVirtual<uint8_t*>(results_ptr);
      const X_HRESULT result = ::rex::kernel::xam::detail::WriteGtaStatsResults(
                                   std::span<uint8_t>(output, results_size), results_ptr, views)
                                   ? X_E_SUCCESS
                                   : X_E_FAIL;
      for (size_t view_index = 0; view_index < views.size(); ++view_index) {
        const auto& view = views[view_index];
        REXKRNL_INFO(
            "gta4-stats-op operation=direct-read view={:08X} attributes={} total={} page={} "
            "hresult={:08X}",
            view.id, static_cast<uint32_t>(guest_specs[view_index].column_count),
            view.total_rows.value_or(static_cast<uint32_t>(view.rows.size())),
            view.rows.size(), static_cast<uint32_t>(result));
      }
      return result;
    }
    case 0x000B0025: {
      if (!buffer || (buffer_length && buffer_length != 24)) {
        return X_E_INVALIDARG;
      }

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint64_t xuid = memory::load_and_swap<uint64_t>(buffer + 8);
      uint32_t num_views = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t views_ptr = memory::load_and_swap<uint32_t>(buffer + 20);

      auto session = LookupSession(kernel_state_, memory_, obj_ptr);
      auto* live = kernel_state_->live_compatibility();
      auto* stats = live ? live->stats_service() : nullptr;
      if (!session)
        return SessionResult(X_ERROR_INVALID_HANDLE);
      if (!xuid)
        return X_E_INVALIDARG;
      auto views = StatsViewsFromGuest(memory_, views_ptr, num_views, xuid);
      if (!views)
        return X_E_INVALIDARG;
      const X_HRESULT stats_status = StatsServiceResult(live, stats);
      if (XFAILED(stats_status)) return stats_status;
      REXKRNL_DEBUG("XSessionWriteStats({:08X}, {:016X}, {}, {:08X})", obj_ptr, xuid, num_views,
                    views_ptr);
      const X_HRESULT result = stats->Write(session->record().session_id, xuid, *views)
                                   ? X_E_SUCCESS
                                   : X_E_FAIL;
      for (const auto& view : *views) {
        REXKRNL_INFO(
            "gta4-stats-op operation=write view={:08X} target={:016X} properties={} "
            "session={:016X} hresult={:08X}",
            view.id, xuid, view.rows.empty() ? 0 : view.rows.front().columns.size(),
            session->record().session_id, static_cast<uint32_t>(result));
      }
      return result;
    }
    case 0x000B0026: {
      if (!buffer ||
          (buffer_length &&
           buffer_length != sizeof(rex::kernel::xam::detail::GtaFlushStatsRequest))) {
        return X_E_INVALIDARG;
      }

      const auto obj_ptr = rex::kernel::xam::detail::GtaFlushStatsSessionObject(
          std::span<const uint8_t>(
              buffer, buffer_length ? buffer_length
                                    : offsetof(rex::kernel::xam::detail::GtaFlushStatsRequest,
                                               opaque_tail)));
      if (!obj_ptr) return X_E_INVALIDARG;

      auto session = LookupSession(kernel_state_, memory_, *obj_ptr);
      auto* live = kernel_state_->live_compatibility();
      auto* stats = live ? live->stats_service() : nullptr;
      if (!session)
        return SessionResult(X_ERROR_INVALID_HANDLE);
      const X_HRESULT stats_status = StatsServiceResult(live, stats);
      if (XFAILED(stats_status)) return stats_status;
      REXKRNL_DEBUG("XSessionFlushStats({:08X})", *obj_ptr);
      return stats->Flush(session->record().session_id) ? X_E_SUCCESS : X_E_FAIL;
    }
    case 0x000B0036: {
      // Called after opening xbox live arcade and clicking on xbox live v5759
      // to 5787 and called after clicking xbox live in the game library from
      // v6683 to v6717
      // Does not get sent a buffer
      REXKRNL_DEBUG("XInvalidateGamerTileCache, unimplemented");
      return X_E_FAIL;
    }
    case 0x000B003D: {
      if (!buffer || (buffer_length && buffer_length != 16)) {
        return X_E_INVALIDARG;
      }

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t AnId_buffer_size = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t AnId_buffer_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t block = memory::load_and_swap<uint32_t>(buffer + 12);

      REXKRNL_DEBUG("XUserGetANID({:08X}, {:08X}, {:08X}, {:08X})", user_index, AnId_buffer_size,
                    AnId_buffer_ptr, block);
      auto* live = kernel_state_->live_compatibility();
      if (user_index != 0 || !AnId_buffer_size || !AnId_buffer_ptr || !live) {
        return X_E_INVALIDARG;
      }
      static constexpr char kHexDigits[] = "0123456789abcdef";
      const auto& secret = live->identity().install_secret;
      auto* output = memory_->TranslateVirtual<uint8_t*>(AnId_buffer_ptr);
      for (uint32_t index = 0; index + 1 < AnId_buffer_size; ++index) {
        const uint8_t byte = secret[(index / 2) % secret.size()];
        const uint8_t nibble =
            index & 1 ? static_cast<uint8_t>(byte & 0x0F) : static_cast<uint8_t>(byte >> 4);
        output[index] = static_cast<uint8_t>(kHexDigits[nibble]);
      }
      output[AnId_buffer_size - 1] = 0;
      return X_E_SUCCESS;
    }
    case 0x000B0041: {
      if (!buffer || (buffer_length && buffer_length != 32)) {
        return X_E_INVALIDARG;
      }
      // 00000000 2789fecc 00000000 00000000 200491e0 00000000 200491f0 20049340
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t context_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      auto context = context_ptr ? memory_->TranslateVirtual(context_ptr) : nullptr;
      uint32_t context_id = context ? memory::load_and_swap<uint32_t>(context + 0) : 0;
      REXKRNL_DEBUG("XGIUserGetContext({:08X}, {:08X}, {:08X}))", user_index, context_ptr,
                    context_id);
      auto* live = kernel_state_->live_compatibility();
      if (user_index != 0 || !context || !live) {
        return X_E_INVALIDARG;
      }
      const auto value = live->GetUserContext(context_id);
      if (!value) {
        return X_E_FAIL;
      }
      memory::store_and_swap<uint32_t>(context + 4, *value);
      return X_E_SUCCESS;
    }
    case 0x000B0060: {
      if (!buffer || (buffer_length && buffer_length != 32)) {
        return X_E_INVALIDARG;
      }

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t num_session_ids = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t session_ids_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 24);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 28);

      REXKRNL_DEBUG("XSessionSearchByIds({:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {}, {}, {})",
                    user_index, num_session_ids, session_ids_ptr, results_buffer_size,
                    search_results_ptr, reserved1, reserved2, reserved3);

      return kUnsupportedHresult;
    }
    case 0x000B0065: {
      if (!buffer || (buffer_length && buffer_length != 52)) {
        return X_E_INVALIDARG;
      }

      uint32_t proc_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t num_results = memory::load_and_swap<uint32_t>(buffer + 8);
      uint16_t num_weighted_properties = memory::load_and_swap<uint16_t>(buffer + 12);
      uint16_t num_weighted_contexts = memory::load_and_swap<uint16_t>(buffer + 14);
      uint32_t weighted_search_properties_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t weighted_search_contexts_ptr = memory::load_and_swap<uint32_t>(buffer + 20);
      uint16_t num_props = memory::load_and_swap<uint16_t>(buffer + 24);
      uint16_t num_ctx = memory::load_and_swap<uint16_t>(buffer + 26);
      uint32_t non_weighted_search_properties_ptr = memory::load_and_swap<uint32_t>(buffer + 28);
      uint32_t non_weighted_search_contexts_ptr = memory::load_and_swap<uint32_t>(buffer + 32);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 36);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 40);
      uint32_t num_users = memory::load_and_swap<uint32_t>(buffer + 44);
      uint32_t weighted_search = memory::load_and_swap<uint32_t>(buffer + 48);

      REXKRNL_DEBUG(
          "XSessionSearchWeighted({:08X}, {:08X}, {:08X}, {}, {}, {:08X}, {:08X}, {}, {}, {:08X}, "
          "{:08X}, {:08X}, {:08X}, {:08X}, {:08X})",
          proc_index, user_index, num_results, num_weighted_properties, num_weighted_contexts,
          weighted_search_properties_ptr, weighted_search_contexts_ptr, num_props, num_ctx,
          non_weighted_search_properties_ptr, non_weighted_search_contexts_ptr, results_buffer_size,
          search_results_ptr, num_users, weighted_search);

      return kUnsupportedHresult;
    }
    case 0x000B0071: {
      REXKRNL_DEBUG("XGI 0x000B0071, unimplemented");
      return kUnsupportedHresult;
    }
  }
  REXKRNL_ERROR(
      "Unimplemented XGI message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
