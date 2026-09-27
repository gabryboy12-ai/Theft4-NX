#include "graphics/gta4_native/native_submission_lifetime.h"

#include <catch2/catch_test_macros.hpp>

namespace gta4 = rex::graphics::gta4_native;

namespace {

using Lifetime = gta4::NativeSubmissionLifetime;

constexpr Lifetime::ResourceKey kTexture{0x1000, 1};
constexpr Lifetime::ResourceKey kBuffer{0x2000, 3};

Lifetime::SubmissionSerial SubmitFrame(Lifetime& lifetime, uint32_t slot,
                                       Lifetime::ResourceKey resource) {
  const auto token = lifetime.BeginFrame(slot);
  REQUIRE(token);
  REQUIRE(lifetime.StageQueuedReference(*token, resource));
  const auto serial = lifetime.PrepareSubmission(*token);
  REQUIRE(serial);
  REQUIRE(lifetime.CommitSubmission(*token, *serial));
  return *serial;
}

}  // namespace

TEST_CASE("native submission lifetime commits queued references transactionally") {
  Lifetime lifetime;
  REQUIRE(lifetime.RegisterResource(kTexture, true));
  REQUIRE(lifetime.QueueReference(kTexture));

  const auto token = lifetime.BeginFrame(0);
  REQUIRE(token);
  REQUIRE(lifetime.StageQueuedReference(*token, kTexture));
  const auto serial = lifetime.PrepareSubmission(*token);
  REQUIRE(serial);

  const auto before_commit = lifetime.GetResourceSnapshot(kTexture);
  REQUIRE(before_commit);
  CHECK(before_commit->queued_reference_count == 1);
  CHECK(before_commit->last_use_serial == 0);

  REQUIRE(lifetime.CommitSubmission(*token, *serial));
  const auto after_commit = lifetime.GetResourceSnapshot(kTexture);
  REQUIRE(after_commit);
  CHECK(after_commit->queued_reference_count == 0);
  CHECK(after_commit->last_use_serial == *serial);
  REQUIRE_FALSE(lifetime.BeginFrame(0));

  REQUIRE(lifetime.NotifySlotFenceCompleted(0, *serial));
  CHECK(lifetime.completed_submission_serial() == *serial);
  REQUIRE(lifetime.BeginFrame(0));
}

TEST_CASE("native submission rollback keeps references and never reuses serials") {
  Lifetime lifetime;
  REQUIRE(lifetime.RegisterResource(kBuffer, false));
  REQUIRE(lifetime.QueueReference(kBuffer));

  const auto first_token = lifetime.BeginFrame(0);
  REQUIRE(first_token);
  REQUIRE(lifetime.StageQueuedReference(*first_token, kBuffer));
  const auto failed_serial = lifetime.PrepareSubmission(*first_token);
  REQUIRE(failed_serial);
  REQUIRE(lifetime.RollbackFrame(*first_token));

  const auto rolled_back = lifetime.GetResourceSnapshot(kBuffer);
  REQUIRE(rolled_back);
  CHECK(rolled_back->queued_reference_count == 1);
  CHECK(rolled_back->last_use_serial == 0);

  const auto second_token = lifetime.BeginFrame(0);
  REQUIRE(second_token);
  REQUIRE(lifetime.StageQueuedReference(*second_token, kBuffer));
  const auto committed_serial = lifetime.PrepareSubmission(*second_token);
  REQUIRE(committed_serial);
  CHECK(*committed_serial > *failed_serial);
  REQUIRE(lifetime.CommitSubmission(*second_token, *committed_serial));
  REQUIRE_FALSE(lifetime.RollbackFrame(*second_token));
}

TEST_CASE("native submission abandon releases references without creating a GPU use") {
  Lifetime lifetime;
  REQUIRE(lifetime.RegisterResource(kBuffer, false));
  REQUIRE(lifetime.QueueReference(kBuffer, 2));

  const auto token = lifetime.BeginFrame(1);
  REQUIRE(token);
  REQUIRE(lifetime.StageQueuedReference(*token, kBuffer, 2));
  REQUIRE(lifetime.AbandonFrame(*token));

  const auto resource = lifetime.GetResourceSnapshot(kBuffer);
  REQUIRE(resource);
  CHECK(resource->queued_reference_count == 0);
  CHECK(resource->last_use_serial == 0);
  const auto slot = lifetime.GetFrameSlotSnapshot(1);
  REQUIRE(slot);
  CHECK(slot->state == Lifetime::FrameSlotState::kAvailable);
}

TEST_CASE("native submission slots cannot reserve the same queued reference") {
  Lifetime lifetime;
  REQUIRE(lifetime.RegisterResource(kBuffer, false));
  REQUIRE(lifetime.QueueReference(kBuffer));

  const auto first_token = lifetime.BeginFrame(0);
  const auto second_token = lifetime.BeginFrame(1);
  REQUIRE(first_token);
  REQUIRE(second_token);
  REQUIRE(lifetime.StageQueuedReference(*first_token, kBuffer));
  REQUIRE_FALSE(lifetime.StageQueuedReference(*second_token, kBuffer));

  REQUIRE(lifetime.RollbackFrame(*first_token));
  REQUIRE(lifetime.StageQueuedReference(*second_token, kBuffer));
  const auto serial = lifetime.PrepareSubmission(*second_token);
  REQUIRE(serial);
  REQUIRE(lifetime.CommitSubmission(*second_token, *serial));
}

TEST_CASE("native submission slots reject stale tokens and mismatched fences") {
  Lifetime lifetime;
  REQUIRE(lifetime.RegisterResource(kBuffer, false));
  REQUIRE(lifetime.QueueReference(kBuffer));
  const auto first_token = lifetime.BeginFrame(0);
  REQUIRE(first_token);
  REQUIRE(lifetime.RollbackFrame(*first_token));

  const auto second_token = lifetime.BeginFrame(0);
  REQUIRE(second_token);
  REQUIRE_FALSE(lifetime.StageQueuedReference(*first_token, kBuffer));
  REQUIRE(lifetime.StageQueuedReference(*second_token, kBuffer));
  const auto serial = lifetime.PrepareSubmission(*second_token);
  REQUIRE(serial);
  REQUIRE(lifetime.CommitSubmission(*second_token, *serial));
  REQUIRE_FALSE(lifetime.NotifySlotFenceCompleted(0, 0));
  REQUIRE_FALSE(lifetime.NotifySlotFenceCompleted(1, *serial));
  REQUIRE(lifetime.NotifySlotFenceCompleted(0, *serial));
}

TEST_CASE("native resource retirement waits for queued references and GPU completion") {
  Lifetime lifetime;
  REQUIRE(lifetime.RegisterResource(kBuffer, false));
  REQUIRE(lifetime.QueueReference(kBuffer));
  const auto serial = SubmitFrame(lifetime, 0, kBuffer);

  const auto epoch = lifetime.RequestRetirement(kBuffer);
  REQUIRE(epoch);
  CHECK(*epoch == 0);
  REQUIRE_FALSE(lifetime.QueueReference(kBuffer));
  REQUIRE_FALSE(lifetime.CanRetire(kBuffer));
  REQUIRE_FALSE(lifetime.EraseRetiredResource(kBuffer));

  REQUIRE(lifetime.NotifySlotFenceCompleted(0, serial));
  REQUIRE(lifetime.CanRetire(kBuffer));
  REQUIRE(lifetime.EraseRetiredResource(kBuffer));
  REQUIRE_FALSE(lifetime.GetResourceSnapshot(kBuffer));
}

TEST_CASE("descriptor retirement requires a committed tombstone in both slots") {
  Lifetime lifetime;
  REQUIRE(lifetime.RegisterResource(kTexture, true));
  REQUIRE(lifetime.QueueReference(kTexture));
  const auto serial = SubmitFrame(lifetime, 0, kTexture);
  const auto epoch = lifetime.RequestRetirement(kTexture);
  REQUIRE(epoch);
  REQUIRE(*epoch > 0);

  // A tombstone may not replace a descriptor needed by a queued or in-flight
  // frame, and a submitted slot may not have its table copy mutated.
  REQUIRE_FALSE(lifetime.GetDescriptorTombstones(0, *epoch));
  REQUIRE_FALSE(lifetime.GetDescriptorTombstones(1, *epoch));
  REQUIRE(lifetime.NotifySlotFenceCompleted(0, serial));

  const auto slot_zero_journal = lifetime.GetDescriptorTombstones(0, *epoch);
  REQUIRE(slot_zero_journal);
  REQUIRE(slot_zero_journal->size() == 1);
  const Lifetime::DescriptorTombstone expected{kTexture, *epoch};
  CHECK(slot_zero_journal->front() == expected);
  REQUIRE(lifetime.CommitDescriptorTombstones(0, *epoch));
  REQUIRE_FALSE(lifetime.CanRetire(kTexture));

  const auto slot_one_journal = lifetime.GetDescriptorTombstones(1, *epoch);
  REQUIRE(slot_one_journal);
  REQUIRE(slot_one_journal->size() == 1);
  REQUIRE(lifetime.CommitDescriptorTombstones(1, *epoch));
  REQUIRE(lifetime.CanRetire(kTexture));
}

TEST_CASE("descriptor tombstone query is rollback safe and epoch ordered") {
  Lifetime lifetime;
  constexpr Lifetime::ResourceKey kTexture2{0x1001, 1};
  REQUIRE(lifetime.RegisterResource(kTexture, true));
  REQUIRE(lifetime.RegisterResource(kTexture2, true));
  const auto first_epoch = lifetime.RequestRetirement(kTexture);
  const auto second_epoch = lifetime.RequestRetirement(kTexture2);
  REQUIRE(first_epoch);
  REQUIRE(second_epoch);
  REQUIRE(*second_epoch > *first_epoch);

  const auto uncommitted = lifetime.GetDescriptorTombstones(0, *second_epoch);
  REQUIRE(uncommitted);
  REQUIRE(uncommitted->size() == 2);
  const auto unchanged = lifetime.GetFrameSlotSnapshot(0);
  REQUIRE(unchanged);
  CHECK(unchanged->applied_descriptor_epoch == 0);

  REQUIRE(lifetime.CommitDescriptorTombstones(0, *first_epoch));
  const auto remainder = lifetime.GetDescriptorTombstones(0, *second_epoch);
  REQUIRE(remainder);
  REQUIRE(remainder->size() == 1);
  CHECK(remainder->front().resource == kTexture2);
  REQUIRE(lifetime.CommitDescriptorTombstones(0, *second_epoch));
}

TEST_CASE("later ordered queue completion advances the global completion frontier") {
  Lifetime lifetime;
  constexpr Lifetime::ResourceKey kFirst{0x3000, 1};
  constexpr Lifetime::ResourceKey kSecond{0x3001, 1};
  REQUIRE(lifetime.RegisterResource(kFirst, false));
  REQUIRE(lifetime.RegisterResource(kSecond, false));
  REQUIRE(lifetime.QueueReference(kFirst));
  REQUIRE(lifetime.QueueReference(kSecond));
  const auto first_serial = SubmitFrame(lifetime, 0, kFirst);
  const auto second_serial = SubmitFrame(lifetime, 1, kSecond);
  REQUIRE(second_serial > first_serial);
  REQUIRE(lifetime.RequestRetirement(kFirst));

  REQUIRE(lifetime.NotifySlotFenceCompleted(1, second_serial));
  CHECK(lifetime.completed_submission_serial() == second_serial);
  REQUIRE(lifetime.CanRetire(kFirst));

  // Slot zero remains conservatively unavailable until its own fence callback
  // is consumed, even though ordered-queue completion proves its GPU work done.
  REQUIRE_FALSE(lifetime.BeginFrame(0));
  REQUIRE(lifetime.NotifySlotFenceCompleted(0, first_serial));
  REQUIRE(lifetime.BeginFrame(0));
}

TEST_CASE("descriptor tombstones wait for every queued use") {
  Lifetime lifetime;
  REQUIRE(lifetime.RegisterResource(kTexture, true));
  REQUIRE(lifetime.QueueReference(kTexture));
  const auto epoch = lifetime.RequestRetirement(kTexture);
  REQUIRE(epoch);
  REQUIRE_FALSE(lifetime.GetDescriptorTombstones(0, *epoch));

  const auto token = lifetime.BeginFrame(0);
  REQUIRE(token);
  REQUIRE(lifetime.StageQueuedReference(*token, kTexture));
  REQUIRE(lifetime.AbandonFrame(*token));

  const auto journal = lifetime.GetDescriptorTombstones(0, *epoch);
  REQUIRE(journal);
  REQUIRE(journal->size() == 1);
}
