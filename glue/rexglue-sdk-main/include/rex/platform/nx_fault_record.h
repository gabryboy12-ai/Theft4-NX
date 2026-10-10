/**
 ******************************************************************************
 * ReXGlue runtime - Nintendo Switch: what the exception entry knows about the
 * faults it resumed (exception_handler_switch.cpp).
 ******************************************************************************
 *
 * Run 22: every early crash is a thread resumed after a handled write-watch
 * fault with x0-x30 intact and sp/pc garbage, the two values the dispatcher
 * writes back into the kernel's exception frame. These records say where the
 * pair went wrong: in the handler's copy (the context changed while the
 * handlers ran), in the write-back (the frame read back differs from what was
 * written), or after it (the next fault on the thread shows a pc/sp that the
 * last resume did not write).
 *
 * All fields are written with plain 64-bit stores from the fault path and read
 * by the crash log of the same thread or by a heartbeat (approximate there).
 */

#pragma once

#include <cstdint>

#include <rex/platform.h>

#if REX_PLATFORM_NX

extern "C" {

// One per thread (thread_local in exception_handler_switch.cpp).
struct RexNxThreadFaults {
  uint64_t handled;        // faults resumed on this thread
  uint64_t last_tick;      // armGetSystemTick() of the last one
  uint64_t entry_pc;       // the kernel's frame when the last one came in
  uint64_t entry_sp;
  uint64_t entry_lr;
  uint64_t resume_pc;      // the frame read back just before svcReturnFromException
  uint64_t resume_sp;
  uint64_t frame_address;  // where the kernel put the frame (x1 at the entry)
  uint64_t context_changed;  // tc.sp / tc.pc changed while the handlers ran
  uint64_t frame_wrong;      // the frame read back != what was written
};

// Process-wide (one fault at a time: the kernel's user-exception claim).
struct RexNxResumeStats {
  uint64_t handled;
  uint64_t context_changed;
  uint64_t frame_wrong;
  uint64_t repaired;  // only with rex_nx_resume_guard
  // The first event of either kind.
  uint64_t first_kind;  // 0 none, 1 context changed, 2 frame wrong
  uint64_t first_thread_id;
  uint64_t first_entry_sp;
  uint64_t first_entry_pc;
  uint64_t first_seen_sp;
  uint64_t first_seen_pc;
};

// The calling thread's record (never null).
const RexNxThreadFaults* rex_nx_thread_faults();
const RexNxResumeStats* rex_nx_resume_stats();

// false by default: a wrong sp/pc is counted and recorded but resumed as it
// is. true: the frame gets the entry sp and pc back before the resume.
extern bool rex_nx_resume_guard;
}

#endif  // REX_PLATFORM_NX
