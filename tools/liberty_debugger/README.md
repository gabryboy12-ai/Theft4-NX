# LibertyRecomp CLI Debugger

Interactive CLI debugger for the LibertyRecomp PPC recompiler.

## Features

### Memory Inspection
- **`mem <addr> [size]`** - Hex dump guest memory
- **`watch <addr> [r|w|rw] [size]`** - Set memory watchpoint
- **`vtable <obj_addr> [count]`** - Dump vtable entries
- **`find <pattern> [start] [end]`** - Search memory for pattern

### Thread & Synchronization
- **`threads`** - List all guest threads with state
- **`waits`** - Show blocked threads
- **`events`** - List Event objects
- **`sems`** - List Semaphore objects
- **`signal <handle>`** - Manually signal sync object

### Execution Control
- **`break <addr>`** - Set breakpoint
- **`trace <addr> [count]`** - Trace function calls
- **`pause`** - Pause execution
- **`continue`** - Continue execution

### PPC Context
- **`regs`** - Dump PPC registers
- **`tls [offset]`** - Dump TLS data

### GPU/Rendering
- **`device [offset]`** - Show device context
- **`shaders`** - List loaded shaders

### File System
- **`handles`** - List open file handles

### Global Constructors
- **`ctors`** - List global constructors
- **`bss <addr>`** - Check .bss initialization

### Utilities
- **`null-calls`** - List null function call attempts
- **`stats`** - Show runtime statistics
- **`log <category> [on|off]`** - Toggle logging
- **`quiet`** / **`verbose`** - Control log output
- **`help [command]`** - Show help

## Integration

### Building

The debugger is built as a static library. Add to your main CMakeLists.txt:

```cmake
add_subdirectory(tools/liberty_debugger)
target_link_libraries(LibertyRecomp PRIVATE liberty_debugger)
```

### Initialization

In your main.cpp:

```cpp
#include <liberty_debugger/debugger.h>

int main() {
    // After memory initialization
    Debugger::SetMemoryBase(g_memory.base, PPC_MEMORY_SIZE);
    
    // Before guest thread starts
    if (Config::EnableDebugger) {
        Debugger::Initialize();
        Debugger::StartCLIThread();
    }
    
    // Start guest
    GuestThread::Start({entry, 0, 0, 0});
    
    // Cleanup
    Debugger::Shutdown();
}
```

### Hooking Events

In kernel/imports.cpp or guest code:

```cpp
#include <liberty_debugger/debugger.h>

// Track null/invalid calls
void HandleNullCall(uint32_t lr, uint32_t target) {
    Debugger::OnNullCall(lr, target);
}

// Track function entry (for breakpoints/tracing)
void OnFunctionEntry(uint32_t addr, PPCContext* ctx) {
    Debugger::OnFunctionEntry(addr, ctx);
}

// Track frames
void Video::Present() {
    Debugger::OnFrame();
    // ...
}
```

## Example Session

```
╔═══════════════════════════════════════════════════════════════╗
║           LibertyRecomp Interactive Debugger                  ║
║  Type 'help' for commands, 'help <cmd>' for usage             ║
╚═══════════════════════════════════════════════════════════════╝

liberty> null-calls
#     LR          Target      Count  Function
----  ----------  ----------  -----  --------
1     0x8285337C  0x00000000  23     sub_82853370
2     0x829944F0  0x00000000  3      sub_829944E0

liberty> vtable 0x830F2CB8
Object at 0x830F2CB8
Vtable at 0x82147164:
  [0] 0x00000000  (NULL)
  [1] 0x82145678  sub_82145678
  [2] 0x8214ABCD  sub_8214ABCD

liberty> watch 0x82003890 w
Watchpoint set at 0x82003890

liberty> threads
ID    State      Entry       Name
----  ---------  ----------  ----------------
0     RUNNING    0x82120000  main
1     WAITING    0x827DE858  worker1  (on 0xA82487F0)
2     WAITING    0x827DE858  worker2  (on 0xA82487B0)

liberty> stats
Runtime Statistics:
  Frames:        1234
  Function calls: 5678901
  Allocations:   12345
  Null calls:    47
```

## Architecture

See [ARCHITECTURE.md](ARCHITECTURE.md) for detailed design documentation.

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
