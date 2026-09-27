#pragma once

#include <cstdint>

namespace rex::input {

bool IsInputTraceEnabled();
uint64_t NextInputTraceSequence();
const char* InputTraceVirtualKeyName(uint32_t virtual_key);

// A controller poll may be the direct consequence of a keyboard event. Keep
// that relationship thread-local so driver, merged-state and guest-API records
// can reuse the SDL event sequence without coupling the public XInput ABI to
// diagnostic metadata.
void BeginInputTracePoll();
void LinkInputTraceSequence(uint64_t sequence);
uint64_t InputTraceCausalSequence();

}  // namespace rex::input
