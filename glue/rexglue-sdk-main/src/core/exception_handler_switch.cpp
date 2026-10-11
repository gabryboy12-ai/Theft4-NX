/**
 ******************************************************************************
 * ReXGlue runtime - Nintendo Switch (libnx) exception handler.
 ******************************************************************************
 *
 * Switch does not deliver POSIX signals for CPU faults: sigaction(SIGSEGV) in
 * libnx's newlib port is a stub that returns -1, so the signal-based
 * exception_handler_posix.cpp install path is a silent no-op on NX. That means
 * mmio_handler.cpp's fault handler would never fire on writes to the guest
 * MMIO range.
 *
 * Horizon delivers a user exception by re-entering the process entry point
 * with x0 = exception type and x1 = the ThreadExceptionFrameA64 in the
 * process-local region (x0-x8, lr, sp, pc, pstate, esr, far). x9-x29 and the
 * FP/SIMD registers are still live in the CPU. The thread holds the process's
 * user-exception claim (KProcess::EnterUserException: any other thread that
 * faults waits in the kernel) until svcReturnFromException, which reloads
 * x0-x8, lr, sp, pc and pstate from the frame (result 0), or reports the
 * exception as unhandled with the frame's context (any other result).
 *
 * libnx's own entry (__libnx_exception_entry, weak) cannot resume a fault:
 * it points the frame at __libnx_exception_returnentry, calls
 * svcReturnFromException first, runs __libnx_exception_handler on
 * __nx_exception_stack outside the claim, and then always calls
 * svcBreak(0, 0, 0) (console run 5, "User Break 0x0" after T9's first
 * handled fault). So this file provides the entry itself:
 *
 *   - save q0-q31, fpsr and fpcr in rex_nx_vector_save with the very first
 *     instructions (run 23, D.1: a thread preempted while it holds the claim
 *     may come back with q0-q7 and q16-q31 stale, masseffect-nx
 *     platform-notes; the less code runs before the save, the smaller the
 *     window), then switch to __nx_exception_stack (safe to share: the claim
 *     is held) and save x9-x29 and the frame address there;
 *   - rex_nx_exception_dispatch turns frame + saved registers into a rex::arch
 *     Exception and runs the same handler list as the other platforms
 *     (MMIOHandler::ExceptionCallback etc.), writing modified registers back;
 *   - restore x9-x29, then q0-q31, fpsr and fpcr as the very last
 *     instructions, and svcReturnFromException(0), which resumes at the
 *     (possibly advanced) pc.
 *
 * An unhandled exception must end in Atmosphere's crash report. Returning it
 * to the kernel with svcReturnFromException(0xF801) does not do that:
 * switch-game run 10 closed with no report (svcBreak does: run 5's "User
 * Break"). So the dispatcher resumes the faulting thread, with its own
 * registers and stack, at rex_nx_unhandled_break, which calls
 * svcBreak(BreakReason_Panic) with the fault details as the break argument;
 * LR is set to the faulting PC so the report shows it, and the full register
 * set is logged before (switch-game: game-crash.log).
 *
 * A fault while the entry is already handling one on this thread (inside a
 * handler) must not reuse the exception stack, which holds the outer frame:
 * a depth counter sends it to an emergency stack and straight to svcBreak.
 *
 * Faults are therefore handled one at a time, process-wide. A handler that
 * waits for a lock held by a thread which then faults deadlocks (see
 * docs/switch-port/03-memory.md section 10).
 */

#include <rex/platform.h>

#if REX_PLATFORM_NX

#include <rex/exception_handler.h>

#include <unistd.h>  // write(), STDERR_FILENO — used from the fault-handler path.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <rex/assert.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/platform/nx_fault_record.h>
#include <rex/platform/seh.h>
#include <rex/system/mmio_handler.h>

extern "C" {
#include <switch/arm/counter.h>
#include <switch/arm/thread_context.h>
#include <switch/kernel/svc.h>
#include <switch/kernel/thread.h>
#include <switch/types.h>
}

// The exception entry below runs on __nx_exception_stack (libnx's weak
// default is 0x400 bytes). The dispatcher keeps a HostThreadContext
// (~0x330 bytes) and an Exception on it, and the MMIO / write-watch
// callbacks take locks and call into the memory system, so give it room.
// One stack for the process is enough: the kernel's user-exception claim
// lets one thread at a time run on it.
extern "C" {
alignas(16) u8 __nx_exception_stack[0x10000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
// Nonzero while the entry handles an exception (one thread at a time).
u32 rex_nx_exception_depth = 0;
alignas(16) u8 rex_nx_emergency_stack[0x2000];

// What an unhandled exception passes to svcBreak (and to the report).
struct RexNxFaultInfo {
  u64 pc;
  u64 lr;
  u64 sp;
  u64 far;
  u64 esr;
  u64 type;
};
RexNxFaultInfo rex_nx_fault_info;

// q0-q31, fpsr and fpcr of the thread in the entry, saved by its first
// instructions (before the nested check: a nested fault ends in svcBreak, so
// overwriting the outer save does not matter). One for the process: the claim
// lets one thread at a time in.
struct NxVectorSave {
  uint8_t v[32][16];  // 0x000
  uint64_t fpsr;      // 0x200
  uint64_t fpcr;      // 0x208
};
alignas(16) NxVectorSave rex_nx_vector_save;

bool rex_nx_resume_guard = false;
}

static_assert(offsetof(NxVectorSave, fpsr) == 0x200);
static_assert(sizeof(NxVectorSave) == 0x210);

// Registers the entry saves on the exception stack; the offsets are used by
// the assembly below.
struct NxSavedRegisters {
  uint64_t x9_to_x29[21];  // 0x000
  uint64_t frame;          // 0x0A8: x1 at the entry
};
static_assert(sizeof(NxSavedRegisters) == 0xB0);
static_assert(offsetof(ThreadExceptionFrameA64, sp) == 0x50);
static_assert(offsetof(ThreadExceptionFrameA64, elr_el1) == 0x58);
static_assert(sizeof(ThreadExceptionFrameA64) == 0x78);

extern "C" bool rex_nx_exception_dispatch(uint32_t type, ThreadExceptionFrameA64* frame,
                                          NxSavedRegisters* saved);
extern "C" [[noreturn]] void rex_nx_exception_nested(uint32_t type,
                                                     ThreadExceptionFrameA64* frame);
extern "C" void rex_nx_unhandled_break();

// Strong definition of libnx's weak __libnx_exception_entry (crt0 branches to
// it when the kernel delivers an exception). x0 = type, x1 = frame; x0-x8 are
// in the frame, so x2-x8 are free here.
asm(R"(
    .section .text.__libnx_exception_entry, "ax", %progbits
    .global __libnx_exception_entry
    .type   __libnx_exception_entry, %function
    .balign 16
__libnx_exception_entry:
    // D.1 (run 23): the vector and FP state first, through x2-x4 only (x0-x8
    // are in the frame and come back from it).
    adrp    x2, rex_nx_vector_save
    add     x2, x2, #:lo12:rex_nx_vector_save
    stp     q0,  q1,  [x2, #0x000]
    stp     q2,  q3,  [x2, #0x020]
    stp     q4,  q5,  [x2, #0x040]
    stp     q6,  q7,  [x2, #0x060]
    stp     q8,  q9,  [x2, #0x080]
    stp     q10, q11, [x2, #0x0a0]
    stp     q12, q13, [x2, #0x0c0]
    stp     q14, q15, [x2, #0x0e0]
    stp     q16, q17, [x2, #0x100]
    stp     q18, q19, [x2, #0x120]
    stp     q20, q21, [x2, #0x140]
    stp     q22, q23, [x2, #0x160]
    stp     q24, q25, [x2, #0x180]
    stp     q26, q27, [x2, #0x1a0]
    stp     q28, q29, [x2, #0x1c0]
    stp     q30, q31, [x2, #0x1e0]
    mrs     x3, fpsr
    mrs     x4, fpcr
    str     x3, [x2, #0x200]
    str     x4, [x2, #0x208]
    adrp    x2, rex_nx_exception_depth
    ldr     w3, [x2, #:lo12:rex_nx_exception_depth]
    cbnz    w3, 2f               // a fault inside the handler: see below
    mov     w3, #1
    str     w3, [x2, #:lo12:rex_nx_exception_depth]
    adrp    x2, __nx_exception_stack
    add     x2, x2, #:lo12:__nx_exception_stack
    adrp    x3, __nx_exception_stack_size
    ldr     x3, [x3, #:lo12:__nx_exception_stack_size]
    add     x2, x2, x3
    and     x2, x2, #0xfffffffffffffff0
    sub     x2, x2, #0xb0
    mov     sp, x2
    stp     x9,  x10, [sp, #0x00]
    stp     x11, x12, [sp, #0x10]
    stp     x13, x14, [sp, #0x20]
    stp     x15, x16, [sp, #0x30]
    stp     x17, x18, [sp, #0x40]
    stp     x19, x20, [sp, #0x50]
    stp     x21, x22, [sp, #0x60]
    stp     x23, x24, [sp, #0x70]
    stp     x25, x26, [sp, #0x80]
    stp     x27, x28, [sp, #0x90]
    stp     x29, x1,  [sp, #0xa0]
    mov     x2, sp
    mov     x29, xzr
    bl      rex_nx_exception_dispatch
    and     w6, w0, #0xff        // bool: only the low byte is defined
    ldp     x9,  x10, [sp, #0x00]
    ldp     x11, x12, [sp, #0x10]
    ldp     x13, x14, [sp, #0x20]
    ldp     x15, x16, [sp, #0x30]
    ldp     x17, x18, [sp, #0x40]
    ldp     x19, x20, [sp, #0x50]
    ldp     x21, x22, [sp, #0x60]
    ldp     x23, x24, [sp, #0x70]
    ldp     x25, x26, [sp, #0x80]
    ldp     x27, x28, [sp, #0x90]
    ldr     x29,      [sp, #0xa0]
    adrp    x5, rex_nx_exception_depth
    str     wzr, [x5, #:lo12:rex_nx_exception_depth]
    mov     w0, #0xf801
    cmp     w6, #0
    csel    w0, w0, wzr, eq
    // D.1: the vector and FP state last, right before the return.
    adrp    x2, rex_nx_vector_save
    add     x2, x2, #:lo12:rex_nx_vector_save
    ldr     x3, [x2, #0x200]
    ldr     x4, [x2, #0x208]
    msr     fpsr, x3
    msr     fpcr, x4
    ldp     q0,  q1,  [x2, #0x000]
    ldp     q2,  q3,  [x2, #0x020]
    ldp     q4,  q5,  [x2, #0x040]
    ldp     q6,  q7,  [x2, #0x060]
    ldp     q8,  q9,  [x2, #0x080]
    ldp     q10, q11, [x2, #0x0a0]
    ldp     q12, q13, [x2, #0x0c0]
    ldp     q14, q15, [x2, #0x0e0]
    ldp     q16, q17, [x2, #0x100]
    ldp     q18, q19, [x2, #0x120]
    ldp     q20, q21, [x2, #0x140]
    ldp     q22, q23, [x2, #0x160]
    ldp     q24, q25, [x2, #0x180]
    ldp     q26, q27, [x2, #0x1a0]
    ldp     q28, q29, [x2, #0x1c0]
    ldp     q30, q31, [x2, #0x1e0]
    svc     0x28
    b       .
2:
    // Nested: the exception stack holds the outer frame. Emergency stack,
    // then svcBreak (never returns).
    adrp    x2, rex_nx_emergency_stack
    add     x2, x2, #:lo12:rex_nx_emergency_stack
    add     x2, x2, #0x2000
    mov     sp, x2
    mov     x29, xzr
    bl      rex_nx_exception_nested
    b       .
    .size   __libnx_exception_entry, . - __libnx_exception_entry

    // Where an unhandled exception resumes, on the faulting thread's own
    // registers and stack: svcBreak(BreakReason_Panic, &rex_nx_fault_info,
    // sizeof) so that Atmosphere's creport writes the crash report.
    .section .text.rex_nx_unhandled_break, "ax", %progbits
    .global rex_nx_unhandled_break
    .type   rex_nx_unhandled_break, %function
    .balign 16
rex_nx_unhandled_break:
    mov     x0, #0               // BreakReason_Panic
    adrp    x1, rex_nx_fault_info
    add     x1, x1, #:lo12:rex_nx_fault_info
    mov     x2, #48              // sizeof(RexNxFaultInfo)
    svc     0x26                 // svcBreak
    b       .
    .size   rex_nx_unhandled_break, . - rex_nx_unhandled_break
    .text
)");

namespace rex::arch {

namespace {

constexpr size_t kMaxHandlerCount = 8;
std::pair<ExceptionHandler::Handler, void*> g_handlers[kMaxHandlerCount]{};

inline bool IsDataAbortWrite(uint32_t esr) {
  // EC (bits 31:26): 0b100100 (lower EL data abort) / 0b100101 (same EL).
  const uint32_t ec = (esr >> 26) & 0x3Fu;
  if ((ec & 0x3Eu) != 0x24u) {
    return false;
  }
  // ISS bit 6 = WnR (write-not-read).
  return (esr & (1u << 6)) != 0u;
}

inline bool IsAccessViolation(uint32_t error_desc) {
  // Treat data / instruction aborts and misaligned accesses as AV.
  switch (error_desc) {
    case ThreadExceptionDesc_InstructionAbort:
    case ThreadExceptionDesc_MisalignedPC:
    case ThreadExceptionDesc_MisalignedSP:
    case ThreadExceptionDesc_Other:  // EC <= 0x34, includes data abort
      return true;
    default:
      return false;
  }
}

// The faulting thread's registers: x0-x8, lr, sp, pc and pstate from the
// kernel's frame, the rest as saved by __libnx_exception_entry.
void FillThreadContext(HostThreadContext& tc, const ThreadExceptionFrameA64* frame,
                       const NxSavedRegisters* saved) {
  for (uint32_t i = 0; i < 9; ++i) {
    tc.x[i] = frame->cpu_gprs[i];
  }
  for (uint32_t i = 9; i < 30; ++i) {
    tc.x[i] = saved->x9_to_x29[i - 9];
  }
  tc.x[30] = frame->lr;
  tc.sp = frame->sp;
  tc.pc = frame->elr_el1;
  tc.pstate = frame->pstate;
  tc.fpsr = static_cast<uint32_t>(rex_nx_vector_save.fpsr);
  tc.fpcr = static_cast<uint32_t>(rex_nx_vector_save.fpcr);
  for (uint32_t i = 0; i < 32; ++i) {
    std::memcpy(&tc.v[i], rex_nx_vector_save.v[i], sizeof(tc.v[i]));
  }
}

// Writes the registers a handler changed back where the entry reloads them.
void StoreThreadContext(const HostThreadContext& tc, const Exception& ex,
                        ThreadExceptionFrameA64* frame, NxSavedRegisters* saved) {
  uint32_t modified = ex.modified_x_registers();
  uint32_t idx;
  while (rex::bit_scan_forward(modified, &idx)) {
    modified &= ~(UINT32_C(1) << idx);
    if (idx < 9) {
      frame->cpu_gprs[idx] = tc.x[idx];
    } else if (idx < 30) {
      saved->x9_to_x29[idx - 9] = tc.x[idx];
    } else if (idx == 30) {
      frame->lr = tc.x[30];
    }
  }
  uint32_t modified_v = ex.modified_v_registers();
  while (rex::bit_scan_forward(modified_v, &idx)) {
    modified_v &= ~(UINT32_C(1) << idx);
    std::memcpy(rex_nx_vector_save.v[idx], &tc.v[idx], sizeof(rex_nx_vector_save.v[idx]));
  }
  // Resume sp and pc (a handler may advance pc past the instruction). Two
  // 64-bit stores through general registers: run 22's build copied the pair
  // with one q31 load and store (rex_nx_exception_dispatch+0x1030), and every
  // early crash resumed with exactly this pair wrong.
  const uint64_t sp = *reinterpret_cast<const volatile uint64_t*>(&tc.sp);
  const uint64_t pc = *reinterpret_cast<const volatile uint64_t*>(&tc.pc);
  *reinterpret_cast<volatile uint64_t*>(&frame->sp) = sp;
  *reinterpret_cast<volatile uint64_t*>(&frame->elr_el1) = pc;
}

// --------------------------------------------------------------------------
// Async-signal-safe / fault-handler-safe helpers.
//
// rex_nx_exception_dispatch runs in the same restricted context as a POSIX
// signal handler: spdlog / fmt::format / std::string / malloc are all
// unsafe (spdlog may be mid-write on the faulting thread's mutex). Route
// diagnostic output through write(2) with stack buffers only before an
// unhandled fault goes back to the kernel.
// --------------------------------------------------------------------------

inline void SigSafeWriteCStr(const char* s) {
  if (!s) return;
  size_t n = 0;
  while (s[n]) ++n;
  (void)::write(STDERR_FILENO, s, n);
}

inline size_t SigSafeFormatHex(char* buf, size_t cap, uint64_t value) {
  static const char kHex[] = "0123456789abcdef";
  if (cap < 4) return 0;
  size_t w = 0;
  buf[w++] = '0';
  buf[w++] = 'x';
  int shift = 60;
  while (shift > 0 && ((value >> shift) & 0xF) == 0) shift -= 4;
  while (shift >= 0 && w < cap) {
    buf[w++] = kHex[(value >> shift) & 0xF];
    shift -= 4;
  }
  return w;
}

inline void SigSafeWriteLabelHex(const char* label, uint64_t value) {
  char buf[96];
  size_t w = 0;
  for (const char* p = label; *p && w < sizeof(buf) - 1; ++p) buf[w++] = *p;
  if (w < sizeof(buf) - 1) buf[w++] = '=';
  w += SigSafeFormatHex(buf + w, sizeof(buf) - w - 1, value);
  if (w < sizeof(buf)) buf[w++] = '\n';
  (void)::write(STDERR_FILENO, buf, w);
}

// A fault that no handler claimed while SehGuard is active on this thread.
// seh_switch.cpp cannot turn it into an SehException yet, so say so
// explicitly before the panic instead of reporting a plain crash.
void ReportUnsupportedSehFault(uint64_t fault_addr, uint64_t fault_pc) {
  static const char kMessage[] = "[rex] fault inside SEH_TRY, not yet supported on Switch\n";
  svcOutputDebugString(kMessage, sizeof(kMessage) - 1);
  SigSafeWriteCStr(kMessage);
  SigSafeWriteLabelHex("address", fault_addr);
  SigSafeWriteLabelHex("pc", fault_pc);
}

}  // namespace

void ExceptionHandler::Install(Handler fn, void* data) {
  for (size_t i = 0; i < kMaxHandlerCount; ++i) {
    if (!g_handlers[i].first) {
      g_handlers[i].first = fn;
      g_handlers[i].second = data;
      return;
    }
  }
  assert_always("Too many exception handlers installed");
}

void ExceptionHandler::Uninstall(Handler fn, void* data) {
  for (size_t i = 0; i < kMaxHandlerCount; ++i) {
    if (g_handlers[i].first == fn && g_handlers[i].second == data) {
      for (; i < kMaxHandlerCount - 1; ++i) {
        g_handlers[i] = g_handlers[i + 1];
      }
      g_handlers[kMaxHandlerCount - 1] = {nullptr, nullptr};
      return;
    }
  }
}

}  // namespace rex::arch

// ── Resume records (run 23, D.2) ─────────────────────────────────────────────
// Every access goes through volatile 64-bit loads and stores, so that none of
// these values sits in a vector register in the fault path.
static thread_local RexNxThreadFaults t_nx_faults;
static RexNxResumeStats g_nx_resume_stats;

static inline uint64_t Load64(const uint64_t& v) {
  return *static_cast<const volatile uint64_t*>(&v);
}
static inline void Store64(uint64_t& v, uint64_t value) {
  *static_cast<volatile uint64_t*>(&v) = value;
}

// kind 1: the context's sp/pc changed while the handlers ran; kind 2: the
// frame read back differs from what was written.
static void NoteResumeEvent(uint64_t kind, RexNxThreadFaults& rec, uint64_t seen_sp,
                            uint64_t seen_pc) {
  RexNxResumeStats& s = g_nx_resume_stats;
  if (kind == 1) {
    Store64(rec.context_changed, Load64(rec.context_changed) + 1);
    Store64(s.context_changed, Load64(s.context_changed) + 1);
  } else {
    Store64(rec.frame_wrong, Load64(rec.frame_wrong) + 1);
    Store64(s.frame_wrong, Load64(s.frame_wrong) + 1);
  }
  if (Load64(s.first_kind) == 0) {
    u64 id = 0;
    svcGetThreadId(&id, CUR_THREAD_HANDLE);
    Store64(s.first_thread_id, id);
    Store64(s.first_entry_sp, Load64(rec.entry_sp));
    Store64(s.first_entry_pc, Load64(rec.entry_pc));
    Store64(s.first_seen_sp, seen_sp);
    Store64(s.first_seen_pc, seen_pc);
    Store64(s.first_kind, kind);
  }
}

extern "C" const RexNxThreadFaults* rex_nx_thread_faults() { return &t_nx_faults; }
extern "C" const RexNxResumeStats* rex_nx_resume_stats() { return &g_nx_resume_stats; }

extern "C" uint64_t rex_nx_take_fault_ticks_max() {
  const uint64_t max = Load64(g_nx_resume_stats.handled_ticks_max);
  Store64(g_nx_resume_stats.handled_ticks_max, 0);
  return max;
}

// ── Fault ring (run 24, B) ───────────────────────────────────────────────────
// Memory only: the game writes it out on an unhandled fault, abort or exit.
static RexNxFaultRing g_nx_fault_ring;

extern "C" const RexNxFaultRing* rex_nx_fault_ring() { return &g_nx_fault_ring; }

// A new slot with the fault as it came in: x0-x8, lr, sp and pc from the
// kernel's frame, x9-x29 as the entry saved them.
static RexNxFaultEntry& RecordFaultEntry(uint32_t type, const ThreadExceptionFrameA64* frame,
                                         const NxSavedRegisters* saved, uint64_t start_tick) {
  RexNxFaultRing& ring = g_nx_fault_ring;
  const uint64_t seq = ring.count + 1;
  RexNxFaultEntry& e = ring.entries[(seq - 1) % REX_NX_FAULT_RING_SIZE];
  e.seq = seq;
  e.entry_tick = start_tick;
  e.ticks = 0;
  e.thread_handle = threadGetCurHandle();
  e.handled = 0;
  e.type = type;
  e.pc = frame->elr_el1;
  e.lr = frame->lr;
  e.sp = frame->sp;
  e.far = frame->far;
  e.esr = frame->esr;
  e.resume_pc = 0;
  e.resume_sp = 0;
  for (uint32_t i = 0; i < 9; ++i) e.x[i] = frame->cpu_gprs[i];
  for (uint32_t i = 9; i < 30; ++i) e.x[i] = saved->x9_to_x29[i - 9];
  e.x[30] = frame->lr;
  ring.count = seq;
  return e;
}

// The way out: what svcReturnFromException will load, the time spent, and
// the process counters.
static void FinishFaultEntry(RexNxFaultEntry& e, const ThreadExceptionFrameA64* frame,
                             bool handled) {
  const uint64_t ticks = armGetSystemTick() - e.entry_tick;
  e.resume_pc = frame->elr_el1;
  e.resume_sp = frame->sp;
  e.handled = handled ? 1 : 0;
  e.ticks = ticks;
  RexNxResumeStats& s = g_nx_resume_stats;
  if (handled) {
    Store64(s.handled_ticks, Load64(s.handled_ticks) + ticks);
    if (ticks > Load64(s.handled_ticks_max)) {
      Store64(s.handled_ticks_max, ticks);
    }
  } else {
    Store64(s.unhandled, Load64(s.unhandled) + 1);
  }
}

// Unhandled: record the fault, then resume the thread with its own registers
// at rex_nx_unhandled_break (svcBreak), LR = faulting PC for the report.
static void ResumeAtUnhandledBreak(uint32_t type, ThreadExceptionFrameA64* frame) {
  rex_nx_fault_info = {frame->elr_el1, frame->lr, frame->sp, frame->far, frame->esr, type};
  frame->lr = frame->elr_el1;
  frame->elr_el1 = reinterpret_cast<u64>(&rex_nx_unhandled_break);
}

// Called by __libnx_exception_entry with the user-exception claim held.
// Returns true: a handled exception resumes with the (possibly modified)
// registers, an unhandled one at rex_nx_unhandled_break (svcBreak, crash
// report). false would hand it to the kernel with 0xF801, which ends the
// process without a report (run 10); no path returns it any more.
extern "C" bool rex_nx_exception_dispatch(uint32_t type, ThreadExceptionFrameA64* frame,
                                          NxSavedRegisters* saved) {
  using namespace rex::arch;

  // Run 24 (B): the fault in the ring before anything else runs.
  RexNxFaultEntry& entry = RecordFaultEntry(type, frame, saved, armGetSystemTick());

  const uint64_t fault_pc   = frame->elr_el1;
  const uint64_t fault_addr = frame->far;
  const uint32_t esr        = frame->esr;
  const bool is_write       = IsDataAbortWrite(esr);

  if (!IsAccessViolation(type)) {
    // Signal-safe only: spdlog / fmt::format are not async-safe.
    if (rex::platform::seh_active()) {
      ReportUnsupportedSehFault(fault_addr, fault_pc);
    }
    SigSafeWriteCStr("[rex] Switch exception (non-AV)\n");
    SigSafeWriteLabelHex("type", static_cast<uint64_t>(type));
    SigSafeWriteLabelHex("pc", fault_pc);
    SigSafeWriteLabelHex("far", fault_addr);
    SigSafeWriteLabelHex("esr", static_cast<uint64_t>(esr));
    ResumeAtUnhandledBreak(type, frame);
    FinishFaultEntry(entry, frame, false);
    return true;
  }

  // The frame as it came in (D.2: the crash log shows the last one).
  RexNxThreadFaults& rec = t_nx_faults;
  Store64(rec.entry_pc, Load64(frame->elr_el1));
  Store64(rec.entry_sp, Load64(frame->sp));
  Store64(rec.entry_lr, Load64(frame->lr));
  Store64(rec.frame_address, reinterpret_cast<uint64_t>(frame));

  HostThreadContext thread_context{};
  FillThreadContext(thread_context, frame, saved);

  Exception ex;
  ex.InitializeAccessViolation(
      &thread_context, fault_addr,
      is_write ? Exception::AccessViolationOperation::kWrite
               : Exception::AccessViolationOperation::kRead);

  for (size_t i = 0; i < kMaxHandlerCount && g_handlers[i].first; ++i) {
    if (g_handlers[i].first(&ex, g_handlers[i].second)) {
      // No handler moves sp; the MMIO handler may step pc past the
      // instruction it emulated (+4). Anything else changed in the copy.
      const uint64_t entry_sp = Load64(rec.entry_sp);
      const uint64_t entry_pc = Load64(rec.entry_pc);
      const uint64_t tc_sp = Load64(thread_context.sp);
      const uint64_t tc_pc = Load64(thread_context.pc);
      const bool tc_pc_ok = tc_pc == entry_pc || tc_pc == entry_pc + 4;
      if (tc_sp != entry_sp || !tc_pc_ok) {
        NoteResumeEvent(1, rec, tc_sp, tc_pc);
      }
      StoreThreadContext(thread_context, ex, frame, saved);
      // What svcReturnFromException will load.
      uint64_t out_sp = Load64(frame->sp);
      uint64_t out_pc = Load64(frame->elr_el1);
      if (out_sp != tc_sp || out_pc != tc_pc) {
        NoteResumeEvent(2, rec, out_sp, out_pc);
      }
      if (rex_nx_resume_guard &&
          (out_sp != entry_sp || (out_pc != entry_pc && out_pc != entry_pc + 4))) {
        out_sp = entry_sp;
        out_pc = tc_pc_ok ? tc_pc : entry_pc;
        Store64(frame->sp, out_sp);
        Store64(frame->elr_el1, out_pc);
        Store64(g_nx_resume_stats.repaired, Load64(g_nx_resume_stats.repaired) + 1);
      }
      Store64(rec.resume_sp, out_sp);
      Store64(rec.resume_pc, out_pc);
      Store64(rec.handled, Load64(rec.handled) + 1);
      Store64(rec.last_tick, armGetSystemTick());
      Store64(g_nx_resume_stats.handled, Load64(g_nx_resume_stats.handled) + 1);
      FinishFaultEntry(entry, frame, true);
      return true;
    }
  }

  // Signal-safe only: spdlog / fmt::format are not async-safe.
  if (rex::platform::seh_active()) {
    ReportUnsupportedSehFault(fault_addr, fault_pc);
  }
  SigSafeWriteCStr("[rex] Unhandled Switch access violation\n");
  SigSafeWriteLabelHex("pc", fault_pc);
  SigSafeWriteLabelHex("far", fault_addr);
  SigSafeWriteLabelHex("esr", static_cast<uint64_t>(esr));
  SigSafeWriteLabelHex("is_write", is_write ? 1ULL : 0ULL);
  ResumeAtUnhandledBreak(type, frame);
  FinishFaultEntry(entry, frame, false);
  return true;
}

// A fault while this thread was already inside the entry (a handler or the
// logging faulted). Minimal report on the emergency stack, then svcBreak with
// the inner fault's details; the outer fault was logged when it came in.
extern "C" [[noreturn]] void rex_nx_exception_nested(uint32_t type,
                                                     ThreadExceptionFrameA64* frame) {
  using namespace rex::arch;
  static bool reported = false;
  rex_nx_fault_info = {frame->elr_el1, frame->lr, frame->sp, frame->far, frame->esr, type};
  if (!reported) {
    reported = true;
    SigSafeWriteCStr("[rex] Fault inside the Switch exception handler\n");
    SigSafeWriteLabelHex("pc", frame->elr_el1);
    SigSafeWriteLabelHex("far", frame->far);
    SigSafeWriteLabelHex("esr", static_cast<uint64_t>(frame->esr));
  }
  svcBreak(BreakReason_Panic, reinterpret_cast<uintptr_t>(&rex_nx_fault_info),
           sizeof(rex_nx_fault_info));
  for (;;) {
  }
}

#endif  // REX_PLATFORM_NX
