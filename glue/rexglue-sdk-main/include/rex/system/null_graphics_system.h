/**
 * @file        system/null_graphics_system.h
 * @brief       A guest GPU that keeps the game in step without drawing
 *
 * For a runtime without a renderer (Switch, 06-game-boot-plan.md section
 * 10): the Vd* exports find a graphics system, the command processor
 * consumes the ring and writes back the read pointer, and of the PM4 packets
 * it executes only what the CPU side waits for: register writes (SCRATCH
 * writeback, coherency), memory writes and fences (MEM_WRITE, COND_WRITE,
 * REG_TO_MEM, EVENT_WRITE_*), WAIT_REG_MEM, indirect buffers, interrupts.
 * Draws, shader loads and resolves are skipped. A vblank interrupt is raised
 * at the guest refresh rate, as GraphicsSystem's "GPU VSync" does.
 */

#pragma once

#include <cstdint>
#include <memory>

#include <rex/system/interfaces/graphics.h>

namespace rex::system {

std::unique_ptr<IGraphicsSystem> CreateNullGraphicsSystem();

// Counters of the null GPU created last (all zero if none), for logs.
struct NullGpuStats {
  uint64_t wptr_writes = 0;     // CP_RB_WPTR writes by the guest
  uint64_t packets = 0;         // PM4 packets executed or skipped
  uint64_t draws = 0;           // draw packets skipped
  uint64_t swaps = 0;           // PM4_XE_SWAP (VdSwap)
  uint64_t vblanks = 0;         // vblank interrupts raised
  uint64_t interrupts = 0;      // PM4_INTERRUPT dispatches
  uint64_t unknown_opcodes = 0; // type-3 opcodes it does not know
  uint32_t read_index = 0;      // ring read pointer (dwords)
  uint32_t write_index = 0;     // last CP_RB_WPTR
  uint32_t waiting_opcode = 0;  // WAIT_REG_MEM in progress for >= 1 s: 0x3C, else 0
};
NullGpuStats GetNullGpuStats();

// Asks the threads of the null GPU created last to end, without waiting
// (switch-game's cooperative exit joins them itself).
void RequestNullGpuStop();

}  // namespace rex::system
