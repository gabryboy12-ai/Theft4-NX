/**
 * @file        core/threading_posix.cpp
 * @brief       POSIX platform threading implementations
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>
#include <rex/thread.h>
#include <rex/thread/nx_multi_wait.h>
#include <rex/thread/nx_guest_audio_role.h>

static_assert(REX_PLATFORM_NX, "This file is Switch-only");

#include <switch.h>             // threadGetCurHandle, svc*, UEvent, threadPause/Resume

// Map a generic thread priority value to Switch HOS priority range.
// Switch uses 0x00-0x3F where lower = higher priority. Default is 0x2C (44).
static inline u32 MapToSwitchPriority(int priority) {
  if (priority <= 0) return 0x2C;
  if (priority >= 10) return 0x20;  // high priority (audio/real-time)
  u32 mapped = 0x2C - static_cast<u32>(priority);
  if (mapped < 0x20) mapped = 0x20;
  return mapped;
}

#include <atomic>
#include <array>
#include <cerrno>
#include <cstddef>
#include <ctime>
#include <deque>
#include <limits>
#include <memory>

#include <pthread.h>
#include <semaphore.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <rex/assert.h>
#include <rex/chrono/chrono_steady_cast.h>
#include <rex/logging.h>
#include <rex/thread/timer_queue.h>

#include <sched.h>

#if REX_PLATFORM_ANDROID
#include <dlfcn.h>

#include <rex/main_android.h>
#include <rex/string/util.h>
#endif

#if REX_PLATFORM_LINUX
// SIGEV_THREAD_ID in timer_create(...) is a Linux extension
#define REX_HAS_SIGEV_THREAD_ID 1
#ifdef __GLIBC__
#define sigev_notify_thread_id _sigev_un._tid
#endif
#if __GLIBC__ == 2 && __GLIBC_MINOR__ < 30
#define gettid() syscall(SYS_gettid)
#endif
#else
#define REX_HAS_SIGEV_THREAD_ID 0
#endif

namespace rex::thread {

std::atomic<uint64_t> g_nx_threads_created{0}, g_nx_threads_finished{0}, g_nx_threads_joined{0},
    g_nx_threads_abandoned{0};

NxThreadCounters GetNxThreadCounters() {
  NxThreadCounters c;
  c.created = g_nx_threads_created.load(std::memory_order_relaxed);
  c.finished = g_nx_threads_finished.load(std::memory_order_relaxed);
  c.joined = g_nx_threads_joined.load(std::memory_order_relaxed);
  c.abandoned = g_nx_threads_abandoned.load(std::memory_order_relaxed);
  return c;
}

#if REX_PLATFORM_ANDROID
// May be null if no dynamically loaded functions are required.
static void* android_libc_;
// API 26+.
static int (*android_pthread_getname_np_)(pthread_t pthread, char* buf, size_t n);

void AndroidInitialize() {
  if (rex::GetAndroidApiLevel() >= 26) {
    android_libc_ = dlopen("libc.so", RTLD_NOW);
    assert_not_null(android_libc_);
    if (android_libc_) {
      android_pthread_getname_np_ = reinterpret_cast<decltype(android_pthread_getname_np_)>(
          dlsym(android_libc_, "pthread_getname_np"));
      assert_not_null(android_pthread_getname_np_);
    }
  }
}

void AndroidShutdown() {
  android_pthread_getname_np_ = nullptr;
  if (android_libc_) {
    dlclose(android_libc_);
    android_libc_ = nullptr;
  }
}
#endif

template <typename _Rep, typename _Period>
inline timespec DurationToTimeSpec(std::chrono::duration<_Rep, _Period> duration) {
  auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(duration);
  auto div = ldiv(nanoseconds.count(), 1000000000L);
  return timespec{div.quot, div.rem};
}

// Switch (libnx/newlib) has no POSIX real-time signals, SA_SIGINFO, or
// sa_sigaction. On NX we use libnx kernel APIs instead (svcSetThreadActivity
// for suspend, cooperative polling for user callbacks).
enum class SignalType {
  kThreadSuspend,
  kThreadUserCallback,
  k_Count
};

// Stubs kept so timer_queue and other code that references these names still
// compiles; none of them are ever invoked on Switch.
int GetSystemSignal(SignalType) { return 0; }
SignalType GetSystemSignalType(int) { return SignalType::kThreadSuspend; }
std::array<std::atomic<bool>, static_cast<size_t>(SignalType::k_Count)> signal_handler_installed = {};
void install_signal_handler(SignalType) {}

// TODO(dougvj)
void EnableAffinityConfiguration() {}

namespace {
constexpr uint64_t kNxGuestCores = 0x7;       // cores 0-2
constexpr uint64_t kNxHostWorkerCores = 0x8;  // core 3

uint64_t NxProcessCoreMask() {
  u64 mask = 0;
  svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
  return mask;
}
}  // namespace

uint64_t nx_guest_core_mask() { return NxProcessCoreMask() & kNxGuestCores; }

uint64_t nx_host_worker_core_mask() { return NxProcessCoreMask() & kNxHostWorkerCores; }

namespace {
NxGuestAudioRole g_nx_guest_audio_role;
}

void nx_set_audio_guest_entry(uint32_t entry) {
  g_nx_guest_audio_role.SetEntry(entry);
}

int32_t nx_priority_for_thread(bool guest_thread, std::string_view name) {
  return nx_priority_for_thread(guest_thread, name, 0);
}

int32_t nx_priority_for_thread(bool guest_thread, std::string_view name,
                              uint32_t guest_entry) {
  if (guest_thread) {
    // The game's dedicated mixer blocks on its processing event after each
    // packet. Give it the same deadline band as the native audio producer;
    // keep all ordinary or spinning guest workers in the time-sliced band.
    if (g_nx_guest_audio_role.Matches(true, guest_entry)) return kNxPriorityAudio;
    return kNxPriorityGuest;
  }
  // Audio has the hardest deadline (a 10.7 ms packet ring in the game's XAudio
  // voice): it must preempt everything else the runtime runs.
  if (name.starts_with("Audio Worker") || name.starts_with("XMA Decoder")) {
    return kNxPriorityAudio;
  }
  // The command processor does bulk CPU work per frame; below vblank and host
  // tasks so it cannot starve them, above the guests that wait on it.
  if (name.starts_with("GPU Commands")) {
    return kNxPriorityGpu;
  }
  // Pipeline creation and cache writing are bulk work: time-sliced with the
  // guests instead of holding a core against them.
  if (name.starts_with("Vulkan Pipelines") || name.starts_with("D3D12 Pipelines") ||
      name.starts_with("Shader Translation") || name.starts_with("Vulkan Storage writer") ||
      name.starts_with("D3D12 Storage writer")) {
    return kNxPriorityGuest;
  }
  // GPU VSync, Kernel Host Tasks and every other host thread: short work that
  // guest threads wait on.
  return kNxPriorityHost;
}

uint32_t nx_guest_cpu_core(uint32_t cpu_index) {
  const uint32_t core = cpu_index / 2;  // 0,1 -> 0; 2,3 -> 1; 4,5 -> 2
  const uint64_t guest_mask = nx_guest_core_mask();
  if (guest_mask & (uint64_t(1) << core)) {
    return core;
  }
  return guest_mask ? uint32_t(__builtin_ctzll(guest_mask)) : 0;
}

// uint64_t ticks() { return mach_absolute_time(); }

uint32_t current_thread_system_id() {
  // libnx exposes the raw kernel Handle for the current thread via
  // threadGetCurHandle(). The handle is a 32-bit kernel object ID.
  return static_cast<uint32_t>(threadGetCurHandle());
}

void MaybeYield() {
  sched_yield();
  __sync_synchronize();
}

void SyncMemory() {
  __sync_synchronize();
}

void Sleep(std::chrono::microseconds duration) {
  timespec rqtp = DurationToTimeSpec(duration);
  timespec rmtp = {};
  auto p_rqtp = &rqtp;
  auto p_rmtp = &rmtp;
  int ret = 0;
  do {
    ret = nanosleep(p_rqtp, p_rmtp);
    // Swap requested for remaining in case of signal interruption
    // in which case, we start sleeping again for the remainder
    std::swap(p_rqtp, p_rmtp);
  } while (ret == -1 && errno == EINTR);
}

// TODO(bwrsandman) Implement by allowing alert interrupts from IO operations
thread_local bool alertable_state_ = false;
bool DispatchCurrentThreadUserCallback();
SleepResult AlertableSleep(std::chrono::microseconds duration) {
  alertable_state_ = true;
  auto deadline = std::chrono::steady_clock::now() + duration;
  while (true) {
    if (DispatchCurrentThreadUserCallback()) {
      alertable_state_ = false;
      return SleepResult::kAlerted;
    }
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      alertable_state_ = false;
      return SleepResult::kSuccess;
    }
    auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
    Sleep(std::min(remaining, std::chrono::microseconds(1000)));
  }
}

TlsHandle AllocateTlsHandle() {
  auto key = static_cast<pthread_key_t>(-1);
  auto res = pthread_key_create(&key, nullptr);
  assert_zero(res);
  assert_true(key != static_cast<pthread_key_t>(-1));
  return static_cast<TlsHandle>(key);
}

bool FreeTlsHandle(TlsHandle handle) {
  return pthread_key_delete(static_cast<pthread_key_t>(handle)) == 0;
}

uintptr_t GetTlsValue(TlsHandle handle) {
  return reinterpret_cast<uintptr_t>(pthread_getspecific(static_cast<pthread_key_t>(handle)));
}

bool SetTlsValue(TlsHandle handle, uintptr_t value) {
  return pthread_setspecific(static_cast<pthread_key_t>(handle), reinterpret_cast<void*>(value)) ==
         0;
}

using PosixConditionBase = detail::NxConditionBase<WaitResult>;

// There really is no native POSIX handle for a single wait/signal construct
// pthreads is at a lower level with more handles for such a mechanism.
// This simple wrapper class functions as our handle and uses conditional
// variables for waits and signals.
template <typename T>
class PosixCondition {};

template <>
class PosixCondition<Event> : public PosixConditionBase {
 public:
  PosixCondition(bool manual_reset, bool initial_state)
      : signal_(initial_state), manual_reset_(manual_reset) {}
  virtual ~PosixCondition() = default;

  bool Signal() override {
    if (nx_sync::LockFreeRelease().load(std::memory_order_relaxed)) {
      // C5: set without the mutex; wake only if someone may be waiting.
      signal_.store(true, std::memory_order_seq_cst);
      WakeSleepers(!manual_reset_);
      return true;
    }
    auto lock = std::unique_lock<std::mutex>(mutex_);
    signal_.store(true, std::memory_order_seq_cst);
    // An auto-reset event lets exactly one waiter through.
    NotifySignaledLocked(!manual_reset_);
    return true;
  }

  void Reset() {
    auto lock = std::unique_lock<std::mutex>(mutex_);
    signal_.store(false, std::memory_order_seq_cst);
  }

 private:
  // Consumers (post_execution) run under mutex_; producers may not.
  inline bool signaled() const override { return signal_.load(std::memory_order_seq_cst); }
  inline void post_execution() override {
    if (!manual_reset_) {
      signal_.store(false, std::memory_order_seq_cst);
    }
  }
  std::atomic<bool> signal_;
  const bool manual_reset_;
};

template <>
class PosixCondition<Semaphore> : public PosixConditionBase {
 public:
  PosixCondition(uint32_t initial_count, uint32_t maximum_count)
      : count_(initial_count), maximum_count_(maximum_count) {}

  bool Signal() override { return Release(1, nullptr); }

  bool Release(uint32_t release_count, int* out_previous_count) {
    if (nx_sync::LockFreeRelease().load(std::memory_order_relaxed)) {
      // C5: GTA IV's job dispatcher (main thread) releases one count per job.
      // It used to block on this mutex for half its time, behind job threads
      // that had just woken on the same semaphore. Producers only add
      // (atomically); consumers subtract under the mutex after checking the
      // count there, so the count never underflows.
      uint32_t count = count_.load(std::memory_order_relaxed);
      do {
        if (release_count > maximum_count_ - count) {
          return false;
        }
      } while (!count_.compare_exchange_weak(count, count + release_count,
                                             std::memory_order_seq_cst,
                                             std::memory_order_relaxed));
      if (out_previous_count) {
        *out_previous_count = count;
      }
      WakeSleepers(release_count == 1);
      return true;
    }
    auto lock = std::unique_lock<std::mutex>(mutex_);
    const uint32_t count = count_.load(std::memory_order_relaxed);
    if (release_count > maximum_count_ - count) {
      return false;
    }
    if (out_previous_count) {
      *out_previous_count = count;
    }
    count_.fetch_add(release_count, std::memory_order_seq_cst);
    NotifySignaledLocked(release_count == 1);
    return true;
  }

 private:
  inline bool signaled() const override { return count_.load(std::memory_order_seq_cst) > 0; }
  inline void post_execution() override {
    count_.fetch_sub(1, std::memory_order_seq_cst);
    // The waiter that consumed a count passes the rest on (PassOnLocked); with
    // the fast path off this is the original wake-everyone behaviour.
    if (!nx_sync::NotifyOne().load(std::memory_order_relaxed)) {
      cond_.notify_all();
    }
  }
  std::atomic<uint32_t> count_;
  const uint32_t maximum_count_;
};

template <>
class PosixCondition<Mutant> : public PosixConditionBase {
 public:
  explicit PosixCondition(bool initial_owner) : count_(0) {
    if (initial_owner) {
      count_ = 1;
      owner_ = std::this_thread::get_id();
    }
  }

  bool Signal() override { return Release(); }

  bool Release() {
    if (owner_ == std::this_thread::get_id() && count_ > 0) {
      auto lock = std::unique_lock<std::mutex>(mutex_);
      --count_;
      // Free to be acquired by another thread
      if (count_ == 0) {
        NotifySignaledLocked(true);
      }
      return true;
    }
    return false;
  }

  void* native_handle() const override { return const_cast<std::mutex&>(mutex_).native_handle(); }

 private:
  inline bool signaled() const override {
    return count_ == 0 || owner_ == std::this_thread::get_id();
  }
  inline void post_execution() override {
    count_++;
    owner_ = std::this_thread::get_id();
  }
  uint32_t count_;
  std::thread::id owner_;
};

template <>
class PosixCondition<Timer> : public PosixConditionBase {
 public:
  explicit PosixCondition(bool manual_reset)
      : callback_(nullptr), signal_(false), manual_reset_(manual_reset) {}

  virtual ~PosixCondition() { Cancel(); }

  bool Signal() override {
    std::lock_guard<std::mutex> lock(mutex_);
    signal_ = true;
    NotifySignaledLocked(!manual_reset_);
    return true;
  }

  void SetOnce(std::chrono::steady_clock::time_point due_time, std::function<void()> opt_callback) {
    Cancel();

    std::lock_guard<std::mutex> lock(mutex_);

    callback_ = std::move(opt_callback);
    signal_ = false;
    wait_item_ = QueueTimerOnce(&CompletionRoutine, this, due_time);
  }

  void SetRepeating(std::chrono::steady_clock::time_point due_time,
                    std::chrono::milliseconds period, std::function<void()> opt_callback) {
    Cancel();

    std::lock_guard<std::mutex> lock(mutex_);

    callback_ = std::move(opt_callback);
    signal_ = false;
    wait_item_ = QueueTimerRecurring(&CompletionRoutine, this, due_time, period);
  }

  void Cancel() {
    if (auto wait_item = wait_item_.lock()) {
      wait_item->Disarm();
    }
  }

  void* native_handle() const override {
    assert_always();
    return nullptr;
  }

 private:
  static void CompletionRoutine(void* userdata) {
    assert_not_null(userdata);
    auto timer = reinterpret_cast<PosixCondition<Timer>*>(userdata);
    timer->Signal();
    // As the callback may reset the timer, store local.
    std::function<void()> callback;
    {
      std::lock_guard<std::mutex> lock(timer->mutex_);
      callback = timer->callback_;
    }
    if (callback) {
      callback();
    }
  }

 private:
  inline bool signaled() const override { return signal_; }
  inline void post_execution() override {
    if (!manual_reset_) {
      signal_ = false;
    }
  }
  std::weak_ptr<TimerQueueWaitItem> wait_item_;
  std::function<void()> callback_;
  bool signal_;  // Protected by mutex_
  const bool manual_reset_;
};

struct ThreadStartData {
  std::function<void()> start_routine;
  bool create_suspended;
  Thread* thread_obj;
};

template <>
class PosixCondition<Thread> : public PosixConditionBase {
  enum class State {
    kUninitialized,
    kRunning,
    kSuspended,
    kFinished,
  };

 public:
  PosixCondition()
      : thread_(0),
        signaled_(false),
        exit_code_(0),
        state_(State::kUninitialized),
        suspend_count_(0) {
    sem_init(&suspend_sem_, 0, 0);
#if REX_PLATFORM_ANDROID
    android_pre_api_26_name_[0] = '\0';
#endif
  }
  bool Initialize(Thread::CreationParameters params, ThreadStartData* start_data) {
    start_data->create_suspended = params.create_suspended;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0)
      return false;
    if (pthread_attr_setstacksize(&attr, params.stack_size) != 0) {
      pthread_attr_destroy(&attr);
      return false;
    }
    if (params.initial_priority != 0) {
      // Switch: pthread scheduling policies (SCHED_FIFO, pthread_attr_setschedpolicy)
      // are not available in newlib. Defer priority to post-creation via
      // svcSetThreadPriority, applied in ThreadStartRoutine once the Handle is known.
      nx_deferred_priority_ = params.initial_priority;
    }
    if (pthread_create(&thread_, &attr, ThreadStartRoutine, start_data) != 0) {
      pthread_attr_destroy(&attr);
      return false;
    }
    g_nx_threads_created.fetch_add(1, std::memory_order_relaxed);
    pthread_attr_destroy(&attr);
    return true;
  }

  /// Constructor for existing thread. This should only happen once called by
  /// Thread::GetCurrentThread() on the main thread
  explicit PosixCondition(pthread_t thread)
      : thread_(thread),
        signaled_(false),
        exit_code_(0),
        state_(State::kRunning),
        suspend_count_(0) {
    sem_init(&suspend_sem_, 0, 0);
    // For the main thread this constructor runs in-thread, so we can grab the handle now.
    nx_handle_ = threadGetCurHandle();
  }

  virtual ~PosixCondition() {
    // Match Canary/Edge behavior.
    // Force-cancel/join from the condition destructor can self-join/crash
    // depending on shutdown ordering, so threads must be stopped explicitly.
  }

  bool Signal() override { return true; }

  std::string name() const {
    WaitStarted();
    auto result = std::array<char, 17>{'\0'};
    std::unique_lock<std::mutex> lock(state_mutex_);
    if (state_ != State::kUninitialized && state_ != State::kFinished) {
#if REX_PLATFORM_ANDROID
      // pthread_getname_np was added in API 26 - below that, store the name in
      // this object, which may be only modified through Xenia threading, but
      // should be enough in most cases.
      if (android_pthread_getname_np_) {
        if (android_pthread_getname_np_(thread_, result.data(), result.size() - 1) != 0) {
          assert_always();
        }
      } else {
        std::lock_guard<std::mutex> lock(android_pre_api_26_name_mutex_);
        std::strcpy(result.data(), android_pre_api_26_name_);
      }
#else
      if (pthread_getname_np(thread_, result.data(), result.size() - 1) != 0) {
        assert_always();
      }
#endif
    }
    return std::string(result.data());
  }

  void set_name(const std::string& name) {
    WaitStarted();
    std::unique_lock<std::mutex> lock(state_mutex_);
    if (state_ != State::kUninitialized && state_ != State::kFinished) {
#if REX_PLATFORM_NX
      // libnx has no pthread_setname_np (declared, not implemented) and
      // Horizon threads carry no user-visible name. The name is kept in
      // Thread::name_ by PosixThread::set_name.
      (void)name;
#else
      pthread_setname_np(thread_, std::string(name).c_str());
#endif
#if REX_PLATFORM_ANDROID
      SetAndroidPreApi26Name(name);
#endif
    }
  }

#if REX_PLATFORM_ANDROID
  void SetAndroidPreApi26Name(const std::string_view name) {
    if (android_pthread_getname_np_) {
      return;
    }
    std::lock_guard<std::mutex> lock(android_pre_api_26_name_mutex_);
    rex::string::util_copy_truncating(android_pre_api_26_name_, name,
                                      rex::countof(android_pre_api_26_name_));
  }
#endif

  uint32_t system_id() const { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(thread_)); }

  uint64_t affinity_mask() {
    // Switch: svcGetThreadCoreMask(preferred_core*, affinity_mask*, handle)
    // Must query THIS thread's handle, not the calling thread's.
    WaitStarted();
    s32 preferred_core = -2;
    u64 affinity = 0;
    svcGetThreadCoreMask(&preferred_core, &affinity, this->nx_handle_);
    return static_cast<uint64_t>(affinity);
  }

  void set_affinity_mask(uint64_t mask) {
    // Switch: svcSetThreadCoreMask(handle, ideal_core, affinity_mask)
    // ideal_core -2 is IdealCoreUseProcessValue: the kernel then ignores
    // affinity_mask and pins the thread to the process's default core (0),
    // so pass the lowest core of the mask as the ideal core instead.
    // Only cores in the process core mask are accepted (0-2 for applications).
    u64 process_mask = 0;
    svcGetInfo(&process_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
    const u64 nx_mask = mask & process_mask;
    if (!nx_mask) {
      REXSYS_WARN("set_affinity_mask: mask {:#x} has no core in the process core mask {:#x}", mask,
                  process_mask);
      return;
    }
    set_ideal_core(static_cast<uint32_t>(__builtin_ctzll(nx_mask)), nx_mask);
  }

  void set_ideal_core(uint32_t ideal_core, uint64_t mask) {
    // Must target THIS thread's handle, not the calling thread's.
    WaitStarted();
    u64 process_mask = 0;
    svcGetInfo(&process_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
    const u32 nx_mask = static_cast<u32>(mask & process_mask);
    if (ideal_core >= 64 || !(nx_mask & (u64(1) << ideal_core))) {
      REXSYS_WARN("set_ideal_core: core {} not in mask {:#x} & process core mask {:#x}",
                  ideal_core, mask, process_mask);
      return;
    }
    const Result rc =
        svcSetThreadCoreMask(this->nx_handle_, static_cast<s32>(ideal_core), nx_mask);
    if (R_FAILED(rc)) {
      REXSYS_WARN("set_ideal_core: svcSetThreadCoreMask(ideal {}, mask {:#x}) rc={:#x}",
                  ideal_core, nx_mask, rc);
    }
  }

  int32_t ideal_core() {
    WaitStarted();
    s32 preferred_core = -2;
    u64 affinity = 0;
    svcGetThreadCoreMask(&preferred_core, &affinity, this->nx_handle_);
    return preferred_core;
  }

  int priority() {
    WaitStarted();
    // Switch: use svcGetThreadPriority with the stored kernel Handle.
    s32 prio = 0;
    Result rc = svcGetThreadPriority(&prio, nx_handle_);
    if (R_FAILED(rc)) return -1;
    return static_cast<int>(prio);
  }

  void set_priority(int new_priority) {
    WaitStarted();
    // Switch: use svcSetThreadPriority with HOS priority mapping.
    u32 hos_prio = MapToSwitchPriority(new_priority);
    svcSetThreadPriority(nx_handle_, hos_prio);
  }

  void set_nx_priority(int32_t hos_priority) {
    WaitStarted();
    const Result set_rc = svcSetThreadPriority(nx_handle_, static_cast<u32>(hos_priority));
    s32 actual_priority = -1;
    const Result read_rc = svcGetThreadPriority(&actual_priority, nx_handle_);
    REXSYS_INFO("NX scheduling: thread {:08X}, requested priority {:#x}, set rc {:#x}, "
                "read rc {:#x}, actual priority {:#x}",
                nx_handle_, hos_priority, set_rc, read_rc, actual_priority);
    if (R_FAILED(set_rc) || R_FAILED(read_rc) || actual_priority != hos_priority) {
      REXSYS_WARN("NX scheduling: priority assignment not verified for thread {:08X}",
                  nx_handle_);
    }
  }

  // Written once by the thread itself at start; INVALID_HANDLE (0) before.
  uint32_t nx_kernel_handle() const { return nx_handle_; }

  void QueueUserCallback(std::function<void()> callback) {
    WaitStarted();
    {
      std::unique_lock<std::mutex> lock(callback_mutex_);
      user_callbacks_.push_back(std::move(callback));
      has_pending_user_callbacks_.store(true, std::memory_order_release);
    }

    // If the callback is queued on the current thread, don't self-signal.
    // Alertable waits drain this queue in normal thread context.
    if (pthread_equal(thread_, pthread_self())) {
      if (alertable_state_) {
        DispatchQueuedUserCallbacks();
      }
      return;
    }

    // Switch: no POSIX signals. Callback is queued above; target thread will
    // dispatch it from its next alertable wait via DispatchQueuedUserCallbacks.
  }

  bool DispatchQueuedUserCallbacks() {
    if (!has_pending_user_callbacks_.load(std::memory_order_acquire)) {
      return false;
    }

    std::function<void()> callback;
    {
      std::unique_lock<std::mutex> lock(callback_mutex_);
      if (user_callbacks_.empty()) {
        has_pending_user_callbacks_.store(false, std::memory_order_release);
        return false;
      }
      callback = std::move(user_callbacks_.front());
      user_callbacks_.pop_front();
      has_pending_user_callbacks_.store(!user_callbacks_.empty(), std::memory_order_release);
    }
    if (callback) {
      callback();
    }
    return true;
  }

  bool Resume(uint32_t* out_previous_suspend_count = nullptr) {
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = 0;
    }
    WaitStarted();
    std::unique_lock<std::mutex> lock(state_mutex_);
    if (suspend_count_ == 0) {
      return false;
    }
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = suspend_count_;
    }
    --suspend_count_;
    if (suspend_count_ == 0 && state_ == State::kSuspended) {
      state_ = State::kRunning;
      // Switch: kernel-resume the thread.
      svcSetThreadActivity(nx_handle_, ThreadActivity_Runnable);
      sem_post(&suspend_sem_);
    }
    state_signal_.notify_all();
    return true;
  }

  bool Suspend(uint32_t* out_previous_suspend_count = nullptr) {
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = 0;
    }
    WaitStarted();
    bool is_current_thread = pthread_self() == thread_;
    {
      std::unique_lock<std::mutex> lock(state_mutex_);
      if (out_previous_suspend_count) {
        *out_previous_suspend_count = suspend_count_;
      }
      state_ = State::kSuspended;
      ++suspend_count_;
    }

    if (is_current_thread) {
      WaitSuspended();
      return true;
    }

    // Switch: use svcSetThreadActivity to kernel-pause the thread.
    Result rc = svcSetThreadActivity(nx_handle_, ThreadActivity_Paused);
    return R_SUCCEEDED(rc);
  }

  void Terminate(int exit_code) {
    bool is_current_thread = pthread_self() == thread_;
    {
      std::unique_lock<std::mutex> lock(state_mutex_);
      if (state_ == State::kFinished) {
        if (is_current_thread) {
          // This is really bad. Some thread must have called Terminate() on us
          // just before we decided to terminate ourselves
          assert_always();
          for (;;) {
            // Wait for pthread_cancel() to actually happen.
          }
        }
        return;
      }
      state_ = State::kFinished;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);

      exit_code_ = exit_code;
      signaled_ = true;
      NotifySignaledLocked();
    }
    if (is_current_thread) {
      g_nx_threads_finished.fetch_add(1, std::memory_order_relaxed);
      pthread_exit(reinterpret_cast<void*>(exit_code));
    } else {
      // Switch / libnx: no pthread_cancel and no async cancellation primitive.
      // A hard pthread_join here blocks forever if the target thread is parked
      // in a blocking syscall that never observes signaled_. Degrade to a
      // bounded wait using svcWaitSynchronization on the thread's kernel
      // handle, which signals when the thread exits. If the thread has not
      // exited by the timeout, log and return — the runtime will leak the
      // pthread rather than deadlock the shutdown path.
      REXSYS_WARN(
          "Thread::Terminate: libnx lacks async cancellation; waiting up to "
          "5s for cooperative exit on handle {:#x}",
          static_cast<uint32_t>(nx_handle_));
      constexpr u64 kTerminateTimeoutNs = 5ULL * 1000ULL * 1000ULL * 1000ULL;
      bool exited = false;
      if (nx_handle_ != INVALID_HANDLE) {
        Result rc = svcWaitSynchronizationSingle(nx_handle_, kTerminateTimeoutNs);
        exited = R_SUCCEEDED(rc);
        if (!exited) {
          REXSYS_WARN(
              "Thread::Terminate: svcWaitSynchronizationSingle returned "
              "{:#x}; thread may not have exited.",
              static_cast<uint32_t>(rc));
        }
      }
      // No pthread_detach (docs/switch-port/09-rules.md): post_execution
      // joins, and a join after a detach is undefined (on libnx the record
      // may already be freed). An exited thread is joined there, bounded; one
      // still running is abandoned, its record and stack leaked rather than
      // joined without a bound.
      if (!exited) {
        abandoned_ = true;
        g_nx_threads_abandoned.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }

  void WaitStarted() const {
    std::unique_lock<std::mutex> lock(state_mutex_);
    state_signal_.wait(lock, [this] { return state_ != State::kUninitialized; });
  }

  /// Uses sem_wait because it may be called from signal handler context.
  void WaitSuspended() {
    int ret;
    do {
      ret = sem_wait(&suspend_sem_);
    } while (ret == -1 && errno == EINTR);
  }

  void* native_handle() const override { return reinterpret_cast<void*>(thread_); }

 private:
  static void* ThreadStartRoutine(void* parameter);
  inline bool signaled() const override { return signaled_; }
  // Runs after every successful wait on the thread: join once only (a second
  // pthread_join of the same thread is undefined, like a join after detach).
  inline void post_execution() override {
    if (thread_ && !abandoned_ && !joined_.exchange(true)) {
      pthread_join(thread_, nullptr);
      g_nx_threads_joined.fetch_add(1, std::memory_order_relaxed);
    }
    sem_destroy(&suspend_sem_);
  }
  pthread_t thread_;
  bool signaled_;
  int exit_code_;
  State state_;             // Protected by state_mutex_
  uint32_t suspend_count_;  // Protected by state_mutex_
  sem_t suspend_sem_;
  mutable std::mutex state_mutex_;
  mutable std::mutex callback_mutex_;
  mutable std::condition_variable state_signal_;
  std::deque<std::function<void()>> user_callbacks_;
  std::atomic<bool> has_pending_user_callbacks_{false};
  // HOS kernel Handle from threadGetCurHandle() inside the thread. Used for
  // svcSetThreadPriority/Activity/CoreMask.
  Handle nx_handle_ = INVALID_HANDLE;
  int nx_deferred_priority_ = 0;
  // Terminate timed out with the thread still running: never joined.
  bool abandoned_ = false;
  std::atomic<bool> joined_{false};
#if REX_PLATFORM_ANDROID
  // Name accessible via name() on Android before API 26 which added
  // pthread_getname_np.
  mutable std::mutex android_pre_api_26_name_mutex_;
  char android_pre_api_26_name_[16];
#endif
};

class PosixWaitHandle {
 public:
  virtual ~PosixWaitHandle();
  virtual PosixConditionBase& condition() = 0;
};

PosixWaitHandle::~PosixWaitHandle() = default;

thread_local PosixCondition<Thread>* current_thread_condition_ = nullptr;

bool DispatchCurrentThreadUserCallback() {
  if (!current_thread_condition_) {
    Thread::GetCurrentThread();
  }
  return current_thread_condition_ && current_thread_condition_->DispatchQueuedUserCallbacks();
}

namespace {

constexpr auto kAlertablePollSlice = std::chrono::milliseconds(1);

class ScopedAlertableState {
 public:
  explicit ScopedAlertableState(bool alertable) : alertable_(alertable) {
    if (alertable_) {
      alertable_state_ = true;
    }
  }
  ~ScopedAlertableState() {
    if (alertable_) {
      alertable_state_ = false;
    }
  }

 private:
  bool alertable_;
};

std::chrono::steady_clock::time_point ComputeAlertableDeadline(std::chrono::milliseconds timeout) {
  if (timeout == std::chrono::milliseconds::max()) {
    return std::chrono::steady_clock::time_point::max();
  }
  return std::chrono::steady_clock::now() + timeout;
}

bool HasAlertableTimeoutElapsed(std::chrono::steady_clock::time_point deadline) {
  return deadline != std::chrono::steady_clock::time_point::max() &&
         std::chrono::steady_clock::now() >= deadline;
}

std::chrono::milliseconds ComputeAlertableWaitTimeout(
    std::chrono::steady_clock::time_point deadline) {
  if (deadline == std::chrono::steady_clock::time_point::max()) {
    return kAlertablePollSlice;
  }
  auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now());
  if (remaining <= std::chrono::milliseconds::zero()) {
    return std::chrono::milliseconds::zero();
  }
  return std::min(remaining, kAlertablePollSlice);
}

}  // namespace

// This wraps a condition object as our handle because posix has no single
// native handle for higher level concurrency constructs such as semaphores
template <typename T>
class PosixConditionHandle : public T, public PosixWaitHandle {
 public:
  PosixConditionHandle() = default;
  explicit PosixConditionHandle(bool);
  explicit PosixConditionHandle(pthread_t thread);
  PosixConditionHandle(bool manual_reset, bool initial_state);
  PosixConditionHandle(uint32_t initial_count, uint32_t maximum_count);
  ~PosixConditionHandle() override = default;

  PosixCondition<T>& condition() override { return handle_; }
  void* native_handle() const override { return handle_.native_handle(); }

 protected:
  PosixCondition<T> handle_;
  friend PosixCondition<T>;
};

template <>
PosixConditionHandle<Semaphore>::PosixConditionHandle(uint32_t initial_count,
                                                      uint32_t maximum_count)
    : handle_(initial_count, maximum_count) {}

template <>
PosixConditionHandle<Mutant>::PosixConditionHandle(bool initial_owner) : handle_(initial_owner) {}

template <>
PosixConditionHandle<Timer>::PosixConditionHandle(bool manual_reset) : handle_(manual_reset) {}

template <>
PosixConditionHandle<Event>::PosixConditionHandle(bool manual_reset, bool initial_state)
    : handle_(manual_reset, initial_state) {}

template <>
PosixConditionHandle<Thread>::PosixConditionHandle(pthread_t thread) : handle_(thread) {}

WaitResult Wait(WaitHandle* wait_handle, bool is_alertable, std::chrono::milliseconds timeout) {
  auto posix_wait_handle = dynamic_cast<PosixWaitHandle*>(wait_handle);
  if (posix_wait_handle == nullptr) {
    return WaitResult::kFailed;
  }
  if (!is_alertable) {
    return posix_wait_handle->condition().Wait(timeout);
  }

  ScopedAlertableState alertable_state_guard(true);
  auto deadline = ComputeAlertableDeadline(timeout);

  while (true) {
    if (DispatchCurrentThreadUserCallback()) {
      return WaitResult::kUserCallback;
    }
    if (HasAlertableTimeoutElapsed(deadline)) {
      return WaitResult::kTimeout;
    }
    auto result = posix_wait_handle->condition().Wait(ComputeAlertableWaitTimeout(deadline));
    if (result != WaitResult::kTimeout) {
      return result;
    }
  }
}

WaitResult SignalAndWait(WaitHandle* wait_handle_to_signal, WaitHandle* wait_handle_to_wait_on,
                         bool is_alertable, std::chrono::milliseconds timeout) {
  auto result = WaitResult::kFailed;
  auto posix_wait_handle_to_signal = dynamic_cast<PosixWaitHandle*>(wait_handle_to_signal);
  auto posix_wait_handle_to_wait_on = dynamic_cast<PosixWaitHandle*>(wait_handle_to_wait_on);
  if (posix_wait_handle_to_signal == nullptr || posix_wait_handle_to_wait_on == nullptr) {
    return WaitResult::kFailed;
  }
  if (!posix_wait_handle_to_signal->condition().Signal()) {
    return WaitResult::kFailed;
  }

  if (!is_alertable) {
    return posix_wait_handle_to_wait_on->condition().Wait(timeout);
  }

  ScopedAlertableState alertable_state_guard(true);
  auto deadline = ComputeAlertableDeadline(timeout);
  while (true) {
    if (DispatchCurrentThreadUserCallback()) {
      return WaitResult::kUserCallback;
    }
    if (HasAlertableTimeoutElapsed(deadline)) {
      return WaitResult::kTimeout;
    }
    result = posix_wait_handle_to_wait_on->condition().Wait(ComputeAlertableWaitTimeout(deadline));
    if (result != WaitResult::kTimeout) {
      return result;
    }
  }
}

std::pair<WaitResult, size_t> WaitMultiple(WaitHandle* wait_handles[], size_t wait_handle_count,
                                           bool wait_all, bool is_alertable,
                                           std::chrono::milliseconds timeout) {
  std::vector<PosixConditionBase*> conditions;
  conditions.reserve(wait_handle_count);
  for (size_t i = 0u; i < wait_handle_count; ++i) {
    auto handle = dynamic_cast<PosixWaitHandle*>(wait_handles[i]);
    if (handle == nullptr) {
      return std::make_pair(WaitResult::kFailed, 0);
    }
    conditions.push_back(&handle->condition());
  }
  if (!is_alertable) {
    return PosixConditionBase::WaitMultiple(std::move(conditions), wait_all, timeout);
  }

  ScopedAlertableState alertable_state_guard(true);
  auto deadline = ComputeAlertableDeadline(timeout);
  while (true) {
    if (DispatchCurrentThreadUserCallback()) {
      return std::make_pair(WaitResult::kUserCallback, 0);
    }
    if (HasAlertableTimeoutElapsed(deadline)) {
      return std::make_pair(WaitResult::kTimeout, 0);
    }
    auto result = PosixConditionBase::WaitMultiple(std::vector<PosixConditionBase*>(conditions),
                                                   wait_all, ComputeAlertableWaitTimeout(deadline));
    if (result.first != WaitResult::kTimeout) {
      return result;
    }
  }
}

class PosixEvent : public PosixConditionHandle<Event> {
 public:
  PosixEvent(bool manual_reset, bool initial_state)
      : PosixConditionHandle(manual_reset, initial_state) {}
  ~PosixEvent() override = default;
  void Set() override { handle_.Signal(); }
  void Reset() override { handle_.Reset(); }
  void Pulse() override {
    using namespace std::chrono_literals;
    handle_.Signal();
    MaybeYield();
    Sleep(10us);
    handle_.Reset();
  }
};

std::unique_ptr<Event> Event::CreateManualResetEvent(bool initial_state) {
  return std::make_unique<PosixEvent>(true, initial_state);
}

std::unique_ptr<Event> Event::CreateAutoResetEvent(bool initial_state) {
  return std::make_unique<PosixEvent>(false, initial_state);
}

class PosixSemaphore : public PosixConditionHandle<Semaphore> {
 public:
  PosixSemaphore(int initial_count, int maximum_count)
      : PosixConditionHandle(static_cast<uint32_t>(initial_count),
                             static_cast<uint32_t>(maximum_count)) {}
  ~PosixSemaphore() override = default;
  bool Release(int release_count, int* out_previous_count) override {
    if (release_count < 1) {
      return false;
    }
    return handle_.Release(static_cast<uint32_t>(release_count), out_previous_count);
  }
};

std::unique_ptr<Semaphore> Semaphore::Create(int initial_count, int maximum_count) {
  if (initial_count < 0 || initial_count > maximum_count || maximum_count <= 0) {
    return nullptr;
  }
  return std::make_unique<PosixSemaphore>(initial_count, maximum_count);
}

class PosixMutant : public PosixConditionHandle<Mutant> {
 public:
  explicit PosixMutant(bool initial_owner) : PosixConditionHandle(initial_owner) {}
  ~PosixMutant() override = default;
  bool Release() override { return handle_.Release(); }
};

std::unique_ptr<Mutant> Mutant::Create(bool initial_owner) {
  return std::make_unique<PosixMutant>(initial_owner);
}

class PosixTimer : public PosixConditionHandle<Timer> {
  using WClock_ = Timer::WClock_;
  using GClock_ = Timer::GClock_;

 public:
  explicit PosixTimer(bool manual_reset) : PosixConditionHandle(manual_reset) {}
  ~PosixTimer() override = default;

  bool SetOnceAfter(rex::chrono::hundrednanoseconds rel_time,
                    std::function<void()> opt_callback = nullptr) override {
    return SetOnceAt(GClock_::now() + rel_time, std::move(opt_callback));
  }
  bool SetOnceAt(WClock_::time_point due_time,
                 std::function<void()> opt_callback = nullptr) override {
    return SetOnceAt(std::chrono::clock_cast<GClock_>(due_time), std::move(opt_callback));
  };
  bool SetOnceAt(GClock_::time_point due_time,
                 std::function<void()> opt_callback = nullptr) override {
    handle_.SetOnce(due_time, std::move(opt_callback));
    return true;
  }

  bool SetRepeatingAfter(rex::chrono::hundrednanoseconds rel_time, std::chrono::milliseconds period,
                         std::function<void()> opt_callback = nullptr) override {
    return SetRepeatingAt(GClock_::now() + rel_time, period, std::move(opt_callback));
  }
  bool SetRepeatingAt(WClock_::time_point due_time, std::chrono::milliseconds period,
                      std::function<void()> opt_callback = nullptr) override {
    return SetRepeatingAt(std::chrono::clock_cast<GClock_>(due_time), period,
                          std::move(opt_callback));
  }
  bool SetRepeatingAt(GClock_::time_point due_time, std::chrono::milliseconds period,
                      std::function<void()> opt_callback = nullptr) override {
    handle_.SetRepeating(due_time, period, std::move(opt_callback));
    return true;
  }
  bool Cancel() override {
    handle_.Cancel();
    return true;
  }
};

std::unique_ptr<Timer> Timer::CreateManualResetTimer() {
  return std::make_unique<PosixTimer>(true);
}

std::unique_ptr<Timer> Timer::CreateSynchronizationTimer() {
  return std::make_unique<PosixTimer>(false);
}

class PosixThread : public PosixConditionHandle<Thread> {
 public:
  PosixThread() = default;
  explicit PosixThread(pthread_t thread) : PosixConditionHandle(thread) {}
  ~PosixThread() override = default;

  bool Initialize(CreationParameters params, std::function<void()> start_routine) {
    auto start_data = new ThreadStartData({std::move(start_routine), false, this});
    return handle_.Initialize(params, start_data);
  }

  void set_name(std::string name) override {
    handle_.WaitStarted();
    Thread::set_name(name);
    if (name.length() > 15) {
      name = name.substr(0, 15);
    }
    handle_.set_name(name);
  }

  uint32_t system_id() const override { return handle_.system_id(); }

  uint64_t affinity_mask() override { return handle_.affinity_mask(); }
  void set_affinity_mask(uint64_t mask) override { handle_.set_affinity_mask(mask); }
  void set_ideal_core(uint32_t ideal_core, uint64_t mask) override {
    handle_.set_ideal_core(ideal_core, mask);
  }
  int32_t ideal_core() override { return handle_.ideal_core(); }

  int priority() override { return handle_.priority(); }
  void set_priority(int new_priority) override { handle_.set_priority(new_priority); }
  void set_nx_priority(int32_t hos_priority) override { handle_.set_nx_priority(hos_priority); }
  uint32_t nx_kernel_handle() const override { return handle_.nx_kernel_handle(); }

  void QueueUserCallback(std::function<void()> callback) override {
    handle_.QueueUserCallback(std::move(callback));
  }

  bool Resume(uint32_t* out_previous_suspend_count) override {
    return handle_.Resume(out_previous_suspend_count);
  }

  bool Suspend(uint32_t* out_previous_suspend_count) override {
    return handle_.Suspend(out_previous_suspend_count);
  }

  void Terminate(int exit_code) override { handle_.Terminate(exit_code); }

  void WaitSuspended() { handle_.WaitSuspended(); }
};

thread_local PosixThread* current_thread_ = nullptr;

void* PosixCondition<Thread>::ThreadStartRoutine(void* parameter) {
#if !REX_PLATFORM_ANDROID
  if (pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, nullptr) != 0) {
    assert_always();
  }
#endif
  set_current_thread_name("");

  auto start_data = static_cast<ThreadStartData*>(parameter);
  assert_not_null(start_data);
  assert_not_null(start_data->thread_obj);

  auto thread = dynamic_cast<PosixThread*>(start_data->thread_obj);
  auto start_routine = std::move(start_data->start_routine);
  auto create_suspended = start_data->create_suspended;
  delete start_data;

  current_thread_ = thread;
  current_thread_condition_ = &thread->handle_;
  // Switch: capture the HOS kernel Handle for this thread and apply any
  // deferred priority that Initialize() stashed (pthread_attr scheduling
  // isn't available on newlib).
  thread->handle_.nx_handle_ = threadGetCurHandle();
  // Without a requested priority a thread gets the host band explicitly, not
  // whatever libnx's pthread default is. XThread then applies its role's band
  // (nx_priority_for_thread).
  const u32 hos_prio = thread->handle_.nx_deferred_priority_ != 0
                           ? MapToSwitchPriority(thread->handle_.nx_deferred_priority_)
                           : static_cast<u32>(kNxPriorityHost);
  svcSetThreadPriority(thread->handle_.nx_handle_, hos_prio);
  {
    std::unique_lock<std::mutex> lock(thread->handle_.state_mutex_);
    thread->handle_.suspend_count_ = create_suspended ? 1 : 0;
    thread->handle_.state_ = create_suspended ? State::kSuspended : State::kRunning;
    thread->handle_.state_signal_.notify_all();
    if (create_suspended) {
      thread->handle_.state_signal_.wait(lock,
                                         [thread] { return thread->handle_.suspend_count_ == 0; });
    }
  }

  start_routine();

  {
    std::unique_lock<std::mutex> lock(thread->handle_.state_mutex_);
    thread->handle_.state_ = State::kFinished;
  }

  {
    std::unique_lock<std::mutex> lock(thread->handle_.mutex_);
    thread->handle_.exit_code_ = 0;
    thread->handle_.signaled_ = true;
    thread->handle_.NotifySignaledLocked();
  }

  current_thread_ = nullptr;
  current_thread_condition_ = nullptr;
  g_nx_threads_finished.fetch_add(1, std::memory_order_relaxed);
  return nullptr;
}

std::unique_ptr<Thread> Thread::Create(CreationParameters params,
                                       std::function<void()> start_routine) {
  install_signal_handler(SignalType::kThreadSuspend);
  install_signal_handler(SignalType::kThreadUserCallback);
#if REX_PLATFORM_ANDROID
  install_signal_handler(SignalType::kThreadTerminate);
#endif
  auto thread = std::make_unique<PosixThread>();
  if (!thread->Initialize(params, std::move(start_routine)))
    return nullptr;
  assert_not_null(thread);
  return thread;
}

Thread* Thread::GetCurrentThread() {
  if (current_thread_) {
    return current_thread_;
  }

  // Threads not created by Thread::Create (typically main thread) still need
  // process-wide signal handlers used by suspend/APC machinery.
  install_signal_handler(SignalType::kThreadSuspend);
  install_signal_handler(SignalType::kThreadUserCallback);
#if REX_PLATFORM_ANDROID
  install_signal_handler(SignalType::kThreadTerminate);
#endif

  // Should take this route only for threads not created by Thread::Create.
  // The only thread not created by Thread::Create should be the main thread.
  pthread_t handle = pthread_self();

  current_thread_ = new PosixThread(handle);
  current_thread_condition_ = &current_thread_->condition();
  // TODO(bwrsandman): Disabling deleting thread_local current thread to prevent
  //                   assert in destructor. Since this is thread local, the
  //                   "memory leaking" is controlled.
  // atexit([] { delete current_thread_; });

  return current_thread_;
}

void Thread::Exit(int exit_code) {
  if (current_thread_) {
    current_thread_->Terminate(exit_code);
  } else {
    // Should only happen with the main thread
    pthread_exit(reinterpret_cast<void*>(exit_code));
  }
  // Function must not return
  assert_always();
}

void set_current_thread_name(const std::string_view name) {
#if REX_PLATFORM_NX
  // No pthread_setname_np in libnx: keep the name in the SDK thread object
  // (Thread::name_, used for logging/debugging) when there is one.
  if (current_thread_) {
    current_thread_->Thread::set_name(std::string(name));
  }
#else
  pthread_setname_np(pthread_self(), std::string(name).c_str());
#endif
#if REX_PLATFORM_ANDROID
  if (!android_pthread_getname_np_ && current_thread_) {
    current_thread_->condition().SetAndroidPreApi26Name(name);
  }
#endif
}

// Switch: no signal_handler — suspend/resume uses svcSetThreadActivity and
// user callbacks are drained cooperatively from alertable waits.

}  // namespace rex::thread
