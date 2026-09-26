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
 *   - switch to __nx_exception_stack (safe to share: the claim is held),
 *     save x9-x29, q0-q31, fpsr and fpcr there;
 *   - rex_nx_exception_dispatch turns frame + saved registers into a rex::arch
 *     Exception and runs the same handler list as the other platforms
 *     (MMIOHandler::ExceptionCallback etc.), writing modified registers back;
 *   - restore the saved registers and svcReturnFromException: 0 resumes at the
 *     (possibly advanced) pc, 0xF801 leaves an unhandled fault to the kernel
 *     with the original context, so the crash report shows the real PC.
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
#include <rex/platform/seh.h>
#include <rex/system/mmio_handler.h>

extern "C" {
#include <switch/arm/thread_context.h>
#include <switch/kernel/svc.h>
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
}

// Registers the entry saves on the exception stack; the offsets are used by
// the assembly below.
struct NxSavedRegisters {
  uint64_t x9_to_x29[21];  // 0x000
  uint64_t pad;            // 0x0A8
  uint8_t v[32][16];       // 0x0B0
  uint64_t fpsr;           // 0x2B0
  uint64_t fpcr;           // 0x2B8
};
static_assert(offsetof(NxSavedRegisters, v) == 0xB0);
static_assert(offsetof(NxSavedRegisters, fpsr) == 0x2B0);
static_assert(sizeof(NxSavedRegisters) == 0x2C0);
static_assert(sizeof(ThreadExceptionFrameA64) == 0x78);

extern "C" bool rex_nx_exception_dispatch(uint32_t type, ThreadExceptionFrameA64* frame,
                                          NxSavedRegisters* saved);

// Strong definition of libnx's weak __libnx_exception_entry (crt0 branches to
// it when the kernel delivers an exception). x0 = type, x1 = frame; x0-x8 are
// in the frame, so x2-x8 are free here.
asm(R"(
    .section .text.__libnx_exception_entry, "ax", %progbits
    .global __libnx_exception_entry
    .type   __libnx_exception_entry, %function
    .balign 16
__libnx_exception_entry:
    adrp    x2, __nx_exception_stack
    add     x2, x2, #:lo12:__nx_exception_stack
    adrp    x3, __nx_exception_stack_size
    ldr     x3, [x3, #:lo12:__nx_exception_stack_size]
    add     x2, x2, x3
    and     x2, x2, #0xfffffffffffffff0
    sub     x2, x2, #0x2c0
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
    str     x29,      [sp, #0xa0]
    stp     q0,  q1,  [sp, #0xb0]
    stp     q2,  q3,  [sp, #0xd0]
    stp     q4,  q5,  [sp, #0xf0]
    stp     q6,  q7,  [sp, #0x110]
    stp     q8,  q9,  [sp, #0x130]
    stp     q10, q11, [sp, #0x150]
    stp     q12, q13, [sp, #0x170]
    stp     q14, q15, [sp, #0x190]
    stp     q16, q17, [sp, #0x1b0]
    stp     q18, q19, [sp, #0x1d0]
    stp     q20, q21, [sp, #0x1f0]
    stp     q22, q23, [sp, #0x210]
    stp     q24, q25, [sp, #0x230]
    stp     q26, q27, [sp, #0x250]
    stp     q28, q29, [sp, #0x270]
    stp     q30, q31, [sp, #0x290]
    mrs     x3, fpsr
    mrs     x4, fpcr
    add     x5, sp, #0x2b0
    stp     x3, x4, [x5]
    mov     x2, sp
    mov     x29, xzr
    bl      rex_nx_exception_dispatch
    and     w6, w0, #0xff        // bool: only the low byte is defined
    add     x5, sp, #0x2b0
    ldp     x3, x4, [x5]
    msr     fpsr, x3
    msr     fpcr, x4
    ldp     q0,  q1,  [sp, #0xb0]
    ldp     q2,  q3,  [sp, #0xd0]
    ldp     q4,  q5,  [sp, #0xf0]
    ldp     q6,  q7,  [sp, #0x110]
    ldp     q8,  q9,  [sp, #0x130]
    ldp     q10, q11, [sp, #0x150]
    ldp     q12, q13, [sp, #0x170]
    ldp     q14, q15, [sp, #0x190]
    ldp     q16, q17, [sp, #0x1b0]
    ldp     q18, q19, [sp, #0x1d0]
    ldp     q20, q21, [sp, #0x1f0]
    ldp     q22, q23, [sp, #0x210]
    ldp     q24, q25, [sp, #0x230]
    ldp     q26, q27, [sp, #0x250]
    ldp     q28, q29, [sp, #0x270]
    ldp     q30, q31, [sp, #0x290]
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
    mov     w0, #0xf801
    cmp     w6, #0
    csel    w0, w0, wzr, eq
    svc     0x28
    b       .
    .size   __libnx_exception_entry, . - __libnx_exception_entry
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
  tc.fpsr = static_cast<uint32_t>(saved->fpsr);
  tc.fpcr = static_cast<uint32_t>(saved->fpcr);
  for (uint32_t i = 0; i < 32; ++i) {
    std::memcpy(&tc.v[i], saved->v[i], sizeof(tc.v[i]));
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
    std::memcpy(saved->v[idx], &tc.v[idx], sizeof(saved->v[idx]));
  }
  frame->sp = tc.sp;
  frame->elr_el1 = tc.pc;  // resume PC (a handler may advance past the instruction)
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

// Called by __libnx_exception_entry with the user-exception claim held.
// true: resume with the (possibly modified) registers; false: the kernel
// treats the exception as unhandled, with the original context.
extern "C" bool rex_nx_exception_dispatch(uint32_t type, ThreadExceptionFrameA64* frame,
                                          NxSavedRegisters* saved) {
  using namespace rex::arch;

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
    return false;
  }

  HostThreadContext thread_context{};
  FillThreadContext(thread_context, frame, saved);

  Exception ex;
  ex.InitializeAccessViolation(
      &thread_context, fault_addr,
      is_write ? Exception::AccessViolationOperation::kWrite
               : Exception::AccessViolationOperation::kRead);

  for (size_t i = 0; i < kMaxHandlerCount && g_handlers[i].first; ++i) {
    if (g_handlers[i].first(&ex, g_handlers[i].second)) {
      StoreThreadContext(thread_context, ex, frame, saved);
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
  return false;
}

#endif  // REX_PLATFORM_NX
