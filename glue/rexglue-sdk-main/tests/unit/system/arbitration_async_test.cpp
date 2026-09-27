#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <functional>
#include <mutex>
#include <thread>

#include <rex/system/xam/arbitration_async.h>

#include "system/xam/xsession_internal.h"
#include "network/community_retry_policy.h"

namespace {

using rex::system::xam::ArbitrationAsyncManager;
using rex::system::xam::ArbitrationAsyncOperation;
using rex::system::xam::ArbitrationPollAction;
using rex::system::xam::ClassifyArbitrationPollResponse;
using rex::system::xam::SessionLifecycleState;
using rex::system::xam::SessionRecord;
using rex::system::xam::XSESSION_ARBITRATION_CONTEXT;

class SerialTestDispatcher {
 public:
  SerialTestDispatcher() : worker_([this] { WorkerMain(); }) {}

  ~SerialTestDispatcher() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    condition_.notify_all();
    worker_.join();
  }

  void Post(std::function<void()> task) {
    {
      std::lock_guard lock(mutex_);
      tasks_.push_back(std::move(task));
    }
    condition_.notify_one();
  }

 private:
  void WorkerMain() {
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
        if (stopping_ && tasks_.empty())
          return;
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }
      task();
    }
  }

  std::mutex mutex_;
  std::condition_variable condition_;
  bool stopping_ = false;
  std::deque<std::function<void()>> tasks_;
  std::thread worker_;
};

SessionRecord MakeArbitrationRecord(uint64_t session_id, uint64_t nonce) {
  SessionRecord record;
  record.title_id = 0x545407F2;
  record.media_id = 0x12345678;
  record.title_version = 1;
  record.protocol_version = 1;
  record.session_id = session_id;
  record.nonce = nonce;
  record.max_public_slots = 4;
  record.max_private_slots = 2;
  record.open_public_slots = 4;
  record.open_private_slots = 2;
  return record;
}

TEST_CASE("arbitration HTTP wait does not occupy ordered deferred dispatcher",
          "[live][session][arbitration][async]") {
  ArbitrationAsyncManager manager;
  SerialTestDispatcher dispatcher;
  std::mutex gate_mutex;
  std::condition_variable gate_condition;
  bool worker_started = false;
  bool release_worker = false;
  std::promise<bool> submitted;
  std::promise<void> unrelated_deferred_completed;
  std::promise<void> terminal_completed;
  std::atomic<uint32_t> terminal_count{0};
  std::atomic<uint32_t> cancellation_count{0};

  auto operation = manager.Reserve(0x1000, [&] { cancellation_count.fetch_add(1); });
  REQUIRE(operation);

  dispatcher.Post([&] {
    submitted.set_value(manager.Submit(*operation, [&](const ArbitrationAsyncOperation& running) {
      {
        std::lock_guard lock(gate_mutex);
        worker_started = true;
      }
      gate_condition.notify_all();
      {
        std::unique_lock lock(gate_mutex);
        gate_condition.wait(lock, [&] { return release_worker; });
      }
      dispatcher.Post([&, running] {
        if (manager.TryFinish(running)) {
          terminal_count.fetch_add(1);
        }
        terminal_completed.set_value();
      });
    }));
  });
  REQUIRE(submitted.get_future().get());
  {
    std::unique_lock lock(gate_mutex);
    gate_condition.wait(lock, [&] { return worker_started; });
  }

  dispatcher.Post([&] { unrelated_deferred_completed.set_value(); });
  unrelated_deferred_completed.get_future().get();
  CHECK(manager.IsCurrent(*operation));
  CHECK(terminal_count.load() == 0);

  {
    std::lock_guard lock(gate_mutex);
    release_worker = true;
  }
  gate_condition.notify_all();
  terminal_completed.get_future().get();
  CHECK(terminal_count.load() == 1);
  CHECK(cancellation_count.load() == 0);
}

TEST_CASE("cancel retires arbitration generation before late response",
          "[live][session][arbitration][async]") {
  ArbitrationAsyncManager manager;
  std::promise<void> worker_started;
  std::promise<void> cancelled;
  std::promise<void> late_response_checked;
  std::atomic<uint32_t> cancellation_count{0};
  std::atomic<uint32_t> terminal_count{0};

  auto operation = manager.Reserve(0x2000, [&] {
    cancellation_count.fetch_add(1);
    cancelled.set_value();
  });
  REQUIRE(operation);
  REQUIRE(manager.Submit(*operation, [&](const ArbitrationAsyncOperation& running) {
    worker_started.set_value();
    running.cancellation->WaitUntil(std::chrono::steady_clock::time_point::max());
    if (manager.TryFinish(running))
      terminal_count.fetch_add(1);
    late_response_checked.set_value();
  }));

  worker_started.get_future().get();
  REQUIRE(manager.Cancel(operation->overlapped_ptr));
  cancelled.get_future().get();
  late_response_checked.get_future().get();
  CHECK(cancellation_count.load() == 1);
  CHECK(terminal_count.load() == 0);
  CHECK_FALSE(manager.Cancel(operation->overlapped_ptr));
}

TEST_CASE("arbitration terminal completion is generation-gated exactly once",
          "[live][session][arbitration][async]") {
  ArbitrationAsyncManager manager;
  std::atomic<uint32_t> cancellation_count{0};
  auto operation = manager.Reserve(0x3000, [&] { cancellation_count.fetch_add(1); });
  REQUIRE(operation);
  CHECK(manager.TryFinish(*operation));
  CHECK_FALSE(manager.TryFinish(*operation));
  CHECK_FALSE(manager.Cancel(operation->overlapped_ptr));
  CHECK(cancellation_count.load() == 0);
}

TEST_CASE("arbitration worker shutdown cancels and joins active work",
          "[live][session][arbitration][shutdown]") {
  ArbitrationAsyncManager manager;
  std::promise<void> worker_started;
  std::atomic<uint32_t> cancellation_count{0};
  auto operation = manager.Reserve(0x4000, [&] { cancellation_count.fetch_add(1); });
  REQUIRE(operation);
  REQUIRE(manager.Submit(*operation, [&](const ArbitrationAsyncOperation& running) {
    worker_started.set_value();
    running.cancellation->WaitUntil(std::chrono::steady_clock::time_point::max());
  }));

  worker_started.get_future().get();
  manager.Shutdown();
  CHECK(cancellation_count.load() == 1);
  CHECK_FALSE(manager.IsCurrent(*operation));
}

TEST_CASE("arbitration polling rejects terminal responses after deadline",
          "[live][session][arbitration][deadline]") {
  CHECK(ClassifyArbitrationPollResponse(200, false, false) == ArbitrationPollAction::kComplete);
  CHECK(ClassifyArbitrationPollResponse(202, false, false) ==
        ArbitrationPollAction::kRefreshRevision);
  CHECK(ClassifyArbitrationPollResponse(200, false, true) == ArbitrationPollAction::kStop);
  CHECK(ClassifyArbitrationPollResponse(200, true, false) == ArbitrationPollAction::kStop);
  CHECK(ClassifyArbitrationPollResponse(0, false, false) == ArbitrationPollAction::kRetry);
  CHECK(ClassifyArbitrationPollResponse(409, false, false) == ArbitrationPollAction::kFail);
}

TEST_CASE("explicit precondition retries rotate community idempotency keys",
          "[live][community][idempotency]") {
  using LibertyRecomp::Network::RequiresFreshIdempotencyKey;
  CHECK(RequiresFreshIdempotencyKey(412));
  CHECK_FALSE(RequiresFreshIdempotencyKey(0));
  CHECK_FALSE(RequiresFreshIdempotencyKey(202));
  CHECK_FALSE(RequiresFreshIdempotencyKey(500));
}

TEST_CASE("arbitration completion validates session and nonce snapshots",
          "[live][session][arbitration][validation]") {
  constexpr uint64_t session_id = 0xAE00000000000040ULL;
  constexpr uint64_t nonce = 0xBA00000000000040ULL;
  constexpr uint64_t different_session_id = 0xAE00000000000041ULL;
  constexpr uint64_t different_nonce = 0xBA00000000000041ULL;
  XSESSION_ARBITRATION_CONTEXT context{
      .session_id = session_id,
      .nonce = nonce,
      .registration_duration_seconds = 300,
      .flags = 0,
      .results_buffer_size = rex::system::xam::kSessionArbitrationResultsSize,
      .results_ptr = 0x10000000};
  SessionRecord current = MakeArbitrationRecord(session_id, nonce);
  SessionRecord registered = current;
  registered.lifecycle_state = SessionLifecycleState::kRegistration;

  CHECK(rex::system::xam::detail::IsSessionArbitrationCompletionCompatible(context, current,
                                                                           registered));

  current.nonce = different_nonce;
  CHECK_FALSE(rex::system::xam::detail::IsSessionArbitrationCompletionCompatible(context, current,
                                                                                 registered));
  current = MakeArbitrationRecord(session_id, nonce);
  registered.session_id = different_session_id;
  CHECK_FALSE(rex::system::xam::detail::IsSessionArbitrationCompletionCompatible(context, current,
                                                                                 registered));
  registered = current;
  registered.lifecycle_state = SessionLifecycleState::kRegistration;
  current.lifecycle_state = SessionLifecycleState::kInGame;
  CHECK_FALSE(rex::system::xam::detail::IsSessionArbitrationCompletionCompatible(context, current,
                                                                                 registered));
}

}  // namespace
