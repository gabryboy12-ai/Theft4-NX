/**
 * @file ppc/generated.h
 * @brief The SDK API recompiled code is compiled against, and nothing more.
 *
 * The generated init header includes only this for the recompiled
 * translation units (they define REX_RECOMPILED_TU before including it). It
 * holds what generated code must see inline:
 *
 *   - the PPCContext layout and the function macros (ppc/context.h),
 *   - the vector/FP intrinsics it inlines (ppc/intrinsics.h),
 *   - the image descriptor (image_info.h),
 *   - on Switch, the guest translation table (memory/guest_table.h),
 *
 * and declarations of the runtime services the generated macros call out of
 * line (src/system/ppc_generated.cpp): traps, the timebase, the interrupt
 * lock and recognised MMIO. A change elsewhere in the runtime (kernel,
 * hooks, memory system, logging) therefore does not recompile generated
 * code; a change here, in those four headers or in their includes does.
 */

#pragma once

#include <cstdint>

#include <rex/image_info.h>
#include <rex/platform.h>
#include <rex/ppc/context.h>
#include <rex/ppc/intrinsics.h>
#include <rex/types.h>

#if REX_PLATFORM_NX
#include <rex/memory/guest_table.h>
#endif

namespace rex::ppc {

// tw/td/twi/tdi and service traps (types 20/26 print the guest string).
void Trap(PPCContext& ctx, uint8_t* base, uint16_t trap_type);

// mftb: the guest timebase.
uint64_t QueryTimebase();

// mfmsr / mtmsrd(ee) interrupt lock on the runtime's global critical region.
// CheckGlobalLock returns the MSR[EE] value the game sees (0 or 0x8000).
uint64_t CheckGlobalLock();
void EnterGlobalLock();
void LeaveGlobalLock();

// Loads and stores the recompiler recognised as MMIO (0x7F000000-0x7FFFFFFF).
uint32_t MmioLoad(uint32_t guest_address);
void MmioStore(uint32_t guest_address, uint32_t value);

}  // namespace rex::ppc
