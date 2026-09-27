// Standalone C++20 boundary tests for room-probe scheduling and async receipts.
#include <array>
#include <cassert>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

#include "native_room_light_probe.h"

namespace {
using namespace rex::graphics::gta4_native;
using Queue = NativeRoomLightProbeQueue;
using Key = NativeRoomLightProbeKey;
using Receipt = NativeRoomLightProbeReceipt;
using Completion = NativeRoomLightProbeCompletion;
using Part = Receipt::Part;
using State = Receipt::State;

Key MakeKey(uint64_t instance, uint32_t selector = 11, uint32_t effective = 1) {
  return {instance, 100, 200, selector, 0, 300, effective};
}

std::shared_ptr<Receipt> MakeReceipt(Key key = MakeKey(1), uint32_t frame = 11,
                                     bool stencil = true) {
  return std::make_shared<Receipt>(key, frame, 8, 9, stencil);
}

void Publish(const std::shared_ptr<Receipt>& receipt, Part part, bool success = true) {
  receipt->Begin(part);
  Completion completion(receipt, part);
  completion.SetSuccess(success);
}

void QueueTracksDistinctFrameSightings() {
  Queue queue;
  const Key key = MakeKey(1);
  assert(queue.Observe(key, 11) == Queue::Observation::kAdded);
  assert(queue.Observe(key, 11) == Queue::Observation::kExisting);
  assert(queue.Observe(key, 12) == Queue::Observation::kExisting);
  assert(queue.Observe(key, 17) == Queue::Observation::kExisting);
  const auto& entry = queue.entries().at(key);
  // Expected values independently calculated with Python from [11, 11, 12, 17].
  assert(entry.first_seen_frame == 11);
  assert(entry.last_seen_frame == 17);
  assert(entry.sightings == 3);
  assert(!queue.Next(12));
  assert(queue.Next(17) == key);
}

void QueueExcludesInFlightWork() {
  Queue queue;
  const Key preferred = MakeKey(1);
  const Key other = MakeKey(2);
  queue.Observe(preferred, 11);
  queue.Observe(other, 11);
  assert(queue.Next(11) == preferred);
  assert(queue.Begin(preferred));
  assert(!queue.Begin(preferred));
  assert(queue.entries().at(preferred).in_flight);
  assert(queue.Next(11) == other);
  queue.Observe(preferred, 12);
  assert(!queue.Next(12));
  assert(queue.Finish(preferred, false));
  assert(!queue.entries().at(preferred).in_flight);
  assert(queue.Next(12) == preferred);
}

void PriorityDoesNotSelectStaleOrInFlightKeys() {
  Queue queue;
  const Key priority = MakeKey(9, 6);
  const Key ordinary = MakeKey(1, 11);
  queue.Observe(priority, 11);
  queue.Observe(ordinary, 11);
  assert(queue.Next(11) == ordinary);
  assert(queue.Next(11, 6) == priority);
  assert(queue.Begin(priority));
  assert(queue.Next(11, 6) == ordinary);
  assert(queue.Finish(priority, false));
  // Explicit priority may outrank an unattempted nonpriority entry.
  assert(queue.Next(11, 6) == priority);
  queue.Observe(ordinary, 12);
  assert(queue.Next(12, 6) == ordinary);
  assert(!queue.Next(17, 6));
}

void RetryBudgetAndSuccessfulRetirement() {
  Queue queue;
  const Key failed = MakeKey(1);
  queue.Observe(failed, 11);
  // Python verified the full sequence against kAttemptLimit's current value.
  static_assert(Queue::kAttemptLimit == 3);
  for (const uint32_t expected_attempts : {1u, 2u, 3u}) {
    assert(queue.Next(11) == failed);
    assert(queue.Begin(failed));
    assert(queue.Finish(failed, false));
    assert(queue.entries().at(failed).attempts == expected_attempts);
  }
  assert(!queue.Next(11));
  assert(!queue.Begin(failed));
  assert(!queue.Finish(failed, true));
  assert(!queue.entries().at(failed).readback_complete);
  const Key good = MakeKey(2);
  queue.Observe(good, 11);
  assert(queue.Begin(good));
  assert(queue.Finish(good, true));
  queue.Observe(good, 12);
  assert(!queue.Next(12));
  assert(!queue.Begin(good));
  assert(!queue.Finish(good, false));
  assert(queue.entries().at(good).readback_complete);
}

void EffectiveModesAreDistinctWorkItems() {
  Queue queue;
  const Key first = MakeKey(1, 11, 0);
  const Key second = MakeKey(1, 11, 1);
  assert(first != second);
  assert(queue.Observe(first, 11) == Queue::Observation::kAdded);
  assert(queue.Observe(second, 11) == Queue::Observation::kAdded);
  assert(queue.Begin(first));
  assert(queue.Next(11) == second);
  assert(queue.Finish(first, true));
  assert(!queue.entries().at(second).readback_complete);
  assert(queue.Next(11) == second);
}

void ReceiptOutOfOrderCompletionRequiresAllParts() {
  const auto receipt = MakeReceipt();
  assert(!receipt->Terminal());
  assert(!receipt->Succeeded());
  for (Part part : {Part::kColor, Part::kStencil, Part::kQuery}) {
    receipt->Begin(part);
  }
  receipt->Complete(Part::kQuery, true);
  assert(!receipt->Terminal());
  receipt->Complete(Part::kColor, true);
  assert(!receipt->Terminal());
  receipt->Complete(Part::kStencil, true);
  assert(receipt->Terminal());
  assert(receipt->Succeeded());
}

void InlineFallbackPublishesOnlyAfterCallbackReturns() {
  const auto receipt = MakeReceipt(MakeKey(1), 11, false);
  Publish(receipt, Part::kQuery);
  receipt->Begin(Part::kColor);
  const auto analyze = [receipt] {
    Completion completion(receipt, Part::kColor);
    assert(!receipt->Terminal());
    completion.SetSuccess(true);
    assert(receipt->Get(Part::kColor) == State::kPending);
  };
  const auto reject_enqueue = [](const std::function<void()>&) { return false; };
  if (!reject_enqueue(analyze)) {
    analyze();
  }
  assert(receipt->Terminal());
  assert(receipt->Succeeded());
}

void EarlyReturnAndExceptionFailThePart() {
  const auto early = MakeReceipt();
  early->Begin(Part::kColor);
  const auto early_analysis = [early] {
    Completion completion(early, Part::kColor);
    return;
  };
  early_analysis();
  assert(early->Get(Part::kColor) == State::kFailed);
  const auto exceptional = MakeReceipt();
  exceptional->Begin(Part::kStencil);
  try {
    Completion completion(exceptional, Part::kStencil);
    throw std::runtime_error("injected artifact failure");
  } catch (const std::runtime_error&) {
  }
  assert(exceptional->Get(Part::kStencil) == State::kFailed);
}

void FailUnstartedPreservesPendingAndTerminalStates() {
  const auto receipt = MakeReceipt();
  receipt->Begin(Part::kColor);
  Publish(receipt, Part::kQuery);
  receipt->FailUnstarted();
  assert(receipt->Get(Part::kColor) == State::kPending);
  assert(receipt->Get(Part::kStencil) == State::kFailed);
  assert(receipt->Get(Part::kQuery) == State::kSucceeded);
  assert(!receipt->Terminal());
  receipt->Complete(Part::kColor, true);
  assert(receipt->Terminal());
  assert(!receipt->Succeeded());
}

void TerminalFailureCannotBeResurrected() {
  const auto receipt = MakeReceipt();
  Publish(receipt, Part::kColor, false);
  Publish(receipt, Part::kStencil);
  Publish(receipt, Part::kQuery);
  assert(receipt->Terminal());
  assert(!receipt->Succeeded());
  receipt->Begin(Part::kColor);
  receipt->Complete(Part::kColor, true);
  receipt->FailUnstarted();
  assert(receipt->Get(Part::kColor) == State::kFailed);
  assert(receipt->Terminal());
  assert(!receipt->Succeeded());
  receipt->Complete(Part::kQuery, false);
  assert(receipt->Succeeded(Part::kQuery));
}

void UnstartedSuccessCannotForgeAReceipt() {
  const auto receipt = MakeReceipt();
  receipt->Complete(Part::kColor, true);
  assert(receipt->Get(Part::kColor) == State::kNotStarted);
  receipt->FailUnstarted();
  assert(receipt->Terminal());
  assert(!receipt->Succeeded());
}

void IndependentReceiptsSurviveSlotReuse() {
  auto slot = MakeReceipt(MakeKey(1), 11, false);
  const std::weak_ptr<Receipt> old = slot;
  Publish(slot, Part::kQuery);
  slot->Begin(Part::kColor);
  std::function<void()> delayed = [receipt = slot] {
    assert(receipt->frame == 11);
    assert(receipt->key == MakeKey(1));
    Completion completion(receipt, Part::kColor);
    completion.SetSuccess(true);
  };
  slot = MakeReceipt(MakeKey(2), 17, false);
  const auto current = slot;
  assert(!old.expired());
  delayed();
  assert(old.lock()->Succeeded());
  assert(current->frame == 17);
  assert(current->Get(Part::kColor) == State::kNotStarted);
  assert(current->Get(Part::kQuery) == State::kNotStarted);
  assert(!current->Succeeded());
  delayed = {};
  assert(old.expired());
  Publish(current, Part::kColor);
  Publish(current, Part::kQuery);
  assert(current->Succeeded());
}

void AsyncCompletionPublishesPriorWrites() {
  const auto receipt = MakeReceipt(MakeKey(1), 11, false);
  Publish(receipt, Part::kQuery);
  receipt->Begin(Part::kColor);
  int published_artifact = 0;
  std::promise<void> entered;
  std::promise<void> resume;
  auto ready = entered.get_future();
  auto proceed = resume.get_future();
  std::thread worker([receipt, &published_artifact, &entered, &proceed] {
    Completion completion(receipt, Part::kColor);
    entered.set_value();
    proceed.wait();
    published_artifact = 42;
    completion.SetSuccess(true);
  });
  ready.wait();
  receipt->FailUnstarted();
  assert(receipt->Get(Part::kColor) == State::kPending);
  assert(!receipt->Terminal());
  resume.set_value();
  while (!receipt->Succeeded()) {
    std::this_thread::yield();
  }
  // Receipt acquire loads synchronize the callback's publication before join.
  assert(published_artifact == 42);
  worker.join();
}

void AsyncExceptionTerminallyFails() {
  const auto receipt = MakeReceipt(MakeKey(1), 11, false);
  Publish(receipt, Part::kQuery);
  receipt->Begin(Part::kColor);
  auto task = std::async(std::launch::async, [receipt] {
    Completion completion(receipt, Part::kColor);
    throw std::runtime_error("injected async failure");
  });
  try {
    task.get();
    assert(false);
  } catch (const std::runtime_error&) {
  }
  assert(receipt->Terminal());
  assert(!receipt->Succeeded());
}

void InputSchemaRequiresFinalPublication() {
  const auto receipt = std::make_shared<Receipt>(MakeKey(1), 11, 8, 9, true, true);
  Publish(receipt, Part::kColor);
  Publish(receipt, Part::kStencil);
  Publish(receipt, Part::kQuery);
  assert(receipt->inputs_required);
  assert(!receipt->Terminal());
  receipt->Begin(Part::kInputs);
  receipt->FailUnstarted();
  assert(receipt->Get(Part::kInputs) == State::kPending);
  {
    Completion completion(receipt, Part::kInputs);
    completion.SetSuccess(true);
    assert(!receipt->Terminal());
  }
  assert(receipt->Terminal());
  assert(receipt->Succeeded());
}

void MissingOrPartialInputsCannotSucceed() {
  const auto receipt = std::make_shared<Receipt>(MakeKey(1), 11, 8, 9, false, true);
  Publish(receipt, Part::kColor);
  Publish(receipt, Part::kQuery);
  receipt->FailUnstarted();
  assert(receipt->Terminal());
  assert(receipt->Get(Part::kInputs) == State::kFailed);
  Publish(receipt, Part::kInputs);
  assert(!receipt->Succeeded());
  const auto partial = std::make_shared<Receipt>(MakeKey(2), 17, 8, 9, false, true);
  Publish(partial, Part::kColor);
  Publish(partial, Part::kQuery);
  partial->Begin(Part::kInputs);
  try {
    Completion completion(partial, Part::kInputs);
    throw std::runtime_error("metadata flush failed after texture publication");
  } catch (const std::runtime_error&) {
  }
  assert(partial->Terminal());
  assert(!partial->Succeeded());
}

void LegacyReceiptsDoNotRequireNewInputSchema() {
  const auto legacy = MakeReceipt(MakeKey(1), 11, false);
  assert(!legacy->inputs_required);
  assert(legacy->Succeeded(Part::kInputs));
  Publish(legacy, Part::kColor);
  Publish(legacy, Part::kQuery);
  assert(legacy->Succeeded());
}

void InputGridMatchesRawCopyCoordinates() {
  // All expected coordinates/offsets/extents were independently calculated in Python.
  for (uint32_t y = 0; y < kNativeRoomLightInputGridAxis; ++y) {
    for (uint32_t x = 0; x < kNativeRoomLightInputGridAxis; ++x) {
      const auto point = NativeRoomLightInputPoint(1, 1, x, y);
      assert(point.x == 0 && point.y == 0);
      assert(point.width == 1 && point.height == 1);
      assert(point.offset < 4096);
      assert(point.offset % 16 == 0);
    }
  }
  const auto single_last = NativeRoomLightInputPoint(1, 1, 15, 15);
  assert(single_last.offset == 4080);
  const auto first = NativeRoomLightInputPoint(3456, 2234, 0, 0);
  assert(first.x == 108 && first.y == 69 && first.offset == 0);
  const auto last = NativeRoomLightInputPoint(3456, 2234, 15, 15);
  assert(last.x == 3348 && last.y == 2163 && last.offset == 4080);
  const auto block = NativeRoomLightInputPoint(3456, 2234, 15, 15, 4);
  assert(block.x == 3348 && block.y == 2160 && block.offset == 4080);
  assert(block.width == 4 && block.height == 4);
  const auto edge = NativeRoomLightInputPoint(3, 7, 15, 15, 4);
  assert(edge.x == 0 && edge.y == 4 && edge.width == 3 && edge.height == 3);
  assert(NativeRoomLightInputPoint(0, 1, 0, 0).width == 0);
  assert(NativeRoomLightInputPoint(1, 0, 0, 0).height == 0);
  assert(NativeRoomLightInputPoint(1, 1, 16, 0).width == 0);
  assert(NativeRoomLightInputPoint(1, 1, 0, 16).height == 0);
  assert(NativeRoomLightInputPoint(1, 1, 0, 0, 2).width == 0);
}

void InputReceiptOwnershipHandoffDoesNotPublishEarly() {
  const auto receipt = std::make_shared<Receipt>(MakeKey(1), 11, 8, 9, true, true);
  receipt->Begin(Part::kInputs);
  {
    Completion recording(receipt, Part::kInputs);
    recording.Release();
  }
  assert(receipt->Get(Part::kInputs) == State::kPending);
  Publish(receipt, Part::kInputs);
  assert(receipt->Succeeded(Part::kInputs));
  const auto failed = std::make_shared<Receipt>(MakeKey(1), 11, 8, 9, true, true);
  failed->Begin(Part::kInputs);
  try {
    Completion recording(failed, Part::kInputs);
    throw std::bad_alloc();
  } catch (const std::bad_alloc&) {
  }
  assert(failed->Get(Part::kInputs) == State::kFailed);
}

}  // namespace

int main() {
  const std::array tests = {
      QueueTracksDistinctFrameSightings, QueueExcludesInFlightWork,
      PriorityDoesNotSelectStaleOrInFlightKeys, RetryBudgetAndSuccessfulRetirement,
      EffectiveModesAreDistinctWorkItems, ReceiptOutOfOrderCompletionRequiresAllParts,
      InlineFallbackPublishesOnlyAfterCallbackReturns, EarlyReturnAndExceptionFailThePart,
      FailUnstartedPreservesPendingAndTerminalStates, TerminalFailureCannotBeResurrected,
      UnstartedSuccessCannotForgeAReceipt, IndependentReceiptsSurviveSlotReuse,
      AsyncCompletionPublishesPriorWrites, AsyncExceptionTerminallyFails,
      InputSchemaRequiresFinalPublication, MissingOrPartialInputsCannotSucceed,
      LegacyReceiptsDoNotRequireNewInputSchema, InputGridMatchesRawCopyCoordinates,
      InputReceiptOwnershipHandoffDoesNotPublishEarly,
  };
  for (auto test : tests) {
    test();
  }
  std::cout << tests.size() << " room-light queue/receipt tests passed\n";
}
