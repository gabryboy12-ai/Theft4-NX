#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Debugger {

// =============================================================================
// Breakpoint Configuration
// =============================================================================
struct BreakpointInfo {
    uint32_t addr = 0;
    bool enabled = true;
    uint64_t hitCount = 0;
    void* originalFunc = nullptr;  // Saved original function pointer
    std::string condition;         // Optional condition expression
};

// =============================================================================
// Watchpoint Configuration
// =============================================================================
enum class WatchType : uint8_t {
    Read,
    Write,
    ReadWrite
};

struct WatchpointInfo {
    uint32_t addr = 0;
    uint32_t size = 4;
    WatchType type = WatchType::Write;
    bool enabled = true;
    uint64_t hitCount = 0;
    uint64_t lastValue = 0;
};

// =============================================================================
// Function Tracing
// =============================================================================
struct TraceInfo {
    uint32_t addr = 0;
    uint32_t maxCalls = 0;       // 0 = unlimited
    uint32_t callCount = 0;
    bool logArgs = true;
    bool logReturn = true;
    bool enabled = true;
};

// =============================================================================
// Null/Invalid Call Tracking
// =============================================================================
struct NullCallInfo {
    uint32_t targetAddr = 0;
    uint64_t count = 0;
    uint32_t lastLR = 0;
    uint32_t firstLR = 0;
    std::string funcName;        // Resolved function name if available
};

// =============================================================================
// Thread Info (snapshot)
// =============================================================================
struct ThreadInfo {
    uint32_t id = 0;
    uint32_t entryPoint = 0;
    uint32_t tlsBase = 0;
    uint32_t stackBase = 0;
    uint32_t stackLimit = 0;
    uint32_t currentLR = 0;
    enum class State { Running, Waiting, Suspended, Terminated } state = State::Running;
    uint32_t waitingOn = 0;      // Handle being waited on (if state == Waiting)
    std::string name;
};

// =============================================================================
// Sync Object Info (snapshot)
// =============================================================================
struct EventInfo {
    uint32_t handle = 0;
    bool manualReset = false;
    bool signaled = false;
    uint32_t waitersCount = 0;
};

struct SemaphoreInfo {
    uint32_t handle = 0;
    uint32_t count = 0;
    uint32_t maxCount = 0;
    uint32_t waitersCount = 0;
};

// =============================================================================
// Statistics
// =============================================================================
struct DebugStats {
    std::atomic<uint64_t> nullCallCount{0};
    std::atomic<uint64_t> invalidAddrCallCount{0};
    std::atomic<uint64_t> frameCount{0};
    std::atomic<uint64_t> allocCount{0};
    std::atomic<uint64_t> freeCount{0};
    std::atomic<uint64_t> totalAllocBytes{0};
    std::atomic<uint64_t> functionCalls{0};
    std::atomic<uint64_t> watchpointHits{0};
    std::atomic<uint64_t> breakpointHits{0};
};

// =============================================================================
// Main Debug State
// =============================================================================
class DebugState {
public:
    DebugState() = default;
    ~DebugState() = default;

    // Non-copyable
    DebugState(const DebugState&) = delete;
    DebugState& operator=(const DebugState&) = delete;

    // -------------------------------------------------------------------------
    // Breakpoints
    // -------------------------------------------------------------------------
    bool AddBreakpoint(uint32_t addr);
    bool RemoveBreakpoint(uint32_t addr);
    bool EnableBreakpoint(uint32_t addr, bool enable);
    BreakpointInfo* GetBreakpoint(uint32_t addr);
    std::vector<BreakpointInfo> GetAllBreakpoints() const;
    void ClearBreakpoints();

    // -------------------------------------------------------------------------
    // Watchpoints
    // -------------------------------------------------------------------------
    bool AddWatchpoint(uint32_t addr, uint32_t size, WatchType type);
    bool RemoveWatchpoint(uint32_t addr);
    WatchpointInfo* GetWatchpoint(uint32_t addr);
    std::vector<WatchpointInfo> GetAllWatchpoints() const;
    void ClearWatchpoints();
    bool CheckWatchpoint(uint32_t addr, uint32_t size, bool isWrite);

    // -------------------------------------------------------------------------
    // Tracing
    // -------------------------------------------------------------------------
    bool AddTrace(uint32_t addr, uint32_t maxCalls = 0);
    bool RemoveTrace(uint32_t addr);
    TraceInfo* GetTrace(uint32_t addr);
    std::vector<TraceInfo> GetAllTraces() const;
    void ClearTraces();

    // -------------------------------------------------------------------------
    // Null Call Tracking
    // -------------------------------------------------------------------------
    void RecordNullCall(uint32_t lr, uint32_t targetAddr);
    std::vector<NullCallInfo> GetNullCalls() const;
    void ClearNullCalls();

    // -------------------------------------------------------------------------
    // Logging Control
    // -------------------------------------------------------------------------
    void SetLogCategory(const std::string& category, bool enabled);
    bool IsLogCategoryEnabled(const std::string& category) const;
    void SetLogFilter(const std::string& pattern);
    std::string GetLogFilter() const;
    void SetQuietMode(bool quiet);
    bool IsQuietMode() const;

    // -------------------------------------------------------------------------
    // Execution Control
    // -------------------------------------------------------------------------
    void Pause();
    void Continue();
    bool IsPaused() const;
    void WaitIfPaused();  // Called by guest threads
    void SetCurrentThread(uint32_t threadId);
    uint32_t GetCurrentThread() const;

    // -------------------------------------------------------------------------
    // Statistics
    // -------------------------------------------------------------------------
    DebugStats& Stats() { return m_stats; }
    const DebugStats& Stats() const { return m_stats; }

private:
    mutable std::mutex m_mutex;

    // Breakpoints: guest_addr -> info
    std::unordered_map<uint32_t, BreakpointInfo> m_breakpoints;

    // Watchpoints: guest_addr -> info
    std::unordered_map<uint32_t, WatchpointInfo> m_watchpoints;

    // Traces: guest_addr -> info
    std::unordered_map<uint32_t, TraceInfo> m_traces;

    // Null calls: LR -> info
    std::unordered_map<uint32_t, NullCallInfo> m_nullCalls;

    // Logging
    std::unordered_set<std::string> m_enabledCategories;
    std::string m_logFilter;
    bool m_quietMode = false;

    // Execution control
    std::atomic<bool> m_isPaused{false};
    std::atomic<uint32_t> m_currentThreadId{0};
    std::condition_variable m_pauseCV;
    std::mutex m_pauseMutex;

    // Statistics
    DebugStats m_stats;
};

} // namespace Debugger
