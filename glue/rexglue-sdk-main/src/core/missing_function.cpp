/**
 * @file core/missing_function.cpp
 * @brief Default (weak) stop for missing Xbox exports and runtime functions.
 */

#include <rex/diagnostics/missing_function.h>

#include <cstdio>
#include <cstdlib>

#include <rex/logging.h>

namespace rex::diagnostics {

__attribute__((weak)) void StopAtMissingFunction(const char* kind, const char* name,
                                                 uint32_t guest_lr) {
  REXLOG_ERROR("HEADLESS STARTUP BLOCKED: {} ({}, guest LR={:08X})", name, kind, guest_lr);
  std::fprintf(stderr, "HEADLESS STARTUP BLOCKED: %s (%s, guest LR=%08x)\n", name, kind,
               guest_lr);
  std::fflush(stderr);
  rex::FlushLogging();
  std::abort();
}

}  // namespace rex::diagnostics
