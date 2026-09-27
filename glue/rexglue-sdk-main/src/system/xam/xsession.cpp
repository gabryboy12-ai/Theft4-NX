/**
 ******************************************************************************
 * @file        xsession.cpp
 * @brief       Xbox 360 session object backed by LibertyRecomp directories.
 ******************************************************************************
 */

#include <rex/system/xam/xsession.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <ranges>

#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xam/multiplayer_validation.h>
#include <rex/system/xam/user_profile.h>
#include "xsession_internal.h"
#include <rex/system/xmemory.h>

namespace rex::system::xam {

namespace detail {
bool IsGta4MigrateFollowerUserIndex(uint32_t user_index);
bool HasUsableSessionJoinDescriptor(const SessionRecord& session);
bool AddSessionMemberWithSlotFallback(SessionRecord& record, SessionMember& member);
std::optional<uint32_t> SessionSearchResultsRequiredSize(
    std::span<const SessionRecord> sessions);
X_RESULT WriteSessionSearchResultsToBuffer(std::span<uint8_t> output,
                                           uint32_t output_guest_address,
                                           std::span<const SessionRecord> sessions);
}  // namespace detail

namespace {

constexpr uint32_t kSessionFlagHost = 0x00000001;
constexpr uint32_t kMemberFlagPrivate = 0x00000001;
constexpr uint32_t kNoUserIndex = 0xFFFFFFFF;
// GTA IV's generated XSessionMigrate caller passes 0xFE when this machine is
// following a newly elected host. This is title-specific and must not replace
// the normal UINT32_MAX no-user value in other XSession structures.
constexpr uint32_t kGta4MigrateFollowerUserIndex = 0x000000FE;
constexpr uint16_t kDefaultGamePort = 3074;
constexpr uint32_t kMaximumSearchResults = 64;
constexpr uint32_t kMaximumSearchContexts = 64;
constexpr uint32_t kMaximumSearchProperties = 64;
constexpr uint32_t kMaximumPropertySize = 512;
// sub_82A37228 validates the caller's buffer against 8 + count * 1326.
constexpr uint32_t kGta4SearchResultBudget = 1326;

struct TitleIdentity {
  uint32_t title_id = 0;
  uint32_t media_id = 0;
  uint32_t title_version = 0;
};

bool IsWritableGuestRange(memory::Memory* memory, uint32_t address, size_t size) {
  if (!memory || !address || !size || size > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  const uint64_t end = static_cast<uint64_t>(address) + size - 1;
  if (end > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  auto* heap = memory->LookupHeap(address);
  if (!heap || heap != memory->LookupHeap(static_cast<uint32_t>(end))) {
    return false;
  }
  memory::HeapAllocationInfo allocation{};
  if (!heap->QueryRegionInfo(address, &allocation) ||
      !(allocation.state & memory::kMemoryAllocationCommit)) {
    return false;
  }
  const uint64_t allocation_end =
      static_cast<uint64_t>(allocation.allocation_base) + allocation.allocation_size;
  if (end >= allocation_end) {
    return false;
  }
  return heap->QueryRangeAccess(address, static_cast<uint32_t>(end)) ==
         memory::PageAccess::kReadWrite;
}

TitleIdentity GetTitleIdentity(KernelState* kernel_state) {
  TitleIdentity identity{.title_id = kernel_state->title_id()};
  auto module = kernel_state->GetExecutableModule();
  if (!module) {
    return identity;
  }
  xex2_opt_execution_info* execution_info = nullptr;
  if (XSUCCEEDED(module->GetOptHeader(XEX_HEADER_EXECUTION_INFO, &execution_info)) &&
      execution_info) {
    identity.media_id = execution_info->media_id;
    identity.title_version = execution_info->version_value;
  }
  return identity;
}

bool RemoveMemberFromRecord(SessionRecord& record, uint64_t xuid) {
  auto member = std::ranges::find(record.members, xuid, &SessionMember::xuid);
  if (member == record.members.end()) {
    return true;
  }
  if (member->private_slot) {
    record.open_private_slots = std::min(record.max_private_slots, record.open_private_slots + 1);
  } else {
    record.open_public_slots = std::min(record.max_public_slots, record.open_public_slots + 1);
  }
  record.members.erase(member);
  return true;
}

void PublishValidationMembership(const SessionRecord& record) {
  std::vector<MultiplayerValidationMember> members;
  members.reserve(record.members.size());
  for (const SessionMember& member : record.members) {
    if (member.multiplayer_peer_id >= kMaximumSessionMembers) continue;
    members.push_back({.xuid = member.xuid,
                       .peer_id = static_cast<uint8_t>(member.multiplayer_peer_id)});
  }
  PublishMultiplayerValidationSession(record.session_id, members);
}

XUserDataType PropertyType(uint32_t id) {
  return static_cast<XUserDataType>((id >> 28) & 0x0F);
}

size_t ScalarPropertySize(XUserDataType type) {
  switch (type) {
    case XUserDataType::kContext:
    case XUserDataType::kInt32:
    case XUserDataType::kFloat:
      return sizeof(uint32_t);
    case XUserDataType::kInt64:
    case XUserDataType::kDouble:
    case XUserDataType::kDateTime:
      return sizeof(uint64_t);
    case XUserDataType::kWString:
    case XUserDataType::kBinary:
    case XUserDataType::kUnset:
      return 0;
  }
  return 0;
}

SessionProperty ReadProperty(KernelState* kernel_state, const XUSER_PROPERTY& property) {
  SessionProperty result;
  result.id = property.property_id;
  const auto type =
      property.data.type == XUserDataType::kUnset ? PropertyType(result.id) : property.data.type;
  if (type == XUserDataType::kWString || type == XUserDataType::kBinary) {
    const uint32_t size = property.data.value.binary.size;
    const uint32_t pointer = property.data.value.binary.pointer;
    if (size && pointer) {
      const auto* bytes = kernel_state->memory()->TranslateVirtual<const uint8_t*>(pointer);
      result.value.assign(bytes, bytes + size);
    }
  } else {
    const size_t size = ScalarPropertySize(type);
    const auto* bytes = reinterpret_cast<const uint8_t*>(&property.data.value);
    result.value.assign(bytes, bytes + size);
  }
  return result;
}

struct SearchResultLayout {
  size_t contexts_offset = 0;
  size_t properties_offset = 0;
  std::vector<size_t> property_data_offsets;
};

struct SearchResultsLayout {
  size_t required_size = 0;
  std::vector<SearchResultLayout> results;
};

bool AdvanceOffset(size_t& offset, size_t amount) {
  if (amount > std::numeric_limits<size_t>::max() - offset) {
    return false;
  }
  offset += amount;
  return true;
}

bool AlignOffset(size_t& offset, size_t alignment) {
  const size_t mask = alignment - 1;
  if (offset > std::numeric_limits<size_t>::max() - mask) {
    return false;
  }
  offset = (offset + mask) & ~mask;
  return true;
}

std::optional<SearchResultsLayout> BuildSearchResultsLayout(
    std::span<const SessionRecord> sessions) {
  SearchResultsLayout layout;
  layout.required_size = sizeof(XSESSION_SEARCHRESULT_HEADER);
  if (sessions.size() > kMaximumSearchResults ||
      !AdvanceOffset(layout.required_size, sessions.size() * sizeof(XSESSION_SEARCHRESULT))) {
    return std::nullopt;
  }
  layout.results.resize(sessions.size());
  for (size_t session_index = 0; session_index < sessions.size(); ++session_index) {
    const SessionRecord& session = sessions[session_index];
    if (!IsValidSessionRecord(session)) {
      return std::nullopt;
    }
    SearchResultLayout& result = layout.results[session_index];
    if (!session.contexts.empty()) {
      if (!AlignOffset(layout.required_size, alignof(XUSER_CONTEXT))) {
        return std::nullopt;
      }
      result.contexts_offset = layout.required_size;
      if (!AdvanceOffset(layout.required_size,
                         session.contexts.size() * sizeof(XUSER_CONTEXT))) {
        return std::nullopt;
      }
    }
    if (!session.properties.empty()) {
      if (!AlignOffset(layout.required_size, alignof(XUSER_PROPERTY))) {
        return std::nullopt;
      }
      result.properties_offset = layout.required_size;
      if (!AdvanceOffset(layout.required_size,
                         session.properties.size() * sizeof(XUSER_PROPERTY))) {
        return std::nullopt;
      }
      result.property_data_offsets.resize(session.properties.size());
      for (size_t property_index = 0; property_index < session.properties.size();
           ++property_index) {
        const SessionProperty& property = session.properties[property_index];
        const XUserDataType type = PropertyType(property.id);
        if ((type != XUserDataType::kWString && type != XUserDataType::kBinary) ||
            property.value.empty()) {
          continue;
        }
        if (type == XUserDataType::kWString &&
            !AlignOffset(layout.required_size, alignof(rex::be<char16_t>))) {
          return std::nullopt;
        }
        result.property_data_offsets[property_index] = layout.required_size;
        if (!AdvanceOffset(layout.required_size, property.value.size())) {
          return std::nullopt;
        }
      }
    }
  }
  if (layout.required_size > std::numeric_limits<uint32_t>::max()) {
    return std::nullopt;
  }
  return layout;
}

std::vector<SessionContext> ReadSearchContexts(KernelState* kernel_state,
                                               const XGI_SESSION_SEARCH& request) {
  std::vector<SessionContext> result;
  const uint32_t count = request.context_count;
  const uint32_t pointer = request.contexts_ptr;
  if (!count || !pointer) {
    return result;
  }
  const auto* contexts = kernel_state->memory()->TranslateVirtual<const XUSER_CONTEXT*>(pointer);
  result.reserve(count);
  for (uint32_t index = 0; index < count; ++index) {
    result.push_back({.id = contexts[index].context_id, .value = contexts[index].value});
  }
  return result;
}

std::vector<SessionProperty> ReadSearchProperties(KernelState* kernel_state,
                                                  const XGI_SESSION_SEARCH& request) {
  std::vector<SessionProperty> result;
  const uint32_t count = request.property_count;
  const uint32_t pointer = request.properties_ptr;
  if (!count || !pointer) {
    return result;
  }
  const auto* properties = kernel_state->memory()->TranslateVirtual<const XUSER_PROPERTY*>(pointer);
  result.reserve(count);
  for (uint32_t index = 0; index < count; ++index) {
    result.push_back(ReadProperty(kernel_state, properties[index]));
  }
  return result;
}

X_RESULT WriteSearchResults(KernelState* kernel_state, uint32_t results_ptr,
                            uint32_t results_buffer_size, std::span<const SessionRecord> sessions) {
  const auto required_size = detail::SessionSearchResultsRequiredSize(sessions);
  if (!required_size) {
    return X_ERROR_INVALID_PARAMETER;
  }
  if (!results_ptr || results_buffer_size < *required_size) {
    return X_ERROR_INSUFFICIENT_BUFFER;
  }
  if (!IsWritableGuestRange(kernel_state->memory(), results_ptr, *required_size)) {
    return X_ERROR_INVALID_PARAMETER;
  }
  auto* output = kernel_state->memory()->TranslateVirtual<uint8_t*>(results_ptr);
  return detail::WriteSessionSearchResultsToBuffer(
      std::span<uint8_t>(output, *required_size), results_ptr, sessions);
}

}  // namespace

namespace detail {

bool IsGta4MigrateFollowerUserIndex(uint32_t user_index) {
  return user_index == kGta4MigrateFollowerUserIndex;
}

bool HasUsableSessionJoinDescriptor(const SessionRecord& session) {
  return session.session_id && session.nonce && session.host_xuid &&
         session.host_machine_id && session.host_ipv4 && session.host_port &&
         std::ranges::any_of(session.exchange_key,
                             [](uint8_t byte) { return byte != 0; }) &&
         std::ranges::any_of(session.host_ethernet_address,
                             [](uint8_t byte) { return byte != 0; });
}

bool AddSessionMemberWithSlotFallback(SessionRecord& record, SessionMember& member) {
  auto existing =
      std::ranges::find(record.members, member.xuid, &SessionMember::xuid);
  if (existing != record.members.end()) {
    if (existing->private_slot == member.private_slot) {
      // A repeated JoinLocal is also the title's opportunity to publish the
      // transport identity selected after socket setup. Keep directory-owned
      // route identity, but do not turn a same-class join into a no-op when
      // the local machine/address/port became authoritative after Create.
      if (member.machine_id) existing->machine_id = member.machine_id;
      if (member.virtual_ipv4) existing->virtual_ipv4 = member.virtual_ipv4;
      if (member.online_port) existing->online_port = member.online_port;
      member = *existing;
      return true;
    }

    // XSessionCreate provisionally publishes the host as a public member.
    // GTA then calls XSessionJoinLocal with the title-selected slot class. A
    // slot-class change is a real directory mutation: release the old class
    // only after proving that the requested class has capacity, then return
    // the authoritative member through the in/out parameter so Join can
    // forward the reclassification to the directory.
    if (member.private_slot) {
      if (!record.open_private_slots) {
        member = *existing;
        return true;
      }
      record.open_public_slots =
          std::min(record.max_public_slots, record.open_public_slots + 1);
      --record.open_private_slots;
    } else {
      if (!record.open_public_slots) {
        member = *existing;
        return true;
      }
      record.open_private_slots =
          std::min(record.max_private_slots, record.open_private_slots + 1);
      --record.open_public_slots;
    }
    existing->private_slot = member.private_slot;
    if (member.machine_id) existing->machine_id = member.machine_id;
    if (member.virtual_ipv4) existing->virtual_ipv4 = member.virtual_ipv4;
    if (member.online_port) existing->online_port = member.online_port;
    member = *existing;
    return true;
  }
  if (member.private_slot && record.open_private_slots) {
    --record.open_private_slots;
  } else if (record.open_public_slots) {
    member.private_slot = false;
    --record.open_public_slots;
  } else if (record.open_private_slots) {
    member.private_slot = true;
    --record.open_private_slots;
  } else {
    return false;
  }
  record.members.push_back(member);
  return true;
}

std::optional<uint32_t> SessionSearchResultsRequiredSize(
    std::span<const SessionRecord> sessions) {
  const auto layout = BuildSearchResultsLayout(sessions);
  if (!layout) {
    return std::nullopt;
  }
  return static_cast<uint32_t>(layout->required_size);
}

X_RESULT WriteSessionSearchResultsToBuffer(std::span<uint8_t> output,
                                           uint32_t output_guest_address,
                                           std::span<const SessionRecord> sessions) {
  const auto layout = BuildSearchResultsLayout(sessions);
  if (!layout || !output_guest_address) {
    return X_ERROR_INVALID_PARAMETER;
  }
  if (output.size() < layout->required_size) {
    return X_ERROR_INSUFFICIENT_BUFFER;
  }
  const uint64_t output_end = static_cast<uint64_t>(output_guest_address) +
                              static_cast<uint64_t>(layout->required_size) - 1;
  if (output_end > std::numeric_limits<uint32_t>::max()) {
    return X_ERROR_INVALID_PARAMETER;
  }

  std::memset(output.data(), 0, layout->required_size);
  auto* header = reinterpret_cast<XSESSION_SEARCHRESULT_HEADER*>(output.data());
  auto* results = reinterpret_cast<XSESSION_SEARCHRESULT*>(output.data() + sizeof(*header));
  header->search_results_count = static_cast<uint32_t>(sessions.size());
  header->search_results_ptr =
      output_guest_address + static_cast<uint32_t>(sizeof(*header));

  for (size_t session_index = 0; session_index < sessions.size(); ++session_index) {
    const SessionRecord& session = sessions[session_index];
    const SearchResultLayout& result_layout = layout->results[session_index];
    XSESSION_SEARCHRESULT& result = results[session_index];
    SessionRecordToGuestInfo(session, result.info);
    result.open_public_slots = session.open_public_slots;
    result.open_private_slots = session.open_private_slots;
    result.filled_public_slots = session.max_public_slots - session.open_public_slots;
    result.filled_private_slots = session.max_private_slots - session.open_private_slots;

    if (!session.contexts.empty()) {
      auto* contexts = reinterpret_cast<XUSER_CONTEXT*>(
          output.data() + result_layout.contexts_offset);
      for (size_t context_index = 0; context_index < session.contexts.size(); ++context_index) {
        contexts[context_index].context_id = session.contexts[context_index].id;
        contexts[context_index].value = session.contexts[context_index].value;
      }
      result.contexts_count = static_cast<uint32_t>(session.contexts.size());
      result.contexts_ptr =
          output_guest_address + static_cast<uint32_t>(result_layout.contexts_offset);
    }

    if (!session.properties.empty()) {
      auto* properties = reinterpret_cast<XUSER_PROPERTY*>(
          output.data() + result_layout.properties_offset);
      for (size_t property_index = 0; property_index < session.properties.size();
           ++property_index) {
        const SessionProperty& source = session.properties[property_index];
        XUSER_PROPERTY& destination = properties[property_index];
        destination.property_id = source.id;
        destination.data.type = PropertyType(source.id);
        if (destination.data.type == XUserDataType::kWString ||
            destination.data.type == XUserDataType::kBinary) {
          if (!source.value.empty()) {
            const size_t data_offset = result_layout.property_data_offsets[property_index];
            std::memcpy(output.data() + data_offset, source.value.data(), source.value.size());
            destination.data.value.binary.size = static_cast<uint32_t>(source.value.size());
            destination.data.value.binary.pointer =
                output_guest_address + static_cast<uint32_t>(data_offset);
          }
        } else if (!source.value.empty()) {
          std::memcpy(&destination.data.value, source.value.data(),
                      std::min(source.value.size(), sizeof(destination.data.value)));
        }
      }
      result.properties_count = static_cast<uint32_t>(session.properties.size());
      result.properties_ptr =
          output_guest_address + static_cast<uint32_t>(result_layout.properties_offset);
    }
  }
  return X_ERROR_SUCCESS;
}

X_RESULT WriteSessionArbitrationResultsToBuffer(std::span<uint8_t> output,
                                                uint32_t output_guest_address,
                                                std::span<const SessionMember> members) {
  constexpr uint32_t kTrustworthy = 1;

  struct MachineRegistrant {
    uint64_t machine_id = 0;
    std::vector<uint64_t> users;
  };

  if (output.size() < kSessionArbitrationResultsSize) {
    return X_ERROR_INSUFFICIENT_BUFFER;
  }
  if (!output_guest_address || members.empty() || members.size() > kMaximumSessionMembers) {
    return X_ERROR_INVALID_PARAMETER;
  }

  std::vector<MachineRegistrant> machines;
  machines.reserve(members.size());
  std::vector<uint64_t> seen_users;
  seen_users.reserve(members.size());
  for (const auto& member : members) {
    if (!member.xuid || !member.machine_id ||
        std::ranges::find(seen_users, member.xuid) != seen_users.end()) {
      return X_ERROR_INVALID_PARAMETER;
    }
    seen_users.push_back(member.xuid);
    auto machine = std::ranges::find(machines, member.machine_id, &MachineRegistrant::machine_id);
    if (machine == machines.end()) {
      if (machines.size() == kMaximumSessionMembers) {
        return X_ERROR_INVALID_PARAMETER;
      }
      machines.push_back({.machine_id = member.machine_id, .users = {}});
      machine = std::prev(machines.end());
    }
    if (machine->users.size() == kMaximumArbitrationUsersPerMachine) {
      return X_ERROR_INVALID_PARAMETER;
    }
    machine->users.push_back(member.xuid);
  }

  std::memset(output.data(), 0, kSessionArbitrationResultsSize);
  auto* header = reinterpret_cast<XSESSION_REGISTRATION_RESULTS*>(output.data());
  auto* registrants = reinterpret_cast<XSESSION_REGISTRANT*>(header + 1);
  auto* user_slots = reinterpret_cast<rex::be<uint64_t>*>(registrants + kMaximumSessionMembers);

  header->registrant_count = static_cast<uint32_t>(machines.size());
  header->registrants_ptr = output_guest_address + sizeof(*header);
  for (size_t machine_index = 0; machine_index < machines.size(); ++machine_index) {
    const auto& machine = machines[machine_index];
    auto& registrant = registrants[machine_index];
    auto* machine_users = user_slots + machine_index * kMaximumArbitrationUsersPerMachine;
    registrant.machine_id = machine.machine_id;
    registrant.trustworthy = kTrustworthy;
    registrant.user_count = static_cast<uint32_t>(machine.users.size());
    registrant.users_ptr =
        output_guest_address +
        static_cast<uint32_t>(reinterpret_cast<uint8_t*>(machine_users) - output.data());
    for (size_t user_index = 0; user_index < machine.users.size(); ++user_index) {
      machine_users[user_index] = machine.users[user_index];
    }
  }
  return X_ERROR_SUCCESS;
}

X_RESULT WriteSessionArbitrationResultsToGuest(memory::Memory* memory, uint32_t results_ptr,
                                               uint32_t results_buffer_size,
                                               std::span<const SessionMember> members) {
  if (results_buffer_size < kSessionArbitrationResultsSize) {
    return X_ERROR_INSUFFICIENT_BUFFER;
  }
  if (!IsWritableGuestRange(memory, results_ptr, kSessionArbitrationResultsSize)) {
    return X_ERROR_INVALID_PARAMETER;
  }
  auto* output = memory->TranslateVirtual<uint8_t*>(results_ptr);
  return WriteSessionArbitrationResultsToBuffer(
      std::span<uint8_t>(output, kSessionArbitrationResultsSize), results_ptr, members);
}

bool IsSessionArbitrationCompletionCompatible(
    const XSESSION_ARBITRATION_CONTEXT& context,
    const SessionRecord& current, const SessionRecord& registered) {
  const bool current_lifecycle_valid =
      current.lifecycle_state == SessionLifecycleState::kLobby ||
      current.lifecycle_state == SessionLifecycleState::kRegistration;
  return context.session_id && context.nonce &&
         current.session_id == context.session_id &&
         current.nonce == context.nonce && current_lifecycle_valid &&
         registered.session_id == context.session_id &&
         registered.nonce == context.nonce &&
         registered.lifecycle_state == SessionLifecycleState::kRegistration &&
         IsValidSessionRecord(registered);
}

X_RESULT WriteSessionDetailsToGuest(memory::Memory* memory, uint32_t details_size_ptr,
                                    uint32_t details_ptr,
                                    const XSESSION_LOCAL_DETAILS& details_template,
                                    std::span<const SessionMember> members, uint64_t local_xuid) {
  if (!IsWritableGuestRange(memory, details_size_ptr, sizeof(rex::be<uint32_t>)) ||
      members.size() > kMaximumSessionMembers) {
    return X_ERROR_INVALID_PARAMETER;
  }

  auto* size_value = memory->TranslateVirtual<rex::be<uint32_t>*>(details_size_ptr);
  const uint32_t declared_size = *size_value;
  const size_t required_size =
      sizeof(XSESSION_LOCAL_DETAILS) + members.size() * sizeof(XSESSION_MEMBER);
  static_assert(sizeof(XSESSION_LOCAL_DETAILS) + kMaximumSessionMembers * sizeof(XSESSION_MEMBER) <=
                std::numeric_limits<uint32_t>::max());
  *size_value = static_cast<uint32_t>(required_size);

  if (!details_ptr || declared_size < sizeof(XSESSION_LOCAL_DETAILS)) {
    return X_ERROR_INSUFFICIENT_BUFFER;
  }

  const size_t member_capacity =
      (declared_size - sizeof(XSESSION_LOCAL_DETAILS)) / sizeof(XSESSION_MEMBER);
  const size_t returned_member_count = std::min(members.size(), member_capacity);
  const size_t written_size =
      sizeof(XSESSION_LOCAL_DETAILS) + returned_member_count * sizeof(XSESSION_MEMBER);
  if (!IsWritableGuestRange(memory, details_ptr, written_size)) {
    return X_ERROR_INVALID_PARAMETER;
  }

  XSESSION_LOCAL_DETAILS output = details_template;
  output.actual_member_count = static_cast<uint32_t>(members.size());
  output.returned_member_count = static_cast<uint32_t>(returned_member_count);
  output.session_members_ptr =
      returned_member_count ? details_ptr + static_cast<uint32_t>(sizeof(XSESSION_LOCAL_DETAILS))
                            : 0;

  auto* details = memory->TranslateVirtual<XSESSION_LOCAL_DETAILS*>(details_ptr);
  std::memcpy(details, &output, sizeof(output));
  auto* guest_members = reinterpret_cast<XSESSION_MEMBER*>(details + 1);
  for (size_t index = 0; index < returned_member_count; ++index) {
    guest_members[index].online_xuid = members[index].xuid;
    guest_members[index].user_index = members[index].xuid == local_xuid ? 0 : kNoUserIndex;
    guest_members[index].flags = members[index].private_slot ? kMemberFlagPrivate : 0;
  }
  return X_ERROR_SUCCESS;
}

}  // namespace detail

uint64_t XnkidToUint64(const XNKID& id) {
  rex::be<uint64_t> value;
  std::memcpy(&value, id.value.data(), id.value.size());
  return value;
}

void Uint64ToXnkid(uint64_t value, XNKID& id) {
  rex::be<uint64_t> encoded = value;
  std::memcpy(id.value.data(), &encoded, id.value.size());
}

void SessionRecordToGuestInfo(const SessionRecord& record, XSESSION_INFO& info) {
  std::memset(&info, 0, sizeof(info));
  Uint64ToXnkid(record.session_id, info.session_id);
  info.host_address.local_ipv4 = record.host_ipv4;
  info.host_address.online_ipv4 = record.host_ipv4;
  info.host_address.online_port = record.host_port;
  info.host_address.ethernet_address = record.host_ethernet_address;
  rex::be<uint64_t> machine_id = record.host_machine_id;
  std::memcpy(info.host_address.online_identity.data(), &machine_id, sizeof(machine_id));
  info.exchange_key.value = record.exchange_key;
}

XSession::XSession(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

XSession::~XSession() {
  auto* live = kernel_state_->live_compatibility();
  if (created_ && live) {
    bool directory_cleanup_succeeded = true;
    if (host_ && live->session_directory()) {
      directory_cleanup_succeeded =
          live->session_directory()->Delete(record_.session_id);
    } else if (live->session_directory()) {
      const uint64_t local_xuid = live->identity().xuid;
      if (std::ranges::find(record_.members, local_xuid,
                            &SessionMember::xuid) != record_.members.end()) {
        directory_cleanup_succeeded =
            live->session_directory()->Leave(record_.session_id, local_xuid);
      }
    }
    if (!directory_cleanup_succeeded) {
      REXSYS_WARN(
          "gta4-session-op operation=destructor-cleanup role={} session={:016X} "
          "result=directory-unavailable",
          host_ ? "host" : "peer", record_.session_id);
    }
    live->UnregisterKey(record_.session_id);
    live->UnregisterRoute(record_.host_ipv4);
    const bool was_active = live->active_session_id() == record_.session_id;
    live->ClearActiveSession(record_.session_id);
    if (was_active) {
      live->CloseVoiceSession(record_.session_id);
    }
  }
}

X_STATUS XSession::Initialize() {
  auto* object = CreateNative<X_KSESSION>();
  if (!object) {
    return X_STATUS_NO_MEMORY;
  }
  object->handle = handle();
  return X_STATUS_SUCCESS;
}

X_RESULT XSession::Create(const XGI_SESSION_CREATE& request) {
  if (created_ || !request.session_info_ptr || !request.nonce_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }
  auto* live = kernel_state_->live_compatibility();
  if (!live || !live->available() || !live->session_directory()) {
    return X_ERROR_NOT_LOGGED_ON;
  }
  if (request.user_index != 0) {
    return X_ERROR_NO_SUCH_USER;
  }
  if (request.public_slots > kMaximumSessionMembers ||
      request.private_slots > kMaximumSessionMembers ||
      request.private_slots > kMaximumSessionMembers - request.public_slots) {
    return X_ERROR_INVALID_PARAMETER;
  }

  auto* session_info =
      kernel_state_->memory()->TranslateVirtual<XSESSION_INFO*>(request.session_info_ptr);
  auto* nonce = kernel_state_->memory()->TranslateVirtual<rex::be<uint64_t>*>(request.nonce_ptr);
  const auto title = GetTitleIdentity(kernel_state_);

  host_ = (static_cast<uint32_t>(request.flags) & kSessionFlagHost) != 0;
  record_.title_id = title.title_id;
  record_.media_id = title.media_id;
  record_.title_version = title.title_version;
  record_.protocol_version = live->config().session_protocol_version;
  record_.flags = request.flags;
  record_.max_public_slots = request.public_slots;
  record_.max_private_slots = request.private_slots;
  record_.open_public_slots = request.public_slots;
  record_.open_private_slots = request.private_slots;
  record_.contexts = live->user_contexts();
  record_.properties = live->user_properties();

  if (host_) {
    record_.session_id = live->GenerateSessionId();
    record_.nonce = live->GenerateNonce();
    live->GenerateExchangeKey(record_.exchange_key);
    record_.host_xuid = live->identity().xuid;
    record_.host_machine_id = live->identity().machine_id;
    record_.host_ipv4 = live->local_ipv4();
    record_.host_port = live->online_port() ? live->online_port() : kDefaultGamePort;
    record_.host_ethernet_address = live->identity().ethernet_address;
    if (!live->session_directory()->Create(record_)) {
      return X_ERROR_FUNCTION_FAILED;
    }
    if (auto authoritative = live->session_directory()->Get(record_.session_id)) {
      record_ = std::move(*authoritative);
    }
  } else {
    const uint64_t requested_id = XnkidToUint64(session_info->session_id);
    auto existing = live->session_directory()->Get(requested_id);
    if (!existing || !detail::HasUsableSessionJoinDescriptor(*existing) ||
        (live->config().backend == LiveBackend::kCommunity &&
         existing->host_peer_id.empty())) {
      return X_ERROR_NOT_FOUND;
    }
    record_ = *existing;
  }

  SessionRecordToGuestInfo(record_, *session_info);
  *nonce = record_.nonce;
  live->RegisterKey(record_.session_id, record_.exchange_key);
  live->RegisterRoute(record_.host_ipv4, record_);
  live->SetActiveSession(record_.session_id);
  (void)live->ConfigureVoiceSession(record_.session_id,
                                    VoiceSessionTransition::kNewSession);
  created_ = true;
  RefreshLocalDetails();
  REXSYS_INFO("Created {} session {:016X} with {} public and {} private slots",
              host_ ? "host" : "peer", record_.session_id, record_.max_public_slots,
              record_.max_private_slots);
  return X_ERROR_SUCCESS;
}

X_RESULT XSession::Delete(const XGI_SESSION_STATE& request) {
  if (!created_) {
    return X_ERROR_SUCCESS;
  }
  auto* live = kernel_state_->live_compatibility();
  if (live && live->session_directory()) {
    auto* directory = live->session_directory();
    if (host_) {
      if (!directory->Delete(record_.session_id)) {
        return X_ERROR_FUNCTION_FAILED;
      }
    } else {
      const uint64_t local_xuid = live->identity().xuid;
      const bool local_member =
          std::ranges::find(record_.members, local_xuid,
                            &SessionMember::xuid) != record_.members.end();
      if (local_member && !directory->Leave(record_.session_id, local_xuid)) {
        const SessionLookupResult current = directory->Lookup(record_.session_id);
        if (current.state == SessionLookupState::kUnavailable ||
            (current.session &&
             std::ranges::find(current.session->members, local_xuid,
                               &SessionMember::xuid) !=
                 current.session->members.end())) {
          return X_ERROR_FUNCTION_FAILED;
        }
      }
    }
  }
  if (live) {
    live->UnregisterKey(record_.session_id);
    live->UnregisterRoute(record_.host_ipv4);
    const bool was_active = live->active_session_id() == record_.session_id;
    live->ClearActiveSession(record_.session_id);
    if (was_active) {
      live->CloseVoiceSession(record_.session_id);
    }
  }
  created_ = false;
  host_ = false;
  record_.lifecycle_state = SessionLifecycleState::kDeleted;
  local_details_.state = static_cast<uint32_t>(XSessionState::kDeleted);
  return X_ERROR_SUCCESS;
}

X_RESULT XSession::Join(const XGI_SESSION_MANAGE& request) {
  if (!created_ || !request.count || request.count > kMaximumSessionMembers) {
    return X_ERROR_INVALID_PARAMETER;
  }
  auto* live = kernel_state_->live_compatibility();
  if (!live || !live->session_directory()) {
    return X_ERROR_NOT_LOGGED_ON;
  }
  if (!SyncRecordFromDirectory()) {
    return X_ERROR_NOT_FOUND;
  }

  const auto* xuids =
      request.xuids_ptr
          ? kernel_state_->memory()->TranslateVirtual<const rex::be<uint64_t>*>(request.xuids_ptr)
          : nullptr;
  const auto* user_indices =
      request.user_indices_ptr
          ? kernel_state_->memory()->TranslateVirtual<const rex::be<uint32_t>*>(
                request.user_indices_ptr)
          : nullptr;
  const auto* private_slots =
      request.private_slots_ptr
          ? kernel_state_->memory()->TranslateVirtual<const rex::be<uint32_t>*>(
                request.private_slots_ptr)
          : nullptr;

  SessionRecord candidate = record_;
  std::vector<SessionMember> members;
  std::vector<SessionMember> local_joins;
  members.reserve(request.count);
  local_joins.reserve(request.count);
  for (uint32_t index = 0; index < request.count; ++index) {
    uint64_t xuid = 0;
    if (xuids) {
      xuid = xuids[index];
    } else {
      if (!user_indices || user_indices[index] != 0) {
        return X_ERROR_NO_SUCH_USER;
      }
      xuid = kernel_state_->user_profile()->xuid();
    }
    if (!xuid) {
      return X_ERROR_INVALID_PARAMETER;
    }
    SessionMember member{.xuid = xuid, .private_slot = private_slots && private_slots[index] != 0};
    if (xuid == live->identity().xuid) {
      member.machine_id = live->identity().machine_id;
      member.virtual_ipv4 = live->local_ipv4();
      member.online_port = live->online_port() ? live->online_port() : kDefaultGamePort;
    }
    const auto existing =
        std::ranges::find(candidate.members, xuid, &SessionMember::xuid);
    const std::optional<SessionMember> previous_member =
        existing == candidate.members.end()
            ? std::nullopt
            : std::optional<SessionMember>(*existing);
    if (!detail::AddSessionMemberWithSlotFallback(candidate, member)) {
      return X_ERROR_FUNCTION_FAILED;
    }
    const auto updated =
        std::ranges::find(candidate.members, xuid, &SessionMember::xuid);
    if (updated == candidate.members.end()) return X_ERROR_FUNCTION_FAILED;
    members.push_back(member);
    if (xuid == live->identity().xuid &&
        (!previous_member || *previous_member != *updated)) {
      local_joins.push_back(*updated);
    }
  }

  // GTA distributes remote roster notifications to every machine. Only the
  // local identity may authenticate its own server join; remote entries are
  // title-facing roster replication, including on the host. Sending the host
  // snapshot through PATCH is both racy and forbidden by the server contract.
  std::vector<uint64_t> joined_local_xuids;
  joined_local_xuids.reserve(local_joins.size());
  for (const auto& member : local_joins) {
    if (!live->session_directory()->Join(record_.session_id, member)) {
      for (const uint64_t joined_xuid : joined_local_xuids) {
        live->session_directory()->Leave(record_.session_id, joined_xuid);
      }
      REXSYS_WARN(
          "gta4-session-op operation=join role={} local={} remote={} session={:016X} "
          "result={}",
          host_ ? "host" : "peer", local_joins.size(),
          members.size() - local_joins.size(), record_.session_id,
          X_ERROR_FUNCTION_FAILED);
      return X_ERROR_FUNCTION_FAILED;
    }
    joined_local_xuids.push_back(member.xuid);
  }

  record_ = std::move(candidate);
  if (!local_joins.empty()) {
    if (auto authoritative = live->session_directory()->Get(record_.session_id)) {
      // Preserve remote roster notifications that were part of this title
      // operation but have not yet reached the directory observer.
      for (const auto& member : members) {
        if (member.xuid != live->identity().xuid) {
          SessionMember replicated = member;
          (void)detail::AddSessionMemberWithSlotFallback(*authoritative, replicated);
        }
      }
      record_ = std::move(*authoritative);
    }
  }
  PublishValidationMembership(record_);
  for (const SessionMember& joined : members) {
    const auto authoritative_member =
        std::ranges::find(record_.members, joined.xuid, &SessionMember::xuid);
    if (authoritative_member == record_.members.end() ||
        authoritative_member->multiplayer_peer_id >= kMaximumSessionMembers) {
      continue;
    }
    PublishMultiplayerValidationStage(
        MultiplayerValidationStage::kXSessionJoin,
        static_cast<uint8_t>(authoritative_member->multiplayer_peer_id));
  }
  // Refresh the route/key from the authoritative post-join record. This also
  // repairs any discovery descriptor that changed between search and join.
  live->RegisterKey(record_.session_id, record_.exchange_key);
  live->RegisterRoute(record_.host_ipv4, record_);
  live->SetActiveSession(record_.session_id);
  (void)live->ConfigureVoiceSession(
      record_.session_id, VoiceSessionTransition::kMembershipChanged);
  RefreshLocalDetails();
  REXSYS_INFO(
      "gta4-session-op operation=join role={} local={} remote={} members={} "
      "session={:016X} result={}",
      host_ ? "host" : "peer", local_joins.size(), members.size() - local_joins.size(),
      record_.members.size(), record_.session_id, X_ERROR_SUCCESS);
  return X_ERROR_SUCCESS;
}

X_RESULT XSession::Leave(const XGI_SESSION_MANAGE& request) {
  if (!created_ || !request.count || request.count > kMaximumSessionMembers) {
    return X_ERROR_INVALID_PARAMETER;
  }
  auto* live = kernel_state_->live_compatibility();
  if (!live || !live->session_directory()) {
    return X_ERROR_NOT_LOGGED_ON;
  }
  auto* directory = live->session_directory();
  const SessionLookupResult lookup = directory->Lookup(record_.session_id);
  if (lookup.state == SessionLookupState::kUnavailable) {
    return X_ERROR_FUNCTION_FAILED;
  }
  const bool directory_absent = lookup.state == SessionLookupState::kAbsent;
  if (lookup.session) {
    record_ = *lookup.session;
    PublishValidationMembership(record_);
  } else if (host_) {
    return X_ERROR_NOT_FOUND;
  }
  const auto* xuids =
      request.xuids_ptr
          ? kernel_state_->memory()->TranslateVirtual<const rex::be<uint64_t>*>(request.xuids_ptr)
          : nullptr;
  const auto* user_indices =
      request.user_indices_ptr
          ? kernel_state_->memory()->TranslateVirtual<const rex::be<uint32_t>*>(
                request.user_indices_ptr)
          : nullptr;
  SessionRecord candidate = record_;
  std::vector<SessionMember> removed_members;
  removed_members.reserve(request.count);
  for (uint32_t index = 0; index < request.count; ++index) {
    uint64_t xuid = 0;
    if (xuids) {
      xuid = xuids[index];
    } else {
      if (!user_indices || user_indices[index] != 0) {
        return X_ERROR_NO_SUCH_USER;
      }
      xuid = kernel_state_->user_profile()->xuid();
    }
    if (!xuid) {
      return X_ERROR_INVALID_PARAMETER;
    }
    const auto existing = std::ranges::find(candidate.members, xuid, &SessionMember::xuid);
    if (existing != candidate.members.end()) {
      removed_members.push_back(*existing);
      RemoveMemberFromRecord(candidate, xuid);
    }
  }

  bool used_authoritative_leave = false;
  for (const auto& member : removed_members) {
    const bool local_member = member.xuid == live->identity().xuid;
    const bool authoritative_leave = local_member || host_;
    bool completed_by_authoritative_absence = false;
    if (authoritative_leave && !directory_absent &&
        !directory->Leave(record_.session_id, member.xuid)) {
      const SessionLookupResult current = directory->Lookup(record_.session_id);
      if (current.session) {
        record_ = *current.session;
      } else if (local_member &&
                 current.state == SessionLookupState::kAbsent) {
        // The host may remove a private-session peer before the title's P2P
        // LeaveLocal notification reaches that peer. Server absence is then
        // the authoritative completion of the requested local removal.
        completed_by_authoritative_absence = true;
      } else {
        RefreshLocalDetails();
        REXSYS_WARN(
            "gta4-session-op operation=leave role={} local={} remote={} session={:016X} "
            "result={}",
            host_ ? "host" : "peer", local_member ? 1 : 0,
            local_member ? 0 : 1, record_.session_id,
            X_ERROR_FUNCTION_FAILED);
        return X_ERROR_FUNCTION_FAILED;
      }
      if (current.state != SessionLookupState::kAbsent) {
        RefreshLocalDetails();
        REXSYS_WARN(
            "gta4-session-op operation=leave role={} local={} remote={} session={:016X} "
            "result={}",
            host_ ? "host" : "peer", local_member ? 1 : 0,
            local_member ? 0 : 1, record_.session_id,
            X_ERROR_FUNCTION_FAILED);
        return X_ERROR_FUNCTION_FAILED;
      }
    }
    used_authoritative_leave =
        used_authoritative_leave ||
        (authoritative_leave && !directory_absent &&
         !completed_by_authoritative_absence);
  }

  record_ = std::move(candidate);
  if (used_authoritative_leave) {
    if (auto authoritative = live->session_directory()->Get(record_.session_id)) {
      // Nonhosts also receive remote leave notifications. Reapply those local
      // roster removals after refreshing the authenticated local mutation.
      if (!host_) {
        for (const auto& member : removed_members) {
          if (member.xuid != live->identity().xuid) {
            RemoveMemberFromRecord(*authoritative, member.xuid);
          }
        }
      }
      record_ = std::move(*authoritative);
    }
  }
  PublishValidationMembership(record_);
  RefreshLocalDetails();
  const auto local_leaves = std::ranges::count_if(
      removed_members,
      [live](const SessionMember& member) { return member.xuid == live->identity().xuid; });
  REXSYS_INFO(
      "gta4-session-op operation=leave role={} local={} remote={} members={} "
      "session={:016X} result={}",
      host_ ? "host" : "peer", local_leaves, removed_members.size() - local_leaves,
      record_.members.size(), record_.session_id, X_ERROR_SUCCESS);
  return X_ERROR_SUCCESS;
}

X_RESULT XSession::Start(const XGI_SESSION_STATE& request) {
  if (!created_) {
    return X_ERROR_FUNCTION_FAILED;
  }
  if (!SyncRecordFromDirectory()) {
    return X_ERROR_NOT_FOUND;
  }
  SessionRecord updated = record_;
  updated.lifecycle_state = SessionLifecycleState::kInGame;
  auto* live = kernel_state_->live_compatibility();
  if (host_ &&
      (!live || !live->session_directory() || !live->session_directory()->Modify(updated))) {
    return X_ERROR_FUNCTION_FAILED;
  }
  record_ = std::move(updated);
  RefreshLocalDetails();
  REXSYS_INFO("Modified session {:016X} to {} public and {} private slots", record_.session_id,
              record_.max_public_slots, record_.max_private_slots);
  return X_ERROR_SUCCESS;
}

X_RESULT XSession::End(const XGI_SESSION_STATE& request) {
  if (!created_) {
    return X_ERROR_FUNCTION_FAILED;
  }
  if (!SyncRecordFromDirectory()) {
    return X_ERROR_NOT_FOUND;
  }
  SessionRecord updated = record_;
  updated.lifecycle_state = SessionLifecycleState::kReporting;
  auto* live = kernel_state_->live_compatibility();
  if (host_ &&
      (!live || !live->session_directory() || !live->session_directory()->Modify(updated))) {
    return X_ERROR_FUNCTION_FAILED;
  }
  record_ = std::move(updated);
  RefreshLocalDetails();
  return X_ERROR_SUCCESS;
}

X_RESULT XSession::Modify(const XGI_SESSION_MODIFY& request) {
  if (!created_) {
    return X_ERROR_FUNCTION_FAILED;
  }
  if (!SyncRecordFromDirectory()) {
    return X_ERROR_NOT_FOUND;
  }
  if (request.public_slots > kMaximumSessionMembers ||
      request.private_slots > kMaximumSessionMembers ||
      request.private_slots > kMaximumSessionMembers - request.public_slots) {
    return X_ERROR_INVALID_PARAMETER;
  }
  const uint32_t occupied_public = record_.max_public_slots - record_.open_public_slots;
  const uint32_t occupied_private = record_.max_private_slots - record_.open_private_slots;
  if (request.public_slots < occupied_public || request.private_slots < occupied_private) {
    return X_ERROR_INVALID_PARAMETER;
  }
  SessionRecord updated = record_;
  updated.contexts = kernel_state_->live_compatibility()->user_contexts();
  updated.properties = kernel_state_->live_compatibility()->user_properties();
  updated.flags = request.flags;
  updated.max_public_slots = request.public_slots;
  updated.max_private_slots = request.private_slots;
  updated.open_public_slots = updated.max_public_slots - occupied_public;
  updated.open_private_slots = updated.max_private_slots - occupied_private;
  auto* directory = kernel_state_->live_compatibility()->session_directory();
  if (host_ && (!directory || !directory->Modify(updated))) {
    return X_ERROR_FUNCTION_FAILED;
  }
  record_ = std::move(updated);
  RefreshLocalDetails();
  return X_ERROR_SUCCESS;
}

X_RESULT XSession::GetDetails(const XGI_SESSION_DETAILS& request) {
  if (!created_) {
    return X_ERROR_INVALID_PARAMETER;
  }
  if (!SyncRecordFromDirectory()) {
    return X_ERROR_NOT_FOUND;
  }
  RefreshLocalDetails();
  return detail::WriteSessionDetailsToGuest(
      kernel_state_->memory(), request.details_buffer_size_ptr, request.details_ptr, local_details_,
      record_.members, kernel_state_->user_profile()->xuid());
}

X_RESULT XSession::Migrate(const XGI_SESSION_MIGRATE& request) {
  if (!created_ || !request.session_info_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }
  auto* live = kernel_state_->live_compatibility();
  auto* directory = live ? live->session_directory() : nullptr;
  if (!live || !directory) {
    return X_ERROR_NOT_LOGGED_ON;
  }
  const bool follower = detail::IsGta4MigrateFollowerUserIndex(request.user_index);
  if (!follower && !SyncRecordFromDirectory()) {
    return X_ERROR_NOT_FOUND;
  }

  const uint64_t old_session_id = record_.session_id;
  const uint32_t old_host_ipv4 = record_.host_ipv4;
  if (!follower) {
    if (request.user_index != 0) {
      return X_ERROR_NO_SUCH_USER;
    }
    SessionRecord replacement = record_;
    replacement.session_id = live->GenerateSessionId();
    replacement.nonce = live->GenerateNonce();
    live->GenerateExchangeKey(replacement.exchange_key);
    replacement.host_xuid = live->identity().xuid;
    replacement.host_machine_id = live->identity().machine_id;
    replacement.host_ipv4 = live->local_ipv4();
    replacement.host_port = live->online_port() ? live->online_port() : kDefaultGamePort;
    replacement.host_ethernet_address = live->identity().ethernet_address;
    auto migrated = directory->Migrate(old_session_id, replacement);
    if (!migrated || migrated->session_id != replacement.session_id ||
        migrated->previous_session_id != old_session_id) {
      return X_ERROR_FUNCTION_FAILED;
    }
    // The directory owns the host's routable address, peer slot, revision,
    // and old-XNKID alias. Use its committed replacement rather than the
    // pre-normalization proposal that was sent to the backend.
    record_ = std::move(*migrated);
    host_ = true;
  } else {
    // The community directory resolves an old session ID to its migrated
    // replacement for existing members. Public search is the wrong primitive:
    // GTA may migrate while in-game, with join-in-progress disabled, and the
    // replacement may use a nonzero search procedure.
    auto replacement = directory->ResolveMigration(old_session_id);
    if (!replacement) {
      return X_ERROR_NOT_FOUND;
    }
    record_ = std::move(*replacement);
    host_ = false;
    directory->PublishLocalSessionPresence(record_.session_id);
  }

  live->UnregisterKey(old_session_id);
  live->UnregisterRoute(old_host_ipv4);
  live->RegisterKey(record_.session_id, record_.exchange_key);
  live->RegisterRoute(record_.host_ipv4, record_);
  live->ClearActiveSession(old_session_id);
  live->SetActiveSession(record_.session_id);
  (void)live->ConfigureVoiceSession(record_.session_id,
                                    VoiceSessionTransition::kMigration);

  auto* info = kernel_state_->memory()->TranslateVirtual<XSESSION_INFO*>(request.session_info_ptr);
  SessionRecordToGuestInfo(record_, *info);
  RefreshLocalDetails();
  return X_ERROR_SUCCESS;
}

X_RESULT XSession::ArbitrationRegister(const XGI_SESSION_ARBITRATION_REGISTER& request) {
  if (!SyncRecordFromDirectory()) {
    REXSYS_WARN("Rejected arbitration registration: session {:016X} is absent from directory",
                record_.session_id);
    return X_ERROR_NOT_FOUND;
  }
  XSESSION_ARBITRATION_CONTEXT context;
  const X_RESULT preparation = PrepareArbitrationRegister(request, context);
  if (preparation != X_ERROR_SUCCESS) return preparation;

  auto* live = kernel_state_->live_compatibility();
  SessionRecord registered = record_;
  registered.lifecycle_state = SessionLifecycleState::kRegistration;
  if (live && live->config().backend == LiveBackend::kCommunity) {
    auto frozen = live->session_directory()->RegisterArbitration(
        record_.session_id, request.nonce,
        request.registration_duration_seconds, request.flags, {});
    return CompleteArbitrationRegister(context, std::move(frozen));
  } else if (host_ &&
             (!live || !live->session_directory() ||
              !live->session_directory()->Modify(registered))) {
    REXSYS_WARN("Rejected arbitration registration for session {:016X}: directory update failed",
                record_.session_id);
    return X_ERROR_FUNCTION_FAILED;
  }

  return CompleteArbitrationRegister(context, std::move(registered));
}

X_RESULT XSession::PrepareArbitrationRegister(
    const XGI_SESSION_ARBITRATION_REGISTER& request,
    XSESSION_ARBITRATION_CONTEXT& context) const {
  if (!created_ || !request.registration_duration_seconds ||
      request.registration_duration_seconds >
          kMaximumSessionArbitrationDurationSeconds) {
    REXSYS_WARN("Rejected arbitration registration: created={}, duration={}s", created_,
                static_cast<uint32_t>(request.registration_duration_seconds));
    return X_ERROR_INVALID_PARAMETER;
  }
  if (!request.results_ptr ||
      request.results_buffer_size < kSessionArbitrationResultsSize) {
    return X_ERROR_INSUFFICIENT_BUFFER;
  }
  if (request.nonce != record_.nonce) {
    REXSYS_WARN("Rejected arbitration registration for session {:016X}: nonce mismatch",
                record_.session_id);
    return X_ERROR_INVALID_PARAMETER;
  }
  if (record_.lifecycle_state != SessionLifecycleState::kLobby &&
      record_.lifecycle_state != SessionLifecycleState::kRegistration) {
    REXSYS_WARN("Rejected arbitration registration for session {:016X}: lifecycle {}",
                record_.session_id, static_cast<uint32_t>(record_.lifecycle_state));
    return X_ERROR_INVALID_PARAMETER;
  }
  auto* live = kernel_state_->live_compatibility();
  if (!live || !live->available() || !live->session_directory()) {
    return X_ERROR_NOT_LOGGED_ON;
  }

  context = {.session_id = record_.session_id,
             .nonce = static_cast<uint64_t>(request.nonce),
             .registration_duration_seconds =
                 static_cast<uint32_t>(request.registration_duration_seconds),
             .flags = static_cast<uint32_t>(request.flags),
             .results_buffer_size =
                 static_cast<uint32_t>(request.results_buffer_size),
             .results_ptr = static_cast<uint32_t>(request.results_ptr)};
  return X_ERROR_SUCCESS;
}

X_RESULT XSession::CompleteArbitrationRegister(
    const XSESSION_ARBITRATION_CONTEXT& context,
    std::optional<SessionRecord> registered) {
  if (!created_ || !registered ||
      !detail::IsSessionArbitrationCompletionCompatible(
                         context, record_, *registered)) {
    REXSYS_WARN(
        "Rejected arbitration completion for session {:016X}: stale, invalid, or "
        "cancelled directory result",
        context.session_id);
    return X_ERROR_FUNCTION_FAILED;
  }

  const X_RESULT write_result = detail::WriteSessionArbitrationResultsToGuest(
      kernel_state_->memory(), context.results_ptr, context.results_buffer_size,
      registered->members);
  if (write_result != X_ERROR_SUCCESS) {
    REXSYS_WARN("Rejected arbitration roster for session {:016X}: marshalling error {}",
                context.session_id, write_result);
    return write_result;
  }
  record_ = std::move(*registered);
  RefreshLocalDetails();
  const auto* results =
      kernel_state_->memory()->TranslateVirtual<const XSESSION_REGISTRATION_RESULTS*>(
          context.results_ptr);
  REXSYS_INFO(
      "Registered arbitration roster for session {:016X}: {} machines, {} users, flags {:08X}, "
      "duration {}s",
      record_.session_id, static_cast<uint32_t>(results->registrant_count), record_.members.size(),
      context.flags, context.registration_duration_seconds);
  return X_ERROR_SUCCESS;
}

X_RESULT XSession::Search(KernelState* kernel_state, XGI_SESSION_SEARCH& request) {
  auto* live = kernel_state->live_compatibility();
  if (!live || !live->available() || !live->session_directory()) {
    return X_ERROR_NOT_LOGGED_ON;
  }
  const uint32_t maximum_results = request.maximum_results;
  if (request.user_index != 0 || maximum_results > kMaximumSearchResults ||
      request.context_count > kMaximumSearchContexts ||
      request.property_count > kMaximumSearchProperties ||
      (request.context_count && !request.contexts_ptr) ||
      (request.property_count && !request.properties_ptr)) {
    return X_ERROR_INVALID_PARAMETER;
  }
  if (request.property_count) {
    const auto* properties =
        kernel_state->memory()->TranslateVirtual<const XUSER_PROPERTY*>(request.properties_ptr);
    for (uint32_t index = 0; index < request.property_count; ++index) {
      const auto type = properties[index].data.type == XUserDataType::kUnset
                            ? PropertyType(properties[index].property_id)
                            : properties[index].data.type;
      if ((type == XUserDataType::kWString || type == XUserDataType::kBinary) &&
          (properties[index].data.value.binary.size > kMaximumPropertySize ||
           (properties[index].data.value.binary.size &&
            !properties[index].data.value.binary.pointer))) {
        return X_ERROR_INVALID_PARAMETER;
      }
    }
  }
  const uint32_t minimum_size = static_cast<uint32_t>(
      sizeof(XSESSION_SEARCHRESULT_HEADER) + maximum_results * kGta4SearchResultBudget);
  if (!request.results_buffer_size) {
    request.results_buffer_size = minimum_size;
    return X_ERROR_INSUFFICIENT_BUFFER;
  }
  if (!request.results_ptr || request.results_buffer_size < sizeof(XSESSION_SEARCHRESULT_HEADER)) {
    request.results_buffer_size = minimum_size;
    return X_ERROR_INSUFFICIENT_BUFFER;
  }

  const auto title = GetTitleIdentity(kernel_state);
  const auto contexts = ReadSearchContexts(kernel_state, request);
  const auto properties = ReadSearchProperties(kernel_state, request);
  auto sessions = live->session_directory()->Search(
      title.title_id, title.media_id, title.title_version, live->config().session_protocol_version,
      request.procedure_index, contexts, properties, maximum_results);
  REXSYS_INFO("Session search procedure {} returned {} result(s)",
              static_cast<uint32_t>(request.procedure_index), sessions.size());
  const auto required_size = detail::SessionSearchResultsRequiredSize(sessions);
  if (!required_size) {
    return X_ERROR_INVALID_PARAMETER;
  }
  if (request.results_buffer_size < *required_size) {
    request.results_buffer_size = *required_size;
    return X_ERROR_INSUFFICIENT_BUFFER;
  }
  return WriteSearchResults(kernel_state, request.results_ptr, request.results_buffer_size,
                            sessions);
}

X_RESULT XSession::SearchById(KernelState* kernel_state, XGI_SESSION_SEARCH_BY_ID& request) {
  auto* live = kernel_state->live_compatibility();
  if (!live || !live->available() || !live->session_directory()) {
    return X_ERROR_NOT_LOGGED_ON;
  }
  if (request.user_index != 0) {
    return X_ERROR_NO_SUCH_USER;
  }
  const uint32_t caller_budget = static_cast<uint32_t>(
      sizeof(XSESSION_SEARCHRESULT_HEADER) + kGta4SearchResultBudget);
  if (!request.results_ptr || !request.results_buffer_size) {
    request.results_buffer_size = caller_budget;
    return X_ERROR_INSUFFICIENT_BUFFER;
  }
  if (request.results_buffer_size < sizeof(XSESSION_SEARCHRESULT_HEADER)) {
    request.results_buffer_size = static_cast<uint32_t>(sizeof(XSESSION_SEARCHRESULT_HEADER));
    return X_ERROR_INSUFFICIENT_BUFFER;
  }
  auto session = live->session_directory()->Get(XnkidToUint64(request.session_id));
  if (!session || session->protocol_version != live->config().session_protocol_version) {
    const std::span<const SessionRecord> empty;
    return WriteSearchResults(kernel_state, request.results_ptr, request.results_buffer_size,
                              empty);
  }
  const std::array<SessionRecord, 1> result = {*session};
  const auto required_size = detail::SessionSearchResultsRequiredSize(result);
  if (!required_size) {
    return X_ERROR_INVALID_PARAMETER;
  }
  if (request.results_buffer_size < *required_size) {
    request.results_buffer_size = *required_size;
    return X_ERROR_INSUFFICIENT_BUFFER;
  }
  return WriteSearchResults(kernel_state, request.results_ptr, request.results_buffer_size, result);
}

bool XSession::SyncRecordFromDirectory() {
  auto* live = kernel_state_->live_compatibility();
  auto* directory = live ? live->session_directory() : nullptr;
  if (!directory) {
    return false;
  }
  auto current = directory->Get(record_.session_id);
  if (!current) {
    return false;
  }
  record_ = std::move(*current);
  PublishValidationMembership(record_);
  return true;
}

void XSession::RefreshLocalDetails() {
  local_details_.user_index_host = host_ ? 0 : kNoUserIndex;
  local_details_.flags = record_.flags;
  local_details_.max_public_slots = record_.max_public_slots;
  local_details_.max_private_slots = record_.max_private_slots;
  local_details_.available_public_slots = record_.open_public_slots;
  local_details_.available_private_slots = record_.open_private_slots;
  local_details_.actual_member_count = static_cast<uint32_t>(record_.members.size());
  // The marshaller derives this from the caller's declared output capacity.
  local_details_.returned_member_count = 0;
  local_details_.state = static_cast<uint32_t>(record_.lifecycle_state);
  local_details_.nonce = record_.nonce;
  SessionRecordToGuestInfo(record_, local_details_.session_info);
}

}  // namespace rex::system::xam
