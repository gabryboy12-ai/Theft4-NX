/**
 * @file diagnostics/missing_function.h
 * @brief One stop point for Xbox exports and runtime functions this build lacks.
 *
 * Reached when the game calls an Xbox kernel export (or the runtime reaches a
 * function) that this platform or build does not implement: exports of the
 * headless kernel that need media or UI, exports that need a graphics backend
 * when none is loaded, runtime paths that do not exist on the platform, and
 * (tools/switch-game) imports the SDK does not define at all.
 *
 * The default implementation logs, flushes the log and aborts. It is weak: an
 * executable can define its own (switch-game records the stop durably and
 * parks the thread instead of crashing). It must not return.
 */

#pragma once

#include <cstdint>

namespace rex::diagnostics {

// `kind` says why the function is missing, `name` is the export or function
// name, `guest_lr` the guest return address (0 when not called from guest code).
[[noreturn]] void StopAtMissingFunction(const char* kind, const char* name, uint32_t guest_lr);

}  // namespace rex::diagnostics
