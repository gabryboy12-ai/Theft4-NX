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
// Logging works as in switch-smoke: every line is appended to game.log through
// its own descriptor, fsync'd and closed before the next operation, and the
// SDK logger writes through the same path, so after a crash the last line is
// the last operation attempted. An unhandled fault is also written to
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
// Exit is svcExitProcess: guest threads are still running, and returning to
// the loader would unload code they execute (04-threads-exit.md section 5).
// svcExitProcess ends every thread of the process at once; the only thing
// to avoid before it is drawing on the console after consoleExit (run 11:
// the termination line's printf faulted in ConsoleSwRenderer_drawChar and
// produced a crash report on every +). Console output goes through
// ConsolePrint under g_console_mutex, and CloseConsole shuts it once.
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

#include <fcntl.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>

#include <rex/cvar.h>
#include <rex/diagnostics/export_stubs.h>
#include <rex/diagnostics/missing_function.h>
#include <rex/diagnostics/policy.h>
#include <rex/exception_handler.h>
#include <rex/kernel/init.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

#include <spdlog/sinks/base_sink.h>

#include "gta4_init.h"

namespace {

constexpr const char* kRootDir = "sdmc:/switch/theft4";
constexpr const char* kGameDir = "sdmc:/switch/theft4/game";
constexpr const char* kLogPath = "sdmc:/switch/theft4/game.log";
constexpr const char* kPreviousLogPath = "sdmc:/switch/theft4/game.prev.log";
constexpr const char* kCrashPath = "sdmc:/switch/theft4/game-crash.log";
constexpr const char* kConfigPath = "sdmc:/switch/theft4/game.toml";

bool g_logging_ready = false;
std::atomic<bool> g_stopped{false};
std::atomic<bool> g_main_thread_returned{false};
u64 g_module_base = 0;
u64 g_module_text_size = 0;  // the R-X block at the base (for the stack scan)
u64 g_host_main_thread_id = 0;
int g_crash_fd = -1;

}  // namespace

// Start of the NRO image (see switch-smoke/main.cpp, StepModuleBase).
extern "C" char _start;

namespace {

// ── Console ────────────────────────────────────────────────────────────────

// The console belongs to whoever holds this mutex; after CloseConsole nothing
// draws (the framebuffer is gone). Bounded: a thread that cannot take it
// within 200 ms skips the screen, and the line still reaches game.log.
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

void ConsoleRefresh() {
  if (g_console_mutex.try_lock()) {
    if (g_console_open) {
      consoleUpdate(nullptr);
    }
    g_console_mutex.unlock();
  }
}

void CloseConsole() {
  std::lock_guard<std::timed_mutex> lock(g_console_mutex);
  if (g_console_open) {
    g_console_open = false;
    consoleExit(nullptr);
  }
}

// ── Durable log (as in switch-smoke) ───────────────────────────────────────

std::timed_mutex g_log_file_mutex;

void DurableWriteLocked(const char* data, size_t size) {
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
}

void DurableWrite(const char* data, size_t size) {
  std::lock_guard<std::timed_mutex> lock(g_log_file_mutex);
  DurableWriteLocked(data, size);
}

// For stderr, which the fault path of exception_handler_switch.cpp also
// writes: never wait more than 200 ms for the log (a thread that faulted
// while writing it must not deadlock the report). false = not written.
bool DurableWriteBounded(const char* data, size_t size) {
  if (!g_log_file_mutex.try_lock_for(std::chrono::milliseconds(200))) {
    return false;
  }
  DurableWriteLocked(data, size);
  g_log_file_mutex.unlock();
  return true;
}

class DurableSink final : public spdlog::sinks::base_sink<std::mutex> {
 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    spdlog::memory_buf_t formatted;
    formatter_->format(msg, formatted);
    DurableWrite(formatted.data(), formatted.size());
  }
  void flush_() override {}
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

// Installed last: runs only for access violations no runtime handler claimed
// (MMIO, physical write watch). Records the fault; returning false leaves it
// unhandled, and the runtime ends it in svcBreak (Atmosphere crash report).
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
  if (g_crash_fd >= 0) fsync(g_crash_fd);
  return false;
}

// ── Terminations ───────────────────────────────────────────────────────────

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
// included, reaches these first.
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
  rex::cvar::LoadConfig(kConfigPath);
  rex::cvar::FinalizeInit();
  GAME_INFO("cvars from argv, then %s", kConfigPath);

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
    row.name = t->name();
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

// Every 10 s: one line; at 5, 20, 60 s and then every 120 s: the snapshot.
struct Heartbeat {
  u64 start = 0;
  u64 next_line_s = 10;
  u64 next_snapshot_s = 5;

  void Tick() {
    const u64 seconds = armTicksToNs(armGetSystemTick() - start) / 1'000'000'000ull;
    if (seconds >= next_line_s) {
      next_line_s = seconds + 10;
      size_t distinct = 0;
      uint64_t calls = 0;
      rex::diagnostics::GetExportCallTotals(&distinct, &calls);
      const auto a = rex::memory::nx::GetGuestArenaStats();
      GAME_INFO("heartbeat %" PRIu64 " s: exports %zu distinct, %" PRIu64
                " calls; guest memory %zu blocks (peak %zu of %zu)",
                seconds, distinct, calls, a.mapped_blocks, a.peak_mapped_blocks, a.pool_blocks);
    }
    if (seconds >= next_snapshot_s) {
      next_snapshot_s = next_snapshot_s < 20 ? 20 : next_snapshot_s < 60 ? 60 : seconds + 120;
      char when[32];
      std::snprintf(when, sizeof(when), "at %" PRIu64 " s", seconds);
      LogThreadSnapshot(when);
    }
  }
};

// What + records before leaving: where the threads are, the exports and
// stubs called, the guest memory peak.
void LogExitSummary() {
  static std::atomic<bool> done{false};
  if (done.exchange(true)) {
    return;
  }
  LogThreadSnapshot("at exit");
  rex::diagnostics::LogExportCallSummary("at exit");
  LogStubSummaryOnce("at exit");
  LogArena("at exit");
  rex::FlushLogging();
}

[[noreturn]] void WaitForPlusAndExit(PadState* pad) {
  ConsolePrint("\nPress + to exit.\n");
  while (appletMainLoop()) {
    padUpdate(pad);
    if (padGetButtonsDown(pad) & HidNpadButton_Plus) {
      break;
    }
    ConsoleRefresh();
    svcSleepThread(16'000'000);
  }
  GAME_INFO("exit requested (+)%s", g_stopped.load() ? " after the stop" : "");
  LogExitSummary();
  CloseConsole();
  svcExitProcess();  // wrapped: logged (game.log only: the console is closed)
}

PadState g_pad;

}  // namespace

// The first missing function stops the run: recorded durably, then this
// thread parks (the main thread waits for +). Later calls, from other
// threads, park without logging again.
namespace rex::diagnostics {
[[noreturn]] void StopAtMissingFunction(const char* kind, const char* name, uint32_t guest_lr) {
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

int GameMain(int argc, char** argv) {
  rex::cvar::Init(argc, argv);
  consoleInit(nullptr);
  g_console_open = true;  // still single-threaded
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&g_pad);
  svcGetThreadId(&g_host_main_thread_id, CUR_THREAD_HANDLE);
  ConsolePrint("Theft4 switch-game (no graphics)\n\n");

  StartLogging();
  CaptureTerminations();
  rex::arch::ExceptionHandler::Install(CrashLogHandler, nullptr);
  if (!CheckGameFiles()) {
    WaitForPlusAndExit(&g_pad);
  }

  GAME_INFO("about to construct rex::Runtime (game:, d: = %s)", kGameDir);
  const std::filesystem::path root(kRootDir);
  auto runtime = std::make_unique<rex::Runtime>(
      std::filesystem::path(kGameDir), root / "user", std::filesystem::path(kGameDir) / "update",
      root / "runtime-cache", std::filesystem::path{}, root / "marketplace", root / "saves");

  rex::RuntimeConfig config;
  config.tool_mode = false;  // no graphics object: "Runtime initialized without graphics system"
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
  // The 360's utility-drive caches, which the PC run showed the game opening
  // (cache:/valid.txt, cache1:/valid.txt). To be verified on the console: the
  // runtime left cache: unresolved on purpose (Runtime::SetupVfs).
  GAME_INFO("about to mount cache: and cache1: on %s/cache and %s/cache1", kRootDir, kRootDir);
  if (!runtime->MountCacheDevices(root / "cache", root / "cache1")) {
    GAME_ERROR("cache: or cache1: not mounted (see the [sdk] lines above)");
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
  thread->Resume();
  GAME_INFO("main guest thread resumed; waiting (the log records the first stop)");

  // The UI thread: watch for + and for the main guest thread ending; log the
  // heartbeat and the thread snapshots.
  bool seen_running = false;
  Heartbeat heartbeat;
  heartbeat.start = armGetSystemTick();
  while (appletMainLoop()) {
    padUpdate(&g_pad);
    if (padGetButtonsDown(&g_pad) & HidNpadButton_Plus) {
      break;
    }
    seen_running |= thread->is_running();
    if (seen_running && !g_main_thread_returned.load() && !thread->is_running()) {
      g_main_thread_returned.store(true);
      GAME_INFO("main guest thread is no longer running");
    }
    heartbeat.Tick();
    ConsoleRefresh();
    svcSleepThread(16'000'000);
  }
  GAME_INFO("exit requested (+)%s", g_stopped.load() ? " after the stop" : "");
  // The runtime is never destroyed: guest threads are still running, and
  // ~Runtime unmaps guest memory and tears the kernel down under them. In
  // run 12 that happened between + and svcExitProcess ("guest memory at
  // exit: 0 blocks mapped", no thread snapshot at exit) and the process still
  // ended in a crash report. svcExitProcess, in main, ends everything at once.
  (void)runtime.release();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const int rc = GameMain(argc, argv);
  GAME_INFO("main returned %d; leaving with svcExitProcess, which ends the guest threads too",
            rc);
  LogExitSummary();
  CloseConsole();
  svcExitProcess();  // wrapped: logged (game.log only: the console is closed)
}
