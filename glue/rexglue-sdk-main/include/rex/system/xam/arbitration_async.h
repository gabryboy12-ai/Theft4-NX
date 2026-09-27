/**
 ******************************************************************************
 * @file        arbitration_async.h
 * @brief       Bounded worker and exactly-once gate for arbitration HTTP work.
 ******************************************************************************
 */

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

namespace rex::system::xam {

class ArbitrationCancellation final {
 public:
  void Cancel();
  bool IsCancelled() const;

  // Returns true when cancelled, false when the deadline expires normally.
  bool WaitUntil(std::chrono::steady_clock::time_point deadline);

 private:
  std::atomic<bool> cancelled_{false};
  mutable std::mutex mutex_;
  std::condition_variable condition_;
};

struct ArbitrationAsyncOperation {
  uint32_t overlapped_ptr = 0;
  uint64_t generation = 0;
  std::shared_ptr<ArbitrationCancellation> cancellation;

  explicit operator bool() const { return overlapped_ptr && generation && cancellation; }
};

// A single bounded worker is intentional. GTA IV has only one arbitration
// registration in flight, and rejecting accidental duplicates prevents a
// second retail-length request from building an unbounded shutdown tail.
class ArbitrationAsyncManager final {
 public:
  using Work = std::function<void(const ArbitrationAsyncOperation&)>;
  using CancellationCompletion = std::function<void()>;

  static constexpr size_t kMaximumOperations = 1;

  ArbitrationAsyncManager();
  ~ArbitrationAsyncManager();

  ArbitrationAsyncManager(const ArbitrationAsyncManager&) = delete;
  ArbitrationAsyncManager& operator=(const ArbitrationAsyncManager&) = delete;

  // Reservation happens before the initial host-dispatch task is enqueued, so
  // XMsgCancelIORequest can cancel even if that task has not run yet.
  std::optional<ArbitrationAsyncOperation> Reserve(uint32_t overlapped_ptr,
                                                   CancellationCompletion cancellation_completion);
  bool Submit(const ArbitrationAsyncOperation& operation, Work work);

  bool IsCurrent(const ArbitrationAsyncOperation& operation) const;
  // Claims the only terminal completion for this generation.
  bool TryFinish(const ArbitrationAsyncOperation& operation);

  bool Cancel(uint32_t overlapped_ptr);
  bool Cancel(const ArbitrationAsyncOperation& operation);
  void CancelAll();
  void Shutdown();

 private:
  struct PendingOperation {
    ArbitrationAsyncOperation operation;
    CancellationCompletion cancellation_completion;
    bool submitted = false;
  };

  struct WorkItem {
    ArbitrationAsyncOperation operation;
    Work work;
  };

  static bool SameGeneration(const ArbitrationAsyncOperation& left,
                             const ArbitrationAsyncOperation& right);
  bool IsCurrentLocked(const ArbitrationAsyncOperation& operation) const;
  std::optional<PendingOperation> RemoveLocked(const ArbitrationAsyncOperation& operation);
  std::optional<PendingOperation> RemoveLocked(uint32_t overlapped_ptr);
  void WorkerMain();

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  bool stopping_ = false;
  uint64_t next_generation_ = 0;
  std::unordered_map<uint32_t, PendingOperation> pending_;
  std::deque<WorkItem> work_queue_;
  std::thread worker_;
};

enum class ArbitrationPollAction {
  kComplete,
  kRefreshRevision,
  kRetry,
  kFail,
  kStop,
};

constexpr ArbitrationPollAction ClassifyArbitrationPollResponse(long status, bool cancelled,
                                                                bool deadline_expired) {
  if (cancelled || deadline_expired)
    return ArbitrationPollAction::kStop;
  if (status == 200)
    return ArbitrationPollAction::kComplete;
  if (status == 202 || status == 412) {
    return ArbitrationPollAction::kRefreshRevision;
  }
  if (status == 0 || status >= 500)
    return ArbitrationPollAction::kRetry;
  return ArbitrationPollAction::kFail;
}

}  // namespace rex::system::xam
