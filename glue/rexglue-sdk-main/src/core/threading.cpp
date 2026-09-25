/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <thread>

#include <rex/thread.h>

#if REX_PLATFORM_NX
extern "C" {
#include <switch/kernel/svc.h>
#include <switch/result.h>
}
#endif

namespace rex::thread {

// =============================================================================
// Common code
// =============================================================================

uint32_t logical_processor_count() {
  static uint32_t value = 0;
  if (!value) {
#if REX_PLATFORM_NX
    // devkitA64's libstdc++ hardware_concurrency() is `return 0`. Count the
    // cores this process may run on (3 for applications, 4 if core 3 is
    // granted).
    u64 core_mask = 0;
    if (R_SUCCEEDED(svcGetInfo(&core_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0))) {
      value = uint32_t(__builtin_popcountll(core_mask));
    }
#else
    value = std::thread::hardware_concurrency();
#endif
  }
  return value;
}

thread_local uint32_t current_thread_id_ = UINT_MAX;

uint32_t current_thread_id() {
  return current_thread_id_ == UINT_MAX ? current_thread_system_id() : current_thread_id_;
}

void set_current_thread_id(uint32_t id) {
  current_thread_id_ = id;
}

}  // namespace rex::thread
