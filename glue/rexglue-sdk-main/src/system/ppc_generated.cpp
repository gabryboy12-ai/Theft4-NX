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

namespace rex::ppc {

namespace {

std::atomic<int32_t> g_global_lock_count{0};

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

uint64_t CheckGlobalLock() {
  auto lock = rex::thread::global_critical_region::AcquireDirect();
  return g_global_lock_count.load() ? 0 : 0x8000;
}

void EnterGlobalLock() {
  rex::thread::global_critical_region::mutex().lock();
  g_global_lock_count.fetch_add(1);
}

void LeaveGlobalLock() {
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
