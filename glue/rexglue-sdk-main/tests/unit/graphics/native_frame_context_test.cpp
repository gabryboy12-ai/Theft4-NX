/**
 * @file native_frame_context_test.cpp
 * @brief Two-frame ownership tests for the GTA IV native renderer.
 */

#include <catch2/catch_test_macros.hpp>

#include "graphics/gta4_native/native_frame_context.h"

namespace gta4 = rex::graphics::gta4_native;

namespace {

using Ring = gta4::NativeFrameContextRing;

constexpr Ring::QueryReadbackResources kSlotZeroQueries{0x1000, 0x1001};
constexpr Ring::QueryReadbackResources kSlotOneQueries{0x2000, 0x2001};

void ConfigureRing(Ring& ring) {
  REQUIRE(ring.ConfigureSlot(0, kSlotZeroQueries));
  REQUIRE(ring.ConfigureSlot(1, kSlotOneQueries));
}

Ring::SubmissionSerial Submit(Ring& ring, Ring::FrameToken token) {
  const auto serial = ring.PrepareSubmission(token);
  REQUIRE(serial);
  REQUIRE(ring.CommitSubmission(token, *serial));
  return *serial;
}

void Complete(Ring& ring, Ring::FrameToken token, Ring::SubmissionSerial serial) {
  REQUIRE(ring.BeginWait(token, serial));
  REQUIRE(ring.CompleteWait(token, serial));
}

}  // namespace

TEST_CASE("GTA IV native frame contexts permanently own distinct query resources") {
  Ring ring;
  REQUIRE(Ring::kSlotCount == 2);
  REQUIRE_FALSE(ring.ConfigureSlot(0, {}));
  REQUIRE_FALSE(ring.ConfigureSlot(0, {0x1000, 0x1000}));
  REQUIRE(ring.ConfigureSlot(0, kSlotZeroQueries));
  REQUIRE_FALSE(ring.ConfigureSlot(0, kSlotOneQueries));
  REQUIRE_FALSE(ring.ConfigureSlot(1, {kSlotZeroQueries.readback, 0x3000}));
  REQUIRE(ring.ConfigureSlot(1, kSlotOneQueries));
  REQUIRE_FALSE(ring.ConfigureSlot(2, {0x3000, 0x3001}));

  CHECK(ring.GetSlotResources(0) == kSlotZeroQueries);
  CHECK(ring.GetSlotResources(1) == kSlotOneQueries);
  CHECK_FALSE(ring.GetSlotResources(2));
}

TEST_CASE("GTA IV native frame slots do not require diagnostic query resources") {
  Ring ring;
  const auto frame = ring.BeginFrame(0);
  REQUIRE(frame);
  CHECK_FALSE(ring.ClaimQueryReadback(*frame));
  REQUIRE(ring.RollbackFrame(*frame));
  REQUIRE(ring.ResetSlot(*frame));
}

TEST_CASE("GTA IV native slots require submit wait readback and reset before reuse") {
  Ring ring;
  ConfigureRing(ring);

  const auto frame_zero = ring.BeginFrame(0);
  REQUIRE(frame_zero);
  REQUIRE_FALSE(ring.BeginFrame(1));
  REQUIRE(ring.ClaimQueryReadback(*frame_zero) == kSlotZeroQueries);
  REQUIRE_FALSE(ring.ClaimQueryReadback(*frame_zero));

  const auto serial_zero = Submit(ring, *frame_zero);
  REQUIRE_FALSE(ring.ResetSlot(*frame_zero));
  REQUIRE_FALSE(ring.GetCompletedQueryReadback(*frame_zero));

  // The CPU may record the other slot while slot zero remains in flight.
  const auto frame_one = ring.BeginFrame(1);
  REQUIRE(frame_one);
  REQUIRE_FALSE(ring.BeginFrame(0));
  REQUIRE(ring.RollbackFrame(*frame_one));
  REQUIRE(ring.ResetSlot(*frame_one));

  REQUIRE_FALSE(ring.BeginWait(*frame_zero, 0));
  REQUIRE(ring.BeginWait(*frame_zero, serial_zero));
  REQUIRE_FALSE(ring.ResetSlot(*frame_zero));
  REQUIRE(ring.CancelWait(*frame_zero, serial_zero));
  REQUIRE_FALSE(ring.GetCompletedQueryReadback(*frame_zero));
  Complete(ring, *frame_zero, serial_zero);

  REQUIRE(ring.GetCompletedQueryReadback(*frame_zero) == kSlotZeroQueries);
  REQUIRE_FALSE(ring.ResetSlot(*frame_zero));
  REQUIRE(ring.AcknowledgeQueryReadback(*frame_zero));
  REQUIRE_FALSE(ring.AcknowledgeQueryReadback(*frame_zero));
  REQUIRE(ring.ResetSlot(*frame_zero));

  const auto reused = ring.BeginFrame(0);
  REQUIRE(reused);
  REQUIRE_FALSE(ring.ClaimQueryReadback(*frame_zero));
  REQUIRE_FALSE(ring.GetCompletedQueryReadback(*frame_zero));
  REQUIRE_FALSE(ring.AcknowledgeQueryReadback(*frame_zero));
  REQUIRE(ring.ClaimQueryReadback(*reused) == kSlotZeroQueries);
  const auto reused_serial = Submit(ring, *reused);
  Complete(ring, *reused, reused_serial);
  REQUIRE(ring.GetCompletedQueryReadback(*reused) == kSlotZeroQueries);
  REQUIRE(ring.AcknowledgeQueryReadback(*reused));
  REQUIRE(ring.ResetSlot(*reused));
}

TEST_CASE("GTA IV native query ownership cannot be released in flight or by stale identity") {
  Ring ring;
  ConfigureRing(ring);
  const auto frame = ring.BeginFrame(1);
  REQUIRE(frame);
  REQUIRE(ring.ClaimQueryReadback(*frame) == kSlotOneQueries);
  REQUIRE_FALSE(ring.ReleaseSlot(1, kSlotOneQueries));
  const auto serial = Submit(ring, *frame);
  Complete(ring, *frame, serial);
  REQUIRE(ring.AcknowledgeQueryReadback(*frame));
  REQUIRE(ring.ResetSlot(*frame));

  REQUIRE_FALSE(ring.ReleaseSlot(1, kSlotZeroQueries));
  REQUIRE(ring.ReleaseSlot(1, kSlotOneQueries));
  CHECK_FALSE(ring.GetSlotResources(1));
  REQUIRE(ring.ConfigureSlot(1, kSlotOneQueries));
  CHECK(ring.GetSlotResources(1) == kSlotOneQueries);
}

TEST_CASE("GTA IV native frame slots keep two pending query payloads distinct across reuse") {
  Ring ring;
  ConfigureRing(ring);

  const auto slot_zero = ring.BeginFrame(0);
  REQUIRE(slot_zero);
  REQUIRE(ring.ClaimQueryReadback(*slot_zero) == kSlotZeroQueries);
  const auto serial_zero = Submit(ring, *slot_zero);

  const auto slot_one = ring.BeginFrame(1);
  REQUIRE(slot_one);
  REQUIRE(ring.ClaimQueryReadback(*slot_one) == kSlotOneQueries);
  const auto serial_one = Submit(ring, *slot_one);
  CHECK(serial_one > serial_zero);
  REQUIRE(ring.GetSlotSnapshot(0)->query_readback_claimed);
  REQUIRE(ring.GetSlotSnapshot(1)->query_readback_claimed);

  Complete(ring, *slot_one, serial_one);
  REQUIRE(ring.GetCompletedQueryReadback(*slot_one) == kSlotOneQueries);
  REQUIRE_FALSE(ring.GetCompletedQueryReadback(*slot_zero));
  REQUIRE(ring.AcknowledgeQueryReadback(*slot_one));
  REQUIRE(ring.ResetSlot(*slot_one));

  const auto reused_slot_one = ring.BeginFrame(1);
  REQUIRE(reused_slot_one);
  REQUIRE_FALSE(ring.ClaimQueryReadback(*slot_one));
  REQUIRE(ring.ClaimQueryReadback(*reused_slot_one) == kSlotOneQueries);
  const auto reused_serial_one = Submit(ring, *reused_slot_one);

  Complete(ring, *slot_zero, serial_zero);
  REQUIRE(ring.GetCompletedQueryReadback(*slot_zero) == kSlotZeroQueries);
  REQUIRE(ring.AcknowledgeQueryReadback(*slot_zero));
  REQUIRE(ring.ResetSlot(*slot_zero));
  Complete(ring, *reused_slot_one, reused_serial_one);
  REQUIRE(ring.GetCompletedQueryReadback(*reused_slot_one) == kSlotOneQueries);
  REQUIRE(ring.AcknowledgeQueryReadback(*reused_slot_one));
  REQUIRE(ring.ResetSlot(*reused_slot_one));
}

TEST_CASE("GTA IV native shared layout rollback restores the exact queue tail") {
  Ring ring;
  ConfigureRing(ring);
  constexpr Ring::ObjectKey kImage{0xA000, 7};
  constexpr Ring::Layout kUndefined = 0;
  constexpr Ring::Layout kColorWrite = 2;
  constexpr Ring::Layout kShaderRead = 5;

  REQUIRE(ring.RegisterSharedLayout(kImage, kUndefined));
  const auto frame_zero = ring.BeginFrame(0);
  REQUIRE(frame_zero);
  REQUIRE_FALSE(ring.PredictSharedLayout(*frame_zero, kImage, kColorWrite, kShaderRead));
  REQUIRE(ring.PredictSharedLayout(*frame_zero, kImage, kUndefined, kColorWrite));
  REQUIRE(ring.PredictSharedLayout(*frame_zero, kImage, kColorWrite, kShaderRead));
  REQUIRE(ring.GetPredictedSharedLayout(kImage) == kShaderRead);

  const auto rolled_serial = ring.PrepareSubmission(*frame_zero);
  REQUIRE(rolled_serial);
  REQUIRE(ring.RollbackFrame(*frame_zero));
  REQUIRE(ring.GetPredictedSharedLayout(kImage) == kUndefined);
  REQUIRE(ring.ResetSlot(*frame_zero));

  const auto frame_one = ring.BeginFrame(1);
  REQUIRE(frame_one);
  REQUIRE(ring.PredictSharedLayout(*frame_one, kImage, kUndefined, kColorWrite));
  const auto committed_serial = Submit(ring, *frame_one);
  CHECK(committed_serial > *rolled_serial);
  REQUIRE(ring.GetPredictedSharedLayout(kImage) == kColorWrite);
  Complete(ring, *frame_one, committed_serial);
  REQUIRE(ring.ResetSlot(*frame_one));
}

TEST_CASE("GTA IV native committed layouts feed the next in-flight frame") {
  Ring ring;
  ConfigureRing(ring);
  constexpr Ring::ObjectKey kImage{0xB000, 1};
  constexpr Ring::Layout kInitial = 1;
  constexpr Ring::Layout kTransfer = 3;
  constexpr Ring::Layout kSampled = 4;

  REQUIRE(ring.RegisterSharedLayout(kImage, kInitial));
  const auto frame_zero = ring.BeginFrame(0);
  REQUIRE(frame_zero);
  REQUIRE(ring.PredictSharedLayout(*frame_zero, kImage, kInitial, kTransfer));
  const auto serial_zero = Submit(ring, *frame_zero);

  const auto frame_one = ring.BeginFrame(1);
  REQUIRE(frame_one);
  REQUIRE_FALSE(ring.PredictSharedLayout(*frame_one, kImage, kInitial, kSampled));
  REQUIRE(ring.PredictSharedLayout(*frame_one, kImage, kTransfer, kSampled));
  const auto serial_one = Submit(ring, *frame_one);
  CHECK(serial_one > serial_zero);

  // A later ordered-queue fence proves GPU completion progress, but the older
  // slot still cannot be reset until its own wait transition is consumed.
  Complete(ring, *frame_one, serial_one);
  REQUIRE(ring.ResetSlot(*frame_one));
  REQUIRE_FALSE(ring.BeginFrame(0));
  Complete(ring, *frame_zero, serial_zero);
  REQUIRE(ring.ResetSlot(*frame_zero));
  CHECK(ring.completed_submission_serial() == serial_one);
  CHECK(ring.GetPredictedSharedLayout(kImage) == kSampled);
}

TEST_CASE("GTA IV native deferred destruction journals are retryable and block reuse") {
  Ring ring;
  ConfigureRing(ring);
  constexpr Ring::ObjectKey kImage{0xC000, 9};
  constexpr Ring::Layout kInitialLayout = 1;

  REQUIRE(ring.RegisterSharedLayout(kImage, kInitialLayout));
  const auto frame_zero = ring.BeginFrame(0);
  REQUIRE(frame_zero);
  REQUIRE(ring.MarkObjectUse(*frame_zero, kImage));
  REQUIRE(ring.DeferDestruction(*frame_zero, kImage));
  REQUIRE_FALSE(ring.MarkObjectUse(*frame_zero, kImage));
  REQUIRE_FALSE(ring.DeferDestruction(*frame_zero, kImage));
  const auto serial_zero = Submit(ring, *frame_zero);

  const auto frame_one = ring.BeginFrame(1);
  REQUIRE(frame_one);
  REQUIRE_FALSE(ring.MarkObjectUse(*frame_one, kImage));
  REQUIRE(ring.RollbackFrame(*frame_one));
  REQUIRE(ring.ResetSlot(*frame_one));

  REQUIRE_FALSE(ring.GetDeferredDestructionJournal(*frame_zero));
  Complete(ring, *frame_zero, serial_zero);
  const auto first_read = ring.GetDeferredDestructionJournal(*frame_zero);
  const auto retry_read = ring.GetDeferredDestructionJournal(*frame_zero);
  REQUIRE(first_read);
  REQUIRE(retry_read);
  REQUIRE(first_read->size() == 1);
  CHECK(*first_read == *retry_read);
  CHECK(first_read->front() == kImage);
  REQUIRE_FALSE(ring.ResetSlot(*frame_zero));

  REQUIRE(ring.AcknowledgeDeferredDestructionJournal(*frame_zero));
  REQUIRE(ring.WasDestroyed(kImage));
  REQUIRE_FALSE(ring.GetPredictedSharedLayout(kImage));
  REQUIRE_FALSE(ring.RegisterSharedLayout(kImage, 0));
  REQUIRE(ring.ResetSlot(*frame_zero));

  const auto reused = ring.BeginFrame(0);
  REQUIRE(reused);
  REQUIRE_FALSE(ring.MarkObjectUse(*reused, kImage));
  REQUIRE_FALSE(ring.DeferDestruction(*reused, kImage));
  REQUIRE(ring.RollbackFrame(*reused));
  REQUIRE(ring.ResetSlot(*reused));
}

TEST_CASE("GTA IV native rollback preserves deferred destruction without a GPU wait") {
  Ring ring;
  ConfigureRing(ring);
  constexpr Ring::ObjectKey kObject{0xD000, 3};

  const auto frame = ring.BeginFrame(0);
  REQUIRE(frame);
  REQUIRE(ring.DeferDestruction(*frame, kObject));
  REQUIRE(ring.RollbackFrame(*frame));
  const auto journal = ring.GetDeferredDestructionJournal(*frame);
  REQUIRE(journal);
  REQUIRE(journal->size() == 1);
  REQUIRE_FALSE(ring.ResetSlot(*frame));
  REQUIRE(ring.AcknowledgeDeferredDestructionJournal(*frame));
  REQUIRE(ring.ResetSlot(*frame));
}
