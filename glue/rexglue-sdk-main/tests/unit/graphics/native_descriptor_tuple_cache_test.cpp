#include <catch2/catch_test_macros.hpp>

#include <random>

#include "graphics/gta4_native/native_descriptor_tuple_cache.h"

namespace {
struct Key {
  uint64_t view = 0;
  uint64_t lifetime = 0;
  uint64_t layout_epoch = 0;
  bool operator==(const Key&) const = default;
};
struct Hash {
  size_t operator()(const Key& key) const { return std::hash<uint64_t>{}(key.lifetime); }
};
using Cache = rex::graphics::gta4_native::NativeDescriptorTupleCache<Key, uint64_t, Hash>;

struct ImageKey {
  uint64_t identity = 0;
  std::array<uint64_t, 4> lifetimes{};
  bool operator==(const ImageKey&) const = default;
};
struct ImageHash {
  size_t operator()(const ImageKey& key) const { return std::hash<uint64_t>{}(key.identity); }
};
struct ImageDependencies {
  inline static uint32_t visits = 0;
  const auto& operator()(const ImageKey& key) const {
    ++visits;
    return key.lifetimes;
  }
};
using ImageCache = rex::graphics::gta4_native::NativeDescriptorTupleCache<
    ImageKey, uint64_t, ImageHash, ImageDependencies>;
}

TEST_CASE("descriptor retirement preserves unrelated tuples and waits for slot completion",
          "[gta4-native][descriptor-cache]") {
  Cache cache(2);
  REQUIRE(cache.BeginAfterCommandPoolReset(0));
  const Key old{100, 1, 1};
  const Key other{200, 2, 1};
  REQUIRE(cache.Insert(old, 10));
  REQUIRE(cache.Insert(other, 20));
  cache.MarkSubmitted(8);
  cache.Invalidate([](const Key& key) { return key.lifetime == 1; });
  CHECK_FALSE(cache.Find(old));
  REQUIRE(cache.Find(other));
  CHECK(*cache.Find(other) == 20);
  CHECK(cache.retired_count() == 1);
  CHECK_FALSE(cache.TakeReusable());
  CHECK_FALSE(cache.BeginAfterCommandPoolReset(7));
  REQUIRE(cache.BeginAfterCommandPoolReset(8));
  const auto recycled = cache.TakeReusable();
  REQUIRE(recycled);
  CHECK(*recycled == 10);
  const Key reused_view{100, 3, 1};
  REQUIRE(cache.Insert(reused_view, *recycled));
  CHECK_FALSE(cache.Find(old));
  CHECK_FALSE(cache.Find({100, 3, 2}));
  CHECK(cache.Find(reused_view));
}

TEST_CASE("descriptor cache has bounded bundles under repeated retirement and excess working sets",
          "[gta4-native][descriptor-cache]") {
  Cache cache(2);
  uint64_t allocated = 0;
  for (uint64_t generation = 1; generation < 500; ++generation) {
    REQUIRE(cache.BeginAfterCommandPoolReset(generation));
    const std::unordered_set<Key, Hash> required{{100, generation, 1}, {200, generation, 1},
                                                  {300, generation, 1}};
    cache.Prepare(required);
    uint32_t transient = 0;
    for (const auto& key : required) {
      if (cache.Find(key)) {
        continue;
      }
      if (cache.size() == cache.capacity()) {
        ++transient;
        continue;
      }
      auto bundle = cache.TakeReusable();
      if (!bundle) {
        bundle = ++allocated;
      }
      REQUIRE(cache.Insert(key, *bundle));
    }
    CHECK(transient == 1);
    CHECK(allocated == 2);
    cache.MarkSubmitted(generation);
    cache.Invalidate([](const Key&) { return true; });
    CHECK(cache.size() == 0);
    CHECK(cache.retired_count() == 2);
  }
}

TEST_CASE("descriptor reverse invalidation preserves GPU lifetime and removes cross references",
          "[gta4-native][descriptor-cache]") {
  ImageCache cache(3);
  REQUIRE(cache.BeginAfterCommandPoolReset(0));
  const ImageKey shared{1, {10, 10, 20, 0}};
  const ImageKey second{2, {20, 30, 0, 0}};
  const ImageKey unrelated{3, {40, 0, 0, 0}};
  REQUIRE(cache.Insert(shared, 100));
  REQUIRE(cache.Insert(second, 200));
  REQUIRE(cache.Insert(unrelated, 300));
  cache.MarkSubmitted(8);
  ImageDependencies::visits = 0;
  cache.InvalidateDependency(0);
  cache.InvalidateDependency(99);
  CHECK(ImageDependencies::visits == 0);
  CHECK(cache.size() == 3);
  cache.InvalidateDependency(10);
  CHECK_FALSE(cache.Find(shared));
  CHECK(cache.Find(second));
  CHECK(cache.Find(unrelated));
  CHECK(cache.retired_count() == 1);
  CHECK(ImageDependencies::visits == 1);
  CHECK_FALSE(cache.TakeReusable());
  CHECK_FALSE(cache.BeginAfterCommandPoolReset(7));
  cache.InvalidateDependency(20);
  cache.InvalidateDependency(30);
  CHECK_FALSE(cache.Find(second));
  CHECK(cache.Find(unrelated));
  CHECK(cache.retired_count() == 2);
  CHECK(ImageDependencies::visits == 2);
  REQUIRE(cache.BeginAfterCommandPoolReset(8));
  CHECK(cache.reusable_count() == 2);
  CHECK(cache.retired_count() == 0);
}

TEST_CASE("descriptor reverse index follows pressure eviction and predicate retirement",
          "[gta4-native][descriptor-cache]") {
  ImageCache cache(1);
  REQUIRE(cache.BeginAfterCommandPoolReset(0));
  const ImageKey old{1, {10, 20, 0, 0}};
  const ImageKey replacement{2, {30, 0, 0, 0}};
  REQUIRE(cache.Insert(old, 100));
  cache.Prepare({replacement});
  REQUIRE(cache.TakeReusable() == 100);
  REQUIRE(cache.Insert(replacement, 100));
  cache.InvalidateDependency(10);
  cache.InvalidateDependency(20);
  REQUIRE(cache.Find(replacement));
  CHECK(cache.retired_count() == 0);
  cache.Invalidate([](const ImageKey&) { return true; });
  cache.InvalidateDependency(30);
  CHECK(cache.retired_count() == 1);
  CHECK(cache.size() == 0);
  REQUIRE(cache.BeginAfterCommandPoolReset(0));
  REQUIRE(cache.TakeReusable() == 100);
  REQUIRE(cache.Insert(old, 100));
  cache.InvalidateDependency(20);
  CHECK(cache.retired_count() == 1);
  CHECK(cache.size() == 0);
}

TEST_CASE("descriptor reverse index survives map rehash and cache moves",
          "[gta4-native][descriptor-cache]") {
  ImageCache cache(256);
  REQUIRE(cache.BeginAfterCommandPoolReset(0));
  for (uint64_t identity = 1; identity <= 256; ++identity) {
    REQUIRE(cache.Insert({identity, {identity, 0, 0, 0}}, identity));
  }
  ImageCache moved(std::move(cache));
  ImageCache assigned(1);
  REQUIRE(assigned.BeginAfterCommandPoolReset(0));
  REQUIRE(assigned.Insert({999, {999, 0, 0, 0}}, 999));
  assigned = std::move(moved);
  assigned.InvalidateDependency(999);
  for (uint64_t identity = 1; identity <= 256; ++identity) {
    assigned.InvalidateDependency(identity);
    CHECK_FALSE(assigned.Find({identity, {identity, 0, 0, 0}}));
  }
  CHECK(assigned.size() == 0);
  CHECK(assigned.retired_count() == 256);
}

TEST_CASE("descriptor reverse index agrees with full scans during streaming churn",
          "[gta4-native][descriptor-cache]") {
  ImageCache indexed(64);
  using ScannedCache = rex::graphics::gta4_native::NativeDescriptorTupleCache<
      ImageKey, uint64_t, ImageHash>;
  ScannedCache scanned(64);
  std::mt19937 random(72631);
  std::uniform_int_distribution<uint64_t> lifetime(0, 32);
  std::uniform_int_distribution<size_t> key_index(0, 127);
  std::array<ImageKey, 128> keys{};
  uint64_t identity = 0;
  for (ImageKey& key : keys) {
    key.identity = ++identity;
    for (auto& dependency : key.lifetimes) {
      dependency = lifetime(random);
    }
  }
  uint64_t completed = 0;
  for (uint64_t submission = 1; submission <= 100; ++submission) {
    REQUIRE(indexed.BeginAfterCommandPoolReset(completed));
    REQUIRE(scanned.BeginAfterCommandPoolReset(completed));
    std::unordered_set<ImageKey, ImageHash> required;
    for (size_t pick = 0; pick < 64; ++pick) {
      required.insert(keys[key_index(random)]);
    }
    indexed.Prepare(required);
    scanned.Prepare(required);
    for (const ImageKey& key : required) {
      if (!scanned.Find(key)) {
        CHECK(bool(indexed.TakeReusable()) == bool(scanned.TakeReusable()));
        CHECK(indexed.Insert(key, key.identity) == scanned.Insert(key, key.identity));
      }
    }
    indexed.MarkSubmitted(submission);
    scanned.MarkSubmitted(submission);
    CHECK_FALSE(indexed.BeginAfterCommandPoolReset(completed));
    CHECK_FALSE(scanned.BeginAfterCommandPoolReset(completed));
    for (uint32_t release = 0; release < 8; ++release) {
      const uint64_t retired = lifetime(random);
      indexed.InvalidateDependency(retired);
      scanned.Invalidate([retired](const ImageKey& key) {
        return retired && std::find(key.lifetimes.begin(), key.lifetimes.end(), retired) !=
                              key.lifetimes.end();
      });
    }
    CHECK(indexed.size() == scanned.size());
    CHECK(indexed.retired_count() == scanned.retired_count());
    CHECK(indexed.reusable_count() == scanned.reusable_count());
    for (const ImageKey& key : keys) {
      CHECK(bool(indexed.Find(key)) == bool(scanned.Find(key)));
    }
    completed = submission;
  }
}
