/**
 ******************************************************************************
 * @file        xsession_internal.h
 * @brief       Testable helpers for safe XSession guest-buffer marshalling.
 ******************************************************************************
 */

#pragma once

#include <cstdint>
#include <span>

#include <rex/system/xam/live_compatibility.h>
#include <rex/system/xam/xsession.h>

namespace rex::memory {
class Memory;
}

namespace rex::system::xam::detail {

// Implements the pointer-to-size XSessionGetDetails ABI. A header-sized buffer
// is valid and receives as many complete member records as fit. The size value
// is always updated to the size required for the complete roster.
X_RESULT WriteSessionDetailsToGuest(memory::Memory* memory, uint32_t details_size_ptr,
                                    uint32_t details_ptr,
                                    const XSESSION_LOCAL_DETAILS& details_template,
                                    std::span<const SessionMember> members, uint64_t local_xuid);

// Writes the fixed-capacity XSessionArbitrationRegister result buffer. Members
// are grouped by their authenticated machine ID, with at most four XUIDs on a
// machine and at most 64 machines.
X_RESULT WriteSessionArbitrationResultsToBuffer(std::span<uint8_t> output,
                                                uint32_t output_guest_address,
                                                std::span<const SessionMember> members);
X_RESULT WriteSessionArbitrationResultsToGuest(memory::Memory* memory, uint32_t results_ptr,
                                               uint32_t results_buffer_size,
                                               std::span<const SessionMember> members);
bool IsSessionArbitrationCompletionCompatible(const XSESSION_ARBITRATION_CONTEXT& context,
                                              const SessionRecord& current,
                                              const SessionRecord& registered);

}  // namespace rex::system::xam::detail
