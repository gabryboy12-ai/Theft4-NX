#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "graphics/gta4_native/native_owner_retirement.h"

namespace {
struct Entry {
  uint64_t id = 0;
  uint64_t last_submission = 0;
};
using Queue = rex::graphics::gta4_native::NativeOwnerRetirementQueue<Entry>;
using Watch = rex::graphics::gta4_native::NativeOwnerRetirementWatch<Entry>;
struct Owner {
  Watch retirements;
};
uint64_t Submission(const Entry& entry) { return entry.last_submission; }
}  // namespace

TEST_CASE("persistent buffer retirement requires both owner expiry and GPU completion",
          "[gta4-native][owner-retirement]") {
  auto queue = std::make_shared<Queue>();
  auto owner = std::make_shared<Owner>();
  auto queued_command = owner;
  Entry vertex{1, 4};
  Entry other_conversion{2, 6};
  Entry index{3, 8};
  owner->retirements.Track(queue, vertex);
  owner->retirements.Track(queue, other_conversion);
  owner->retirements.Track(queue, index);
  std::unordered_set<uint64_t> released;
  uint32_t inspected = 0;
  auto submission = [&](const Entry& entry) {
    ++inspected;
    return entry.last_submission;
  };
  auto release = [&](const Entry& entry) { REQUIRE(released.insert(entry.id).second); };
  queue->DrainCompleted(100, submission, release);
  CHECK(inspected == 0);
  CHECK(released.empty());
  owner.reset();
  queue->DrainCompleted(100, submission, release);
  CHECK(inspected == 0);
  // A queued draw can update the last GPU use until its strong owner expires.
  vertex.last_submission = 10;
  queued_command.reset();
  queue->DrainCompleted(6, submission, release);
  CHECK(inspected == 3);
  CHECK(released == std::unordered_set<uint64_t>{2});
  queue->DrainCompleted(6, submission, release);
  CHECK(inspected == 3);
  queue->DrainCompleted(8, submission, release);
  CHECK(released == std::unordered_set<uint64_t>{2, 3});
  queue->DrainCompleted(10, submission, release);
  CHECK(released == std::unordered_set<uint64_t>{1, 2, 3});
  queue->DrainCompleted(100, submission, release);
  CHECK(inspected == 3);
}

TEST_CASE("persistent owner notification keeps stable entries through map rehash",
          "[gta4-native][owner-retirement]") {
  auto queue = std::make_shared<Queue>();
  auto owner = std::make_unique<Owner>();
  std::unordered_map<uint64_t, Entry> entries;
  for (uint64_t id = 1; id <= 4096; ++id) {
    const auto inserted = entries.emplace(id, Entry{id, 7});
    owner->retirements.Track(queue, inserted.first->second);
  }
  owner.reset();
  uint32_t releases = 0;
  queue->DrainCompleted(6, Submission, [&](const Entry&) { ++releases; });
  REQUIRE(releases == 0);
  queue->DrainCompleted(7, Submission, [&](const Entry& entry) {
    const auto cached = entries.find(entry.id);
    REQUIRE(cached != entries.end());
    REQUIRE(&cached->second == &entry);
    entries.erase(cached);
    ++releases;
  });
  CHECK(releases == 4096);
  CHECK(entries.empty());
}

TEST_CASE("persistent cache teardown discards pending and late owner notifications",
          "[gta4-native][owner-retirement]") {
  auto old_queue = std::make_shared<Queue>();
  auto owner = std::make_unique<Owner>();
  auto expired_owner = std::make_unique<Owner>();
  auto old_entry = std::make_unique<Entry>(Entry{1, 20});
  auto pending_entry = std::make_unique<Entry>(Entry{2, 30});
  owner->retirements.Track(old_queue, *old_entry);
  expired_owner->retirements.Track(old_queue, *pending_entry);
  expired_owner.reset();
  old_queue->DrainCompleted(0, Submission, [](const Entry&) { FAIL("GPU use is pending"); });
  old_queue->Close();
  old_entry.reset();
  pending_entry.reset();

  SECTION("owner dies after closed queue's map has been erased") {
    owner.reset();
    old_queue->DrainCompleted(100, [](const Entry&) {
      FAIL("closed queue dereferenced an erased entry");
      return uint64_t{0};
    }, [](const Entry&) { FAIL("closed queue released an erased entry"); });
  }
  SECTION("surviving owner registers new entries after cache recreation") {
    auto new_queue = std::make_shared<Queue>();
    Entry replacement{1, 4};
    owner->retirements.Track(new_queue, replacement);
    old_queue.reset();
    owner.reset();
    uint32_t releases = 0;
    new_queue->DrainCompleted(4, Submission, [&](const Entry& entry) {
      CHECK(&entry == &replacement);
      ++releases;
    });
    CHECK(releases == 1);
  }
  SECTION("owner outlives destruction of the queue itself") {
    old_queue.reset();
    owner.reset();
    SUCCEED("late owner destruction does not require a live renderer");
  }
}

TEST_CASE("persistent owner publication can race consumer drain and closure",
          "[gta4-native][owner-retirement]") {
  auto queue = std::make_shared<Queue>();
  std::vector<std::unique_ptr<Owner>> owners;
  std::vector<std::unique_ptr<Entry>> entries;
  for (uint64_t id = 1; id <= 4096; ++id) {
    entries.push_back(std::make_unique<Entry>(Entry{id, 1}));
    auto owner = std::make_unique<Owner>();
    owner->retirements.Track(queue, *entries.back());
    owners.push_back(std::move(owner));
  }
  std::atomic<bool> begin{false};
  std::atomic<bool> done{false};
  std::thread producer([&] {
    while (!begin.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    for (auto& owner : owners) {
      owner.reset();
    }
    done.store(true, std::memory_order_release);
  });
  begin.store(true, std::memory_order_release);
  std::unordered_set<uint64_t> released;
  auto release = [&](const Entry& entry) { REQUIRE(released.insert(entry.id).second); };
  SECTION("all published entries are consumed exactly once") {
    while (!done.load(std::memory_order_acquire)) {
      queue->DrainCompleted(1, Submission, release);
      std::this_thread::yield();
    }
    producer.join();
    queue->DrainCompleted(1, Submission, release);
    CHECK(released.size() == entries.size());
  }
  SECTION("closure synchronizes with publication before backing storage is erased") {
    queue->DrainCompleted(1, Submission, release);
    queue->Close();
    entries.clear();
    producer.join();
    queue->DrainCompleted(100, [](const Entry&) {
      FAIL("late publication dereferenced erased backing storage");
      return uint64_t{0};
    }, [](const Entry&) { FAIL("late publication reached the release callback"); });
    SUCCEED("publication after closure only disposes pointer batches");
  }
}

TEST_CASE("retirement queue can be destroyed on a producer after consumer teardown",
          "[gta4-native][owner-retirement]") {
  auto queue = std::make_shared<Queue>();
  std::weak_ptr<Queue> weak_queue = queue;
  Entry entry{1, 1};
  auto owner = std::make_unique<Owner>();
  owner->retirements.Track(queue, entry);
  std::atomic<bool> released_consumer{false};
  std::thread producer([retained_queue = queue, owner = std::move(owner), &released_consumer]() mutable {
    while (!released_consumer.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    owner.reset();
    retained_queue.reset();
  });
  queue->Close();
  queue.reset();
  released_consumer.store(true, std::memory_order_release);
  producer.join();
  CHECK(weak_queue.expired());
}
