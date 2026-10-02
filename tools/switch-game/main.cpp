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
// Exit: hold + for 1 s; the process leaves with exit(0) (mode A), or with
// the cooperative stop of mode D (cvar nx_exit_mode = D); never with
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

#include <fcntl.h>
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
#include <rex/kernel/init.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/audio/null_audio_system.h>
#include <rex/system/null_graphics_system.h>
#include <rex/system/thread_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

#include <spdlog/sinks/base_sink.h>

#include "gta4_init.h"
#include "nx_exit.h"
#include "nx_input.h"

REXCVAR_DEFINE_BOOL(nx_null_gpu, true, "Switch",
                    "Give the runtime the null GPU (rex/system/null_graphics_system.h): the "
                    "Vd* exports work and the ring is consumed without drawing. false: the "
                    "run stops at the first graphics export, as up to run 13");
REXCVAR_DEFINE_BOOL(nx_input, true, "Switch",
                    "Give the game the Switch controller as its 360 pad (nx_input.h). false: "
                    "no input system, XamInputGetState says not connected (run 14)");
REXCVAR_DEFINE_STRING(nx_exit_mode, "A", "Switch",
                      "Exit at + (hold 1 s): A = exit(0) with the threads alive, D = every "
                      "thread told to end and joined for 2 s, then exit(0) (nx_exit.h)");

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

void ConsoleRefresh() {
  if (g_console_mutex.try_lock()) {
    if (g_console_open) {
      consoleUpdate(nullptr);
    }
    g_console_mutex.unlock();
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

void DurableWrite(const char* data, size_t size) {
  std::lock_guard<std::timed_mutex> lock(g_log_file_mutex);
  DurableWriteLocked(data, size);
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

// The exit mode chosen at + (0 until then) and its step count. Each step
// goes first to game-exit.log (nx_exit::Step: write + fsync on a descriptor
// opened at start-up), then to game.log.
constexpr const char* kExitStepPath = "sdmc:/switch/theft4/game-exit.log";
char g_exit_mode = 0;
int g_exit_step = 0;

void ExitStep(const char* what) {
  if (!g_exit_mode) {
    return;
  }
  const char* line = theft4::nx_exit::Step(g_exit_mode, ++g_exit_step, what);
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
u64 g_start_tick = 0;
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

// Every 10 s: one line; every 60 s: the call count of each export; at 5, 20,
// 60 s and then every 120 s: the snapshot.
struct Heartbeat {
  u64 start = 0;
  u64 next_line_s = 10;
  u64 next_snapshot_s = 5;
  u64 next_summary_s = 60;

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
      LogClock();
      LogNullGpu();
      LogBootSequencer();
      if (REXCVAR_GET(nx_null_audio)) {
        const auto a = rex::audio::GetNullAudioStats();
        GAME_INFO("null audio: %u clients, %" PRIu64 " callbacks, %" PRIu64
                  " frames submitted, %" PRIu64 " played",
                  a.clients, a.callbacks, a.frames_submitted, a.frames_played);
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
//   A  (default) exit(0), which is what returning from main does (crt0 enters
//      main with LR = exit): atexit handlers, _exit, __libnx_exit (__appExit,
//      then back to hbloader). The game's threads still run.
//   D  (cvar nx_exit_mode = D) every guest thread ends itself at its next
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

[[noreturn]] void ExitGame(const char* reason) {
  const char mode = theft4::nx_exit::ParseExitMode(REXCVAR_GET(nx_exit_mode).c_str());
  g_exit_mode = mode;
  char text[200];
  std::snprintf(text, sizeof(text), "chosen (%s): %s", reason,
                theft4::nx_exit::ExitModeDescription(mode));
  ExitStep(text);
  if (g_logging_ready) {
    LogExitSummary();
  }
  if (mode == 'D') {
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

[[noreturn]] void GameMain(int argc, char** argv) {
  rex::cvar::Init(argc, argv);
  consoleInit(nullptr);
  g_console_open = true;  // still single-threaded
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&g_pad);
  svcGetThreadId(&g_host_main_thread_id, CUR_THREAD_HANDLE);
  g_start_tick = armGetSystemTick();
  ConsolePrint("Theft4 switch-game (no graphics)\n\n");

  StartLogging();
  CaptureTerminations();
  rex::arch::ExceptionHandler::Install(CrashLogHandler, nullptr);
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

  GAME_INFO("about to construct rex::Runtime (game:, d: = %s)", kGameDir);
  const std::filesystem::path root(kRootDir);
  auto runtime = std::make_unique<rex::Runtime>(
      std::filesystem::path(kGameDir), root / "user", std::filesystem::path(kGameDir) / "update",
      root / "runtime-cache", std::filesystem::path{}, root / "marketplace", root / "saves");

  rex::RuntimeConfig config;
  config.tool_mode = false;
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
  LogClock();
  thread->Resume();
  GAME_INFO("main guest thread resumed; waiting (the log records the first stop)");

  // The UI thread: watch for + and for the main guest thread ending; log the
  // heartbeat and the thread snapshots.
  bool seen_running = false;
  Heartbeat heartbeat;
  heartbeat.start = armGetSystemTick();
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
    ConsoleRefresh();
    svcSleepThread(16'000'000);
  }
  ExitGame("the system asked the applet to exit");
}

}  // namespace

int main(int argc, char** argv) {
  GameMain(argc, argv);  // leaves through ExitGame
}
