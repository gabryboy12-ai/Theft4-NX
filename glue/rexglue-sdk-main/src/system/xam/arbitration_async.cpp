/**
 ******************************************************************************
 * @file        arbitration_async.cpp
 * @brief       Bounded worker and exactly-once gate for arbitration HTTP work.
 ******************************************************************************
 */

#include <rex/system/xam/arbitration_async.h>

#include <algorithm>
#include <utility>

namespace rex::system::xam {

void ArbitrationCancellation::Cancel() {
  cancelled_.store(true, std::memory_order_release);
  condition_.notify_all();
}

bool ArbitrationCancellation::IsCancelled() const {
  return cancelled_.load(std::memory_order_acquire);
}

bool ArbitrationCancellation::WaitUntil(std::chrono::steady_clock::time_point deadline) {
  std::unique_lock lock(mutex_);
  return condition_.wait_until(lock, deadline, [this] { return IsCancelled(); });
}

ArbitrationAsyncManager::ArbitrationAsyncManager() : worker_([this] { WorkerMain(); }) {}

ArbitrationAsyncManager::~ArbitrationAsyncManager() {
  Shutdown();
}

std::optional<ArbitrationAsyncOperation> ArbitrationAsyncManager::Reserve(
    uint32_t overlapped_ptr, CancellationCompletion cancellation_completion) {
  if (!overlapped_ptr || !cancellation_completion)
    return std::nullopt;

  std::lock_guard lock(mutex_);
  if (stopping_ || pending_.size() >= kMaximumOperations || pending_.contains(overlapped_ptr)) {
    return std::nullopt;
  }

  try {
    ++next_generation_;
    if (!next_generation_)
      ++next_generation_;
    ArbitrationAsyncOperation operation{
        .overlapped_ptr = overlapped_ptr,
        .generation = next_generation_,
        .cancellation = std::make_shared<ArbitrationCancellation>()};
    pending_.emplace(overlapped_ptr, PendingOperation{.operation = operation,
                                                      .cancellation_completion =
                                                          std::move(cancellation_completion)});
    return operation;
  } catch (...) {
    return std::nullopt;
  }
}

bool ArbitrationAsyncManager::Submit(const ArbitrationAsyncOperation& operation, Work work) {
  if (!operation || !work)
    return false;

  std::lock_guard lock(mutex_);
  const auto found = pending_.find(operation.overlapped_ptr);
  if (stopping_ || found == pending_.end() || found->second.submitted ||
      !SameGeneration(found->second.operation, operation)) {
    return false;
  }
  try {
    work_queue_.push_back({.operation = operation, .work = std::move(work)});
  } catch (...) {
    return false;
  }
  found->second.submitted = true;
  condition_.notify_one();
  return true;
}

bool ArbitrationAsyncManager::IsCurrent(const ArbitrationAsyncOperation& operation) const {
  std::lock_guard lock(mutex_);
  return IsCurrentLocked(operation);
}

bool ArbitrationAsyncManager::TryFinish(const ArbitrationAsyncOperation& operation) {
  std::lock_guard lock(mutex_);
  return RemoveLocked(operation).has_value();
}

bool ArbitrationAsyncManager::Cancel(uint32_t overlapped_ptr) {
  std::optional<PendingOperation> removed;
  {
    std::lock_guard lock(mutex_);
    removed = RemoveLocked(overlapped_ptr);
  }
  if (!removed)
    return false;
  removed->operation.cancellation->Cancel();
  try {
    removed->cancellation_completion();
  } catch (...) {}
  return true;
}

bool ArbitrationAsyncManager::Cancel(const ArbitrationAsyncOperation& operation) {
  std::optional<PendingOperation> removed;
  {
    std::lock_guard lock(mutex_);
    removed = RemoveLocked(operation);
  }
  if (!removed)
    return false;
  removed->operation.cancellation->Cancel();
  try {
    removed->cancellation_completion();
  } catch (...) {}
  return true;
}

void ArbitrationAsyncManager::CancelAll() {
  std::unordered_map<uint32_t, PendingOperation> removed;
  {
    std::lock_guard lock(mutex_);
    removed.swap(pending_);
    work_queue_.clear();
  }
  for (auto& entry : removed) {
    auto& operation = entry.second;
    operation.operation.cancellation->Cancel();
    try {
      operation.cancellation_completion();
    } catch (...) {}
  }
}

void ArbitrationAsyncManager::Shutdown() {
  std::unordered_map<uint32_t, PendingOperation> removed;
  {
    std::lock_guard lock(mutex_);
    if (stopping_)
      return;
    stopping_ = true;
    removed.swap(pending_);
    work_queue_.clear();
  }
  for (auto& entry : removed) {
    auto& operation = entry.second;
    operation.operation.cancellation->Cancel();
    try {
      operation.cancellation_completion();
    } catch (...) {}
  }
  condition_.notify_all();
  if (worker_.joinable())
    worker_.join();
}

bool ArbitrationAsyncManager::SameGeneration(const ArbitrationAsyncOperation& left,
                                             const ArbitrationAsyncOperation& right) {
  return left.overlapped_ptr == right.overlapped_ptr && left.generation == right.generation;
}

bool ArbitrationAsyncManager::IsCurrentLocked(const ArbitrationAsyncOperation& operation) const {
  const auto found = pending_.find(operation.overlapped_ptr);
  return found != pending_.end() && SameGeneration(found->second.operation, operation);
}

std::optional<ArbitrationAsyncManager::PendingOperation> ArbitrationAsyncManager::RemoveLocked(
    const ArbitrationAsyncOperation& operation) {
  const auto found = pending_.find(operation.overlapped_ptr);
  if (found == pending_.end() || !SameGeneration(found->second.operation, operation)) {
    return std::nullopt;
  }
  const ArbitrationAsyncOperation identity = operation;
  PendingOperation removed = std::move(found->second);
  pending_.erase(found);
  std::erase_if(work_queue_,
                [&](const WorkItem& item) { return SameGeneration(item.operation, identity); });
  return removed;
}

std::optional<ArbitrationAsyncManager::PendingOperation> ArbitrationAsyncManager::RemoveLocked(
    uint32_t overlapped_ptr) {
  const auto found = pending_.find(overlapped_ptr);
  if (found == pending_.end())
    return std::nullopt;
  return RemoveLocked(found->second.operation);
}

void ArbitrationAsyncManager::WorkerMain() {
  for (;;) {
    WorkItem item;
    {
      std::unique_lock lock(mutex_);
      condition_.wait(lock, [this] { return stopping_ || !work_queue_.empty(); });
      if (stopping_ && work_queue_.empty())
        return;
      item = std::move(work_queue_.front());
      work_queue_.pop_front();
    }

    if (!IsCurrent(item.operation))
      continue;
    try {
      item.work(item.operation);
    } catch (...) {
      // An escaped HTTP/backend exception must not strand the guest wait.
      Cancel(item.operation);
    }
  }
}

}  // namespace rex::system::xam
