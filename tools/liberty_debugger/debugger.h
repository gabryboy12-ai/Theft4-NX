#pragma once

#include <cstdint>
#include <string>

namespace Debugger {

class DebugState;
class CommandParser;

// =============================================================================
// Debugger Lifecycle
// =============================================================================

// Initialize the debugger system
// Call once at startup, before any guest threads
void Initialize();

// Shutdown the debugger system
// Call once at exit, after guest threads have stopped
void Shutdown();

// Start the CLI input thread
// This spawns a background thread that reads from stdin
void StartCLIThread();

// Stop the CLI thread
void StopCLIThread();

// Check if debugger is initialized
bool IsInitialized();

// =============================================================================
// Event Hooks - Call from guest code
// =============================================================================

// Called when a null or invalid function pointer is called
// lr: Link register (return address) of the caller
// targetAddr: The invalid address that was attempted to call
void OnNullCall(uint32_t lr, uint32_t targetAddr);

// Called when a function is entered (for tracing)
// addr: Guest address of the function
// ctx: Pointer to PPCContext (opaque to avoid header dependency)
void OnFunctionEntry(uint32_t addr, void* ctx);

// Called when a function exits (for tracing)
void OnFunctionExit(uint32_t addr, void* ctx);

// Called on memory read (for watchpoints)
void OnMemoryRead(uint32_t addr, uint32_t size);

// Called on memory write (for watchpoints)
void OnMemoryWrite(uint32_t addr, uint32_t size, uint64_t value);

// Called each frame (for stats)
void OnFrame();

// Called on heap allocation
void OnAlloc(uint32_t addr, uint32_t size);

// Called on heap free
void OnFree(uint32_t addr);

// =============================================================================
// State Access
// =============================================================================

// Get the global debug state
DebugState& GetState();

// Get the command parser
CommandParser& GetParser();

// Check if execution is paused
bool IsPaused();

// Pause execution (blocks guest threads)
void Pause();

// Continue execution
void Continue();

// =============================================================================
// Memory Access Helpers
// =============================================================================

// Set the memory base pointer (call from main.cpp after Memory init)
void SetMemoryBase(void* base, size_t size);

// Translate guest address to host pointer
void* TranslateAddress(uint32_t guestAddr);

// Translate host pointer to guest address
uint32_t MapVirtual(const void* hostPtr);

// Check if address is in valid guest range
bool IsValidAddress(uint32_t guestAddr);

// Read memory safely (returns false if invalid)
bool ReadMemory(uint32_t guestAddr, void* dest, size_t size);

// Read a big-endian uint32_t
bool ReadU32BE(uint32_t guestAddr, uint32_t& outValue);

// Read a big-endian uint16_t
bool ReadU16BE(uint32_t guestAddr, uint16_t& outValue);

// =============================================================================
// Function Name Resolution
// =============================================================================

// Register a function name mapping
void RegisterFunctionName(uint32_t addr, const std::string& name);

// Get function name for address (returns "sub_XXXXXXXX" if not registered)
std::string GetFunctionName(uint32_t addr);

// =============================================================================
// Thread Registry
// =============================================================================

// Register a guest thread
void RegisterThread(uint32_t threadId, uint32_t entryPoint, const std::string& name = "");

// Unregister a guest thread
void UnregisterThread(uint32_t threadId);

// Set thread state
void SetThreadState(uint32_t threadId, int state, uint32_t waitingOn = 0);

} // namespace Debugger
