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
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/audio/null_audio_system.h>
#include <rex/cvar.h>
#include <rex/diagnostics/export_stubs.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

#include "gta4_init.h"


REXCVAR_DEFINE_BOOL(pc_audio, false, "pc-eu-base",
                    "Host audio (SDL). Off by default, as in switch-game, which has no audio "
                    "output: then the null audio system (cvar nx_null_audio) or none");

namespace {

// The guest call chain of a thread from its r1, through the PPC back chain:
// each frame stores the caller's r1 at 0(r1), and the prologue of a non-leaf
// function stores LR at -8 of that caller's r1 ("mflr r12; stw r12,-8(r1);
// stwu r1,-N(r1)"). Only for a thread inside an export, whose guest
// registers do not move. Every address read must lie inside the thread's
// own guest stack, [stack_end, stack_base) from its PCR: run 17 walked off
// the end of a stack without that bound and the heartbeat stopped there.
std::string GuestStack(const PPCContext& ctx, uint32_t stack_end, uint32_t stack_base) {
  auto* memory = rex::system::kernel_state()->memory();
  auto load = [memory](uint32_t address) {
    const uint8_t* p = memory->TranslateVirtual<const uint8_t*>(address);
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
  };
  char item[16];
  std::snprintf(item, sizeof(item), "%08X", uint32_t(ctx.lr));
  std::string out = item;
  uint32_t sp = ctx.r1.u32;
  if (stack_end >= stack_base || sp < stack_end || sp + 4 > stack_base) {
    return out;
  }
  for (int i = 0; i < 24; ++i) {
    const uint32_t prev = load(sp);
    if (prev <= sp || prev + 4 > stack_base || prev - 8 < stack_end || (prev & 3)) break;
    const uint32_t lr = load(prev - 8);
    if (lr < 0x82000000u || lr >= 0x83000000u) break;
    std::snprintf(item, sizeof(item), " %08X", lr);
    out += item;
    sp = prev;
  }
  return out;
}

// Run 15/16: the main thread waits in sub_821200D0, "while (sub_82124490())
// Sleep(1)", i.e. while the bytes at 83137BB7 and 83137BC9 are both set.
// sub_82124080 (from the game's init) sets them; the boot screen sequencer
// sub_821238D0 (drawn and updated by sub_821235B0) clears 83137BC9 when the
// time of the current entry of its table (83137D18, 400 bytes each, index at
// 83137BC0) has run out and the entry's type (+8) is 3. Its time accumulates
// from mftb at 83137BB8, unless the byte at 83137BC8 is set.
void LogBootSequencer() {
  auto* memory = rex::system::kernel_state()->memory();
  auto u8 = [memory](uint32_t a) { return *memory->TranslateVirtual<const uint8_t*>(a); };
  auto u32 = [memory](uint32_t a) {
    const uint8_t* p = memory->TranslateVirtual<const uint8_t*>(a);
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
  };
  auto f32 = [&u32](uint32_t a) {
    const uint32_t v = u32(a);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
  };
  const uint32_t index = u32(0x83137BC0);
  const uint32_t entry = 0x83137D18 + (index < 64 ? index : 0) * 400;
  // The boot sound object (82D09E00): id at +4 (16 bits), sound at +16, whose
  // state sub_822FA498 reads at +6 (16 bits): the entry moves on at state 2.
  const uint32_t sound = u32(0x82D09E00 + 16);
  const int state =
      sound >= 0x40000000u && sound < 0xA0000000u ? int(u32(sound + 4) & 0xFFFF) : -1;
  REXLOG_INFO("boot sequencer: active {} (83137BB7), main waits on {} (83137BC9), paused {} "
              "(83137BC8), index {}, time {:.3f} (delta {:.4f}), scale {:.4f}; entry {:08X}: "
              "{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}; skip word 82B11188 = {:08X}; boot "
              "sound object: id {:04X}, sound {:08X}, state {}",
              u8(0x83137BB7), u8(0x83137BC9), u8(0x83137BC8), index, f32(0x83137BB8),
              f32(0x83137BBC), f32(0x82041714), entry, u32(entry), u32(entry + 4),
              u32(entry + 8), u32(entry + 12), u32(entry + 16), u32(entry + 20),
              u32(0x82B11188), u32(0x82D09E00 + 4) >> 16, sound, state);
  const auto a = rex::audio::GetNullAudioStats();
  REXLOG_INFO("null audio: {} clients, {} callbacks, {} frames submitted, {} played", a.clients,
              a.callbacks, a.frames_submitted, a.frames_played);
}

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
      // As switch-game: the null audio system drives the guest audio engine
      // without output; with nx_null_audio = false, no audio system at all.
      if (REXCVAR_GET(nx_null_audio)) {
        config.audio_factory = [](rex::runtime::FunctionDispatcher* dispatcher) {
          return rex::audio::CreateNullAudioSystem(dispatcher);
        };
      } else {
        config.audio_factory = nullptr;
      }
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
    // cache: and cache1: on working folders, as switch-game does (the game
    // opens cache:\valid.txt at start-up; the runtime leaves them unmounted).
    // Not <user>/cache: that is the runtime's own cache_root.
    runtime()->MountCacheDevices(user_data_root() / "xbox-cache", user_data_root() / "xbox-cache1");
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
      // Every minute the count of each export, so two runs can be compared
      // over time and not only on first calls (06 section 19).
      if (seconds >= next_summary_s_) {
        next_summary_s_ = seconds + 60;
        const std::string when = "at " + std::to_string(seconds) + " s";
        rex::diagnostics::LogExportCallSummary(when.c_str());
      }
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
    LogBootSequencer();
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
      std::string stack;
      for (const auto& x : threads) {
        if (uint64_t(x->handle()) == t.nx_thread_id) {
          name = x->name();
          if (t.current && x->thread_state() && x->thread_state()->context() && x->pcr_ptr()) {
            auto* pcr = rex::system::kernel_state()->memory()->TranslateVirtual<
                rex::system::X_KPCR*>(x->pcr_ptr());
            stack = "; guest stack " + GuestStack(*x->thread_state()->context(),
                                                  pcr->stack_end_ptr, pcr->stack_base_ptr);
          }
        }
      }
      if (t.current) {
        REXLOG_INFO("  thread {:08X} '{}': IN {} for {:.1f} s (guest LR {:08X}), {} export calls{}",
                    uint32_t(t.nx_thread_id), name, t.current, t.seconds_in_current, t.guest_lr,
                    t.calls, stack);
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
  long long next_summary_s_ = 60;
};

}  // namespace

REX_DEFINE_APP(theft4_eu_base, EuBaseApp::Create)
