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
// game-crash.log (registers, PC and LR as offsets from the module base) before
// it goes back to the kernel, which writes Atmosphere's crash report.
//
// Exit is svcExitProcess: guest threads are still running, and returning to
// the loader would unload code they execute (04-threads-exit.md section 5).

#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>

#include <rex/cvar.h>
#include <rex/diagnostics/missing_function.h>
#include <rex/diagnostics/policy.h>
#include <rex/exception_handler.h>
#include <rex/kernel/init.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xthread.h>

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
u64 g_host_main_thread_id = 0;
int g_crash_fd = -1;

}  // namespace

// Start of the NRO image (see switch-smoke/main.cpp, StepModuleBase).
extern "C" char _start;

namespace {

// ── Durable log (as in switch-smoke) ───────────────────────────────────────

std::mutex g_log_file_mutex;

void DurableWrite(const char* data, size_t size) {
  std::lock_guard<std::mutex> lock(g_log_file_mutex);
  int fd = open(kLogPath, O_WRONLY | O_APPEND | O_CREAT, 0666);
  if (fd < 0) {
    static bool reported = false;
    if (!reported) {
      reported = true;
      std::printf("game.log: open failed (errno %d); lines are on screen only\n", errno);
      consoleUpdate(nullptr);
    }
    return;
  }
  (void)write(fd, data, size);
  fsync(fd);
  close(fd);
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
  std::printf("%s\n", line + std::strlen("[game] "));
  consoleUpdate(nullptr);
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
// (MMIO, physical write watch). Records the fault and lets it go to the
// kernel.
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
  GAME_INFO("module base (_start) 0x%" PRIx64 ": ELF address = runtime address - base",
            g_module_base);
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
  GAME_INFO("guest memory %s: %zu blocks mapped (%zu MiB, peak %zu), %zu pages committed",
            when, s.mapped_blocks, s.mapped_blocks * 2, s.peak_mapped_blocks, s.committed_pages);
}

[[noreturn]] void WaitForPlusAndExit(PadState* pad) {
  std::printf("\nPress + to exit.\n");
  consoleUpdate(nullptr);
  while (appletMainLoop()) {
    padUpdate(pad);
    if (padGetButtonsDown(pad) & HidNpadButton_Plus) {
      break;
    }
    consoleUpdate(nullptr);
    svcSleepThread(16'000'000);
  }
  GAME_INFO("exit (svcExitProcess)");
  rex::FlushLogging();
  consoleExit(nullptr);
  svcExitProcess();
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

int main(int argc, char** argv) {
  rex::cvar::Init(argc, argv);
  consoleInit(nullptr);
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&g_pad);
  svcGetThreadId(&g_host_main_thread_id, CUR_THREAD_HANDLE);
  std::printf("Theft4 switch-game (no graphics)\n\n");
  consoleUpdate(nullptr);

  StartLogging();
  rex::arch::ExceptionHandler::Install(CrashLogHandler, nullptr);
  if (!CheckGameFiles()) {
    WaitForPlusAndExit(&g_pad);
  }

  GAME_INFO("about to construct rex::Runtime (game:, d: = %s)", kGameDir);
  const std::filesystem::path root(kRootDir);
  auto runtime = std::make_unique<rex::Runtime>(
      std::filesystem::path(kGameDir), root / "user", std::filesystem::path(kGameDir) / "update",
      root / "cache", std::filesystem::path{}, root / "marketplace", root / "saves");

  rex::RuntimeConfig config;
  config.tool_mode = false;  // no graphics object: "Runtime initialized without graphics system"
  config.kernel_init = rex::kernel::InitializeKernel;
  GAME_INFO("about to Runtime::Setup(PPCImageConfig): code %08X+%X, image %08X+%X",
            uint32_t(PPCImageConfig.code_base), uint32_t(PPCImageConfig.code_size),
            uint32_t(PPCImageConfig.image_base), uint32_t(PPCImageConfig.image_size));
  rex::X_STATUS status = runtime->Setup(PPCImageConfig, std::move(config));
  if (XFAILED(status)) {
    GAME_ERROR("Runtime::Setup failed, status %08X (see the [sdk] lines above)", status);
    WaitForPlusAndExit(&g_pad);
  }
  LogArena("after Setup");

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

  // The UI thread: watch for + and for the main guest thread ending.
  bool seen_running = false;
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
    consoleUpdate(nullptr);
    svcSleepThread(16'000'000);
  }
  GAME_INFO("exit requested (+)%s", g_stopped.load() ? " after the stop" : "");
  rex::FlushLogging();
  consoleExit(nullptr);
  svcExitProcess();
}
