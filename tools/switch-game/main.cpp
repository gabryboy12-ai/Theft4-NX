// switch-game: first NRO of the game on Nintendo Switch, without graphics
// (docs/switch-port/06-game-boot-plan.md section 4).
//
//   1. durable log (sdmc:/switch/theft4/game.log) and crash log
//      (sdmc:/switch/theft4/game-crash.log), module base for crash reports
//   2. game files read in place from sdmc:/switch/theft4/game (default.xex;
//      no default.xexp: option C of the plan, European base executable)
//   3. rex::Runtime with the HLE kernel and no graphics, audio or input;
//      Setup(PPCImageConfig), LoadXexImage, PrepareModuleLaunch
//   4. the game's main thread resumed at the XEX entry point
//
// The run stops explicitly, and says so in the log, at the first Xbox kernel
// export or runtime function this build lacks on Switch
// (rex::diagnostics::StopAtMissingFunction, defined here): exports of the
// headless kernel, exports that need a graphics backend, runtime paths that
// do not exist on Switch, and imports the SDK does not implement at all
// (missing_imports.cpp, generated at build time). The thread that reached it
// is parked; press + to exit.
//
// Logging (run 19): lines go to a buffer that a writer thread appends to
// game.log once a second (one open, write, fsync, close), and every exit
// step, termination, stop and fault writes the buffer at once. Run 18 wrote
// each line as switch-smoke does (open, write, fsync, close on the thread
// that logs): the async read worker paid that for every "[AsyncIO] complete"
// line. cvar nx_log_durable = true brings that mode back for crash hunting:
// after a crash the last line is the last operation attempted. An unhandled fault is also written to
// game-crash.log (registers, PC and LR as offsets from the module base);
// the runtime then ends it in svcBreak on the faulting thread's own stack, so
// Atmosphere writes its crash report (exception_handler_switch.cpp).
//
// Every termination leaves a line in game.log (run 9 ended silently inside
// Runtime::Setup): exit, _exit, _Exit, abort and svcExitProcess are wrapped at
// link time (-Wl,--wrap in CMakeLists.txt) and log their code and the
// caller's LR as an offset from the module base; std::terminate logs the
// exception; an atexit handler marks exit(); main logs its return. stderr is
// copied into game.log too: rex::FatalError, the two-argument asserts that
// stay on in Release, newlib's assert and libstdc++ write only there. Stub
// exports hit are summarised at the stop and at exit.
//
// Exit: hold + for 1 s; the process leaves with the cooperative stop of mode
// D (default since run 18), or with exit(0) at once (mode A, cvar
// nx_exit_mode = A); never with
// svcExitProcess nor with paused threads (run 15, 09-rules.md section 9; see
// "Exit" below and tools/switch-common/nx_exit.h). The console stays open
// until the process ends (mode A of run 15 left it so); console output goes
// through ConsolePrint under g_console_mutex.
//
// Graphics: the null GPU (cvar nx_null_gpu, rex/system/null_graphics_system.h)
// lets the game past the Vd* exports without drawing. Input: the Switch
// controller is the game's 360 pad (cvar nx_input, nx_input.h).
//
// Where the game is (run 11 resumed the main thread and then logged nothing):
// every implemented export is traced on Switch (export_stubs.h). A heartbeat
// line every 10 s, and a snapshot of every XThread at 5, 20 and 60 s, then
// every 2 min and at +: the export it is inside and for how long, the last
// export, the guest LR, and the host PC, LR and return addresses found on its
// stack, as offsets for addr2line (tools/switch-game/where.py decodes them).

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cinttypes>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <typeinfo>
#include <vector>

#include <algorithm>

#include <fcntl.h>
#include <malloc.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/diagnostics/export_stubs.h>
#include <rex/diagnostics/missing_function.h>
#include <rex/diagnostics/policy.h>
#include <rex/exception_handler.h>
#include <rex/filesystem.h>
#include <rex/kernel/init.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/platform/nx_fault_record.h>
#include <rex/runtime.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xfile.h>
#include <rex/audio/null_audio_system.h>
#include <rex/system/null_graphics_system.h>
#include <rex/system/thread_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

#include <spdlog/sinks/base_sink.h>

#include "gta4_init.h"
#include "nx_clocks.h"
#include "nx_exit.h"
#include "nx_input.h"
#include "nx_profiler.h"
#include "nx_threads.h"
#include "theft4_build_commit.h"  // build dir, see build_commit.cmake

#if THEFT4_NX_VULKAN
#include <rex/graphics/nx_gpu_counters.h>
#include <rex/graphics/vulkan/graphics_system.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/window_nx.h>
#include <rex/ui/windowed_app_context_nx.h>
#endif

REXCVAR_DEFINE_BOOL(nx_null_gpu, true, "Switch",
                    "Give the runtime the null GPU (rex/system/null_graphics_system.h): the "
                    "Vd* exports work and the ring is consumed without drawing. false: the "
                    "run stops at the first graphics export, as up to run 13");
REXCVAR_DEFINE_BOOL(nx_input, true, "Switch",
                    "Give the game the Switch controller as its 360 pad (nx_input.h). false: "
                    "no input system, XamInputGetState says not connected (run 14)");
REXCVAR_DEFINE_STRING(nx_gpu, "null", "Switch",
                      "GPU: null = the null GPU (nx_null_gpu = false: none); vulkan = ReXGlue's "
                      "Vulkan backend on NVK, presenting on the default NWindow with the console "
                      "closed (only in a build with THEFT4_SWITCH_GAME_VULKAN; 06 section 24)");
REXCVAR_DEFINE_BOOL(nx_log_durable, false, "Switch",
                    "true: every log line is written to game.log at once (open, write, fsync, "
                    "close on the thread that logs: slow on the SD, for crash hunting); false: a "
                    "writer thread appends the lines once a second, and the exit steps, "
                    "terminations, the stop and faults write them at once (run 19)");
REXCVAR_DEFINE_BOOL(nx_trace_exports, true, "Switch",
                    "Trace every export (counts, first calls, the export each thread is inside "
                    "for the snapshots); false for measurement runs (export_stubs.h)");
REXCVAR_DEFINE_BOOL(nx_disable_auto_sleep, true, "Switch",
                    "appletSetAutoSleepDisabled(true) at start: run 18 was suspended for 406 s "
                    "after about 5 minutes without input (auto-sleep)");
REXCVAR_DEFINE_INT32(nx_nvmap_mb, 0, "Switch",
                     "nx_gpu = vulkan: MB of libnx's nvdrv transfer memory "
                     "(__nx_nv_transfermem_size, bookkeeping of every GPU allocation; libnx 8). 0 = "
                     "leave libnx's (nfsmw-nx: 0x235C SharedMemoryTooSmall when it runs out)");
REXCVAR_DEFINE_BOOL(nx_vk_dump_frames, false, "Switch",
                    "nx_gpu = vulkan: also write the guest output captured at 20, 40, 60, 120 "
                    "and 240 s as PPM files in sdmc:/switch/theft4/frames (the capture and its "
                    "pixel statistics are logged in any case)");
REXCVAR_DEFINE_BOOL(nx_mount_cache, false, "Switch",
                    "Mount cache: and cache1: on the SD (run 12). Default false since run 21: "
                    "the game then reads everything from game: as a 360 without a hard disk "
                    "(run 20's third boot: no copy, same loading times, past the point where "
                    "the two boots with the cache crashed). true: the game copies ~1 GB into "
                    "the cache on every boot (run 19) and reuses what earlier runs left there");
REXCVAR_DEFINE_BOOL(nx_apm_clock, true, "Switch",
                    "In handheld mode request Horizon's standard configuration 0x92220008 (GPU "
                    "460.8 MHz, memory 1331.2, CPU 1020) through apm, at start and on every "
                    "dock/handheld change; docked is left alone. No boost, no overclock; the "
                    "clocks read back are in the heartbeat (nx_clocks.h)");
REXCVAR_DEFINE_BOOL(nx_profile, false, "Switch",
                    "Sample the busy threads once a millisecond (pause, read pc/lr/frames, "
                    "resume) and log a histogram every 10 s; where.py --profile sums it per "
                    "function (nx_profiler.h). Costs a few % of one core");
REXCVAR_DEFINE_BOOL(nx_nv_wrap, true, "Switch",
                    "Count NVK's nvdrv calls (fence waits and queries, kickoffs, nvmaps) in the "
                    "link-time wrappers (run 21). false: the wrappers only call the real "
                    "function (run 22 bisection; the wrap itself stays, it is link-time)");
REXCVAR_DEFINE_BOOL(nx_fault_count_handler, true, "Switch",
                    "Install the handler that counts every access violation (run 21). false for "
                    "the run 22 bisection: the 'faults:' line then stays at 0");
REXCVAR_DEFINE_BOOL(nx_crash_log_after_setup, true, "Switch",
                    "Install the crash-log handler again after Runtime::Setup, so it sees only "
                    "unhandled faults (run 21). false: as in run 20, it stays first and writes "
                    "(with an fsync) every handled write-watch fault too");
REXCVAR_DEFINE_BOOL(nx_stack_watch, true, "Switch",
                    "Fill every new thread's stack with a pattern before it starts and log "
                    "each stack's size and deepest use every minute (run 23, C.3)");
REXCVAR_DEFINE_BOOL(nx_resume_guard, false, "Switch",
                    "After a handled fault, if the sp or pc about to be resumed is not the one "
                    "the fault came in with, put the entry values back (counted in the "
                    "'resume:' line). false: counted and recorded only (run 23)");
REXCVAR_DEFINE_INT32(nx_mem_warn_mib, 64, "Switch",
                     "Heartbeat: an ERROR line, written at once, when the host heap can give "
                     "less than this many MiB (free in newlib's arena plus what sbrk can "
                     "still add)");
REXCVAR_DEFINE_STRING(nx_exit_mode, "D", "Switch",
                      "Exit at + (hold 1 s): D = file I/O gate closed and drained, null audio "
                      "stopped, every thread told to end and joined (2 s each), then exit(0); "
                      "A = exit(0) with the threads alive (runs 15-17) (nx_exit.h)");

namespace {

constexpr const char* kRootDir = "sdmc:/switch/theft4";
constexpr const char* kGameDir = "sdmc:/switch/theft4/game";
constexpr const char* kLogPath = "sdmc:/switch/theft4/game.log";
constexpr const char* kPreviousLogPath = "sdmc:/switch/theft4/game.prev.log";
constexpr const char* kCrashPath = "sdmc:/switch/theft4/game-crash.log";
constexpr const char* kConfigPath = "sdmc:/switch/theft4/game.toml";
constexpr const char* kAltConfigPath = "sdmc:/switch/theft4/switch-game.toml";
constexpr const char* kFrameDumpDir = "sdmc:/switch/theft4/frames";

bool g_logging_ready = false;
std::atomic<bool> g_stopped{false};
#if THEFT4_NX_VULKAN
// nx_gpu = vulkan: the UI context (this thread), the window and the backend.
// Never destroyed, as the runtime.
std::unique_ptr<rex::ui::NxWindowedAppContext> g_ui_context;
std::unique_ptr<rex::ui::Window> g_window;
rex::graphics::vulkan::VulkanGraphicsSystem* g_vulkan_graphics = nullptr;
#endif
std::atomic<bool> g_main_thread_returned{false};
u64 g_module_base = 0;
u64 g_module_text_size = 0;  // the R-X block at the base (for the stack scan)
u64 g_host_main_thread_id = 0;
int g_crash_fd = -1;

}  // namespace

// Start of the NRO image (see switch-smoke/main.cpp, StepModuleBase).
extern "C" char _start;
// End of newlib's heap (libsysbase's sbrk grows towards it; libnx sets it).
extern "C" char* fake_heap_end;
#if THEFT4_NX_VULKAN
// libnx's weak symbol (nv.o, 8 MiB): read by nvInitialize, i.e. when NVK starts.
extern "C" u32 __nx_nv_transfermem_size;
#endif

namespace {

// ── Console ────────────────────────────────────────────────────────────────

// The console belongs to whoever holds this mutex. Bounded: a thread that
// cannot take it within 200 ms skips the screen, and the line still reaches
// game.log.
std::timed_mutex g_console_mutex;
bool g_console_open = false;  // under g_console_mutex

void ConsolePrint(const char* text) {
  if (!g_console_mutex.try_lock_for(std::chrono::milliseconds(200))) {
    return;
  }
  if (g_console_open) {
    std::fputs(text, stdout);
    consoleUpdate(nullptr);
  }
  g_console_mutex.unlock();
}

// Run 19: refreshing every 16 ms cost the UI thread 11-15 % of a core. Only
// stderr text (written to the console without consoleUpdate) needs it now.
std::atomic<bool> g_console_dirty{false};

void ConsoleRefresh() {
  if (g_console_dirty.load(std::memory_order_relaxed) && g_console_mutex.try_lock()) {
    g_console_dirty.store(false, std::memory_order_relaxed);
    if (g_console_open) {
      consoleUpdate(nullptr);
    }
    g_console_mutex.unlock();
  }
}

// ── Log file ───────────────────────────────────────────────────────────────

std::timed_mutex g_log_file_mutex;  // the file, and the order of what reaches it

// Cost of the writes to game.log (each one open, write, fsync, close).
std::atomic<uint64_t> g_log_lines{0}, g_log_file_writes{0}, g_log_file_ns{0},
    g_log_file_ns_max{0};

void DurableWriteLocked(const char* data, size_t size) {
  if (size == 0) {
    return;
  }
  const u64 start = armGetSystemTick();
  int fd = open(kLogPath, O_WRONLY | O_APPEND | O_CREAT, 0666);
  if (fd < 0) {
    static bool reported = false;
    if (!reported) {
      reported = true;
      char text[96];
      std::snprintf(text, sizeof(text),
                    "game.log: open failed (errno %d); lines are on screen only\n", errno);
      ConsolePrint(text);
    }
    return;
  }
  (void)write(fd, data, size);
  fsync(fd);
  close(fd);
  const u64 ns = armTicksToNs(armGetSystemTick() - start);
  g_log_file_writes.fetch_add(1, std::memory_order_relaxed);
  g_log_file_ns.fetch_add(ns, std::memory_order_relaxed);
  if (ns > g_log_file_ns_max.load(std::memory_order_relaxed)) {
    g_log_file_ns_max.store(ns, std::memory_order_relaxed);  // the heartbeat resets it
  }
}

// Buffered mode (nx_log_durable = false): lines wait here for the writer.
bool g_log_buffered = false;
std::mutex g_log_buffer_mutex;
std::string g_log_buffer, g_log_spare;
constexpr size_t kLogBufferFlushBytes = 4u << 20;  // the logging thread writes it itself

// Writes the buffer to the file; g_log_file_mutex held. With try_only (fault
// path) a buffer held by another thread is left alone.
void FlushBufferLocked(bool try_only) {
  std::unique_lock<std::mutex> lock(g_log_buffer_mutex, std::defer_lock);
  if (try_only ? !lock.try_lock() : (lock.lock(), false)) {
    return;
  }
  g_log_spare.swap(g_log_buffer);
  lock.unlock();
  DurableWriteLocked(g_log_spare.data(), g_log_spare.size());
  g_log_spare.clear();
}

void FlushLog() {
  if (!g_log_buffered) {
    return;
  }
  std::lock_guard<std::timed_mutex> lock(g_log_file_mutex);
  FlushBufferLocked(false);
}

// For stderr and the exit steps, which the fault path of
// exception_handler_switch.cpp also reaches: never wait more than 200 ms for
// the log (a thread that faulted while writing it must not deadlock the
// report). The buffer goes first, so the file keeps the order. false = not
// written.
bool DurableWriteBounded(const char* data, size_t size) {
  if (!g_log_file_mutex.try_lock_for(std::chrono::milliseconds(200))) {
    return false;
  }
  if (g_log_buffered) {
    FlushBufferLocked(true);
  }
  DurableWriteLocked(data, size);
  g_log_file_mutex.unlock();
  return true;
}

void DurableWrite(const char* data, size_t size) {
  g_log_lines.fetch_add(1, std::memory_order_relaxed);
  if (g_log_buffered) {
    size_t pending;
    {
      std::lock_guard<std::mutex> lock(g_log_buffer_mutex);
      g_log_buffer.append(data, size);
      pending = g_log_buffer.size();
    }
    if (pending >= kLogBufferFlushBytes) {
      FlushLog();
    }
    return;
  }
  std::lock_guard<std::timed_mutex> lock(g_log_file_mutex);
  DurableWriteLocked(data, size);
}

// The writer: once a second. Its Horizon handle is in the heartbeat's CPU
// line.
Thread g_log_writer;
std::atomic<bool> g_log_writer_ready{false};

void LogWriterMain(void*) {
  for (;;) {
    svcSleepThread(1'000'000'000);
    FlushLog();
  }
}

// Called once, before the runtime starts any thread.
void StartLogWriter() {
  if (REXCVAR_GET(nx_log_durable)) {
    return;
  }
  g_log_buffer.reserve(kLogBufferFlushBytes * 2);
  g_log_spare.reserve(kLogBufferFlushBytes * 2);
  // Priority 0x2C as the exit worker; any core.
  if (R_SUCCEEDED(threadCreate(&g_log_writer, LogWriterMain, nullptr, nullptr, 0x10000, 0x2C, -2)) &&
      R_SUCCEEDED(threadStart(&g_log_writer))) {
    g_log_buffered = true;
    g_log_writer_ready = true;
  }
}

class DurableSink final : public spdlog::sinks::base_sink<std::mutex> {
 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    spdlog::memory_buf_t formatted;
    formatter_->format(msg, formatted);
    DurableWrite(formatted.data(), formatted.size());
  }
  void flush_() override { FlushLog(); }
};

void Report(bool error, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void Report(bool error, const char* fmt, ...) {
  const char* prefix = error ? "[game] ERROR " : "[game] ";
  char line[768];
  const size_t prefix_len = std::strlen(prefix);
  std::memcpy(line, prefix, prefix_len);
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(line + prefix_len, sizeof(line) - prefix_len - 1, fmt, args);
  va_end(args);
  {
    std::string screen(line + std::strlen("[game] "));
    screen += '\n';
    ConsolePrint(screen.c_str());
  }
  if (g_logging_ready) {
    std::string text(line);
    text += '\n';
    DurableWrite(text.data(), text.size());
  }
}

#define GAME_INFO(...) Report(false, __VA_ARGS__)
#define GAME_ERROR(...) Report(true, __VA_ARGS__)

// ── Crash log ──────────────────────────────────────────────────────────────

// Fault-handler context: no malloc, no spdlog, no stdio. Stack buffers and
// write(2) on a descriptor opened at start-up only.
void CrashWrite(const char* s) {
  if (g_crash_fd < 0) return;
  size_t n = 0;
  while (s[n]) ++n;
  (void)write(g_crash_fd, s, n);
}

void CrashHex(const char* label, uint64_t value) {
  static const char kHex[] = "0123456789abcdef";
  char buf[64];
  size_t w = 0;
  for (const char* p = label; *p && w < 40; ++p) buf[w++] = *p;
  buf[w++] = ' ';
  buf[w++] = '0';
  buf[w++] = 'x';
  for (int shift = 60; shift >= 0; shift -= 4) buf[w++] = kHex[(value >> shift) & 0xF];
  buf[w++] = '\n';
  buf[w] = 0;
  CrashWrite(buf);
}

void CrashDec(const char* label, uint64_t value) {
  char digits[24];
  int n = 0;
  do {
    digits[n++] = char('0' + value % 10);
    value /= 10;
  } while (value && n < 20);
  char buf[96];
  size_t w = 0;
  for (const char* p = label; *p && w < 60; ++p) buf[w++] = *p;
  while (n) buf[w++] = digits[--n];
  buf[w++] = '\n';
  buf[w] = 0;
  CrashWrite(buf);
}

u64 g_start_tick = 0;
// Free host heap at the last heartbeat, in bytes (LogHostMemory).
std::atomic<uint64_t> g_heap_headroom{0};

// The last heartbeat's memory and Vulkan lines, for the crash log (D.2, D.3).
// Two buffers: the heartbeat writes the idle one, then publishes it.
char g_last_beat[2][1024];
std::atomic<int> g_last_beat_index{0};

void SetLastBeat(const std::string& text) {
  const int next = 1 - g_last_beat_index.load(std::memory_order_relaxed);
  std::snprintf(g_last_beat[next], sizeof(g_last_beat[next]), "%s", text.c_str());
  g_last_beat_index.store(next, std::memory_order_release);
}

// Fault-handler safe: time since start, process memory now, heap headroom at
// the last heartbeat and that heartbeat's lines, then fsync (D.3).
void CrashWriteState(const char* why) {
  u64 used = 0, total = 0;
  svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
  CrashWrite(why);
  CrashWrite("\n");
  CrashDec("uptime ms ", armTicksToNs(armGetSystemTick() - g_start_tick) / 1'000'000);
  CrashDec("process MiB used ", used >> 20);
  CrashDec("process MiB total ", total >> 20);
  CrashDec("heap MiB free at the last heartbeat ",
           g_heap_headroom.load(std::memory_order_relaxed) >> 20);
  CrashWrite("last heartbeat:\n");
  CrashWrite(g_last_beat[g_last_beat_index.load(std::memory_order_acquire)]);
  CrashWrite("\n");
  if (g_crash_fd >= 0) fsync(g_crash_fd);
}

// Run 21: every access violation, handled or not (the Vulkan backend's write
// watch on guest memory takes one per watched page the guest writes). Installed
// first; never claims the fault.
std::atomic<uint64_t> g_faults{0};
bool FaultCountHandler(rex::arch::Exception*, void*) {
  g_faults.fetch_add(1, std::memory_order_relaxed);
  return false;
}

// Runs only for access violations no runtime handler claimed (MMIO, physical
// write watch): installed before Runtime::Setup, it is installed again after
// it so that it comes after the MMIO handler (run 20 logged every handled
// write-watch fault here, 2282 in run 20's third boot, each with an fsync).
// Records the fault; returning false leaves it unhandled, and the runtime ends
// it in svcBreak (Atmosphere crash report).
bool CrashLogHandler(rex::arch::Exception* ex, void* /*data*/) {
  const auto* tc = ex->thread_context();
  CrashWrite("=== unhandled access violation ===\n");
  CrashHex("module base", g_module_base);
  CrashHex("pc         ", ex->pc());
  CrashHex("pc - base  ", ex->pc() - g_module_base);
  CrashHex("lr         ", tc->x[30]);
  CrashHex("lr - base  ", tc->x[30] - g_module_base);
  CrashHex("sp         ", tc->sp);
  CrashHex("far        ", ex->fault_address());
  CrashHex("write      ",
           ex->access_violation_operation() == rex::arch::Exception::AccessViolationOperation::kWrite);
  char name[] = "x00        ";
  for (int i = 0; i < 31; ++i) {
    name[1] = char('0' + i / 10);
    name[2] = char('0' + i % 10);
    CrashHex(name, tc->x[i]);
  }
  CrashWriteState("state:");
  // The lines still in the buffer (nx_log_durable = false), if nobody holds it.
  if (g_log_buffered && g_log_file_mutex.try_lock()) {
    FlushBufferLocked(true);
    g_log_file_mutex.unlock();
  }
  return false;
}

void CrashYesNo(const char* label, bool yes) {
  CrashWrite(label);
  CrashWrite(yes ? "yes\n" : "NO\n");
}

// Run 23 (D.2): installed last, after the MMIO handler and the crash log, so
// it runs only for a fault nobody handled. The thread (name, id, stack range,
// whether sp and x29 are in it), the last fault the entry resumed on this
// thread (the sp and pc it came in with and the ones it went back with), the
// process-wide resume counters, every other thread's pc, lr and sp; then
// fsync, before the dispatcher sends the thread to svcBreak.
bool CrashFinalHandler(rex::arch::Exception* ex, void* /*data*/) {
  const auto* tc = ex->thread_context();
  const RexNxThreadFaults* rec = rex_nx_thread_faults();
  u64 id = 0;
  svcGetThreadId(&id, CUR_THREAD_HANDLE);
  theft4::nx_threads::Info info;
  theft4::nx_threads::FindById(id, &info);
  CrashWrite("=== unhandled fault: thread, stack, last resume ===\n");
  CrashDec("thread id ", id);
  CrashWrite("thread name ");
  CrashWrite(!info.found ? "(not in the thread table)" : info.name[0] ? info.name : "?");
  CrashWrite("\n");
  if (info.start) {
    CrashHex("thread start - base", info.start - g_module_base);
  }
  uint64_t lo = info.stack_lo, hi = info.stack_hi;
  if (!info.found) {
    // The block of the last good sp, else of x29.
    theft4::nx_threads::QueryBlock(rec->handled ? rec->resume_sp : tc->x[29], &lo, &hi);
    CrashWrite("stack range from svcQueryMemory (thread not in the table)\n");
  }
  CrashHex("stack lo   ", lo);
  CrashHex("stack hi   ", hi);
  CrashHex("sp         ", tc->sp);
  CrashYesNo("sp inside the stack: ", tc->sp >= lo && tc->sp <= hi);
  CrashYesNo("x29 inside the stack: ", tc->x[29] >= lo && tc->x[29] <= hi);
  CrashDec("faults resumed on this thread ", rec->handled);
  if (rec->handled) {
    CrashDec("ms since the last one ", armTicksToNs(armGetSystemTick() - rec->last_tick) / 1'000'000);
    CrashHex("last fault: pc - base   ", rec->entry_pc - g_module_base);
    CrashHex("last fault: sp (known)  ", rec->entry_sp);
    CrashHex("last fault: lr          ", rec->entry_lr);
    CrashHex("resumed with pc - base  ", rec->resume_pc - g_module_base);
    CrashHex("resumed with sp         ", rec->resume_sp);
    CrashHex("kernel frame at         ", rec->frame_address);
    CrashYesNo("this fault's pc is the resumed pc: ", ex->pc() == rec->resume_pc);
    CrashYesNo("this fault's sp is the resumed sp: ", tc->sp == rec->resume_sp);
  }
  CrashDec("this thread: context changed in the handler ", rec->context_changed);
  CrashDec("this thread: frame read back wrong ", rec->frame_wrong);
  const RexNxResumeStats* s = rex_nx_resume_stats();
  CrashDec("process: faults resumed ", s->handled);
  CrashDec("process: context changed in the handler ", s->context_changed);
  CrashDec("process: frame read back wrong ", s->frame_wrong);
  CrashDec("process: repaired (nx_resume_guard) ", s->repaired);
  if (s->first_kind) {
    CrashDec("first event kind (1 context, 2 frame) ", s->first_kind);
    CrashDec("first event thread id ", s->first_thread_id);
    CrashHex("first event entry sp ", s->first_entry_sp);
    CrashHex("first event entry pc ", s->first_entry_pc);
    CrashHex("first event seen sp  ", s->first_seen_sp);
    CrashHex("first event seen pc  ", s->first_seen_pc);
  }
  CrashWrite("all other threads (paused one at a time, then resumed):\n");
  theft4::nx_threads::WriteAllThreads(CrashWrite, id);
  CrashWrite("=== end of the unhandled fault ===\n");
  if (g_crash_fd >= 0) fsync(g_crash_fd);
  return false;
}

// ── Terminations ───────────────────────────────────────────────────────────

// The exit mode chosen at + (0 until then) and its step count. Each step
// goes first to game-exit.log (nx_exit::Step: write + fsync on a descriptor
// opened at start-up), then to game.log.
constexpr const char* kExitStepPath = "sdmc:/switch/theft4/game-exit.log";
char g_exit_mode = 0;
int g_exit_step = 0;
u64 g_exit_start_tick = 0;  // when the mode was chosen

// Each step carries its time since the mode was chosen (run 19).
void ExitStep(const char* what) {
  if (!g_exit_mode) {
    return;
  }
  char timed[300];
  std::snprintf(timed, sizeof(timed), "+%" PRIu64 " ms: %s",
                armTicksToNs(armGetSystemTick() - g_exit_start_tick) / 1'000'000, what);
  const char* line = theft4::nx_exit::Step(g_exit_mode, ++g_exit_step, timed);
  char text[300];
  const int n = std::snprintf(text, sizeof(text), "[game] %s", line);
  DurableWriteBounded(text, size_t(n < int(sizeof(text)) ? n : int(sizeof(text)) - 1));
}

u64 CallerOffset(const void* return_address) {
  return reinterpret_cast<u64>(return_address) - g_module_base;
}

// One summary of the stub exports hit, at the stop or at the first exit path.
void LogStubSummaryOnce(const char* when) {
  static std::atomic<bool> done{false};
  if (!done.exchange(true)) {
    rex::diagnostics::LogExportStubSummary(when);
  }
}

// A termination line. Re-entry on the same thread (a wrapped call reached
// from inside the logging itself) is dropped rather than recursing.
thread_local bool t_in_termination_log = false;

void LogTermination(const char* what, int code, const void* caller) {
  ExitStep(what);
  {
    // D.3: before anything that can fail, a durable line in the crash log.
    char why[96];
    std::snprintf(why, sizeof(why), "=== termination: %s(%d) ===", what, code);
    CrashWriteState(why);
  }
  if (t_in_termination_log || !g_logging_ready) {
    return;
  }
  t_in_termination_log = true;
  GAME_ERROR("TERMINATION: %s(%d) called from LR 0x%" PRIx64 " = module base + 0x%" PRIx64
             " (ELF address)",
             what, code, reinterpret_cast<u64>(caller), CallerOffset(caller));
  LogStubSummaryOnce("at exit");
  rex::FlushLogging();
  t_in_termination_log = false;
}

}  // namespace

// Link-time wrappers (-Wl,--wrap=exit,--wrap=_exit,--wrap=_Exit,--wrap=abort,
// --wrap=svcExitProcess): every reference in the NRO, newlib and libnx
// included, reaches these first. Nothing of ours calls svcExitProcess any
// more (run 15): its wrapper only traces a call from libnx (fatal, or the
// applet's exit without a loader).
extern "C" {
[[noreturn]] void __real_exit(int code);
[[noreturn]] void __real__exit(int code);
[[noreturn]] void __real__Exit(int code);
[[noreturn]] void __real_abort();
[[noreturn]] void __real_svcExitProcess();

[[noreturn]] void __wrap_exit(int code) {
  LogTermination("exit", code, __builtin_return_address(0));
  __real_exit(code);
}
[[noreturn]] void __wrap__exit(int code) {
  LogTermination("_exit", code, __builtin_return_address(0));
  __real__exit(code);
}
[[noreturn]] void __wrap__Exit(int code) {
  LogTermination("_Exit", code, __builtin_return_address(0));
  __real__Exit(code);
}
[[noreturn]] void __wrap_abort() {
  LogTermination("abort", 0, __builtin_return_address(0));
  __real_abort();
}
[[noreturn]] void __wrap_svcExitProcess() {
  LogTermination("svcExitProcess", 0, __builtin_return_address(0));
  __real_svcExitProcess();
}

// Run 22 (D.2): libnx's own ways out (from any thread, possibly a fault or a
// heap failure): only the fault-safe crash-log writer (write + fsync on the
// descriptor opened at start), then the real function.
[[noreturn]] void __real_fatalThrow(Result err);
[[noreturn]] void __real_diagAbortWithResult(Result res);
Result __real_svcBreak(u32 reason, uintptr_t arg1, uintptr_t arg2);

namespace {
void CrashWriteLibnxExit(const char* what, uint64_t value, const void* caller) {
  CrashWrite("=== ");
  CrashWrite(what);
  CrashWrite(" ===\n");
  CrashHex("value      ", value);
  CrashHex("caller - base", reinterpret_cast<u64>(caller) - g_module_base);
  CrashWriteState("state:");
}
}  // namespace

[[noreturn]] void __wrap_fatalThrow(Result err) {
  CrashWriteLibnxExit("libnx fatalThrow (result)", err, __builtin_return_address(0));
  __real_fatalThrow(err);
}
[[noreturn]] void __wrap_diagAbortWithResult(Result res) {
  CrashWriteLibnxExit("libnx diagAbortWithResult (result)", res, __builtin_return_address(0));
  __real_diagAbortWithResult(res);
}
Result __wrap_svcBreak(u32 reason, uintptr_t arg1, uintptr_t arg2) {
  CrashWriteLibnxExit("svcBreak (reason)", reason, __builtin_return_address(0));
  return __real_svcBreak(reason, arg1, arg2);
}
}

namespace {

void OnTerminate() {
  std::string what = "no active exception";
  if (std::exception_ptr e = std::current_exception()) {
    try {
      std::rethrow_exception(e);
    } catch (const std::exception& x) {
      what = std::string("uncaught ") + typeid(x).name() + ": " + x.what();
    } catch (...) {
      what = "uncaught non-std exception";
    }
  }
  GAME_ERROR("TERMINATION: std::terminate (%s)", what.c_str());
  std::abort();  // wrapped: logs its caller too
}

void OnExit() {
  ExitStep("atexit handler (exit(): next the other atexit handlers, then _exit)");
  GAME_ERROR("atexit: the process is leaving through exit()");
  LogStubSummaryOnce("at exit");
  rex::FlushLogging();
}

// stderr -> screen (the console's own device) and game.log, one line at a
// time with an "[stderr] " prefix.
const devoptab_t* g_console_stderr = nullptr;
devoptab_t g_stderr_device{};
std::mutex g_stderr_mutex;
std::string g_stderr_pending;

ssize_t StderrWrite(struct _reent* r, void* fd, const char* ptr, size_t len) {
  // Screen only while the console is open and free right now (this path also
  // runs from the fault handler, which must not wait for it).
  if (g_console_stderr && g_console_stderr->write_r && g_console_mutex.try_lock()) {
    if (g_console_open) {
      g_console_stderr->write_r(r, fd, ptr, len);
      g_console_dirty.store(true, std::memory_order_relaxed);
    }
    g_console_mutex.unlock();
  }
  std::unique_lock<std::mutex> lock(g_stderr_mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    return ssize_t(len);  // re-entered from a fault on this path: screen only
  }
  g_stderr_pending.append(ptr, len);
  size_t start = 0;
  for (size_t nl; (nl = g_stderr_pending.find('\n', start)) != std::string::npos;
       start = nl + 1) {
    const std::string line = "[stderr] " + g_stderr_pending.substr(start, nl - start + 1);
    DurableWriteBounded(line.data(), line.size());
  }
  g_stderr_pending.erase(0, start);
  return ssize_t(len);
}

void CaptureTerminations() {
  g_console_stderr = devoptab_list[STD_ERR];
  g_stderr_device.name = "stderr";
  g_stderr_device.write_r = StderrWrite;
  devoptab_list[STD_ERR] = &g_stderr_device;
  setvbuf(stderr, nullptr, _IONBF, 0);
  std::set_terminate(OnTerminate);
  std::atexit(OnExit);
  GAME_INFO("terminations traced: exit/_exit/_Exit/abort/svcExitProcess wrapped, "
            "std::terminate and atexit handlers, stderr copied to game.log");
}

// ── Steps ──────────────────────────────────────────────────────────────────

void StartLogging() {
  mkdir("sdmc:/switch", 0777);
  mkdir(kRootDir, 0777);
  // One run per game.log; the previous run is kept as game.prev.log.
  std::remove(kPreviousLogPath);
  std::rename(kLogPath, kPreviousLogPath);

  std::string policy_error;
  const bool policy_ok = rex::diagnostics::Configure(true, "logging", &policy_error);
  auto sink = std::make_shared<DurableSink>();
  sink->set_pattern("[sdk] [%l] [%n] [t%t] %v");
  rex::LogConfig config;
  config.default_level = spdlog::level::debug;
  config.extra_sinks.push_back(sink);
  g_logging_ready = true;
  rex::InitLogging(config);
  GAME_INFO("switch-game: log %s, crash log %s", kLogPath, kCrashPath);
  if (!policy_ok) {
    GAME_ERROR("diagnostics::Configure(logging): %s", policy_error.c_str());
  }
  // Run 19: the Vulkan run had no game.toml on the SD (the notes of run 18
  // named it switch-game.toml), so nx_gpu stayed null. Both names are read,
  // game.toml first, and each one found or missing is a [game] line.
  std::string loaded;
  for (const char* path : {kConfigPath, kAltConfigPath}) {
    struct stat st {};
    const bool found = stat(path, &st) == 0;
    GAME_INFO("config %s: %s", path, found ? "found, loaded" : "not found");
    if (found) {
      rex::cvar::LoadConfig(path);
      loaded += loaded.empty() ? "" : " + ";
      loaded += std::strrchr(path, '/') + 1;
    }
  }
  rex::cvar::FinalizeInit();
  GAME_INFO("cvars from argv, then %s and %s; nx_gpu = %s", kConfigPath, kAltConfigPath,
            REXCVAR_GET(nx_gpu).c_str());
  // Run 24 (C): which NRO and which configuration are running, in one line:
  // the build's commit, the file(s) read, every Switch cvar, and every cvar
  // of any category that is not at its default.
  // Report() cuts a line at 768 bytes: the lists go out in pieces.
  {
#if THEFT4_NX_VULKAN
    constexpr const char* kNroKind = "Vulkan";
#else
    constexpr const char* kNroKind = "null GPU only";
#endif
    GAME_INFO("build: commit %s, %s NRO; config read: %s", THEFT4_BUILD_COMMIT, kNroKind,
              loaded.empty() ? "none (all defaults)" : loaded.c_str());
    const auto log_list = [](const char* title, const std::vector<std::string>& names) {
      std::string line;
      int part = 1;
      for (const std::string& name : names) {
        line += " " + name + "=" + rex::cvar::GetFlagByName(name);
        if (line.size() > 560) {
          GAME_INFO("%s (%d):%s", title, part++, line.c_str());
          line.clear();
        }
      }
      if (!line.empty() || part == 1) {
        GAME_INFO("%s (%d):%s", title, part, line.empty() ? " none" : line.c_str());
      }
    };
    log_list("build: Switch cvars", rex::cvar::ListFlagsByCategory("Switch"));
    log_list("build: cvars not at the default", rex::cvar::ListModifiedFlags());
  }
  StartLogWriter();
  GAME_INFO("log: %s", g_log_buffered
                           ? "buffered, written once a second by its own thread and at once on "
                             "exit steps, terminations, the stop and faults (nx_log_durable = false)"
                       : REXCVAR_GET(nx_log_durable)
                           ? "durable, every line written at once (nx_log_durable = true)"
                           : "durable: the writer thread could not start");

  g_crash_fd = open(kCrashPath, O_WRONLY | O_APPEND | O_CREAT, 0666);
  if (g_crash_fd < 0) {
    GAME_ERROR("cannot open %s (errno %d): unhandled faults only in Atmosphere's crash report",
               kCrashPath, errno);
  }

  g_module_base = reinterpret_cast<u64>(&_start);
  {
    MemoryInfo mi{};
    u32 pi = 0;
    if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, g_module_base)) && mi.addr == g_module_base) {
      g_module_text_size = mi.size;
    }
  }
  GAME_INFO("module base (_start) 0x%" PRIx64 ", code block 0x%" PRIx64
            " bytes: ELF address = runtime address - base",
            g_module_base, g_module_text_size);
  u64 total = 0, used = 0;
  svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
  GAME_INFO("applet type %d, memory total %" PRIu64 " MiB, used %" PRIu64 " MiB",
            int(appletGetAppletType()), total >> 20, used >> 20);
}

bool CheckGameFiles() {
  std::error_code ec;
  const std::filesystem::path game(kGameDir);
  const bool xex = std::filesystem::is_regular_file(game / "default.xex", ec);
  const bool xexp = std::filesystem::exists(game / "default.xexp", ec);
  const auto size = xex ? std::filesystem::file_size(game / "default.xex", ec) : 0;
  GAME_INFO("game files in %s: default.xex %s (%" PRIu64 " bytes), default.xexp %s", kGameDir,
            xex ? "present" : "MISSING", uint64_t(size), xexp ? "PRESENT" : "absent");
  if (!xex) {
    GAME_ERROR("copy the European default.xex (and the game data) to %s", kGameDir);
    return false;
  }
  if (xexp) {
    // The code was generated from the base executable; the loader would apply
    // the patch and run code that no longer matches (06 section 2).
    GAME_ERROR("remove %s/default.xexp: this build is generated from the base executable",
               kGameDir);
    return false;
  }
  return true;
}

void LogArena(const char* when) {
  const auto s = rex::memory::nx::GetGuestArenaStats();
  u64 used = 0;
  svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
  GAME_INFO("guest memory %s: %zu blocks mapped (%zu MiB, peak %zu = %zu MiB), %zu pages "
            "committed; pool %zu blocks (%zu MiB, cvar nx_guest_pool_mib), %zu free",
            when, s.mapped_blocks, s.mapped_blocks * 2, s.peak_mapped_blocks,
            s.peak_mapped_blocks * 2, s.committed_pages, s.pool_blocks, s.pool_blocks * 2,
            s.pool_free_blocks);
}

// ── Where every thread is ──────────────────────────────────────────────────

// A return address found on a stack: inside this module, right after a bl
// or blr (the check crash_match.py makes).
bool IsReturnAddress(u64 value) {
  if (value < g_module_base + 4 || value >= g_module_base + g_module_text_size || (value & 3)) {
    return false;
  }
  const u32 insn = *reinterpret_cast<const u32*>(value - 4);
  return (insn & 0xFC000000u) == 0x94000000u ||  // bl
         (insn & 0xFFFFFC1Fu) == 0xD63F0000u;    // blr
}

constexpr int kStackReturns = 12;
constexpr size_t kStackScanWords = 4096;  // 32 KiB above sp

struct ThreadRow {
  // Filled before any thread is paused (these take locks and allocate).
  std::string name;
  uint32_t handle = 0;
  uint32_t thread_id = 0;
  bool guest = false;
  bool running = false;
  uint32_t nx_handle = 0;
  const PPCContext* guest_ctx = nullptr;
  uint32_t stack_end = 0, stack_base = 0;  // guest stack from the PCR (GuestStack)
  rex::diagnostics::ExportThreadSnapshot exports{};
  bool has_exports = false;
  // Filled while the thread is paused: no locks, no allocation.
  Result pause_rc = 0;
  Result context_rc = 0;
  u64 pc = 0, lr = 0, sp = 0;
  u64 returns[kStackReturns] = {};
  int n_returns = 0;
  u32 guest_lr = 0;
  u32 guest_r1 = 0;
};

// Guest memory read big-endian (the addresses below are mapped: stacks and
// the game's data section).
uint32_t GuestU32(uint32_t address) {
  const uint8_t* p =
      rex::system::kernel_state()->memory()->TranslateVirtual<const uint8_t*>(address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

// The guest call chain from r1 through the PPC back chain: 0(r1) holds the
// caller's r1, and a non-leaf prologue stores LR at -8 of the caller's r1
// ("mflr r12; stw r12,-8(r1); stwu r1,-N(r1)"). Only for a thread inside an
// export, whose guest registers do not move. A function that saves LR through
// __savegprlr leaves that slot to the generated code, so the frame above it
// can show a stale return address (run 16 analysis, 06 section 19). Every
// address read lies inside the thread's own guest stack, [stack_end,
// stack_base) from its PCR (PC run 17 walked off a stack without it).
std::string GuestStack(const PPCContext& ctx, uint32_t stack_end, uint32_t stack_base) {
  char item[16];
  std::snprintf(item, sizeof(item), "%08X", uint32_t(ctx.lr));
  std::string out = item;
  uint32_t sp = ctx.r1.u32;
  if (stack_end >= stack_base || sp < stack_end || sp + 4 > stack_base) {
    return out;
  }
  for (int i = 0; i < 24; ++i) {
    const uint32_t prev = GuestU32(sp);
    if (prev <= sp || prev + 4 > stack_base || prev - 8 < stack_end || (prev & 3)) break;
    const uint32_t lr = GuestU32(prev - 8);
    if (lr < 0x82000000u || lr >= 0x83000000u) break;
    std::snprintf(item, sizeof(item), " %08X", lr);
    out += item;
    sp = prev;
  }
  return out;
}

// Runs 15 and 16 (PC too): the main thread waits in sub_821200D0, "while
// (sub_82124490()) Sleep(1)", i.e. while the bytes at 83137BB7 and 83137BC9
// are both set. The boot screen sequencer sub_821238D0 clears 83137BC9; its
// entry 0 (table at 83137D18, 400 bytes each, index at 83137BC0) lasts 6000
// ms and has type 1: it moves on only when the boot sound it starts
// (sub_822FAD50 on the object at 82D09E00) reaches state 2, or when the word
// at 82B11188 is not 0. Its time accumulates at 83137BB8 unless 83137BC8 is
// set. Addresses of the European base executable (config-eu-base).
void LogBootSequencer() {
  auto u8 = [](uint32_t a) {
    return *rex::system::kernel_state()->memory()->TranslateVirtual<const uint8_t*>(a);
  };
  auto f32 = [](uint32_t a) {
    const uint32_t v = GuestU32(a);
    float f;
    std::memcpy(&f, &v, 4);
    return double(f);
  };
  const uint32_t index = GuestU32(0x83137BC0);
  const uint32_t entry = 0x83137D18 + (index < 64 ? index : 0) * 400;
  const uint32_t sound_object = 0x82D09E00;
  const uint32_t sound = GuestU32(sound_object + 16);
  GAME_INFO("boot sequencer: active %u, main waits on %u, paused %u, index %u, time %.3f s; "
            "entry %08X: duration %d ms, type %u; skip word 82B11188 = %08X; boot sound object: "
            "id %04X, sound %08X, state %d",
            u8(0x83137BB7), u8(0x83137BC9), u8(0x83137BC8), index, f32(0x83137BB8), entry,
            int32_t(GuestU32(entry)), GuestU32(entry + 8), GuestU32(0x82B11188),
            GuestU32(sound_object + 4) >> 16, sound,
            sound >= 0x40000000u && sound < 0xA0000000u ? int(GuestU32(sound + 4) & 0xFFFF)
                                                        : -1);
}

// One line per XThread. Each thread is paused (svcSetThreadActivity) only
// long enough to read its registers and scan its stack; a thread the game
// had already suspended is read but not resumed by us.
void LogThreadSnapshot(const char* when) {
  auto* kernel = rex::system::kernel_state();
  if (!kernel || g_module_text_size == 0) {
    return;
  }
  // The object table is guarded by the kernel's global lock, which a guest
  // thread may hold for good (raised IRQL, spin lock): this is the UI
  // thread, so it only tries. Without the XThreads, the rows come from the
  // export traces, which carry each thread's Horizon handle too.
  std::vector<rex::system::object_ref<rex::system::XThread>> threads;
  const bool have_xthreads = kernel->object_table()->TryGetObjectsByType<rex::system::XThread>(
      &threads, std::chrono::milliseconds(200));
  rex::diagnostics::ExportThreadSnapshot traces[64];
  const size_t n_traces = rex::diagnostics::SnapshotExportThreads(traces, 64);

  std::vector<ThreadRow> rows(have_xthreads ? threads.size() : n_traces);
  if (!have_xthreads) {
    for (size_t k = 0; k < n_traces; ++k) {
      rows[k].name = "? (kernel lock busy)";
      rows[k].thread_id = uint32_t(traces[k].nx_thread_id);
      rows[k].nx_handle = traces[k].nx_handle;
      rows[k].exports = traces[k];
      rows[k].has_exports = true;
    }
  }
  for (size_t i = 0; i < threads.size(); ++i) {
    auto& t = threads[i];
    ThreadRow& row = rows[i];
    row.name = t->thread_name();
    row.handle = t->handle();
    row.thread_id = t->thread_id();
    row.guest = t->is_guest_thread();
    row.running = t->is_running();
    if (auto* host = t->thread()) {
      row.nx_handle = host->nx_kernel_handle();
    }
    if (auto* state = t->thread_state()) {
      row.guest_ctx = state->context();
    }
    if (t->pcr_ptr()) {
      auto* pcr = kernel->memory()->TranslateVirtual<rex::system::X_KPCR*>(t->pcr_ptr());
      row.stack_end = pcr->stack_end_ptr;
      row.stack_base = pcr->stack_base_ptr;
    }
    for (size_t k = 0; k < n_traces; ++k) {
      if (row.nx_handle && traces[k].nx_handle == row.nx_handle) {
        row.exports = traces[k];
        row.has_exports = true;
      }
    }
  }

  u64 self_id = 0;
  svcGetThreadId(&self_id, CUR_THREAD_HANDLE);
  for (ThreadRow& row : rows) {
    if (!row.nx_handle) {
      continue;
    }
    u64 id = 0;
    if (R_SUCCEEDED(svcGetThreadId(&id, row.nx_handle)) && id == self_id) {
      continue;
    }
    row.pause_rc = svcSetThreadActivity(row.nx_handle, ThreadActivity_Paused);
    ThreadContext ctx{};
    row.context_rc = svcGetThreadContext3(&ctx, row.nx_handle);
    if (R_SUCCEEDED(row.context_rc)) {
      row.pc = ctx.pc.x;
      row.lr = ctx.lr;
      row.sp = ctx.sp;
      MemoryInfo mi{};
      u32 pi = 0;
      if (row.sp && R_SUCCEEDED(svcQueryMemory(&mi, &pi, row.sp)) && (mi.perm & Perm_R)) {
        const u64 end = std::min<u64>(mi.addr + mi.size, row.sp + kStackScanWords * 8);
        for (u64 a = row.sp & ~u64(7); a + 8 <= end && row.n_returns < kStackReturns; a += 8) {
          const u64 value = *reinterpret_cast<const u64*>(a);
          if (IsReturnAddress(value)) {
            row.returns[row.n_returns++] = value;
          }
        }
      }
    }
    if (row.guest_ctx) {
      row.guest_lr = u32(row.guest_ctx->lr);
      row.guest_r1 = row.guest_ctx->r1.u32;
    }
    if (R_SUCCEEDED(row.pause_rc)) {
      svcSetThreadActivity(row.nx_handle, ThreadActivity_Runnable);
    }
  }

  if (have_xthreads) {
    GAME_INFO("threads %s: %zu XThreads (addr2line: tools/switch-game/where.py game.log)", when,
              rows.size());
  } else {
    GAME_INFO("threads %s: the kernel's global lock stayed busy for 200 ms (a guest thread at "
              "raised IRQL or in a spin lock?); %zu threads from the export traces instead",
              when, rows.size());
  }
  for (const ThreadRow& row : rows) {
    char exports[224] = "no export call yet";
    if (row.has_exports) {
      if (row.exports.current) {
        std::snprintf(exports, sizeof(exports),
                      "IN %s for %.1f s (guest LR %08X), %" PRIu64 " export calls",
                      row.exports.current, row.exports.seconds_in_current, row.exports.guest_lr,
                      row.exports.calls);
      } else {
        std::snprintf(exports, sizeof(exports),
                      "in guest code, last export %s (guest LR %08X), %" PRIu64 " export calls",
                      row.exports.last ? row.exports.last : "-", row.exports.guest_lr,
                      row.exports.calls);
      }
    }
    std::string stack;
    if (row.guest_ctx && row.has_exports && row.exports.current) {
      stack = " guest stack " + GuestStack(*row.guest_ctx, row.stack_end, row.stack_base) +
              "; host";
    }
    for (int k = 0; k < row.n_returns; ++k) {
      char item[24];
      std::snprintf(item, sizeof(item), " +0x%" PRIx64, row.returns[k] - g_module_base);
      stack += item;
    }
    if (R_SUCCEEDED(row.context_rc) && row.pc) {
      GAME_INFO("  thread %08X id %u '%s' %s%s: %s; guest lr %08X r1 %08X; host pc +0x%" PRIx64
                " lr +0x%" PRIx64 " stack%s%s",
                row.handle, row.thread_id, row.name.c_str(), row.guest ? "guest" : "host",
                row.running ? ", running" : ", not running", exports, row.guest_lr,
                row.guest_r1, row.pc - g_module_base, row.lr - g_module_base, stack.c_str(),
                R_SUCCEEDED(row.pause_rc) ? "" : " (already paused by the game)");
    } else {
      GAME_INFO("  thread %08X id %u '%s' %s%s: %s; guest lr %08X r1 %08X; host context not "
                "read (handle %08X, pause rc 0x%X, context rc 0x%X)",
                row.handle, row.thread_id, row.name.c_str(), row.guest ? "guest" : "host",
                row.running ? ", running" : ", not running", exports, row.guest_lr,
                row.guest_r1, row.nx_handle, row.pause_rc, row.context_rc);
    }
  }
}

// Run 12 spun in `while (mftb == 0)`: both clocks, so the log shows them
// moving from one heartbeat to the next.
void LogClock() {
  using rex::chrono::Clock;
  GAME_INFO("clock: host tick %" PRIu64 " (%" PRIu64 " Hz), guest time base %" PRIu64
            " (%" PRIu64 " Hz)",
            Clock::QueryHostTickCount(), Clock::QueryHostTickFrequency(),
            Clock::QueryGuestTickCount(), Clock::guest_tick_frequency());
}

void LogNullGpu() {
  if (!REXCVAR_GET(nx_null_gpu)) {
    return;
  }
  const auto g = rex::system::GetNullGpuStats();
  GAME_INFO("null GPU: %" PRIu64 " WPTR writes, ring read %08X / write %08X, %" PRIu64
            " packets (%" PRIu64 " draws skipped, %" PRIu64 " unknown), %" PRIu64
            " swaps, %" PRIu64 " vblanks, %" PRIu64 " PM4 interrupts%s",
            g.wptr_writes, g.read_index, g.write_index, g.packets, g.draws, g.unknown_opcodes,
            g.swaps, g.vblanks, g.interrupts,
            g.waiting_opcode ? "; IN WAIT_REG_MEM for more than 1 s" : "");
}

// Every change of the buttons held (Switch HidNpadButton mask), with its time
// since the start: the log says what was pressed and when (run 15 could not).
u64 g_buttons_now = 0;
u64 g_button_changes = 0;

void LogButtonChange(u64 buttons) {
  if (buttons == g_buttons_now) {
    return;
  }
  ++g_button_changes;
  const double seconds = double(armTicksToNs(armGetSystemTick() - g_start_tick)) / 1e9;
  GAME_INFO("buttons 0x%" PRIx64 " at %.3f s (was 0x%" PRIx64 ")", buttons, seconds,
            g_buttons_now);
  g_buttons_now = buttons;
}

// ── CPU time per thread, threads, memory, Vulkan (run 19) ──────────────────

// CPU time of a thread of this process from the kernel (svcGetInfo
// ThreadTickCount, all cores), in system ticks; 0 if refused.
u64 ThreadCpuTicks(Handle handle) {
  u64 ticks = 0;
  const u32 type = hosversionAtLeast(13, 0, 0) ? InfoType_ThreadTickCount
                                               : InfoType_ThreadTickCountDeprecated;
  if (R_FAILED(svcGetInfo(&ticks, type, handle, UINT64_MAX))) {
    return 0;
  }
  return ticks;
}

struct CpuRow {
  Handle handle = 0;
  uint32_t id = 0;  // XThread id; 0 = not an XThread
  const char* kind = "";
  std::string name;
  u64 ticks = 0;
  double percent = 0;  // of one core over the window
};

// CPU of the GPU Commands thread (ReXGlue's command processor) over the last
// window, in ns: the Vulkan line divides it by the draws (B.1).
uint64_t g_cp_window_ns = 0;

Handle g_main_thread_handle = 0;
Handle g_resource_limit = 0;

// One line: every XThread's CPU over the window (100% = one core), the
// guest and host sums, this UI thread and the log writer, the busiest ten.
void LogCpu(std::vector<std::pair<Handle, u64>>* last, u64 window_ticks) {
  std::vector<CpuRow> rows;
  auto* kernel = rex::system::kernel_state();
  std::vector<rex::system::object_ref<rex::system::XThread>> threads;
  if (kernel) {
    kernel->object_table()->TryGetObjectsByType<rex::system::XThread>(
        &threads, std::chrono::milliseconds(200));
  }
  for (auto& t : threads) {
    if (auto* host = t->thread()) {
      CpuRow r;
      r.handle = host->nx_kernel_handle();
      r.id = t->thread_id();
      r.kind = t->is_guest_thread() ? "guest" : "host";
      r.name = t->thread_name();
      theft4::nx_threads::SetName(r.handle, r.name.c_str());  // run 23: stacks, crash log
      rows.push_back(r);
    }
  }
  CpuRow ui;
  ui.handle = g_main_thread_handle;
  ui.kind = "ui";
  rows.push_back(ui);
  if (g_log_writer_ready) {
    CpuRow w;
    w.handle = g_log_writer.handle;
    w.kind = "log";
    theft4::nx_threads::SetName(w.handle, "log writer");
    rows.push_back(w);
  }
  std::vector<std::pair<Handle, u64>> now;
  double guest = 0, host = 0, other = 0;
  for (CpuRow& r : rows) {
    r.ticks = ThreadCpuTicks(r.handle);
    now.emplace_back(r.handle, r.ticks);
    u64 before = 0;
    for (const auto& [h, t] : *last) {
      if (h == r.handle) before = t;
    }
    const u64 delta = r.ticks > before ? r.ticks - before : 0;
    r.percent = window_ticks ? 100.0 * double(delta) / double(window_ticks) : 0;
    (r.kind[0] == 'g' ? guest : r.kind[0] == 'h' ? host : other) += r.percent;
    if (r.name.rfind("GPU Commands", 0) == 0) {
      g_cp_window_ns = armTicksToNs(delta);
    }
  }
  last->swap(now);
  if (theft4::nx_profiler::Running()) {
    // Run 21 (B.3): the threads above 3 % of a core are the ones sampled.
    std::vector<theft4::nx_profiler::BusyThread> busy;
    for (const CpuRow& r : rows) {
      if (r.percent < 3.0 || r.kind[0] == 'l') continue;
      theft4::nx_profiler::BusyThread b;
      b.handle = r.handle;
      b.id = r.id;
      b.percent = r.percent;
      std::snprintf(b.kind, sizeof(b.kind), "%s", r.kind);
      std::snprintf(b.name, sizeof(b.name), "%s", r.name.empty() ? r.kind : r.name.c_str());
      busy.push_back(b);
    }
    theft4::nx_profiler::SetBusyThreads(busy.data(), busy.size());
  }
  std::sort(rows.begin(), rows.end(),
            [](const CpuRow& a, const CpuRow& b) { return a.percent > b.percent; });
  std::string top;
  for (size_t i = 0; i < rows.size() && i < 10; ++i) {
    char item[48];
    if (rows[i].id) {
      std::snprintf(item, sizeof(item), " id %u %s %.1f%%;", rows[i].id, rows[i].kind,
                    rows[i].percent);
    } else {
      std::snprintf(item, sizeof(item), " %s %.1f%%;", rows[i].kind, rows[i].percent);
    }
    top += item;
  }
  GAME_INFO("cpu (100%% = one core): guest threads %.1f%%, host XThreads %.1f%%, ui + log "
            "%.1f%%; busiest:%s",
            guest, host, other, top.c_str());
}

// Horizon's thread count against the process limit, and the host threads of
// the runtime (created, finished, joined: joined ones gave their handle back).
void LogThreadCounts() {
  s64 current = -1, limit = -1;
  if (g_resource_limit) {
    svcGetResourceLimitCurrentValue(&current, g_resource_limit, LimitableResource_Threads);
    svcGetResourceLimitLimitValue(&limit, g_resource_limit, LimitableResource_Threads);
  }
  const auto c = rex::thread::GetNxThreadCounters();
  GAME_INFO("threads: Horizon %" PRId64 " of %" PRId64 " (resource limit); runtime host threads "
            "created %" PRIu64 ", finished %" PRIu64 ", joined %" PRIu64 ", abandoned %" PRIu64
            " (%" PRIu64 " still hold a handle)",
            current, limit, c.created, c.finished, c.joined, c.abandoned, c.created - c.joined);
}

// Process memory and the host heap (newlib), with their peaks; the guest
// pool's peak is on the heartbeat line.
u64 g_peak_used = 0, g_peak_heap = 0;
std::string g_beat_memory, g_beat_vulkan;  // for the crash log (SetLastBeat)
bool g_memory_low = false;

// Run 21 (D.1): the process's memory from the kernel says little (libnx takes
// the whole heap at start: 3185 of 3189 MiB in every run). What can still be
// allocated is newlib's: the free blocks of its arena plus what sbrk can add
// up to fake_heap_end. Vulkan memory and the guest pool come from there too.
void LogHostMemory() {
  u64 used = 0, total = 0;
  svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
  const struct mallinfo mi = mallinfo();
  g_peak_used = std::max<u64>(g_peak_used, used);
  g_peak_heap = std::max<u64>(g_peak_heap, u64(mi.uordblks));
  const char* top = static_cast<const char*>(sbrk(0));
  const uint64_t sbrk_left =
      fake_heap_end && top && fake_heap_end > top ? uint64_t(fake_heap_end - top) : 0;
  const uint64_t headroom = sbrk_left + uint64_t(mi.fordblks);
  g_heap_headroom.store(headroom, std::memory_order_relaxed);
  char line[320];
  std::snprintf(line, sizeof(line),
                "host memory: process %" PRIu64 " of %" PRIu64 " MiB (peak %" PRIu64
                "), heap arena %zu MiB, in use %zu MiB (peak %" PRIu64
                "); free: %zu MiB in the arena + %" PRIu64 " MiB sbrk can add = %" PRIu64
                " MiB headroom",
                used >> 20, total >> 20, g_peak_used >> 20, size_t(mi.arena) >> 20,
                size_t(mi.uordblks) >> 20, g_peak_heap >> 20, size_t(mi.fordblks) >> 20,
                sbrk_left >> 20, headroom >> 20);
  GAME_INFO("%s", line);
  g_beat_memory = line;
  const uint64_t warn = uint64_t(std::max(0, REXCVAR_GET(nx_mem_warn_mib))) << 20;
  if (headroom < warn && !g_memory_low) {
    g_memory_low = true;
    GAME_ERROR("memory LOW: %" PRIu64 " MiB of host heap left (under nx_mem_warn_mib = %d)",
               headroom >> 20, REXCVAR_GET(nx_mem_warn_mib));
    FlushLog();  // at once, not at the next writer pass
  } else if (headroom >= warn + (16u << 20)) {
    g_memory_low = false;
  }
}

// Log cost over the window: lines, writes to game.log and their time.
void LogLogCost(uint64_t* last_lines, uint64_t* last_writes, uint64_t* last_ns) {
  const uint64_t lines = g_log_lines.load(std::memory_order_relaxed);
  const uint64_t writes = g_log_file_writes.load(std::memory_order_relaxed);
  const uint64_t ns = g_log_file_ns.load(std::memory_order_relaxed);
  const uint64_t n = writes - *last_writes;
  GAME_INFO("log: %" PRIu64 " lines, %" PRIu64 " writes to game.log (%.1f ms total, mean %.3f ms, "
            "max %.3f ms) in the window; %s",
            lines - *last_lines, n, double(ns - *last_ns) / 1e6,
            n ? double(ns - *last_ns) / double(n) / 1e6 : 0.0,
            double(g_log_file_ns_max.exchange(0, std::memory_order_relaxed)) / 1e6,
            g_log_buffered ? "buffered" : "durable");
  *last_lines = lines;
  *last_writes = writes;
  *last_ns = ns;
}

void LogThreadSnapshot(const char* when);

// KeDelayExecutionThread by interval since start (run 20, B.3): a relative
// interval under 1 ms becomes 0 ms in XThread::Delay, i.e. a yield, so the
// game's short sleeps may spin.
void LogDelayCounts() {
  const auto d = rex::system::GetDelayCounters();
  GAME_INFO("KeDelayExecutionThread since start: %" PRIu64 " zero, %" PRIu64
            " under 1 ms (became 0), %" PRIu64 " 1-9 ms, %" PRIu64 " 10 ms or more, %" PRIu64
            " absolute; %" PRIu64 " alertable",
            d.zero, d.sub_millisecond, d.short_ms, d.long_ms, d.absolute, d.alertable);
}

#if THEFT4_NX_VULKAN
// nx_gpu = vulkan: presents, draws and submits of the window, the failures,
// the Vulkan heaps (VK_EXT_memory_budget); after 20 s without a present, one
// snapshot of the threads per stall (CheckVulkanStall).
struct VulkanBeat {
  uint64_t presents = 0, draws = 0, submits = 0;
  uint64_t submit_ns = 0, present_ns = 0, fence_waits = 0, fence_wait_ns = 0;
  uint64_t nv_kickoffs = 0, nv_waits = 0, nv_wait_ns = 0, nv_queries = 0;
  uint64_t submit_failures = 0, present_failures = 0;
  uint64_t pipelines = 0, pipeline_ns = 0, pipelines_cp = 0, pipeline_ns_cp = 0;
  uint64_t shaders = 0, shader_ns = 0, shaders_cp = 0, shader_ns_cp = 0;
  uint64_t stall_presents = UINT64_MAX;
  u64 last_present_change = 0;
  bool stall_reported = false;
};
VulkanBeat g_vk_beat;

// libnx's nvdrv calls made by NVK, counted through link-time wrappers
// (-Wl,--wrap in CMakeLists.txt, Vulkan build only; as sa2-nx's switch_perf).
struct NvCalls {
  std::atomic<uint64_t> kickoffs{0};
  std::atomic<uint64_t> fence_waits{0}, fence_wait_ns{0};  // nvFenceWait with a timeout
  std::atomic<uint64_t> fence_queries{0};                  // nvFenceWait(…, 0)
  std::atomic<uint64_t> maps_created{0}, maps_closed{0};
  std::atomic<int64_t> map_bytes{0};                       // live nvmap bytes
};
NvCalls g_nv;
std::atomic<bool> g_nv_count{true};  // cvar nx_nv_wrap: false = pure passthrough

// VkResults that mean memory or the device: a crash-log entry (D.2).
const char* VkFailureName(int32_t result) {
  switch (result) {
    case -1: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case -2: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case -4: return "VK_ERROR_DEVICE_LOST";
    default: return nullptr;
  }
}

}  // namespace

#if THEFT4_NX_VULKAN
// nvdrv calls of NVK (see NvCalls): count, time inside, nvmap bytes.
extern "C" {
Result __real_nvFenceWait(NvFence* f, s32 timeout_us);
Result __real_nvGpuChannelKickoff(NvGpuChannel* c);
Result __real_nvMapCreate(NvMap* m, void* cpu_addr, u32 size, u32 align, NvKind kind,
                          bool is_cpu_cacheable);
void __real_nvMapClose(NvMap* m);

Result __wrap_nvFenceWait(NvFence* f, s32 timeout_us) {
  if (!g_nv_count.load(std::memory_order_relaxed)) {
    return __real_nvFenceWait(f, timeout_us);
  }
  if (timeout_us == 0) {
    g_nv.fence_queries.fetch_add(1, std::memory_order_relaxed);
    return __real_nvFenceWait(f, timeout_us);
  }
  const u64 start = armGetSystemTick();
  const Result rc = __real_nvFenceWait(f, timeout_us);
  g_nv.fence_wait_ns.fetch_add(armTicksToNs(armGetSystemTick() - start),
                               std::memory_order_relaxed);
  g_nv.fence_waits.fetch_add(1, std::memory_order_relaxed);
  return rc;
}
Result __wrap_nvGpuChannelKickoff(NvGpuChannel* c) {
  if (!g_nv_count.load(std::memory_order_relaxed)) {
    return __real_nvGpuChannelKickoff(c);
  }
  g_nv.kickoffs.fetch_add(1, std::memory_order_relaxed);
  return __real_nvGpuChannelKickoff(c);
}
Result __wrap_nvMapCreate(NvMap* m, void* cpu_addr, u32 size, u32 align, NvKind kind,
                          bool is_cpu_cacheable) {
  const Result rc = __real_nvMapCreate(m, cpu_addr, size, align, kind, is_cpu_cacheable);
  if (R_SUCCEEDED(rc) && g_nv_count.load(std::memory_order_relaxed)) {
    g_nv.maps_created.fetch_add(1, std::memory_order_relaxed);
    g_nv.map_bytes.fetch_add(int64_t(size), std::memory_order_relaxed);
  }
  return rc;
}
void __wrap_nvMapClose(NvMap* m) {
  if (m && m->has_init && g_nv_count.load(std::memory_order_relaxed)) {
    g_nv.maps_closed.fetch_add(1, std::memory_order_relaxed);
    g_nv.map_bytes.fetch_sub(int64_t(m->size), std::memory_order_relaxed);
  }
  __real_nvMapClose(m);
}
}
#endif

namespace {

void LogVulkan(double window_s) {
  if (!g_vulkan_graphics) {
    return;
  }
  auto& c = rex::graphics::nx_gpu_counters();
  const uint64_t presents = c.presents.load(std::memory_order_relaxed);
  const uint64_t draws = c.draws.load(std::memory_order_relaxed);
  const uint64_t submits = c.submits.load(std::memory_order_relaxed);
  const uint64_t new_presents = presents - g_vk_beat.presents;
  std::string heaps = "no VK_EXT_memory_budget";
  auto* provider = static_cast<rex::ui::vulkan::VulkanProvider*>(g_vulkan_graphics->provider());
  if (const auto* device = provider ? provider->vulkan_device() : nullptr) {
    const auto& ifn = device->vulkan_instance()->functions();
    if (device->extensions().ext_EXT_memory_budget && ifn.vkGetPhysicalDeviceMemoryProperties2) {
      VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
      budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
      VkPhysicalDeviceMemoryProperties2 properties{};
      properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
      properties.pNext = &budget;
      ifn.vkGetPhysicalDeviceMemoryProperties2(device->physical_device(), &properties);
      heaps.clear();
      for (uint32_t h = 0; h < properties.memoryProperties.memoryHeapCount; ++h) {
        char item[96];
        std::snprintf(item, sizeof(item), "%sheap %u %" PRIu64 " of %" PRIu64 " MiB",
                      h ? ", " : "", h, uint64_t(budget.heapUsage[h]) >> 20,
                      uint64_t(budget.heapBudget[h]) >> 20);
        heaps += item;
      }
    }
  }
  char line[512];
  std::snprintf(line, sizeof(line),
                "vulkan: %" PRIu64 " presents (%.1f/s, frame %.1f ms), %" PRIu64
                " draws (+%" PRIu64 "), %" PRIu64 " submits (+%" PRIu64 "); failures: submit %" PRIu64
                " (last %d), present %" PRIu64 " (last %d); %s",
                presents, window_s > 0 ? double(new_presents) / window_s : 0.0,
                new_presents ? window_s * 1000.0 / double(new_presents) : 0.0, draws,
                draws - g_vk_beat.draws, submits, submits - g_vk_beat.submits,
                c.submit_failures.load(std::memory_order_relaxed),
                c.last_submit_failure.load(std::memory_order_relaxed),
                c.present_failures.load(std::memory_order_relaxed),
                c.last_present_failure.load(std::memory_order_relaxed), heaps.c_str());
  GAME_INFO("%s", line);
  g_beat_vulkan = line;
  // Run 21 (B.1): where the command processor's and the presenter's time goes.
  const uint64_t new_draws = draws - g_vk_beat.draws;
  const uint64_t new_submits = submits - g_vk_beat.submits;
  const uint64_t submit_ns = c.submit_ns.load(std::memory_order_relaxed);
  const uint64_t present_ns = c.present_ns.load(std::memory_order_relaxed);
  const uint64_t fence_waits = c.fence_waits.load(std::memory_order_relaxed);
  const uint64_t fence_wait_ns = c.fence_wait_ns.load(std::memory_order_relaxed);
  const uint64_t nv_kickoffs = g_nv.kickoffs.load(std::memory_order_relaxed);
  const uint64_t nv_waits = g_nv.fence_waits.load(std::memory_order_relaxed);
  const uint64_t nv_wait_ns = g_nv.fence_wait_ns.load(std::memory_order_relaxed);
  const uint64_t nv_queries = g_nv.fence_queries.load(std::memory_order_relaxed);
  const double ms = 1e-6;
  GAME_INFO("vulkan time in the window: vkQueueSubmit %.2f ms per call, vkQueuePresentKHR %.2f "
            "ms per present, %" PRIu64 " blocking fence waits of the command processor %.0f ms "
            "(%.0f ms per frame); GPU Commands thread %.0f ms of CPU = %.0f us per draw, %.0f "
            "draws per frame; nvdrv: %" PRIu64 " kickoffs, %" PRIu64 " fence waits %.0f ms, %" PRIu64
            " fence queries, nvmaps %" PRIu64 " live (%" PRId64 " MiB), %" PRIu64 " created",
            new_submits ? double(submit_ns - g_vk_beat.submit_ns) * ms / double(new_submits) : 0.0,
            new_presents ? double(present_ns - g_vk_beat.present_ns) * ms / double(new_presents)
                         : 0.0,
            fence_waits - g_vk_beat.fence_waits, double(fence_wait_ns - g_vk_beat.fence_wait_ns) * ms,
            new_presents ? double(fence_wait_ns - g_vk_beat.fence_wait_ns) * ms / double(new_presents)
                         : 0.0,
            double(g_cp_window_ns) * ms,
            new_draws ? double(g_cp_window_ns) / 1000.0 / double(new_draws) : 0.0,
            new_presents ? double(new_draws) / double(new_presents) : 0.0,
            nv_kickoffs - g_vk_beat.nv_kickoffs, nv_waits - g_vk_beat.nv_waits,
            double(nv_wait_ns - g_vk_beat.nv_wait_ns) * ms, nv_queries - g_vk_beat.nv_queries,
            g_nv.maps_created.load(std::memory_order_relaxed) -
                g_nv.maps_closed.load(std::memory_order_relaxed),
            g_nv.map_bytes.load(std::memory_order_relaxed) >> 20,
            g_nv.maps_created.load(std::memory_order_relaxed));
  // Run 22 (C.2): pipeline creation and shader translation in the window. No
  // pipeline or shader storage on disk exists on Switch (Runtime never calls
  // InitializeShaderStorage there; Mesa's cache is off): every one is new.
  {
    const uint64_t pipelines = c.pipelines.load(std::memory_order_relaxed);
    const uint64_t pipeline_ns = c.pipeline_ns.load(std::memory_order_relaxed);
    const uint64_t pipelines_cp = c.pipelines_cp.load(std::memory_order_relaxed);
    const uint64_t pipeline_ns_cp = c.pipeline_ns_cp.load(std::memory_order_relaxed);
    const uint64_t shaders = c.shaders.load(std::memory_order_relaxed);
    const uint64_t shader_ns = c.shader_ns.load(std::memory_order_relaxed);
    const uint64_t shaders_cp = c.shaders_cp.load(std::memory_order_relaxed);
    const uint64_t shader_ns_cp = c.shader_ns_cp.load(std::memory_order_relaxed);
    const uint64_t np = pipelines - g_vk_beat.pipelines, ns_ = shaders - g_vk_beat.shaders;
    GAME_INFO("pipelines: %" PRIu64 " created in the window (%.1f/s, %.0f ms in "
              "vkCreateGraphicsPipelines, %.1f ms each), of which %" PRIu64 " on GPU Commands "
              "(%.0f ms); shaders: %" PRIu64 " translated (%.0f ms, %.1f ms each), %" PRIu64
              " on GPU Commands (%.0f ms); since start %" PRIu64 " pipelines, %" PRIu64
              " shaders; disk storage: none on Switch",
              np, window_s > 0 ? double(np) / window_s : 0.0,
              double(pipeline_ns - g_vk_beat.pipeline_ns) * ms,
              np ? double(pipeline_ns - g_vk_beat.pipeline_ns) * ms / double(np) : 0.0,
              pipelines_cp - g_vk_beat.pipelines_cp,
              double(pipeline_ns_cp - g_vk_beat.pipeline_ns_cp) * ms, ns_,
              double(shader_ns - g_vk_beat.shader_ns) * ms,
              ns_ ? double(shader_ns - g_vk_beat.shader_ns) * ms / double(ns_) : 0.0,
              shaders_cp - g_vk_beat.shaders_cp,
              double(shader_ns_cp - g_vk_beat.shader_ns_cp) * ms, pipelines, shaders);
    g_vk_beat.pipelines = pipelines;
    g_vk_beat.pipeline_ns = pipeline_ns;
    g_vk_beat.pipelines_cp = pipelines_cp;
    g_vk_beat.pipeline_ns_cp = pipeline_ns_cp;
    g_vk_beat.shaders = shaders;
    g_vk_beat.shader_ns = shader_ns;
    g_vk_beat.shaders_cp = shaders_cp;
    g_vk_beat.shader_ns_cp = shader_ns_cp;
  }
  // D.2: a new submit or present failure that means memory or the device goes
  // to the crash log with the reason and this heartbeat; the game goes on (a
  // lost device ends in rex::FatalError, which the termination path logs).
  const uint64_t submit_failures = c.submit_failures.load(std::memory_order_relaxed);
  const uint64_t present_failures = c.present_failures.load(std::memory_order_relaxed);
  const int32_t failures[2] = {submit_failures != g_vk_beat.submit_failures
                                   ? c.last_submit_failure.load(std::memory_order_relaxed)
                                   : 0,
                               present_failures != g_vk_beat.present_failures
                                   ? c.last_present_failure.load(std::memory_order_relaxed)
                                   : 0};
  for (const int32_t result : failures) {
    if (const char* name = VkFailureName(result)) {
      GAME_ERROR("vulkan: %s (%d): written to %s with this heartbeat", name, result, kCrashPath);
      SetLastBeat(g_beat_memory + "\n" + g_beat_vulkan);
      char why[96];
      std::snprintf(why, sizeof(why), "=== vulkan failure: %s (%d) ===", name, result);
      CrashWriteState(why);
      FlushLog();
    }
  }
  g_vk_beat.submit_failures = submit_failures;
  g_vk_beat.present_failures = present_failures;
  g_vk_beat.presents = presents;
  g_vk_beat.draws = draws;
  g_vk_beat.submits = submits;
  g_vk_beat.submit_ns = submit_ns;
  g_vk_beat.present_ns = present_ns;
  g_vk_beat.fence_waits = fence_waits;
  g_vk_beat.fence_wait_ns = fence_wait_ns;
  g_vk_beat.nv_kickoffs = nv_kickoffs;
  g_vk_beat.nv_waits = nv_waits;
  g_vk_beat.nv_wait_ns = nv_wait_ns;
  g_vk_beat.nv_queries = nv_queries;
}

// Run 20 (A.2): at 20, 40, 60, 120 and 240 s the guest output the presenter
// holds is read back (CaptureGuestOutput, R8G8B8X8) on this UI thread. The
// log gets its size and how much of it is not black; with nx_vk_dump_frames
// it is also written as a binary PPM in kFrameDumpDir (on the SD only).
void CaptureFrame(uint64_t seconds) {
  auto* presenter = g_vulkan_graphics ? g_vulkan_graphics->presenter() : nullptr;
  if (!presenter) {
    GAME_INFO("frame at %" PRIu64 " s: no presenter", seconds);
    return;
  }
  rex::ui::RawImage image;
  const u64 start = armGetSystemTick();
  if (!presenter->CaptureGuestOutput(image) || !image.width || !image.height) {
    GAME_INFO("frame at %" PRIu64 " s: CaptureGuestOutput gave nothing (no guest output yet?)",
              seconds);
    return;
  }
  const u64 capture_ms = armTicksToNs(armGetSystemTick() - start) / 1'000'000;
  uint64_t lit = 0, sum = 0;
  uint32_t max_value = 0;
  for (uint32_t y = 0; y < image.height; ++y) {
    const uint8_t* row = image.data.data() + size_t(y) * image.stride;
    for (uint32_t x = 0; x < image.width; ++x) {
      const uint32_t v = uint32_t(row[x * 4]) + row[x * 4 + 1] + row[x * 4 + 2];
      lit += v > 24;  // more than ~8 per channel
      sum += v;
      max_value = std::max(max_value, v);
    }
  }
  const double pixels = double(image.width) * double(image.height);
  GAME_INFO("frame at %" PRIu64 " s: guest output %ux%u (capture %" PRIu64
            " ms), %.2f%% of pixels not black, mean %.1f, max %u (of 765)",
            seconds, image.width, image.height, capture_ms, 100.0 * double(lit) / pixels,
            double(sum) / pixels, max_value);
  if (!REXCVAR_GET(nx_vk_dump_frames)) {
    return;
  }
  mkdir(kFrameDumpDir, 0777);
  char path[96];
  std::snprintf(path, sizeof(path), "%s/frame-%03" PRIu64 "s.ppm", kFrameDumpDir, seconds);
  FILE* file = std::fopen(path, "wb");
  if (!file) {
    GAME_ERROR("frame at %" PRIu64 " s: cannot create %s (errno %d)", seconds, path, errno);
    return;
  }
  std::fprintf(file, "P6\n%u %u\n255\n", image.width, image.height);
  std::vector<uint8_t> rgb(size_t(image.width) * 3);
  for (uint32_t y = 0; y < image.height; ++y) {
    const uint8_t* row = image.data.data() + size_t(y) * image.stride;
    for (uint32_t x = 0; x < image.width; ++x) {
      rgb[x * 3] = row[x * 4];
      rgb[x * 3 + 1] = row[x * 4 + 1];
      rgb[x * 3 + 2] = row[x * 4 + 2];
    }
    std::fwrite(rgb.data(), 1, rgb.size(), file);
  }
  std::fclose(file);
  GAME_INFO("frame at %" PRIu64 " s: written to %s", seconds, path);
}

void CheckFrameCaptures(u64 start_tick) {
  static const uint64_t kTimes[] = {20, 40, 60, 120, 240};
  static size_t next = 0;
  if (!g_vulkan_graphics || next >= std::size(kTimes)) {
    return;
  }
  const uint64_t seconds = armTicksToNs(armGetSystemTick() - start_tick) / 1'000'000'000ull;
  if (seconds >= kTimes[next]) {
    CaptureFrame(kTimes[next++]);
  }
}

// Every loop of the UI thread (cheap): 20 s without a present -> one
// snapshot, again only after presents move.
void CheckVulkanStall() {
  if (!g_vulkan_graphics) {
    return;
  }
  const u64 now = armGetSystemTick();
  const uint64_t presents =
      rex::graphics::nx_gpu_counters().presents.load(std::memory_order_relaxed);
  if (presents != g_vk_beat.stall_presents) {
    g_vk_beat.stall_presents = presents;
    g_vk_beat.last_present_change = now;
    g_vk_beat.stall_reported = false;
    return;
  }
  if (!g_vk_beat.stall_reported &&
      armTicksToNs(now - g_vk_beat.last_present_change) >= 20'000'000'000ull) {
    g_vk_beat.stall_reported = true;
    char when[80];
    std::snprintf(when, sizeof(when), "after 20 s without a Vulkan present (%" PRIu64 " so far)",
                  presents);
    LogThreadSnapshot(when);
  }
}
#endif  // THEFT4_NX_VULKAN

// Every 10 s: one line; every 60 s: the call count of each export; at 5, 20,
// 60 s and then every 120 s: the snapshot.
struct Heartbeat {
  u64 start = 0;
  u64 next_line_s = 10;
  u64 next_snapshot_s = 5;
  u64 next_summary_s = 60;
  u64 last_line_s = 0;
  rex::audio::NullAudioStats last_audio;
  rex::system::file_io::IoStats last_io;
  std::vector<std::pair<Handle, u64>> last_cpu;
  u64 last_line_tick = 0;
  u64 last_rates_s = 0;
  uint64_t last_log_lines = 0, last_log_writes = 0, last_log_ns = 0;
  uint64_t last_faults = 0;

  void Tick() {
    const u64 now_tick = armGetSystemTick();
    const u64 seconds = armTicksToNs(now_tick - start) / 1'000'000'000ull;
    if (seconds >= next_line_s) {
      next_line_s = seconds + 10;
      size_t distinct = 0;
      uint64_t calls = 0;
      rex::diagnostics::GetExportCallTotals(&distinct, &calls);
      const auto a = rex::memory::nx::GetGuestArenaStats();
      GAME_INFO("heartbeat %" PRIu64 " s: exports %zu distinct, %" PRIu64
                " calls (tracing %s); guest memory %zu blocks (peak %zu of %zu)",
                seconds, distinct, calls, rex::diagnostics::g_export_tracing ? "on" : "OFF",
                a.mapped_blocks, a.peak_mapped_blocks, a.pool_blocks);
      LogDelayCounts();
      LogClock();
      // Run 21 (C): the clocks as the console runs them now.
      const std::string clocks = theft4::nx_clocks::Describe(theft4::nx_clocks::Read());
      GAME_INFO("clocks: %s", clocks.c_str());
      {
        const uint64_t faults = g_faults.load(std::memory_order_relaxed);
        GAME_INFO("faults: %" PRIu64 " access violations since start, %" PRIu64
                  " in the window (handled ones are the Vulkan write watch on guest memory)",
                  faults, faults - last_faults);
        last_faults = faults;
        // Run 23 (D.2): the faults the exception entry resumed, and whether
        // the sp / pc it resumed with ever differed from the ones it got.
        const RexNxResumeStats* r = rex_nx_resume_stats();
        GAME_INFO("resume: %" PRIu64 " faults resumed since start; sp/pc changed in the handler "
                  "%" PRIu64 ", frame read back wrong %" PRIu64 ", repaired %" PRIu64
                  " (nx_resume_guard %d)",
                  r->handled, r->context_changed, r->frame_wrong, r->repaired,
                  int(rex_nx_resume_guard));
      }
      LogNullGpu();
      LogBootSequencer();
      const double window_s = double(seconds - last_line_s);
      last_line_s = seconds;
      if (REXCVAR_GET(nx_null_audio)) {
        // Run 17: 187 callbacks a second up to 120 s, then 45-100: the window
        // rate, how long the guest callback takes and how often the worker
        // fell more than 100 ms behind say whether the callback is slow.
        const auto a = rex::audio::GetNullAudioStats();
        const uint64_t calls = a.callbacks - last_audio.callbacks;
        GAME_INFO("null audio: %u clients, %" PRIu64 " callbacks, %" PRIu64
                  " frames submitted, %" PRIu64 " played; last %.0f s: %.1f callbacks/s, "
                  "callback mean %.3f ms max %.3f ms, %" PRIu64 " periods, %" PRIu64 " resyncs",
                  a.clients, a.callbacks, a.frames_submitted, a.frames_played, window_s,
                  window_s > 0 ? double(calls) / window_s : 0.0,
                  calls ? double(a.callback_ns - last_audio.callback_ns) / double(calls) / 1e6
                        : 0.0,
                  double(a.callback_ns_max) / 1e6, a.periods - last_audio.periods,
                  a.resyncs - last_audio.resyncs);
        last_audio = a;
      }
      {
        // Reads and writes of the guest files (host transfer times) and the
        // operations in flight, which the exit waits for.
        const auto io = rex::system::file_io::TakeIoStats();
        const uint64_t reads = io.reads - last_io.reads;
        const uint64_t writes = io.writes - last_io.writes;
        const double read_mb = double(io.read_bytes - last_io.read_bytes) / 1e6;
        GAME_INFO("file I/O: %" PRIu64 " reads %.1f MB, %" PRIu64 " writes %.1f MB; last %.0f s: "
                  "%" PRIu64 " reads %.2f MB = %.2f MB/s, read mean %.3f ms max %.3f ms, %" PRIu64
                  " writes, write mean %.3f ms max %.3f ms; in flight %u",
                  io.reads, double(io.read_bytes) / 1e6, io.writes, double(io.write_bytes) / 1e6,
                  window_s, reads, read_mb, window_s > 0 ? read_mb / window_s : 0.0,
                  reads ? double(io.read_ns - last_io.read_ns) / double(reads) / 1e6 : 0.0,
                  double(io.read_ns_max) / 1e6, writes,
                  writes ? double(io.write_ns - last_io.write_ns) / double(writes) / 1e6 : 0.0,
                  double(io.write_ns_max) / 1e6, io.in_flight);
        // Run 19: how the reads of the window spread, and short transfers.
        std::string latency, size;
        static const char* const kLatency[] = {"<0.25", "<0.5", "<1", "<2", "<4",
                                               "<8",    "<16",  "<32", ">=32"};
        static const char* const kSize[] = {"<4K", "<16K", "<=32K", "<128K", "<1M", ">=1M"};
        for (int b = 0; b < rex::system::file_io::kReadLatencyBuckets; ++b) {
          char item[32];
          std::snprintf(item, sizeof(item), " %s:%" PRIu64, kLatency[b],
                        io.read_latency[b] - last_io.read_latency[b]);
          latency += item;
        }
        for (int b = 0; b < rex::system::file_io::kReadSizeBuckets; ++b) {
          char item[32];
          std::snprintf(item, sizeof(item), " %s:%" PRIu64, kSize[b],
                        io.read_size[b] - last_io.read_size[b]);
          size += item;
        }
        uint64_t short_reads = 0, short_writes = 0;
        rex::filesystem::GetNxShortTransfers(&short_reads, &short_writes);
        GAME_INFO("file reads in the window: ms%s; bytes%s; short reads %" PRIu64
                  ", short writes %" PRIu64 " since start",
                  latency.c_str(), size.c_str(), short_reads, short_writes);
        last_io = io;
      }
      const u64 window_ticks = last_line_tick ? now_tick - last_line_tick : 0;
      last_line_tick = now_tick;
      LogCpu(&last_cpu, window_ticks);
      LogThreadCounts();
      LogHostMemory();
      LogLogCost(&last_log_lines, &last_log_writes, &last_log_ns);
#if THEFT4_NX_VULKAN
      LogVulkan(window_s);
#endif
      {
        char when[64];
        std::snprintf(when, sizeof(when), "heartbeat %" PRIu64 " s; clocks ", seconds);
        SetLastBeat(when + clocks + "\n" + g_beat_memory + "\n" + g_beat_vulkan);
      }
      GAME_INFO("buttons: %" PRIu64 " changes, now 0x%" PRIx64, g_button_changes,
                g_buttons_now);
      if (REXCVAR_GET(nx_input)) {
        uint64_t published = 0, reads = 0;
        theft4::nx_input::GetCounters(&published, &reads);
        GAME_INFO("input: %" PRIu64 " pad states published, %" PRIu64 " XamInputGetState reads",
                  published, reads);
      }
    }
    // Every minute the count of each export, as pc-eu-base does, so the two
    // runs compare over time and not only on first calls (06 section 19).
    if (seconds >= next_summary_s) {
      next_summary_s = seconds + 60;
      char when[32];
      std::snprintf(when, sizeof(when), "at %" PRIu64 " s", seconds);
      rex::diagnostics::LogExportCallSummary(when);
      rex::diagnostics::LogExportCallRates(when, double(seconds - last_rates_s), 20);
      last_rates_s = seconds;
      // Run 23 (C.3): size and deepest use of every thread's stack.
      theft4::nx_threads::LogStacks([](const char* text) { GAME_INFO("%s", text); });
    }
    if (seconds >= next_snapshot_s) {
      next_snapshot_s = next_snapshot_s < 20 ? 20 : next_snapshot_s < 60 ? 60 : seconds + 120;
      char when[32];
      std::snprintf(when, sizeof(when), "at %" PRIu64 " s", seconds);
      LogThreadSnapshot(when);
    }
  }
};

// What + records before leaving, with the game's threads still running:
// the exports and stubs called, the guest memory, the clock, the null GPU.
// No thread snapshot: it pauses each thread, and nothing pauses threads on
// the way out any more.
void LogExitSummary() {
  static std::atomic<bool> done{false};
  if (done.exchange(true)) {
    return;
  }
  rex::diagnostics::LogExportCallSummary("at exit");
  LogStubSummaryOnce("at exit");
  LogArena("at exit");
  LogClock();
  LogNullGpu();
  rex::FlushLogging();
}

// ── Exit (09-rules.md section 9, tools/switch-common/nx_exit.h) ────────────
//
// + held for 1 s, or the system asking the applet to exit:
//   A  (cvar nx_exit_mode = A) exit(0), which is what returning from main
//      does (crt0 enters main with LR = exit): atexit handlers, _exit,
//      __libnx_exit (__appExit, then back to hbloader). The game's threads
//      still run. Run 17: an access violation after _exit (06 §23).
//   D  (default since run 18; to be verified) the file exports refuse new
//      operations (C0000120) and those in flight get 2 s; the null audio
//      worker stops (flag, 2 s); every guest thread ends itself at its next
//      export (svcExitThread from the export entry hook), the null GPU's
//      threads are told to stop, join for at most 2 s, then exit(0) as A.
// Run 15: A and D leave cleanly, in the game and in the smoke. The modes that
// paused every thread and ended in svcExitProcess (B with __appExit, C
// without) showed "The software was closed because an error occurred" and
// are gone. Every step is in game-exit.log (write + fsync on its own
// descriptor) and in game.log.

PadState g_pad;

constexpr size_t kMaxExitThreads = 256;
constexpr u64 kJoinTimeoutMs = 2000;
std::vector<rex::system::object_ref<rex::system::XThread>> g_exit_xthreads;  // reserved at start
Handle g_exit_handles[kMaxExitThreads];

// The XThreads (kernel object table, waited for at most 200 ms) and the
// threads of the export traces (each with its Horizon handle).
size_t CollectExitThreadHandles() {
  size_t n = 0;
  if (auto* kernel = rex::system::kernel_state()) {
    kernel->object_table()->TryGetObjectsByType<rex::system::XThread>(
        &g_exit_xthreads, std::chrono::milliseconds(200));
    for (auto& t : g_exit_xthreads) {
      if (auto* host = t->thread(); host && n < kMaxExitThreads) {
        g_exit_handles[n++] = host->nx_kernel_handle();
      }
    }
  }
  rex::diagnostics::ExportThreadSnapshot traces[64];
  const size_t n_traces = rex::diagnostics::SnapshotExportThreads(traces, 64);
  for (size_t k = 0; k < n_traces && n < kMaxExitThreads; ++k) {
    g_exit_handles[n++] = traces[k].nx_handle;
  }
  return n;
}

// Mode D: guest threads end themselves at their next export.
void ExitThisThreadAtExport() {
  u64 self = 0;
  svcGetThreadId(&self, CUR_THREAD_HANDLE);
  if (self != g_host_main_thread_id) {
    svcExitThread();
  }
}

void ExitCooperative() {
  const size_t n = CollectExitThreadHandles();
  ExitStep("telling every thread to end: guest threads call svcExitThread at their next "
           "export, the null GPU's threads stop");
  rex::diagnostics::SetExportEntryHook(ExitThisThreadAtExport);
  rex::system::RequestNullGpuStop();
  char text[160];
  std::snprintf(text, sizeof(text), "joining %zu threads for at most 2 s", n);
  ExitStep(text);
  u64 self_id = 0;
  svcGetThreadId(&self_id, CUR_THREAD_HANDLE);
  const u64 start = armGetSystemTick();
  size_t ended = 0, waited = 0;
  for (size_t i = 0; i < n; ++i) {
    bool duplicate = false;
    for (size_t k = 0; k < i && !duplicate; ++k) duplicate = g_exit_handles[k] == g_exit_handles[i];
    u64 id = 0;
    if (duplicate || (R_SUCCEEDED(svcGetThreadId(&id, g_exit_handles[i])) && id == self_id)) {
      continue;
    }
    ++waited;
    const u64 spent_ns = armTicksToNs(armGetSystemTick() - start);
    const u64 limit_ns = kJoinTimeoutMs * 1'000'000ull;
    const s64 left = spent_ns < limit_ns ? s64(limit_ns - spent_ns) : 0;
    if (R_SUCCEEDED(svcWaitSynchronizationSingle(g_exit_handles[i], left))) {
      ++ended;
    }
  }
  std::snprintf(text, sizeof(text),
                "%zu of %zu threads ended within 2 s; next exit(0), as a return from main",
                ended, waited);
  ExitStep(text);
}

// What is still running when the exit starts (run 17 crashed after _exit in
// mode A, 06 §23): the guest file operations in flight and the host XThreads.
// Names only: nothing is paused.
void LogExitState() {
  char text[256];
  std::snprintf(text, sizeof(text), "file operations in flight: %u (opens, reads, writes)",
                rex::system::file_io::IoInFlight());
  ExitStep(text);
  if (!g_logging_ready) {
    return;
  }
  if (auto* kernel = rex::system::kernel_state()) {
    kernel->object_table()->TryGetObjectsByType<rex::system::XThread>(
        &g_exit_xthreads, std::chrono::milliseconds(200));
    std::string hosts;
    size_t count = 0;
    for (auto& t : g_exit_xthreads) {
      if (!t->is_guest_thread() && t->is_running()) {
        hosts += hosts.empty() ? "" : ", ";
        hosts += t->thread_name();
        ++count;
      }
    }
    GAME_INFO("exit: %zu host XThreads running: %s", count, hosts.c_str());
    g_exit_xthreads.clear();
  }
}

// Mode D, before the threads are told to end: (a) the file exports refuse new
// opens, reads and writes (C0000120, CANCELLED); (b) the operations in flight
// get at most 2 s to finish.
void StopFileIo() {
  rex::system::file_io::CloseIoGate();
  char text[160];
  std::snprintf(text, sizeof(text),
                "file I/O gate closed (new opens, reads, writes -> C0000120); waiting for %u in "
                "flight, at most 2 s",
                rex::system::file_io::IoInFlight());
  ExitStep(text);
  const u64 start = armGetSystemTick();
  const bool idle = rex::system::file_io::WaitIoIdle(uint32_t(kJoinTimeoutMs));
  std::snprintf(text, sizeof(text), "file I/O: %s after %" PRIu64 " ms (%u in flight)",
                idle ? "idle" : "STILL BUSY", armTicksToNs(armGetSystemTick() - start) / 1'000'000,
                rex::system::file_io::IoInFlight());
  ExitStep(text);
}

// (c) The null audio worker (a host XThread that runs the guest callback
// every 5.333 ms) stops at its next period: flag, then at most 2 s.
void StopNullAudioWorker() {
  if (!REXCVAR_GET(nx_null_audio)) {
    return;
  }
  ExitStep("stopping the null audio worker (flag, at most 2 s)");
  const u64 start = armGetSystemTick();
  const bool ended = rex::audio::StopNullAudio(uint32_t(kJoinTimeoutMs));
  char text[96];
  std::snprintf(text, sizeof(text), "null audio worker %s after %" PRIu64 " ms",
                ended ? "ended" : "STILL RUNNING",
                armTicksToNs(armGetSystemTick() - start) / 1'000'000);
  ExitStep(text);
}

[[noreturn]] void ExitGame(const char* reason) {
  const char mode = theft4::nx_exit::ParseExitMode(REXCVAR_GET(nx_exit_mode).c_str());
  g_exit_start_tick = armGetSystemTick();
  g_exit_mode = mode;
  char text[200];
  std::snprintf(text, sizeof(text), "chosen (%s): %s", reason,
                theft4::nx_exit::ExitModeDescription(mode));
  ExitStep(text);
  if (g_logging_ready) {
    LogExitSummary();
  }
  LogExitState();
  if (mode == 'D') {
    StopFileIo();
    StopNullAudioWorker();
    ExitCooperative();
  } else {
    ExitStep("exit(0), as a return from main; the game's threads still run");
  }
  std::exit(0);
}

[[noreturn]] void WaitForPlusAndExit(PadState* pad) {
  ConsolePrint("\nPress + to exit.\n");
  for (;;) {
    bool plus = false;
    while (appletMainLoop()) {
      padUpdate(pad);
      LogButtonChange(padGetButtons(pad));
      if (padGetButtonsDown(pad) & HidNpadButton_Plus) {
        plus = true;
        break;
      }
      ConsoleRefresh();
      svcSleepThread(16'000'000);
    }
    ExitGame(!plus             ? "the system asked the applet to exit"
             : g_stopped.load() ? "+ after the stop"
                                : "+");
  }
}


}  // namespace

// The first missing function stops the run: recorded durably, then this
// thread parks (the main thread waits for +). Later calls, from other
// threads, park without logging again.
namespace rex::diagnostics {
[[noreturn]] void StopAtMissingFunction(const char* kind, const char* name, uint32_t guest_lr) {
  // Exit D in progress: a guest thread refused a file operation (C0000120)
  // may go for an error screen it cannot have. Not a stop of the run: park.
  if (rex::system::file_io::IoGateClosed()) {
    for (;;) {
      svcSleepThread(INT64_MAX);
    }
  }
  if (!g_stopped.exchange(true)) {
    const bool guest = rex::system::XThread::IsInThread();
    GAME_ERROR("STOP at the first function missing on Switch: %s (%s), guest LR %08X, %s",
               name, kind, guest_lr,
               guest ? ("guest thread " + rex::system::XThread::GetCurrentThread()->name()).c_str()
                     : "host thread");
    LogArena("at the stop");
    LogStubSummaryOnce("at the stop");
    rex::FlushLogging();
  }
  u64 self = 0;
  svcGetThreadId(&self, CUR_THREAD_HANDLE);
  if (self == g_host_main_thread_id) {
    WaitForPlusAndExit(&g_pad);
  }
  for (;;) {
    svcSleepThread(INT64_MAX);
  }
}
}  // namespace rex::diagnostics

namespace {

// A thread_local read as the runtime's hot helpers do it (ThreadState::Get,
// XThread::GetCurrentThread): a call, then __aarch64_read_tp, which is
// itself a call on devkitA64.
thread_local uint32_t t_tls_benchmark = 0;

__attribute__((noinline, noipa)) uint32_t BumpThreadLocal() { return ++t_tls_benchmark; }

double MeasureThreadLocalNs(uint32_t iterations) {
  const u64 start = armGetSystemTick();
  for (uint32_t i = 0; i < iterations; ++i) {
    BumpThreadLocal();
    asm volatile("" ::: "memory");
  }
  return iterations ? double(armTicksToNs(armGetSystemTick() - start)) / double(iterations) : 0.0;
}

void ClockLog(bool error, const char* text) {
  if (error) {
    GAME_ERROR("%s", text);
  } else {
    GAME_INFO("%s", text);
  }
}

void ProfileLog(const char* text) { GAME_INFO("%s", text); }

[[noreturn]] void GameMain(int argc, char** argv) {
  rex::cvar::Init(argc, argv);
  // Run 23 (C.3): before the threads of the log and the runtime exist.
  theft4::nx_threads::Start(REXCVAR_GET(nx_stack_watch), reinterpret_cast<u64>(&_start));
  rex_nx_resume_guard = REXCVAR_GET(nx_resume_guard);
  consoleInit(nullptr);
  g_console_open = true;  // still single-threaded
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&g_pad);
  svcGetThreadId(&g_host_main_thread_id, CUR_THREAD_HANDLE);
  g_start_tick = armGetSystemTick();
  ConsolePrint("Theft4 switch-game (no graphics)\n\n");

  StartLogging();
  CaptureTerminations();
  if (REXCVAR_GET(nx_fault_count_handler)) {
    rex::arch::ExceptionHandler::Install(FaultCountHandler, nullptr);
  }
  rex::arch::ExceptionHandler::Install(CrashLogHandler, nullptr);
#if THEFT4_NX_VULKAN
  g_nv_count.store(REXCVAR_GET(nx_nv_wrap), std::memory_order_relaxed);
#endif
  GAME_INFO("run 21 pieces: nx_apm_clock %d, nx_nv_wrap %d, nx_fault_count_handler %d, "
            "nx_crash_log_after_setup %d, nx_profile %d, nx_vk_mesa_messenger (in the SDK) on "
            "unless set false; nx_mount_cache %d",
            int(REXCVAR_GET(nx_apm_clock)), int(REXCVAR_GET(nx_nv_wrap)),
            int(REXCVAR_GET(nx_fault_count_handler)), int(REXCVAR_GET(nx_crash_log_after_setup)),
            int(REXCVAR_GET(nx_profile)), int(REXCVAR_GET(nx_mount_cache)));
  GAME_INFO("run 23 pieces: vector registers saved first in the exception entry (always), "
            "crash record with threads and stacks (always), nx_stack_watch %d, "
            "nx_resume_guard %d",
            int(REXCVAR_GET(nx_stack_watch)), int(REXCVAR_GET(nx_resume_guard)));
  theft4::nx_clocks::Start(REXCVAR_GET(nx_apm_clock), ClockLog);
  if (REXCVAR_GET(nx_profile)) {
    theft4::nx_profiler::Start(g_module_base, g_module_text_size, ProfileLog);
    GAME_INFO("profile: %s (cvar nx_profile): busy threads sampled once a millisecond",
              theft4::nx_profiler::Running() ? "on" : "the sampling thread did NOT start");
  }
  // What the exit needs, made now while allocating is safe (ExitGame).
  g_exit_xthreads.reserve(kMaxExitThreads);
  if (!theft4::nx_exit::OpenStepLog(kExitStepPath)) {
    GAME_ERROR("cannot open %s (errno %d): the exit steps go to game.log only", kExitStepPath,
               errno);
  }
  {
    const char mode = theft4::nx_exit::ParseExitMode(REXCVAR_GET(nx_exit_mode).c_str());
    GAME_INFO("exit: hold + for 1 s -> mode %c, %s (cvar nx_exit_mode); steps in %s", mode,
              theft4::nx_exit::ExitModeDescription(mode), kExitStepPath);
  }
  if (!CheckGameFiles()) {
    WaitForPlusAndExit(&g_pad);
  }
  // Run 19: what the heartbeat's thread lines need, auto-sleep, and the cost
  // of the export trace (C.3 of the run 19 list).
  g_main_thread_handle = threadGetCurHandle();
  {
    u64 limit_handle = 0;
    if (R_SUCCEEDED(svcGetInfo(&limit_handle, InfoType_ResourceLimit, INVALID_HANDLE, 0))) {
      g_resource_limit = Handle(limit_handle);
    }
  }
  if (REXCVAR_GET(nx_disable_auto_sleep)) {
    const Result rc = appletSetAutoSleepDisabled(true);
    GAME_INFO("auto-sleep: appletSetAutoSleepDisabled(true) -> 0x%X (cvar nx_disable_auto_sleep)",
              rc);
  }
  {
    const double trace_ns = rex::diagnostics::MeasureExportTraceNs(1'000'000);
    const double tls_ns = MeasureThreadLocalNs(1'000'000);
    rex::diagnostics::SetExportTracing(REXCVAR_GET(nx_trace_exports));
    GAME_INFO("export trace: %.1f ns per traced call (1M calls, this thread, no contention); "
              "thread_local read through a call %.1f ns; tracing %s (cvar nx_trace_exports)",
              trace_ns, tls_ns, REXCVAR_GET(nx_trace_exports) ? "on" : "OFF");
  }

  GAME_INFO("about to construct rex::Runtime (game:, d: = %s)", kGameDir);
  const std::filesystem::path root(kRootDir);
  auto runtime = std::make_unique<rex::Runtime>(
      std::filesystem::path(kGameDir), root / "user", std::filesystem::path(kGameDir) / "update",
      root / "runtime-cache", std::filesystem::path{}, root / "marketplace", root / "saves");

  rex::RuntimeConfig config;
  config.tool_mode = false;
  // Phase (a) of the renderer (06 section 24, not tried on the console yet):
  // ReXGlue's Vulkan backend, created here as the null GPU is; the UI thread
  // is this loop (PumpFromUIThread), the window the default NWindow, which
  // the console gives up first.
  const bool want_vulkan = REXCVAR_GET(nx_gpu) == "vulkan";
#if THEFT4_NX_VULKAN
  if (want_vulkan) {
    {
      std::lock_guard<std::timed_mutex> lock(g_console_mutex);
      if (g_console_open) {
        consoleExit(nullptr);
        g_console_open = false;
      }
    }
    // Mesa's environment, read when NVK starts (nfsmw-nx does both): no shader
    // cache on the SD; nvdrv's transfer memory only if nx_nvmap_mb says so.
    setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
    if (REXCVAR_GET(nx_nvmap_mb) > 0) {
      __nx_nv_transfermem_size = uint32_t(REXCVAR_GET(nx_nvmap_mb)) << 20;
    }
    GAME_INFO("vulkan: MESA_SHADER_CACHE_DISABLE=true, nvdrv transfer memory %u MiB "
              "(cvar nx_nvmap_mb, 0 = libnx's)",
              __nx_nv_transfermem_size >> 20);
    g_ui_context = std::make_unique<rex::ui::NxWindowedAppContext>();
    runtime->set_app_context(g_ui_context.get());
    auto vulkan = std::make_unique<rex::graphics::vulkan::VulkanGraphicsSystem>();
    g_vulkan_graphics = vulkan.get();
    config.graphics = std::move(vulkan);
    GAME_INFO("graphics: ReXGlue's Vulkan backend on NVK (cvar nx_gpu = vulkan); console closed");
  } else if (GAME_INFO("graphics: this NRO has the Vulkan backend, but nx_gpu = %s: put "
                       "nx_gpu = \"vulkan\" in %s",
                       REXCVAR_GET(nx_gpu).c_str(), kConfigPath),
             false) {
  } else
#else
  if (want_vulkan) {
    GAME_ERROR("nx_gpu = vulkan, but this build has no Vulkan backend "
               "(THEFT4_SWITCH_GAME_VULKAN off): the null GPU instead");
  }
#endif
  // Run 13 stopped at VdSetGraphicsInterruptCallback: without a graphics
  // object every Vd* export that needs one stops the run. The null GPU
  // consumes the ring and raises the interrupts without drawing.
  if (REXCVAR_GET(nx_null_gpu)) {
    config.graphics = rex::system::CreateNullGraphicsSystem();
    GAME_INFO("graphics: null GPU (cvar nx_null_gpu)");
  } else {
    GAME_INFO("graphics: none (nx_null_gpu = false): the run stops at the first Vd* export "
              "that needs a GPU");
  }
  // Run 14: XamInputGetState answered "not connected" (0x48F) where the PC
  // has a pad, and the game never called XamInputGetCapabilities.
  if (REXCVAR_GET(nx_input)) {
    config.input_factory = [](bool) { return theft4::nx_input::CreatePadInputSystem(); };
    GAME_INFO("input: the Switch controller as the 360 pad of user 0 (cvar nx_input)");
  } else {
    GAME_INFO("input: none (nx_input = false)");
  }
  // Runs 13-16: XAudioRegisterRenderDriverClient failed with C0000002 (no
  // audio system), and the first boot screen waits for its sound (06 §19).
  if (REXCVAR_GET(nx_null_audio)) {
    config.audio_factory = [](rex::runtime::FunctionDispatcher* dispatcher) {
      return rex::audio::CreateNullAudioSystem(dispatcher);
    };
    GAME_INFO("audio: the null audio system, frames dropped at the real rate (cvar nx_null_audio)");
  } else {
    GAME_INFO("audio: none (nx_null_audio = false)");
  }
  // InitializeKernel with a line on each side: run 9 ended between the
  // kernel host-task worker and SetupVfs, i.e. around here.
  config.kernel_init = [](rex::Runtime* runtime, rex::system::KernelState* kernel_state) {
    GAME_INFO("about to kernel_init (InitializeKernel: XAM apps, xboxkrnl, xam, xbdm)");
    rex::kernel::InitializeKernel(runtime, kernel_state);
    GAME_INFO("kernel_init done; next in Setup: SetupVfs");
  };
  GAME_INFO("about to Runtime::Setup(PPCImageConfig): code %08X+%X, image %08X+%X",
            uint32_t(PPCImageConfig.code_base), uint32_t(PPCImageConfig.code_size),
            uint32_t(PPCImageConfig.image_base), uint32_t(PPCImageConfig.image_size));
  rex::X_STATUS status = runtime->Setup(PPCImageConfig, std::move(config));
  if (XFAILED(status)) {
    GAME_ERROR("Runtime::Setup failed, status %08X (see the [sdk] lines above)", status);
    WaitForPlusAndExit(&g_pad);
  }
  LogArena("after Setup");
  // Setup installed the MMIO handler (and the write watch); the crash log goes
  // after it again, so that it sees only the faults nobody handled.
  if (REXCVAR_GET(nx_crash_log_after_setup)) {
    rex::arch::ExceptionHandler::Uninstall(CrashLogHandler, nullptr);
    rex::arch::ExceptionHandler::Install(CrashLogHandler, nullptr);
  }
  // Run 23 (D.2): last of all, only for the faults nobody handled.
  rex::arch::ExceptionHandler::Install(CrashFinalHandler, nullptr);
#if THEFT4_NX_VULKAN
  if (g_vulkan_graphics) {
    // As ReXApp does: the window, then the presenter Setup made on this thread.
    g_window = rex::ui::Window::Create(*g_ui_context, "Theft4", rex::ui::NxWindow::kWidth,
                                       rex::ui::NxWindow::kHeight);
    if (!g_window || !g_window->Open()) {
      GAME_ERROR("Vulkan: the NWindow window did not open");
    } else if (!g_vulkan_graphics->presenter()) {
      GAME_ERROR("Vulkan: GPU set up without a presenter");
    } else {
      g_window->SetPresenter(g_vulkan_graphics->presenter());
      GAME_INFO("Vulkan: presenter on the default NWindow (%ux%u)", rex::ui::NxWindow::kWidth,
                rex::ui::NxWindow::kHeight);
    }
  }
#endif
  // The 360's utility-drive caches, which the PC run showed the game opening
  // (cache:/valid.txt, cache1:/valid.txt). To be verified on the console: the
  // runtime left cache: unresolved on purpose (Runtime::SetupVfs).
  if (!REXCVAR_GET(nx_mount_cache)) {
    GAME_INFO("cache: and cache1: NOT mounted (cvar nx_mount_cache = false)");
  } else {
    GAME_INFO("about to mount cache: and cache1: on %s/cache and %s/cache1", kRootDir, kRootDir);
    if (!runtime->MountCacheDevices(root / "cache", root / "cache1")) {
      GAME_ERROR("cache: or cache1: not mounted (see the [sdk] lines above)");
    }
  }

  GAME_INFO("about to LoadXexImage(game:/default.xex)");
  status = runtime->LoadXexImage("game:/default.xex");
  if (XFAILED(status)) {
    GAME_ERROR("LoadXexImage failed, status %08X", status);
    WaitForPlusAndExit(&g_pad);
  }
  auto module = runtime->kernel_state()->GetExecutableModule();
  GAME_INFO("XEX loaded: title %08X, entry point %08X, stack %u KiB", module->title_id(),
            module->entry_point(), module->stack_size() >> 10);
  LogArena("after LoadXexImage");

  GAME_INFO("about to PrepareModuleLaunch");
  auto thread = runtime->PrepareModuleLaunch();
  if (!thread) {
    GAME_ERROR("PrepareModuleLaunch failed");
    WaitForPlusAndExit(&g_pad);
  }
  GAME_INFO("about to resume the main guest thread '%s' at %08X", thread->name().c_str(),
            module->entry_point());
  LogClock();
  thread->Resume();
  GAME_INFO("main guest thread resumed; waiting (the log records the first stop)");

  // The UI thread: watch for + and for the main guest thread ending; log the
  // heartbeat and the thread snapshots.
  bool seen_running = false;
  Heartbeat heartbeat;
  heartbeat.start = armGetSystemTick();
  heartbeat.last_line_tick = heartbeat.start;
  // The runtime is never destroyed: ~Runtime unmaps guest memory and tears
  // the kernel down under the guest threads (run 12).
  (void)runtime.release();
  const bool input = REXCVAR_GET(nx_input);
  u64 plus_since = 0;
  while (appletMainLoop()) {
    padUpdate(&g_pad);
    LogButtonChange(padGetButtons(&g_pad));
    if (input) {
      theft4::nx_input::Publish(&g_pad, false);
    }
    // + is START for the game; held for 1 s it exits.
    if (padGetButtons(&g_pad) & HidNpadButton_Plus) {
      if (!plus_since) {
        plus_since = armGetSystemTick();
      } else if (armTicksToNs(armGetSystemTick() - plus_since) >= 1'000'000'000ull) {
        ExitGame(g_stopped.load() ? "+ held after the stop" : "+ held for 1 s");
      }
    } else {
      plus_since = 0;
    }
    seen_running |= thread->is_running();
    if (seen_running && !g_main_thread_returned.load() && !thread->is_running()) {
      g_main_thread_returned.store(true);
      GAME_INFO("main guest thread is no longer running");
    }
    heartbeat.Tick();
#if THEFT4_NX_VULKAN
    CheckVulkanStall();
    CheckFrameCaptures(heartbeat.start);
    if (g_ui_context) {
      g_ui_context->PumpFromUIThread();  // posted functions, then the paint
    }
#endif
    ConsoleRefresh();
    svcSleepThread(16'000'000);
  }
  ExitGame("the system asked the applet to exit");
}

}  // namespace

int main(int argc, char** argv) {
  GameMain(argc, argv);  // leaves through ExitGame
}
