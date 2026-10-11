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
 *
 * Run 24 (B): the fault ring and the time per fault below. Nothing on the
 * path of a handled fault writes a file: the records stay in memory until an
 * unhandled fault, an abort or an exit writes them out.
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
  // Run 24 (B): time in the dispatcher (its first line to its return, every
  // handler included; the assembly entry and the kernel are outside it), in
  // armGetSystemTick() ticks, and the faults no handler claimed.
  uint64_t handled_ticks;      // sum over the handled faults
  uint64_t handled_ticks_max;  // the longest since rex_nx_take_fault_ticks_max()
  uint64_t unhandled;
};

// The calling thread's record (never null).
const RexNxThreadFaults* rex_nx_thread_faults();
const RexNxResumeStats* rex_nx_resume_stats();
// The longest handled fault since the last call, in ticks; starts a new
// window (heartbeat).
uint64_t rex_nx_take_fault_ticks_max();

// Run 24 (B): the last faults the entry saw, handled or not, in memory only.
// The dispatcher fills one slot per fault with plain stores (no I/O, no
// allocation, no lock: the claim lets one thread at a time in); the game
// writes the ring to disk only on an unhandled fault, an abort or an exit.
#define REX_NX_FAULT_RING_SIZE 32
struct RexNxFaultEntry {
  uint64_t seq;            // 1, 2, ... (0: slot never used)
  uint64_t entry_tick;     // armGetSystemTick() at the dispatcher's start
  uint64_t ticks;          // dispatcher start to return
  uint64_t thread_handle;  // libnx handle of the faulting thread
  uint64_t handled;        // 1 resumed where it was, 0 sent to svcBreak
  uint64_t type;           // ThreadExceptionDesc
  uint64_t pc, lr, sp, far, esr;
  uint64_t resume_pc, resume_sp;  // what svcReturnFromException loads
  uint64_t x[31];                 // x0-x30 as they came in
};
struct RexNxFaultRing {
  uint64_t count;  // faults recorded = the last one's seq; slot of seq s: (s - 1) % size
  RexNxFaultEntry entries[REX_NX_FAULT_RING_SIZE];
};
const RexNxFaultRing* rex_nx_fault_ring();

// Run 24 (B): Memory::NxPhysicalAccessViolation (xmemory.cpp), which runs in
// the handler, counts what it used to log.
struct RexNxPhysicalFaultStats {
  uint64_t stale_recovered;  // write fault on a page the heap says writable
  uint64_t last_stale_physical;
  uint64_t unhandled;  // physical write fault no watch and no heap explains
  uint64_t last_unhandled_physical;
  uint64_t last_unhandled_protect;
};
const RexNxPhysicalFaultStats* rex_nx_physical_fault_stats();

// false by default: a wrong sp/pc is counted and recorded but resumed as it
// is. true: the frame gets the entry sp and pc back before the resume.
extern bool rex_nx_resume_guard;
}

#endif  // REX_PLATFORM_NX
