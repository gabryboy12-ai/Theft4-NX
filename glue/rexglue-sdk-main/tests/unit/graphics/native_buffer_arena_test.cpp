#include <catch2/catch_test_macros.hpp>

#include "graphics/gta4_native/native_buffer_arena.h"

namespace gta4 = rex::graphics::gta4_native;

namespace {

constexpr uint64_t kBlockCapacity = 16'777'216;
constexpr uint64_t kAlignment = 16;

gta4::NativeBufferArena MakeArena() {
  return gta4::NativeBufferArena({kBlockCapacity, kAlignment});
}

}  // namespace

TEST_CASE("native buffer arena validates its block contract", "[gta4-native][buffer-arena]") {
  CHECK_FALSE(gta4::NativeBufferArena({0, kAlignment}).valid());
  CHECK_FALSE(gta4::NativeBufferArena({kBlockCapacity, 0}).valid());
  CHECK_FALSE(gta4::NativeBufferArena({kBlockCapacity, 3}).valid());
  CHECK_FALSE(gta4::NativeBufferArena({kBlockCapacity - 1, kAlignment}).valid());
  CHECK(MakeArena().valid());
}

TEST_CASE("native buffer arena reuses committed blocks with aligned best fit",
          "[gta4-native][buffer-arena]") {
  auto arena = MakeArena();
  const auto first = arena.Reserve(1);
  REQUIRE(first);
  CHECK(first.allocation.new_block);
  CHECK(first.allocation.offset == 0);
  CHECK(first.allocation.reserved_size == 16);
  REQUIRE(arena.Commit(first.allocation.id) == gta4::NativeBufferArenaStatus::kSuccess);

  const auto second = arena.Reserve(17);
  REQUIRE(second);
  CHECK_FALSE(second.allocation.new_block);
  CHECK(second.allocation.block_id == first.allocation.block_id);
  CHECK(second.allocation.offset == 16);
  CHECK(second.allocation.reserved_size == 32);
  REQUIRE(arena.Commit(second.allocation.id) == gta4::NativeBufferArenaStatus::kSuccess);

  const auto snapshot = arena.Snapshot();
  CHECK(snapshot.block_count == 1);
  CHECK(snapshot.live_allocation_count == 2);
  CHECK(snapshot.allocation_hits == 1);
  CHECK(snapshot.allocation_misses == 1);
}

TEST_CASE("native buffer arena cancellation rolls a reservation back exactly",
          "[gta4-native][buffer-arena]") {
  auto arena = MakeArena();
  const auto first = arena.Reserve(64);
  REQUIRE(first);
  REQUIRE(arena.Commit(first.allocation.id) == gta4::NativeBufferArenaStatus::kSuccess);
  const auto pending = arena.Reserve(32);
  REQUIRE(pending);
  REQUIRE(arena.Cancel(pending.allocation.id));

  const auto replacement = arena.Reserve(32);
  REQUIRE(replacement);
  CHECK(replacement.allocation.block_id == pending.allocation.block_id);
  CHECK(replacement.allocation.offset == pending.allocation.offset);
}

TEST_CASE("native buffer arena cancellation retires a new backing transaction",
          "[gta4-native][buffer-arena]") {
  auto arena = MakeArena();
  const auto pending = arena.Reserve(32);
  REQUIRE(pending);
  REQUIRE(pending.allocation.new_block);
  const auto cancellation = arena.Cancel(pending.allocation.id);
  REQUIRE(cancellation);
  CHECK(cancellation.destroy_block);
  CHECK(arena.Snapshot().block_count == 0);
}

TEST_CASE("native buffer arena coalesces adjacent released slices",
          "[gta4-native][buffer-arena]") {
  auto arena = MakeArena();
  const auto first = arena.Reserve(16);
  REQUIRE(first);
  REQUIRE(arena.Commit(first.allocation.id) == gta4::NativeBufferArenaStatus::kSuccess);
  const auto second = arena.Reserve(16);
  REQUIRE(second);
  REQUIRE(arena.Commit(second.allocation.id) == gta4::NativeBufferArenaStatus::kSuccess);
  const auto third = arena.Reserve(16);
  REQUIRE(third);
  REQUIRE(arena.Commit(third.allocation.id) == gta4::NativeBufferArenaStatus::kSuccess);

  REQUIRE(arena.Release(second.allocation.id));
  REQUIRE(arena.Release(first.allocation.id));
  REQUIRE(arena.Release(third.allocation.id));
  const auto full = arena.Reserve(kBlockCapacity);
  REQUIRE(full);
  CHECK_FALSE(full.allocation.new_block);
  CHECK(full.allocation.block_id == first.allocation.block_id);
  CHECK(full.allocation.offset == 0);
}

TEST_CASE("native buffer arena gives oversized payloads dedicated aligned blocks",
          "[gta4-native][buffer-arena]") {
  auto arena = MakeArena();
  const auto allocation = arena.Reserve(kBlockCapacity + 1);
  REQUIRE(allocation);
  CHECK(allocation.allocation.new_block);
  CHECK(allocation.allocation.dedicated);
  CHECK(allocation.allocation.reserved_size == 16'777'232);
  CHECK(allocation.allocation.block_capacity == 16'777'232);
  REQUIRE(arena.Commit(allocation.allocation.id) == gta4::NativeBufferArenaStatus::kSuccess);
  const auto release = arena.Release(allocation.allocation.id);
  REQUIRE(release);
  CHECK(release.destroy_block);
  CHECK(arena.Snapshot().block_count == 0);
}

TEST_CASE("native buffer arena rejects invalid allocation state transitions",
          "[gta4-native][buffer-arena]") {
  auto arena = MakeArena();
  CHECK(arena.Reserve(0).status == gta4::NativeBufferArenaStatus::kInvalidSize);
  const auto allocation = arena.Reserve(16);
  REQUIRE(allocation);
  CHECK(arena.Release(allocation.allocation.id).status ==
        gta4::NativeBufferArenaStatus::kAllocationNotLive);
  REQUIRE(arena.Commit(allocation.allocation.id) == gta4::NativeBufferArenaStatus::kSuccess);
  CHECK(arena.Commit(allocation.allocation.id) ==
        gta4::NativeBufferArenaStatus::kAllocationNotPending);
  CHECK(arena.Cancel(allocation.allocation.id).status ==
        gta4::NativeBufferArenaStatus::kAllocationNotPending);
  REQUIRE(arena.Release(allocation.allocation.id));
  CHECK(arena.Release(allocation.allocation.id).status ==
        gta4::NativeBufferArenaStatus::kInvalidAllocation);
}

TEST_CASE("native buffer arena trims only entirely free blocks after reuse grace",
          "[gta4-native][buffer-arena]") {
  auto arena = MakeArena();
  const auto allocation = arena.Reserve(kBlockCapacity);
  REQUIRE(allocation);
  CHECK(arena.TrimFreeBlocks(0, 10, 120).empty());
  REQUIRE(arena.Commit(allocation.allocation.id) == gta4::NativeBufferArenaStatus::kSuccess);
  CHECK(arena.TrimFreeBlocks(0, 10, 120).empty());
  REQUIRE(arena.Release(allocation.allocation.id));
  CHECK(arena.TrimFreeBlocks(0, 10, 120).empty());
  CHECK(arena.TrimFreeBlocks(0, 129, 120).empty());
  const auto reused = arena.Reserve(kBlockCapacity);
  REQUIRE(reused);
  CHECK_FALSE(reused.allocation.new_block);
  CHECK(reused.allocation.block_id == allocation.allocation.block_id);
  REQUIRE(arena.Commit(reused.allocation.id) == gta4::NativeBufferArenaStatus::kSuccess);
  REQUIRE(arena.Release(reused.allocation.id));
  CHECK(arena.TrimFreeBlocks(0, 130, 120).empty());
  CHECK(arena.TrimFreeBlocks(kBlockCapacity, 1000, 120).empty());
  const auto trimmed = arena.TrimFreeBlocks(0, 1000, 120);
  REQUIRE(trimmed.size() == 1);
  CHECK(trimmed.front() == allocation.allocation.block_id);
  CHECK(arena.Snapshot().resident_capacity == 0);
}
