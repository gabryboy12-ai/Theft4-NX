// pc-eu-base: the Switch game's generated code (config-eu-base, European base
// default.xex, option C) run on Windows with ReXGlue's own graphics (the xenos
// plugin on Vulkan), to separate errors of the translation from errors of the
// Switch platform (docs/switch-port/06-game-boot-plan.md section 14; the
// order nfsmw-nx's porting-another-game.md recommends). No hook, no TU: the
// same code switch-game runs.
//
//   theft4-eu-base --game_data_root="D:/.../Grand Theft Auto IV (Europe) ..."
//                  --log_file=theft4-eu-base.log
//
// Like switch-game, every implemented export is traced (REX_TRACE_EXPORTS,
// set by CMakeLists.txt): the log gets the first call of each export, a
// heartbeat every 10 s with the export each thread is inside, and the call
// counts at exit, so the two logs can be compared export by export.

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/diagnostics/export_stubs.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>

#include "gta4_init.h"


REXCVAR_DEFINE_BOOL(pc_audio, false, "pc-eu-base",
                    "Host audio (SDL). Off by default, as in switch-game, which runs without an "
                    "audio system: XAudioRegisterRenderDriverClient then fails the same way on both");

namespace {

class EuBaseApp final : public rex::ReXApp {
 public:
  ~EuBaseApp() override { StopHeartbeat(); }

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& context) {
    return std::unique_ptr<EuBaseApp>(new EuBaseApp(context));
  }

 private:
  explicit EuBaseApp(rex::ui::WindowedAppContext& context)
      : ReXApp(context, "theft4-eu-base", PPCImageConfig) {}

  void OnPreSetup(rex::RuntimeConfig& config) override {
    // ReXGlue's emulated GPU, as in phase (a) of 06 section 11. SetupPresentation
    // has already copied the cvar into config, so the default goes there.
    if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "xenos";
    }
    if (!REXCVAR_GET(pc_audio)) {
      config.audio_factory = nullptr;
      return;
    }
    // SDL's audio subsystem on the main thread, now: the audio driver is
    // created later on the guest thread that registers the audio client,
    // while it holds the kernel's global lock, and asking the main thread
    // from there deadlocked (PC run of 2026-09-30: the game's main thread
    // stayed in XAudioRegisterRenderDriverClient).
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
      REXLOG_WARN("pc-eu-base: SDL_InitSubSystem(SDL_INIT_AUDIO) failed: {}", SDL_GetError());
    }
  }

  void OnPostSetup() override {
    REXLOG_INFO("pc-eu-base: generated code {:08X}+{:X}, image {:08X}+{:X}; exports traced",
                uint32_t(PPCImageConfig.code_base), uint32_t(PPCImageConfig.code_size),
                uint32_t(PPCImageConfig.image_base), uint32_t(PPCImageConfig.image_size));
    heartbeat_ = std::thread([this] { HeartbeatMain(); });
  }

  void OnShutdown() override {
    StopHeartbeat();
    rex::diagnostics::LogExportCallSummary("at exit");
    rex::diagnostics::LogExportStubSummary("at exit");
  }

  // Joined, never detached (docs/switch-port/09-rules.md, rule 3).
  void StopHeartbeat() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    if (heartbeat_.joinable()) {
      heartbeat_.join();
    }
  }

  void HeartbeatMain() {
    const auto start = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(mutex_);
    while (!cv_.wait_for(lock, std::chrono::seconds(10), [this] { return stop_; })) {
      lock.unlock();
      const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::steady_clock::now() - start)
                               .count();
      LogHeartbeat(seconds);
      lock.lock();
    }
  }

  // Export totals, then one line per thread that has called an export, with
  // the XThread's name when the thread is one (the trace records the thread
  // id the runtime set, i.e. the XThread handle).
  static void LogHeartbeat(long long seconds) {
    size_t distinct = 0;
    uint64_t calls = 0;
    rex::diagnostics::GetExportCallTotals(&distinct, &calls);
    REXLOG_INFO("heartbeat {} s: exports {} distinct, {} calls", seconds, distinct, calls);
    rex::diagnostics::ExportThreadSnapshot traces[64];
    const size_t n = rex::diagnostics::SnapshotExportThreads(traces, 64);
    // Only tries: a guest thread may hold the kernel's global lock for good
    // (the first PC runs stopped here after one heartbeat).
    std::vector<rex::system::object_ref<rex::system::XThread>> threads;
    if (auto* kernel = rex::system::kernel_state()) {
      if (!kernel->object_table()->TryGetObjectsByType<rex::system::XThread>(
              &threads, std::chrono::milliseconds(200))) {
        REXLOG_INFO("  (kernel global lock busy for 200 ms: thread names unavailable)");
      }
    }
    for (size_t i = 0; i < n; ++i) {
      const auto& t = traces[i];
      std::string name = "?";
      for (const auto& x : threads) {
        if (uint64_t(x->handle()) == t.nx_thread_id) {
          name = x->name();
        }
      }
      if (t.current) {
        REXLOG_INFO("  thread {:08X} '{}': IN {} for {:.1f} s (guest LR {:08X}), {} export calls",
                    uint32_t(t.nx_thread_id), name, t.current, t.seconds_in_current, t.guest_lr,
                    t.calls);
      } else {
        REXLOG_INFO("  thread {:08X} '{}': in guest code, last export {} (guest LR {:08X}), {} "
                    "export calls",
                    uint32_t(t.nx_thread_id), name, t.last ? t.last : "-", t.guest_lr, t.calls);
      }
    }
  }

  std::thread heartbeat_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
};

}  // namespace

REX_DEFINE_APP(theft4_eu_base, EuBaseApp::Create)
