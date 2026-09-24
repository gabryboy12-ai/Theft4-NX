/**
 * @file        core/seh_switch.cpp
 * @brief       Nintendo Switch (libnx) SEH runtime state.
 *
 * Replaces seh_posix.cpp on NX. libnx's newlib has no working sigaction for
 * CPU faults, and there is no safe way to throw a C++ exception out of
 * __libnx_exception_handler, so a fault inside SEH_TRY cannot be turned into
 * an SehException yet.
 *
 * This file only keeps the per-thread state that SehGuard toggles. Faults are
 * still handled by exception_handler_switch.cpp: when no installed handler
 * (MMIO, write watches) claims a fault and seh_active() is set on the faulting
 * thread, it reports "fault inside SEH_TRY, not yet supported on Switch" with
 * the fault address and PC, then breaks with BreakReason_Panic.
 */

#include <rex/platform.h>
#include <rex/platform/seh.h>

static_assert(REX_PLATFORM_NX, "This file is Switch-only");

#include <rex/platform/exceptions.h>

extern "C" {
#include <switch/kernel/svc.h>
}

namespace rex::platform {

static thread_local SehThreadState tls_seh_state;
static thread_local bool tls_seh_active = false;

SehThreadState& seh_thread_state() {
  return tls_seh_state;
}

int seh_filter(uint32_t /*code*/, void* /*ep*/) {
  // Not used outside Windows: SEH_CATCH is a C++ catch clause.
  return 0;
}

[[noreturn]] void seh_rethrow() {
  // Nothing on NX ever captures a fault into tls_seh_state, so there is no
  // native exception to re-raise.
  static const char kMessage[] = "[rex] seh_rethrow: not supported on Switch\n";
  svcOutputDebugString(kMessage, sizeof(kMessage) - 1);
  svcBreak(BreakReason_Panic, 0, 0);
  for (;;) {
  }
}

void seh_initialize() {
  // No handler to install: __libnx_exception_handler in
  // exception_handler_switch.cpp is the only fault entry point on NX.
  g_seh_initialized.store(true, std::memory_order_relaxed);
}

bool& seh_active() {
  return tls_seh_active;
}

}  // namespace rex::platform
