# LibertyRecomp CLI Debugger Architecture

## Overview

The CLI debugger provides interactive debugging capabilities for the PPC recompiler, enabling inspection of memory, threads, synchronization primitives, and execution control.

## Module Structure

```
tools/liberty_debugger/
├── ARCHITECTURE.md          # This file
├── CMakeLists.txt           # Build configuration
├── debugger.h               # Main public interface
├── debugger.cpp             # Core debugger logic + CLI thread
├── debug_state.h            # Shared state (breakpoints, watchpoints, stats)
├── debug_state.cpp          # State management implementation
├── command_parser.h         # Command tokenizer and dispatcher
├── command_parser.cpp       # Parser implementation
├── commands/
│   ├── commands.h           # Command registry
│   ├── memory_cmd.cpp       # mem, watch, find, vtable, heap
│   ├── thread_cmd.cpp       # threads, thread, waits, deadlock
│   ├── sync_cmd.cpp         # events, sems, signal
│   ├── exec_cmd.cpp         # break, trace, step, continue, callstack
│   ├── ppc_cmd.cpp          # regs, r, ctx, tls
│   ├── gpu_cmd.cpp          # device, shaders, rendertarget, drawcalls
│   ├── file_cmd.cpp         # rpf, streams, handles
│   ├── ctor_cmd.cpp         # ctors, ctor, bss
│   └── util_cmd.cpp         # addr, guest, func, disasm, stats, log
└── README.md                # Usage documentation
```

## Core Components

### 1. DebugState (debug_state.h)

Shared state accessed by both debugger CLI and guest threads:

```cpp
struct BreakpointInfo {
    uint32_t addr;
    bool enabled;
    uint64_t hitCount;
    PPCFunc* originalFunc;  // Saved original function pointer
};

struct WatchpointInfo {
    uint32_t addr;
    uint32_t size;
    enum Type { Read, Write, ReadWrite } type;
    uint64_t hitCount;
};

struct TraceInfo {
    uint32_t addr;
    uint32_t maxCalls;
    uint32_t callCount;
    bool logArgs;
};

struct NullCallInfo {
    uint32_t targetAddr;
    uint64_t count;
    uint32_t lastLR;
};

struct DebugState {
    // Breakpoints: guest_addr -> BreakpointInfo
    std::unordered_map<uint32_t, BreakpointInfo> breakpoints;
    
    // Watchpoints: guest_addr -> WatchpointInfo
    std::unordered_map<uint32_t, WatchpointInfo> watchpoints;
    
    // Function tracing: guest_addr -> TraceInfo
    std::unordered_map<uint32_t, TraceInfo> traces;
    
    // Statistics counters
    std::atomic<uint64_t> nullCallCount{0};
    std::atomic<uint64_t> invalidAddrCallCount{0};
    std::atomic<uint64_t> frameCount{0};
    std::atomic<uint64_t> allocCount{0};
    
    // Null call tracking: LR -> NullCallInfo
    std::unordered_map<uint32_t, NullCallInfo> nullCalls;
    
    // Log filtering
    std::unordered_set<std::string> enabledLogCategories;
    std::string logFilterPattern;
    
    // Execution control
    std::atomic<bool> isPaused{false};
    std::atomic<uint32_t> currentThreadId{0};
    std::condition_variable pauseCV;
    std::mutex pauseMutex;
    
    // Thread-safe access
    mutable std::mutex stateMutex;
};
```

### 2. Debugger Interface (debugger.h)

```cpp
namespace Debugger {
    // Lifecycle
    void Initialize();
    void Shutdown();
    void StartCLIThread();
    
    // Called from guest code hooks
    void OnNullCall(uint32_t lr, uint32_t targetAddr);
    void OnFunctionEntry(uint32_t addr, PPCContext* ctx);
    void OnFunctionExit(uint32_t addr, PPCContext* ctx);
    void OnMemoryRead(uint32_t addr, uint32_t size);
    void OnMemoryWrite(uint32_t addr, uint32_t size, uint64_t value);
    void OnFrame();
    
    // State access
    DebugState& GetState();
    bool IsPaused();
    void Pause();
    void Continue();
}
```

### 3. Command Parser (command_parser.h)

```cpp
struct CommandContext {
    std::vector<std::string> args;
    std::ostream& out;
    DebugState& state;
    Memory& memory;
};

using CommandFunc = std::function<bool(CommandContext&)>;

struct CommandEntry {
    std::string name;
    std::string usage;
    std::string help;
    CommandFunc func;
};

class CommandParser {
public:
    void RegisterCommand(const CommandEntry& entry);
    bool Execute(const std::string& line, std::ostream& out);
    void PrintHelp(std::ostream& out);
    std::vector<std::string> GetCompletions(const std::string& partial);
private:
    std::vector<CommandEntry> m_commands;
};
```

## Integration with LibertyRecomp

The debugger is designed as a **library** that LibertyRecomp links against. Integration points:

### 1. main.cpp
```cpp
#include <liberty_debugger/debugger.h>

int main(...) {
    // After KiSystemStartup, before GuestThread::Start
    if (Config::EnableDebugger) {
        Debugger::Initialize();
        Debugger::StartCLIThread();
    }
    
    GuestThread::Start({entry, 0, 0, 0});
    
    if (Config::EnableDebugger) {
        Debugger::Shutdown();
    }
}
```

### 2. kernel/imports.cpp - Null Call Detection
```cpp
// In null/invalid call handler
#ifdef ENABLE_DEBUGGER
Debugger::OnNullCall(ctx.lr.u32, targetAddr);
#endif
```

### 3. gpu/video.cpp - Frame Counting
```cpp
void Video::Present() {
#ifdef ENABLE_DEBUGGER
    Debugger::OnFrame();
#endif
    // ...
}
```

## Thread Safety

- `DebugState` uses `std::mutex` for container access
- Atomic counters for stats (lock-free reads)
- `std::condition_variable` for pause/continue synchronization
- CLI thread is read-mostly, guest threads do writes

## Command Categories

### Priority 1: Diagnostic (Current Pain Points)

| Command | Description |
|---------|-------------|
| `null-calls` | List all null function call attempts with LR addresses |
| `vtable <obj>` | Dump vtable entries with function names |
| `threads` | List all guest threads with state |
| `waits` | Show blocked threads and what they're waiting on |
| `trace <addr>` | Log N calls with args |
| `watch <addr>` | Memory watchpoint for corruption |

### Priority 2: Inspection

| Command | Description |
|---------|-------------|
| `mem <addr> [len]` | Hex dump guest memory |
| `regs` | Dump PPC registers |
| `events` | List Event objects |
| `sems` | List Semaphore objects |
| `ctors` | List global constructors |

### Priority 3: Control

| Command | Description |
|---------|-------------|
| `break <addr>` | Set breakpoint |
| `signal <handle>` | Manually signal Event/Semaphore |
| `stub <addr>` | Replace function with return stub |
| `continue` | Resume execution |

## Memory Map Reference

```
Guest Address Ranges:
  0x82000000 - 0x82A00000  Code (.text)
  0x82A00000 - 0x82B00000  Global constructors
  0x82090000 - 0x82270000  Read-only data (.rdata, vtables)
  0x830E5900              Shader table (128 slots)
  0x830F5000 - 0x830F8000  Worker globals
  0x83100000+             Heap
  0xA0000000+             Kernel objects
```

## Example Session

```
liberty> null-calls
#1  LR=0x8285337C  target=0x0        count=23  (sub_82853370)
#2  LR=0x829944F0  target=0x0        count=3   (sub_829944E0)

liberty> vtable 0x830F2CB8
Vtable at 0x830F2CB8:
  [0] 0x00000000  (NULL)
  [1] 0x82145678  sub_82145678
  [2] 0x8214ABCD  sub_8214ABCD

liberty> threads
  #0  main     RUNNING  entry=0x82120000  tls=0x80001000
  #1  worker1  WAITING  entry=0x827DE858  blocked=Sem@0xA82487F0
  #2  worker2  WAITING  entry=0x827DE858  blocked=Sem@0xA82487B0

liberty> watch 0x82003890 w
Watchpoint #1 set: write to 0x82003890

liberty> continue
WATCHPOINT #1 HIT: write to 0x82003890
  Value: 0x636F6D6D ("comm")
  Thread: #9 (gpu)
  LR: 0x82626210
```
