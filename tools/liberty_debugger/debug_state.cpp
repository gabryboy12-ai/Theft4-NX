#include "debug_state.h"
#include <algorithm>

namespace Debugger {

// =============================================================================
// Breakpoints
// =============================================================================

bool DebugState::AddBreakpoint(uint32_t addr) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_breakpoints.count(addr)) {
        return false;  // Already exists
    }
    BreakpointInfo info;
    info.addr = addr;
    info.enabled = true;
    info.hitCount = 0;
    info.originalFunc = nullptr;
    m_breakpoints[addr] = info;
    return true;
}

bool DebugState::RemoveBreakpoint(uint32_t addr) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_breakpoints.erase(addr) > 0;
}

bool DebugState::EnableBreakpoint(uint32_t addr, bool enable) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_breakpoints.find(addr);
    if (it == m_breakpoints.end()) {
        return false;
    }
    it->second.enabled = enable;
    return true;
}

BreakpointInfo* DebugState::GetBreakpoint(uint32_t addr) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_breakpoints.find(addr);
    return (it != m_breakpoints.end()) ? &it->second : nullptr;
}

std::vector<BreakpointInfo> DebugState::GetAllBreakpoints() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<BreakpointInfo> result;
    result.reserve(m_breakpoints.size());
    for (const auto& [addr, info] : m_breakpoints) {
        result.push_back(info);
    }
    return result;
}

void DebugState::ClearBreakpoints() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_breakpoints.clear();
}

// =============================================================================
// Watchpoints
// =============================================================================

bool DebugState::AddWatchpoint(uint32_t addr, uint32_t size, WatchType type) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_watchpoints.count(addr)) {
        return false;
    }
    WatchpointInfo info;
    info.addr = addr;
    info.size = size;
    info.type = type;
    info.enabled = true;
    info.hitCount = 0;
    info.lastValue = 0;
    m_watchpoints[addr] = info;
    return true;
}

bool DebugState::RemoveWatchpoint(uint32_t addr) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_watchpoints.erase(addr) > 0;
}

WatchpointInfo* DebugState::GetWatchpoint(uint32_t addr) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_watchpoints.find(addr);
    return (it != m_watchpoints.end()) ? &it->second : nullptr;
}

std::vector<WatchpointInfo> DebugState::GetAllWatchpoints() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<WatchpointInfo> result;
    result.reserve(m_watchpoints.size());
    for (const auto& [addr, info] : m_watchpoints) {
        result.push_back(info);
    }
    return result;
}

void DebugState::ClearWatchpoints() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_watchpoints.clear();
}

bool DebugState::CheckWatchpoint(uint32_t addr, uint32_t size, bool isWrite) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [wpAddr, info] : m_watchpoints) {
        if (!info.enabled) continue;
        
        // Check for overlap
        uint32_t wpEnd = wpAddr + info.size;
        uint32_t accessEnd = addr + size;
        if (addr < wpEnd && accessEnd > wpAddr) {
            // Check type match
            bool typeMatch = false;
            switch (info.type) {
                case WatchType::Read:
                    typeMatch = !isWrite;
                    break;
                case WatchType::Write:
                    typeMatch = isWrite;
                    break;
                case WatchType::ReadWrite:
                    typeMatch = true;
                    break;
            }
            if (typeMatch) {
                info.hitCount++;
                m_stats.watchpointHits++;
                return true;
            }
        }
    }
    return false;
}

// =============================================================================
// Tracing
// =============================================================================

bool DebugState::AddTrace(uint32_t addr, uint32_t maxCalls) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_traces.count(addr)) {
        return false;
    }
    TraceInfo info;
    info.addr = addr;
    info.maxCalls = maxCalls;
    info.callCount = 0;
    info.logArgs = true;
    info.logReturn = true;
    info.enabled = true;
    m_traces[addr] = info;
    return true;
}

bool DebugState::RemoveTrace(uint32_t addr) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_traces.erase(addr) > 0;
}

TraceInfo* DebugState::GetTrace(uint32_t addr) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_traces.find(addr);
    return (it != m_traces.end()) ? &it->second : nullptr;
}

std::vector<TraceInfo> DebugState::GetAllTraces() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<TraceInfo> result;
    result.reserve(m_traces.size());
    for (const auto& [addr, info] : m_traces) {
        result.push_back(info);
    }
    return result;
}

void DebugState::ClearTraces() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_traces.clear();
}

// =============================================================================
// Null Call Tracking
// =============================================================================

void DebugState::RecordNullCall(uint32_t lr, uint32_t targetAddr) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stats.nullCallCount++;
    
    auto it = m_nullCalls.find(lr);
    if (it == m_nullCalls.end()) {
        NullCallInfo info;
        info.targetAddr = targetAddr;
        info.count = 1;
        info.firstLR = lr;
        info.lastLR = lr;
        m_nullCalls[lr] = info;
    } else {
        it->second.count++;
        it->second.lastLR = lr;
        if (it->second.targetAddr == 0) {
            it->second.targetAddr = targetAddr;
        }
    }
}

std::vector<NullCallInfo> DebugState::GetNullCalls() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<NullCallInfo> result;
    result.reserve(m_nullCalls.size());
    for (const auto& [lr, info] : m_nullCalls) {
        NullCallInfo copy = info;
        copy.lastLR = lr;  // Use key as LR
        result.push_back(copy);
    }
    // Sort by count descending
    std::sort(result.begin(), result.end(), 
              [](const NullCallInfo& a, const NullCallInfo& b) {
                  return a.count > b.count;
              });
    return result;
}

void DebugState::ClearNullCalls() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_nullCalls.clear();
}

// =============================================================================
// Logging Control
// =============================================================================

void DebugState::SetLogCategory(const std::string& category, bool enabled) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (enabled) {
        m_enabledCategories.insert(category);
    } else {
        m_enabledCategories.erase(category);
    }
}

bool DebugState::IsLogCategoryEnabled(const std::string& category) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_quietMode) return false;
    if (m_enabledCategories.empty()) return true;  // All enabled by default
    return m_enabledCategories.count(category) > 0;
}

void DebugState::SetLogFilter(const std::string& pattern) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_logFilter = pattern;
}

std::string DebugState::GetLogFilter() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_logFilter;
}

void DebugState::SetQuietMode(bool quiet) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_quietMode = quiet;
}

bool DebugState::IsQuietMode() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_quietMode;
}

// =============================================================================
// Execution Control
// =============================================================================

void DebugState::Pause() {
    m_isPaused.store(true, std::memory_order_release);
}

void DebugState::Continue() {
    {
        std::lock_guard<std::mutex> lock(m_pauseMutex);
        m_isPaused.store(false, std::memory_order_release);
    }
    m_pauseCV.notify_all();
}

bool DebugState::IsPaused() const {
    return m_isPaused.load(std::memory_order_acquire);
}

void DebugState::WaitIfPaused() {
    if (!m_isPaused.load(std::memory_order_acquire)) {
        return;
    }
    std::unique_lock<std::mutex> lock(m_pauseMutex);
    m_pauseCV.wait(lock, [this]() {
        return !m_isPaused.load(std::memory_order_acquire);
    });
}

void DebugState::SetCurrentThread(uint32_t threadId) {
    m_currentThreadId.store(threadId, std::memory_order_release);
}

uint32_t DebugState::GetCurrentThread() const {
    return m_currentThreadId.load(std::memory_order_acquire);
}

} // namespace Debugger
