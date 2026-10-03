/**
 * @file        audio/null_audio_system.cpp
 * @brief       See rex/audio/null_audio_system.h
 */

#include <rex/audio/null_audio_system.h>

#include <atomic>
#include <chrono>
#include <mutex>

#include <rex/audio/xma/decoder.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

REXCVAR_DEFINE_BOOL(nx_null_audio, true, "Audio",
                    "Without an audio output, give the runtime the null audio system "
                    "(rex/audio/null_audio_system.h): the guest audio engine is driven at the "
                    "real rate and its frames are dropped. false: no audio system, "
                    "XAudioRegisterRenderDriverClient fails with C0000002 (runs 13-16)");

namespace rex::audio {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr size_t kMaximumClients = 8;
// Producer credits given at registration. AudioSystem gives the driver's
// RecommendedInitialCredits clamped to audio_maxqframes (64); the Switch
// driver keeps a few frames queued. 8 frames = 43 ms of audio.
constexpr uint32_t kInitialCredits = 8;
// 256 samples per channel at 48 kHz.
constexpr auto kFramePeriod = std::chrono::nanoseconds(1'000'000'000ll * 256 / 48000);

struct Counters {
  std::atomic<uint32_t> clients{0};
  std::atomic<uint64_t> callbacks{0};
  std::atomic<uint64_t> frames_submitted{0};
  std::atomic<uint64_t> frames_played{0};
  std::atomic<uint64_t> callback_ns{0};
  std::atomic<uint64_t> callback_ns_max{0};
  std::atomic<uint64_t> periods{0};
  std::atomic<uint64_t> resyncs{0};
};
Counters g_counters;

class NullAudioSystem;
// The instance created last, for StopNullAudio.
std::atomic<NullAudioSystem*> g_instance{nullptr};

class NullAudioSystem final : public system::IAudioSystem {
 public:
  explicit NullAudioSystem(runtime::FunctionDispatcher* function_dispatcher)
      : function_dispatcher_(function_dispatcher),
        xma_decoder_(std::make_unique<XmaDecoder>(function_dispatcher)) {
    g_instance.store(this);
  }

  ~NullAudioSystem() override {
    NullAudioSystem* self = this;
    g_instance.compare_exchange_strong(self, nullptr);
    Shutdown();
  }

  // StopNullAudio: the worker leaves at its next check of running_.
  bool StopWorker(uint32_t timeout_ms) {
    running_ = false;
    if (!worker_) {
      return true;
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!worker_done_.load()) {
      if (Clock::now() >= deadline) {
        return false;
      }
      rex::thread::Sleep(std::chrono::milliseconds(1));
    }
    return true;
  }

  X_STATUS Setup(system::KernelState* kernel_state) override {
    kernel_state_ = kernel_state;
    const X_STATUS result = xma_decoder_->Setup(kernel_state);
    if (result) {
      return result;
    }
    running_ = true;
    worker_ = system::object_ref<system::XHostThread>(
        new system::XHostThread(kernel_state, 128 * 1024, 0, [this]() {
          WorkerMain();
          return 0;
        }));
    worker_->set_name("Audio Worker (null)");
    worker_->Create();
    REXAPU_INFO("null audio: render clients driven at 48 kHz in 256-sample frames, no output");
    return X_STATUS_SUCCESS;
  }

  void Shutdown() override {
    if (shut_down_.exchange(true)) {
      return;
    }
    running_ = false;
    if (worker_) {
      worker_->Wait(0, 0, 0, nullptr);
      worker_.reset();
    }
    if (xma_decoder_) {
      xma_decoder_->Shutdown();
    }
  }

  X_STATUS RegisterClient(uint32_t callback, uint32_t callback_arg, size_t* out_index) override {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < kMaximumClients; ++i) {
      Client& c = clients_[i];
      if (c.in_use) {
        continue;
      }
      // The callback gets a pointer to its argument, as AudioSystem passes it.
      auto* memory = function_dispatcher_->memory();
      const uint32_t wrapped = memory->SystemHeapAlloc(0x4);
      memory::store_and_swap<uint32_t>(memory->TranslateVirtual(wrapped), callback_arg);
      c = Client{};
      c.in_use = true;
      c.callback = callback;
      c.wrapped_arg = wrapped;
      c.credits = kInitialCredits;
      g_counters.clients.fetch_add(1, std::memory_order_relaxed);
      REXAPU_INFO("null audio: client {} registered, callback {:08X}, argument {:08X}, {} credits",
                  i, callback, callback_arg, kInitialCredits);
      if (out_index) {
        *out_index = i;
      }
      return X_STATUS_SUCCESS;
    }
    return X_STATUS_UNSUCCESSFUL;
  }

  // Waits for a callback in progress (the worker holds dispatch_mutex_ while
  // it runs one), so the guest engine is never called after it unregistered.
  void UnregisterClient(size_t index) override {
    std::lock_guard<std::recursive_mutex> dispatch(dispatch_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= kMaximumClients || !clients_[index].in_use) {
      return;
    }
    Client& c = clients_[index];
    function_dispatcher_->memory()->SystemHeapFree(c.wrapped_arg);
    c = Client{};
    g_counters.clients.fetch_sub(1, std::memory_order_relaxed);
    REXAPU_INFO("null audio: client {} unregistered", index);
  }

  void SubmitFrame(size_t index, uint32_t) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index < kMaximumClients && clients_[index].in_use) {
      ++clients_[index].queued;
    }
    g_counters.frames_submitted.fetch_add(1, std::memory_order_relaxed);
  }

  XmaDecoder* xma_decoder() override { return xma_decoder_.get(); }

 private:
  struct Client {
    bool in_use = false;
    uint32_t callback = 0;
    uint32_t wrapped_arg = 0;
    uint32_t credits = 0;  // callbacks owed: one per frame the game may still submit
    uint32_t queued = 0;   // frames submitted and not yet "played"
  };

  // Each period one queued frame of every client is played, which gives the
  // client one credit back; every credit is one call of the guest callback,
  // which submits the next frame. The same contract as AudioSystem's client
  // semaphores with a driver releasing one unit per frame played. mutex_ is
  // never held while guest code runs: the callback submits frames
  // (SubmitFrame takes it), and another guest thread holding a guest lock the
  // callback wants may be submitting too.
  void WorkerMain() {
    auto next = Clock::now();
    while (running_) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        for (Client& c : clients_) {
          if (c.in_use && c.queued) {
            --c.queued;
            ++c.credits;
            g_counters.frames_played.fetch_add(1, std::memory_order_relaxed);
          }
        }
      }
      for (size_t i = 0; i < kMaximumClients && running_; ++i) {
        for (;;) {
          std::lock_guard<std::recursive_mutex> dispatch(dispatch_mutex_);
          uint32_t callback = 0, wrapped_arg = 0;
          {
            std::lock_guard<std::mutex> lock(mutex_);
            Client& c = clients_[i];
            if (!running_ || !c.in_use || !c.credits) {
              break;
            }
            --c.credits;
            callback = c.callback;
            wrapped_arg = c.wrapped_arg;
          }
          uint64_t args[] = {wrapped_arg};
          const auto start = Clock::now();
          function_dispatcher_->Execute(worker_->thread_state(), callback, args, 1);
          const uint64_t ns = uint64_t(
              std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
          g_counters.callbacks.fetch_add(1, std::memory_order_relaxed);
          g_counters.callback_ns.fetch_add(ns, std::memory_order_relaxed);
          uint64_t seen = g_counters.callback_ns_max.load(std::memory_order_relaxed);
          while (ns > seen && !g_counters.callback_ns_max.compare_exchange_weak(
                                  seen, ns, std::memory_order_relaxed)) {
          }
        }
      }
      g_counters.periods.fetch_add(1, std::memory_order_relaxed);
      next += kFramePeriod;
      const auto now = Clock::now();
      if (now > next + 100ms) {
        next = now;  // far behind (a long callback): no burst to catch up
        g_counters.resyncs.fetch_add(1, std::memory_order_relaxed);
      }
      if (next > now) {
        rex::thread::Sleep(std::chrono::duration_cast<std::chrono::microseconds>(next - now));
      }
    }
    worker_done_ = true;
  }

  runtime::FunctionDispatcher* function_dispatcher_ = nullptr;
  system::KernelState* kernel_state_ = nullptr;
  std::unique_ptr<XmaDecoder> xma_decoder_;
  system::object_ref<system::XHostThread> worker_;
  std::atomic<bool> running_{false};
  std::atomic<bool> shut_down_{false};
  std::atomic<bool> worker_done_{false};
  std::mutex mutex_;                     // clients_
  std::recursive_mutex dispatch_mutex_;  // held while a guest callback runs
  Client clients_[kMaximumClients];
};

}  // namespace

std::unique_ptr<system::IAudioSystem> CreateNullAudioSystem(
    runtime::FunctionDispatcher* function_dispatcher) {
  return std::make_unique<NullAudioSystem>(function_dispatcher);
}

NullAudioStats GetNullAudioStats() {
  NullAudioStats s;
  s.clients = g_counters.clients.load(std::memory_order_relaxed);
  s.callbacks = g_counters.callbacks.load(std::memory_order_relaxed);
  s.frames_submitted = g_counters.frames_submitted.load(std::memory_order_relaxed);
  s.frames_played = g_counters.frames_played.load(std::memory_order_relaxed);
  s.callback_ns = g_counters.callback_ns.load(std::memory_order_relaxed);
  s.callback_ns_max = g_counters.callback_ns_max.exchange(0, std::memory_order_relaxed);
  s.periods = g_counters.periods.load(std::memory_order_relaxed);
  s.resyncs = g_counters.resyncs.load(std::memory_order_relaxed);
  return s;
}

bool StopNullAudio(uint32_t timeout_ms) {
  NullAudioSystem* instance = g_instance.load();
  return instance ? instance->StopWorker(timeout_ms) : true;
}

}  // namespace rex::audio
