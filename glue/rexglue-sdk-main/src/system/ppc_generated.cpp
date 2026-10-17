/**
 * @file        system/ppc_generated.cpp
 * @brief       Runtime services the generated macros call out of line
 *
 * Declared in rex/ppc/generated.h. They used to be inline in the generated
 * init header, which made every recompiled file include the logger, the
 * clock, the thread primitives and the MMIO handler (and, through rex/ppc.h,
 * most of the runtime). None of them is on a hot path: traps, mftb, the
 * interrupt lock and recognised MMIO are rare next to plain loads and stores.
 */

#include <rex/ppc/generated.h>

#include <atomic>
#include <cassert>
#include <string_view>

#include <rex/chrono/clock.h>
#include <rex/logging.h>
#include <rex/system/mmio_handler.h>
#include <rex/thread/mutex.h>
#include <rex/thread/nx_sync_tuning.h>

namespace rex::ppc {

namespace {

std::atomic<int32_t> g_global_lock_count{0};

// C3: per-thread state of the guest interrupt lock (mfmsr / mtmsrd EE). Each
// nesting level records whether it took the global mutex, so the mode can flip
// while a thread is inside a region and Leave still matches its Enter.
constexpr uint32_t kMaxTrackedDepth = 64;
thread_local uint32_t t_irq_depth = 0;
thread_local uint64_t t_irq_locked_levels = 0;

const char* GuestString(uint8_t* base, uint32_t guest_address) {
#if REX_PLATFORM_NX
  (void)base;
  return reinterpret_cast<const char*>(rex::memory::GuestToHost(guest_address));
#elif REX_PLATFORM_WIN32 || REX_PLATFORM_DARWIN
  return reinterpret_cast<const char*>(base + guest_address +
                                       (guest_address >= 0xE0000000u ? 0x1000u : 0u));
#else
  return reinterpret_cast<const char*>(base + guest_address);
#endif
}

}  // namespace

void Trap(PPCContext& ctx, uint8_t* base, uint16_t trap_type) {
  switch (trap_type) {
    case 20:
    case 26: {
      const std::string_view str(GuestString(base, ctx.r3.u32), ctx.r4.u16);
      REXCPU_DEBUG("(service trap) {}", str);
      break;
    }
    case 0:
    case 22:
      REXCPU_WARN("tw/td trap hit (type {})", trap_type);
      break;
    case 25:
      break;
    default:
      REXCPU_WARN("Unknown trap type {}", trap_type);
      break;
  }
}

uint64_t QueryTimebase() {
  return rex::chrono::Clock::QueryGuestTickCount();
}

// GTA IV wraps each lwarx/stwcx. pair in "mtmsrd r13,1 ... mtmsrd rN,1"
// (interrupts off). On the 360 that only stops this hardware thread being
// interrupted; it never excluded the other five, so the compare-and-swap is
// what makes the update atomic. The original emulation took the process-wide
// recursive mutex for it (twice, counting mfmsr), which serialised every job
// queue operation of every thread: ~45% of the job threads' samples in C2.
// With GuestInterruptLockFree the region is tracked per thread only, as in
// XenonRecomp, and MSR[EE] reads back this thread's state.
uint64_t CheckGlobalLock() {
  if (rex::thread::nx_sync::GuestInterruptLockFree().load(std::memory_order_relaxed)) {
    return t_irq_depth ? 0 : 0x8000;
  }
  auto lock = rex::thread::global_critical_region::AcquireDirect();
  return g_global_lock_count.load() ? 0 : 0x8000;
}

void EnterGlobalLock() {
  const uint32_t level = t_irq_depth++;
  if (rex::thread::nx_sync::GuestInterruptLockFree().load(std::memory_order_relaxed)) {
    if (level < kMaxTrackedDepth) t_irq_locked_levels &= ~(uint64_t(1) << level);
    return;
  }
  rex::thread::global_critical_region::mutex().lock();
  g_global_lock_count.fetch_add(1);
  if (level < kMaxTrackedDepth) t_irq_locked_levels |= uint64_t(1) << level;
}

void LeaveGlobalLock() {
  assert(t_irq_depth >= 1 && "LeaveGlobalLock called without matching EnterGlobalLock");
  const uint32_t level = --t_irq_depth;
  const bool locked = level < kMaxTrackedDepth
                          ? ((t_irq_locked_levels >> level) & 1) != 0
                          : !rex::thread::nx_sync::GuestInterruptLockFree().load(
                                std::memory_order_relaxed);
  if (!locked) return;
  [[maybe_unused]] const int32_t old_count = g_global_lock_count.fetch_sub(1);
  assert(old_count >= 1 && "LeaveGlobalLock called without matching EnterGlobalLock");
  rex::thread::global_critical_region::mutex().unlock();
}

uint32_t MmioLoad(uint32_t guest_address) {
  uint32_t value = 0;
  rex::runtime::MMIOHandler::global_handler()->CheckLoad(guest_address, &value);
  return value;
}

void MmioStore(uint32_t guest_address, uint32_t value) {
  rex::runtime::MMIOHandler::global_handler()->CheckStore(guest_address, value);
}

}  // namespace rex::ppc
