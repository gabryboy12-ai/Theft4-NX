#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "../../../gta4-recomp/src/gta4_multiplayer_64_policy.h"
#include "../../../gta4-recomp/src/gta4_player_info_alias_gate.h"

namespace mp64 = gta4::multiplayer64;

TEST_CASE("GTA IV extended CmdJoin uses the retail executable vtable",
          "[gta4][multiplayer][64-player][session]") {
  // Generated sub_829F8710 stores this address after constructing CmdJoin.
  // The preceding word is the RTTI Complete Object Locator pointer, not a
  // callable vtable entry.
  CHECK(mp64::kJoinCommandVtableAddress == 0x820A869C);
}

TEST_CASE("GTA IV player-info projections admit cross-thread callback participants",
          "[gta4][multiplayer][64-player][alias-gate]") {
  struct TestProjection {
    uint32_t actual_player_id = 0;
    uint32_t guest_player_info = 0;
  };

  mp64::JoinableProjectionGate<TestProjection> gate;
  std::atomic<uint32_t> participant_reads = 0;
  std::atomic<uint32_t> participant_starts = 0;
  std::promise<void> participant_joined;
  std::promise<void> release_participant;
  auto release_future = release_participant.get_future();

  auto owner = gate.EnterProjection(
      [] { return uint32_t{mp64::kLegacyPeerCapacity}; },
      [](uint32_t player_id) { return player_id >= mp64::kLegacyPeerCapacity; },
      [](uint32_t player_id) { return TestProjection{player_id, 0xA1000000}; });
  REQUIRE(owner.is_owner());
  REQUIRE(owner.session() != nullptr);
  CHECK(owner.session()->actual_player_id == mp64::kLegacyPeerCapacity);

  std::thread participant([&] {
    auto joined = gate.EnterProjection(
        [&] {
          participant_reads.fetch_add(1, std::memory_order_relaxed);
          return uint32_t{0};
        },
        [](uint32_t player_id) { return player_id >= mp64::kLegacyPeerCapacity; },
        [&](uint32_t player_id) {
          participant_starts.fetch_add(1, std::memory_order_relaxed);
          return TestProjection{player_id, 0};
        });
    CHECK(joined.kind() == decltype(gate)::AdmissionKind::kProjectionParticipant);
    REQUIRE(joined.session() != nullptr);
    CHECK(joined.session()->actual_player_id == mp64::kLegacyPeerCapacity);
    CHECK(joined.session()->guest_player_info == 0xA1000000);
    participant_joined.set_value();
    release_future.wait();
  });

  participant_joined.get_future().wait();
  CHECK(participant_reads.load(std::memory_order_relaxed) == 0);
  CHECK(participant_starts.load(std::memory_order_relaxed) == 0);

  std::atomic<bool> restored = false;
  std::promise<void> completion_started;
  std::promise<void> restoration_finished;
  auto restoration_future = restoration_finished.get_future();
  std::thread completer(
      [owner = std::move(owner), &completion_started, &restoration_finished, &restored]() mutable {
        owner.Complete([&] { completion_started.set_value(); },
                       [&](const TestProjection& projection) {
                         restored.store(projection.actual_player_id == mp64::kLegacyPeerCapacity,
                                        std::memory_order_relaxed);
                       });
        restoration_finished.set_value();
      });

  completion_started.get_future().wait();
  CHECK(restoration_future.wait_for(std::chrono::seconds::zero()) == std::future_status::timeout);
  release_participant.set_value();
  participant.join();
  restoration_future.wait();
  completer.join();
  CHECK(restored.load(std::memory_order_relaxed));
}

TEST_CASE("GTA IV low-primary operations allow worker lifecycle mutation",
          "[gta4][multiplayer][64-player][alias-gate]") {
  struct TestProjection {
    uint32_t actual_player_id = 0;
  };

  mp64::JoinableProjectionGate<TestProjection> gate;
  auto low_operation = gate.EnterProjection(
      [] { return uint32_t{0}; },
      [](uint32_t player_id) { return player_id >= mp64::kLegacyPeerCapacity; },
      [](uint32_t player_id) { return TestProjection{player_id}; });
  REQUIRE(low_operation.kind() == decltype(gate)::AdmissionKind::kOrdinaryOperation);

  std::promise<void> lifecycle_finished;
  auto lifecycle_future = lifecycle_finished.get_future();
  std::thread worker([&] {
    auto mutation = gate.EnterOpaque();
    lifecycle_finished.set_value();
  });

  REQUIRE(lifecycle_future.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  worker.join();
}

TEST_CASE("GTA IV joined projection workers serialize lifecycle mutation",
          "[gta4][multiplayer][64-player][alias-gate]") {
  struct TestProjection {
    uint32_t actual_player_id = 0;
  };
  mp64::JoinableProjectionGate<TestProjection> gate;
  auto owner = gate.EnterProjection(
      [] { return uint32_t{mp64::kLegacyPeerCapacity}; },
      [](uint32_t player_id) { return player_id >= mp64::kLegacyPeerCapacity; },
      [](uint32_t player_id) { return TestProjection{player_id}; });

  std::promise<void> mutation_finished;
  auto mutation_future = mutation_finished.get_future();
  std::thread worker([&] {
    auto participant = gate.EnterRead();
    auto mutation = gate.EnterOpaque();
    auto nested_mutation = gate.EnterOpaque();
    mutation_finished.set_value();
  });
  REQUIRE(mutation_future.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  worker.join();
  owner.Complete([](const TestProjection&) {});
}

TEST_CASE("GTA IV pending projections allow nested reads from their blocking operation",
          "[gta4][multiplayer][64-player][alias-gate]") {
  struct TestProjection {
    uint32_t actual_player_id = 0;
  };
  mp64::JoinableProjectionGate<TestProjection> gate;
  std::promise<void> operation_entered;
  std::promise<void> begin_nested_read;
  std::promise<void> nested_read_finished;
  std::promise<void> release_operation;
  auto begin_nested_future = begin_nested_read.get_future();
  auto release_operation_future = release_operation.get_future();
  auto nested_read_future = nested_read_finished.get_future();
  std::thread operation([&] {
    auto admission = gate.EnterProjection(
        [] { return uint32_t{0}; },
        [](uint32_t player_id) { return player_id >= mp64::kLegacyPeerCapacity; },
        [](uint32_t player_id) { return TestProjection{player_id}; });
    operation_entered.set_value();
    begin_nested_future.wait();
    auto nested = gate.EnterRead();
    nested_read_finished.set_value();
    release_operation_future.wait();
  });
  operation_entered.get_future().wait();

  std::promise<void> high_projection_classified;
  std::thread projector([&] {
    bool classification_reported = false;
    auto owner = gate.EnterProjection([] { return uint32_t{mp64::kLegacyPeerCapacity}; },
                                      [&](uint32_t player_id) {
                                        if (!classification_reported) {
                                          classification_reported = true;
                                          high_projection_classified.set_value();
                                        }
                                        return player_id >= mp64::kLegacyPeerCapacity;
                                      },
                                      [](uint32_t player_id) { return TestProjection{player_id}; });
    owner.Complete([](const TestProjection&) {});
  });
  high_projection_classified.get_future().wait();
  begin_nested_read.set_value();
  REQUIRE(nested_read_future.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  release_operation.set_value();
  operation.join();
  projector.join();
}

TEST_CASE("GTA IV projection restoration remains usable after an exception",
          "[gta4][multiplayer][64-player][alias-gate]") {
  struct TestProjection {
    uint32_t actual_player_id = 0;
  };
  mp64::JoinableProjectionGate<TestProjection> gate;
  auto owner = gate.EnterProjection(
      [] { return uint32_t{mp64::kLegacyPeerCapacity}; },
      [](uint32_t player_id) { return player_id >= mp64::kLegacyPeerCapacity; },
      [](uint32_t player_id) { return TestProjection{player_id}; });
  CHECK_THROWS_AS(
      owner.Complete([](const TestProjection&) { throw std::runtime_error("restore failure"); }),
      std::runtime_error);
  auto reader = gate.EnterRead();
  CHECK(reader.kind() == decltype(gate)::AdmissionKind::kOrdinaryReader);
}

TEST_CASE("GTA IV player-info readers can enter nested lifecycle mutations",
          "[gta4][multiplayer][64-player][alias-gate]") {
  struct TestProjection {
    uint32_t actual_player_id = 0;
  };

  using Gate = mp64::JoinableProjectionGate<TestProjection>;
  Gate gate;
  std::optional<Gate::ReadAdmission> blocking_reader;
  blocking_reader.emplace(gate.EnterRead());

  std::promise<void> reader_entered;
  std::promise<void> upgrade_attempted;
  std::promise<void> opaque_entered;
  auto opaque_future = opaque_entered.get_future();
  std::thread upgrader([&] {
    auto reader = gate.EnterRead();
    reader_entered.set_value();
    upgrade_attempted.set_value();
    auto opaque = gate.EnterOpaque();
    opaque_entered.set_value();
  });

  reader_entered.get_future().wait();
  upgrade_attempted.get_future().wait();
  CHECK(opaque_future.wait_for(std::chrono::seconds::zero()) == std::future_status::timeout);
  blocking_reader.reset();
  opaque_future.wait();
  upgrader.join();
}

TEST_CASE("GTA IV multiplayer peer IDs are classified before guest indexing",
          "[gta4][multiplayer][64-player]") {
  CHECK(mp64::ClassifyPeerId(0) == mp64::PeerIdClass::kLegacy);
  CHECK(mp64::ClassifyPeerId(15) == mp64::PeerIdClass::kLegacy);
  CHECK(mp64::ClassifyPeerId(16) == mp64::PeerIdClass::kExtended);
  CHECK(mp64::ClassifyPeerId(63) == mp64::PeerIdClass::kExtended);
  CHECK(mp64::ClassifyPeerId(64) == mp64::PeerIdClass::kInvalid);
  CHECK(mp64::ClassifyPeerId(mp64::kInvalidPeerId) == mp64::PeerIdClass::kInvalid);

  REQUIRE(mp64::LegacyPeerPointerOffset(0).has_value());
  REQUIRE(mp64::LegacyPeerPointerOffset(15).has_value());
  CHECK(*mp64::LegacyPeerPointerOffset(0) == mp64::kLegacyPeerPointerTableOffset);
  CHECK(*mp64::LegacyPeerPointerOffset(15) < mp64::kPeerManagerCapacityFieldOffset);
  CHECK_FALSE(mp64::LegacyPeerPointerOffset(16).has_value());
  CHECK_FALSE(mp64::LegacyPeerPointerOffset(63).has_value());
  CHECK_FALSE(mp64::LegacyPeerPointerOffset(mp64::kInvalidPeerId).has_value());

  REQUIRE(mp64::LegacyPlayerInfoPointerAddress(15).has_value());
  REQUIRE(mp64::LegacyPlayerInfoGenerationAddress(15).has_value());
  CHECK(*mp64::LegacyPlayerInfoPointerAddress(15) == 0x82C01CAC);
  CHECK(*mp64::LegacyPlayerInfoGenerationAddress(15) == 0x82C01C6C);
  CHECK_FALSE(mp64::LegacyPlayerInfoPointerAddress(16).has_value());
  CHECK_FALSE(mp64::LegacyPlayerInfoGenerationAddress(16).has_value());
  CHECK_FALSE(mp64::LegacyPlayerInfoPointerAddress(mp64::kInvalidPeerId).has_value());
}

TEST_CASE("GTA IV multiplayer guest address helpers reject overflow",
          "[gta4][multiplayer][64-player]") {
  CHECK(mp64::CheckedGuestAddress(0x82000000, 0).has_value());
  CHECK(mp64::CheckedGuestArrayAddress(0x82000000, 63, mp64::kGuestPointerSize).has_value());
  CHECK_FALSE(mp64::CheckedGuestAddress(std::numeric_limits<uint32_t>::max(), 1).has_value());
  CHECK_FALSE(mp64::CheckedGuestArrayAddress(std::numeric_limits<uint32_t>::max(), 1,
                                             mp64::kGuestPointerSize)
                  .has_value());
}

TEST_CASE("GTA IV multiplayer pointer address helpers reject null objects",
          "[gta4][multiplayer][64-player]") {
  CHECK_FALSE(mp64::CheckedGuestPointerAddress(0, 0).has_value());
  CHECK_FALSE(
      mp64::CheckedGuestPointerAddress(0, mp64::kProximityWeightPeerPointerOffset).has_value());
  CHECK(mp64::CheckedGuestPointerAddress(0x82000000,
                                         mp64::kProximityWeightPeerPointerOffset)
            .has_value());
  CHECK_FALSE(mp64::CheckedGuestPointerAddress(std::numeric_limits<uint32_t>::max(), 1)
                  .has_value());
}

TEST_CASE("GTA IV high-player transition seams match the derived retail layout",
          "[gta4][multiplayer][64-player]") {
  CHECK(mp64::kTransitionFallbackStateAddress == 0x82FB0000);
  CHECK(mp64::kTransitionNestedStateOffset == 0x220);
  CHECK(mp64::kTransitionNestedStateBias == 0x60);
  CHECK(mp64::kTransitionByteFlagsOffset == 0x5B);
  CHECK(mp64::kTransitionByteClearMask == 0x7F);
  CHECK(mp64::kTransitionPlayerFlagsOffset == 0x234);
  CHECK(mp64::kTransitionPlayerForceFlag == 0x00800000);
  CHECK(mp64::kTransitionPlayerActiveMask == 0x60000000);
  CHECK(mp64::kTransitionResetAccessorReturnAddress == 0x822D7730);
  CHECK(mp64::kTransitionForceAccessorReturnAddress == 0x822D7C70);
}

TEST_CASE("GTA IV multiplayer slot policy distinguishes widened requests",
          "[gta4][multiplayer][64-player]") {
  CHECK(mp64::ClassifySlotRequest(16, 16).path == mp64::CapacityPath::kLegacy);
  CHECK(mp64::ClassifySlotRequest(32, 0).path == mp64::CapacityPath::kLegacy);
  CHECK(mp64::ClassifySlotRequest(32, 1).path == mp64::CapacityPath::kExtended);
  CHECK(mp64::ClassifySlotRequest(48, 16).path == mp64::CapacityPath::kExtended);
  CHECK(mp64::ClassifySlotRequest(64, 0).path == mp64::CapacityPath::kExtended);
  CHECK(mp64::ClassifySlotRequest(64, 1).path == mp64::CapacityPath::kInvalid);
  CHECK(mp64::ClassifySlotRequest(std::numeric_limits<uint32_t>::max(), 1).path ==
        mp64::CapacityPath::kInvalid);

  const auto public_free_roam = mp64::ExpandFreeRoamSlotRequest(mp64::kFreeRoamGameMode, 16, 0);
  CHECK(public_free_roam.expanded);
  CHECK(public_free_roam.public_slots == 64);
  CHECK(public_free_roam.private_slots == 0);
  const auto private_free_roam = mp64::ExpandFreeRoamSlotRequest(mp64::kFreeRoamGameMode, 12, 4);
  CHECK(private_free_roam.expanded);
  CHECK(private_free_roam.public_slots == 60);
  CHECK(private_free_roam.private_slots == 4);
  const auto competitive = mp64::ExpandFreeRoamSlotRequest(5, 16, 0);
  CHECK_FALSE(competitive.expanded);
  CHECK(competitive.public_slots == 16);

  CHECK(mp64::ClassifyParticipantCount(32).path == mp64::CapacityPath::kLegacy);
  CHECK(mp64::ClassifyParticipantCount(33).path == mp64::CapacityPath::kExtended);
  CHECK(mp64::ClassifyParticipantCount(64).path == mp64::CapacityPath::kExtended);
  CHECK(mp64::ClassifyParticipantCount(65).path == mp64::CapacityPath::kInvalid);

  const mp64::LegacySlotProxy legacy_proxy = mp64::MakeLegacySlotProxy(16, 16);
  REQUIRE(legacy_proxy.valid);
  CHECK(legacy_proxy.public_slots == 16);
  CHECK(legacy_proxy.private_slots == 16);
  const mp64::LegacySlotProxy public_proxy = mp64::MakeLegacySlotProxy(64, 0);
  REQUIRE(public_proxy.valid);
  CHECK(public_proxy.public_slots == 32);
  CHECK(public_proxy.private_slots == 0);
  const mp64::LegacySlotProxy mixed_proxy = mp64::MakeLegacySlotProxy(20, 44);
  REQUIRE(mixed_proxy.valid);
  CHECK(mixed_proxy.public_slots == 20);
  CHECK(mixed_proxy.private_slots == 12);
  const mp64::LegacySlotProxy private_proxy = mp64::MakeLegacySlotProxy(0, 64);
  REQUIRE(private_proxy.valid);
  CHECK(private_proxy.public_slots == 0);
  CHECK(private_proxy.private_slots == 32);
  CHECK_FALSE(mp64::MakeLegacySlotProxy(65, 0).valid);
  CHECK(mp64::LowBits32(0) == 0);
  CHECK(mp64::LowBits32(1) == 1);
  CHECK(mp64::LowBits32(16) == 0xFFFF);
  CHECK(mp64::LowBits32(32) == std::numeric_limits<uint32_t>::max());
  CHECK(mp64::LowBits32(64) == std::numeric_limits<uint32_t>::max());
}

TEST_CASE("GTA IV multiplayer masks represent every peer without undefined shifts",
          "[gta4][multiplayer][64-player]") {
  mp64::PeerMask64 mask;
  CHECK(mask.Set(0));
  CHECK(mask.Set(15));
  CHECK(mask.Set(16));
  CHECK(mask.Set(31));
  CHECK(mask.Set(32));
  CHECK(mask.Set(63));
  CHECK_FALSE(mask.Set(64));
  CHECK_FALSE(mask.Set(mp64::kInvalidPeerId));

  CHECK(mask.Contains(0));
  CHECK(mask.Contains(15));
  CHECK(mask.Contains(16));
  CHECK(mask.Contains(31));
  CHECK(mask.Contains(32));
  CHECK(mask.Contains(63));
  CHECK_FALSE(mask.Contains(64));
  CHECK(mask.has_nonlegacy16_bits());
  CHECK(mask.has_nonlegacy32_bits());
  CHECK(mask.Count() == 6);

  CHECK(mask.Reset(63));
  CHECK_FALSE(mask.Contains(63));
  CHECK_FALSE(mask.Reset(64));

  mp64::PeerMask64 first_extended16;
  REQUIRE(first_extended16.Set(16));
  CHECK(first_extended16.legacy_low16() == 0);
  CHECK(first_extended16.legacy_low32() != 0);

  mp64::PeerMask64 first_extended32;
  REQUIRE(first_extended32.Set(32));
  CHECK(first_extended32.legacy_low16() == 0);
  CHECK(first_extended32.legacy_low32() == 0);
}

TEST_CASE("GTA IV dispatch mask sidecars mirror low words and retain high peers",
          "[gta4][multiplayer][64-player]") {
  mp64::PeerMaskPairRegistry registry;
  constexpr uint32_t kDispatch = 0xA4000000;
  registry.ReplaceLegacyLow32(kDispatch, 0x80000001, 0x00010000);
  REQUIRE(registry.Set(kDispatch, 32, true, true));
  REQUIRE(registry.Set(kDispatch, 63, true, false));

  mp64::PeerMaskPair64 masks = registry.Get(kDispatch);
  CHECK(masks.first.Contains(0));
  CHECK(masks.first.Contains(31));
  CHECK(masks.first.Contains(32));
  CHECK(masks.first.Contains(63));
  CHECK(masks.second.Contains(16));
  CHECK(masks.second.Contains(32));

  registry.ReplaceLegacyLow32(kDispatch, 0, 0);
  masks = registry.Get(kDispatch);
  CHECK_FALSE(masks.first.Contains(0));
  CHECK_FALSE(masks.first.Contains(31));
  CHECK(masks.first.Contains(32));
  CHECK(masks.first.Contains(63));
  CHECK(masks.second.Contains(32));
  REQUIRE(registry.Reset(kDispatch, 32, true, true));
  CHECK_FALSE(registry.Get(kDispatch).first.Contains(32));
  CHECK_FALSE(registry.Get(kDispatch).second.Contains(32));
  REQUIRE(registry.Set(kDispatch, 16, true, true));
  REQUIRE(registry.Set(kDispatch, 63, true, true));
  registry.ReplaceLegacyLow16(kDispatch, 0x0001, 0x8000);
  masks = registry.Get(kDispatch);
  CHECK(masks.first.Contains(0));
  CHECK(masks.second.Contains(15));
  CHECK(masks.first.Contains(16));
  CHECK(masks.second.Contains(16));
  CHECK(masks.first.Contains(63));
  registry.ClearNonLegacy16(kDispatch, true, false);
  masks = registry.Get(kDispatch);
  CHECK_FALSE(masks.first.Contains(16));
  CHECK_FALSE(masks.first.Contains(63));
  CHECK(masks.second.Contains(16));
  CHECK(masks.second.Contains(63));
  registry.Clear(kDispatch, false, true);
  registry.Remove(kDispatch);
  CHECK(registry.Get(kDispatch).first.bits() == 0);
}

TEST_CASE("GTA IV dispatch state sidecars isolate sparse peers and reset reuse",
          "[gta4][multiplayer][64-player]") {
  mp64::DispatchPeerStateRegistry registry;
  constexpr uint32_t kDispatch = 0xA4100000;

  mp64::DispatchPeerState peer16 = registry.Get(kDispatch, 16, 3);
  REQUIRE(peer16.element_states.size() == 3);
  REQUIRE(peer16.pending_elements.size() == 3);
  peer16.element_states[0] = {0x12, 0x34, 0x80, 0x56};
  peer16.element_states[2] = {0x78, 0x9A, 0x00, 0xBC};
  peer16.pending_elements[2] = 1;
  REQUIRE(registry.Set(kDispatch, 16, peer16));

  mp64::DispatchPeerState peer63 = registry.Get(kDispatch, 63, 3);
  peer63.element_states[1] = {0xDE, 0xAD, 0x80, 0xBE};
  peer63.pending_elements[1] = 1;
  REQUIRE(registry.Set(kDispatch, 63, peer63));

  const mp64::DispatchPeerState loaded16 = registry.Get(kDispatch, 16, 3);
  const mp64::DispatchPeerState loaded63 = registry.Get(kDispatch, 63, 3);
  CHECK(loaded16.element_states[0][0] == 0x12);
  CHECK(loaded16.element_states[2][3] == 0xBC);
  CHECK(loaded16.pending_elements[2] == 1);
  CHECK(loaded63.element_states[1][0] == 0xDE);
  CHECK(loaded63.pending_elements[1] == 1);
  CHECK_FALSE(registry.Set(kDispatch, 15, peer16));
  CHECK_FALSE(registry.Set(kDispatch, 64, peer16));

  registry.RemovePeer(kDispatch, 16);
  const mp64::DispatchPeerState reused16 = registry.Get(kDispatch, 16, 3);
  CHECK(reused16.element_states[0][0] == 0);
  CHECK(reused16.element_states[2][3] == 0);
  CHECK(reused16.pending_elements[2] == 0);
  CHECK(registry.Get(kDispatch, 63, 3).pending_elements[1] == 1);

  registry.ResetElement(kDispatch, 1);
  CHECK(registry.Get(kDispatch, 63, 3).element_states[1][0] == 0);
  CHECK(registry.Get(kDispatch, 63, 3).pending_elements[1] == 0);
  CHECK(registry.Get(kDispatch, 16, 3).element_states[0][0] == 0);

  registry.Reset(kDispatch);
  CHECK(registry.Get(kDispatch, 63, 3).element_states[1][0] == 0);
  CHECK(registry.Get(kDispatch, 63, 3).pending_elements[1] == 0);
}

TEST_CASE("GTA IV multiplayer sidecars are isolated by guest manager address",
          "[gta4][multiplayer][64-player]") {
  mp64::PeerManagerRegistry registry;
  CHECK_FALSE(registry.RegisterManager(0));
  REQUIRE(registry.RegisterManager(0x83000000));
  REQUIRE(registry.RegisterManager(0x83010000));

  CHECK(registry.SetPeer(0x83000000, 15, 0xA1000000));
  CHECK(registry.SetPeer(0x83000000, 16, 0xA1000040));
  CHECK(registry.SetPeer(0x83000000, 63, 0xA1000080));
  CHECK(registry.SetPeer(0x83010000, 16, 0xA2000000));
  CHECK_FALSE(registry.SetPeer(0x83000000, 64, 0xA3000000));
  CHECK_FALSE(registry.SetPeer(0x83020000, 16, 0xA3000000));

  CHECK(registry.GetPeer(0x83000000, 15) == 0xA1000000);
  CHECK(registry.GetPeer(0x83000000, 16) == 0xA1000040);
  CHECK(registry.GetPeer(0x83000000, 63) == 0xA1000080);
  CHECK(registry.GetPeer(0x83010000, 16) == 0xA2000000);
  CHECK(registry.GetPeer(0x83000000, 64) == 0);
  CHECK(registry.CountExtendedPeers(0x83000000) == 2);

  std::vector<uint8_t> visited;
  registry.VisitExtendedPeers(0x83000000, [&](uint8_t peer_id, uint32_t guest_peer) {
    CHECK(guest_peer != 0);
    visited.push_back(peer_id);
    return true;
  });
  REQUIRE(visited.size() == 2);
  CHECK(visited.front() == 16);
  CHECK(visited.back() == 63);

  CHECK(registry.RemovePeer(0x83000000, 16));
  CHECK(registry.GetPeer(0x83000000, 16) == 0);
  registry.UnregisterManager(0x83000000);
  CHECK(registry.GetPeer(0x83000000, 63) == 0);
  CHECK(registry.GetPeer(0x83010000, 16) == 0xA2000000);
}

TEST_CASE("GTA IV multiplayer participant sidecar queue enforces 64 entries",
          "[gta4][multiplayer][64-player]") {
  mp64::ParticipantQueue64<uint8_t> queue;
  CHECK(queue.empty());
  CHECK(queue.capacity() == mp64::kExtendedPeerCapacity);
  for (uint8_t peer_id = 0; peer_id < mp64::kExtendedPeerCapacity; ++peer_id) {
    REQUIRE(queue.Push(peer_id));
  }
  CHECK(queue.size() == mp64::kExtendedPeerCapacity);
  CHECK(queue[0] == 0);
  CHECK(queue[mp64::kExtendedPeerCapacity - 1] == 63);
  CHECK_FALSE(queue.Push(mp64::kExtendedPeerCapacity));
}

TEST_CASE("GTA IV relocated session participant table tracks 64 classified records",
          "[gta4][multiplayer][64-player]") {
  mp64::SessionParticipantRegistry registry;
  constexpr uint32_t kSession = 0xA8000000;
  constexpr uint32_t kGuestTable = 0xA9000000;
  CHECK_FALSE(registry.Register(0, kGuestTable));
  CHECK_FALSE(registry.Register(kSession, 0));
  REQUIRE(registry.Register(kSession, kGuestTable));

  for (uint32_t index = 0; index < mp64::kExtendedPeerCapacity; ++index) {
    REQUIRE(registry.Add(kSession, index >= 48));
  }
  const mp64::SessionParticipantState full = registry.Get(kSession);
  CHECK(full.guest_record_table == kGuestTable);
  CHECK(full.count == 64);
  CHECK(full.public_count == 48);
  CHECK(full.private_count == 16);
  CHECK_FALSE(registry.Add(kSession, false));

  REQUIRE(registry.Remove(kSession, false));
  REQUIRE(registry.Remove(kSession, true));
  const mp64::SessionParticipantState sparse = registry.Get(kSession);
  CHECK(sparse.count == 62);
  CHECK(sparse.public_count == 47);
  CHECK(sparse.private_count == 15);
  REQUIRE(registry.SetCounts(kSession, 2, 1, 1));
  CHECK_FALSE(registry.SetCounts(kSession, 65, 64, 1));
  CHECK(registry.RemoveSession(kSession).guest_record_table == kGuestTable);
  CHECK(registry.Get(kSession).guest_record_table == 0);
}

TEST_CASE("GTA IV participant commands retain relocated 64-record payloads",
          "[gta4][multiplayer][64-player]") {
  mp64::ParticipantCommandRegistry registry;
  constexpr uint32_t kCommand = 0xAA000000;
  constexpr uint32_t kRecords = 0xAB000000;
  CHECK_FALSE(
      registry.Set(0, {.guest_record_table = kRecords, .public_count = 48, .private_count = 16}));
  CHECK_FALSE(
      registry.Set(kCommand, {.guest_record_table = 0, .public_count = 48, .private_count = 16}));
  REQUIRE(registry.Set(kCommand,
                       {.guest_record_table = kRecords, .public_count = 48, .private_count = 16}));
  const mp64::ParticipantCommandState state = registry.Get(kCommand);
  CHECK(state.guest_record_table == kRecords);
  CHECK(state.public_count == 48);
  CHECK(state.private_count == 16);
  CHECK(state.count() == 64);
  CHECK(registry.Remove(kCommand).guest_record_table == kRecords);
  CHECK(registry.Get(kCommand).guest_record_table == 0);
}

TEST_CASE("GTA IV migration tasks retain high snapshot cursors out of line",
          "[gta4][multiplayer][64-player]") {
  mp64::MigrationTaskRegistry registry;
  constexpr uint32_t kTask = 0xAC000000;
  constexpr uint32_t kRecords = 0xAD000000;
  CHECK_FALSE(registry.Set(0, {.guest_record_table = kRecords, .count = 64, .current = -1}));
  CHECK_FALSE(registry.Set(kTask, {.guest_record_table = 0, .count = 64, .current = -1}));
  REQUIRE(registry.Set(kTask, {.guest_record_table = kRecords, .count = 64, .current = -1}));
  REQUIRE(registry.SetCurrent(kTask, 31));
  REQUIRE(registry.SetCurrent(kTask, 63));
  REQUIRE(registry.SetCurrent(kTask, 64));
  CHECK_FALSE(registry.SetCurrent(kTask, 65));
  CHECK(registry.Get(kTask).current == 64);
  CHECK(registry.Remove(kTask).guest_record_table == kRecords);
  CHECK(registry.Get(kTask).guest_record_table == 0);
}

TEST_CASE("GTA IV reassignment sidecars retain 64-bit masks and transport state",
          "[gta4][multiplayer][64-player]") {
  mp64::ReassignmentRegistry registry;
  constexpr uint32_t kManager = 0xAE000000;
  mp64::ReassignmentOwnerState state;
  state.initialized = true;
  REQUIRE(state.involved.Set(0));
  REQUIRE(state.involved.Set(16));
  REQUIRE(state.involved.Set(63));
  state.involved.ReplaceLegacyLow16(0x8001);
  REQUIRE(state.confirmed.Set(16));
  REQUIRE(state.sent.Set(63));
  REQUIRE(registry.Set(kManager, 16, state));
  REQUIRE(registry.SetTransport(kManager, 16, 0, 0xAF000000));
  REQUIRE(registry.SetTransport(kManager, 16, 63, 0xAF00004C));

  const mp64::ReassignmentOwnerState stored = registry.Get(kManager, 16);
  CHECK(stored.involved.Contains(0));
  CHECK(stored.involved.Contains(15));
  CHECK(stored.involved.Contains(16));
  CHECK(stored.involved.Contains(63));
  CHECK(stored.confirmed.Contains(16));
  CHECK(stored.sent.Contains(63));
  CHECK(registry.GetTransport(kManager, 16, 0) == 0xAF000000);
  CHECK(registry.GetTransport(kManager, 16, 63) == 0xAF00004C);
  CHECK_FALSE(registry.Set(kManager, 64, state));
  CHECK_FALSE(registry.SetTransport(kManager, 16, 64, 0xAF000098));

  const auto removed = registry.RemoveManager(kManager);
  CHECK(removed[16].guest_transport_states[0] == 0xAF000000);
  CHECK(removed[16].guest_transport_states[63] == 0xAF00004C);
  CHECK(registry.Get(kManager, 16).initialized == false);
}

TEST_CASE("GTA IV extended player-info sidecar supports sparse lifecycle",
          "[gta4][multiplayer][64-player]") {
  mp64::PlayerInfoRegistry registry;
  CHECK_FALSE(registry.Set(15, {0xA0000000, 1}));
  CHECK_FALSE(registry.Set(16, {0, 1}));
  REQUIRE(registry.Set(16, {0xA1000000, 11}));
  REQUIRE(registry.Set(63, {0xA2000000, 12}));
  CHECK(registry.Count() == 2);

  const auto snapshot = registry.Snapshot();
  CHECK(snapshot[16].guest_player_info == 0xA1000000);
  CHECK(snapshot[63].generation == 12);

  CHECK(registry.Get(16).guest_player_info == 0xA1000000);
  CHECK(registry.Get(16).generation == 11);
  CHECK(registry.Get(63).guest_player_info == 0xA2000000);
  CHECK(registry.Get(17).guest_player_info == 0);

  const mp64::PlayerInfoEntry removed = registry.Remove(16);
  CHECK(removed.guest_player_info == 0xA1000000);
  CHECK(removed.generation == 11);
  CHECK(registry.Get(16).guest_player_info == 0);
  CHECK(registry.Count() == 1);
  REQUIRE(registry.Set(16, {0xA3000000, 13}));
  CHECK(registry.Get(16).guest_player_info == 0xA3000000);
  CHECK(registry.Count() == 2);
}

TEST_CASE("GTA IV network endpoint sidecars support sparse peer reuse",
          "[gta4][multiplayer][64-player]") {
  mp64::NetworkEndpointRegistry registry;
  constexpr uint32_t kFirstObject = 0xA5000000;
  constexpr uint32_t kSecondObject = 0xA5001000;
  CHECK_FALSE(registry.Set(kFirstObject, 15, 0xA6000000));
  CHECK_FALSE(registry.Set(kFirstObject, 16, 0));
  REQUIRE(registry.Set(kFirstObject, 16, 0xA6000040));
  CHECK_FALSE(registry.Set(kFirstObject, 16, 0xA60000C0));
  CHECK(registry.Get(kFirstObject, 16) == 0xA6000040);
  REQUIRE(registry.Set(kFirstObject, 16, 0xA6000040));
  REQUIRE(registry.Set(kFirstObject, 63, 0xA6000080));
  REQUIRE(registry.Set(kSecondObject, 16, 0xA7000040));
  CHECK(registry.Get(kFirstObject, 16) == 0xA6000040);
  CHECK(registry.Get(kFirstObject, 63) == 0xA6000080);
  CHECK(registry.Get(kSecondObject, 16) == 0xA7000040);

  std::vector<uint8_t> visited;
  registry.VisitExtended(kFirstObject, [&](uint8_t peer_id, uint32_t endpoint) {
    CHECK(endpoint != 0);
    visited.push_back(peer_id);
    return true;
  });
  REQUIRE(visited.size() == 2);
  CHECK(visited.front() == 16);
  CHECK(visited.back() == 63);

  CHECK(registry.Remove(kFirstObject, 16) == 0xA6000040);
  CHECK(registry.Get(kFirstObject, 16) == 0);
  REQUIRE(registry.Set(kFirstObject, 16, 0xA60000C0));
  CHECK(registry.Get(kFirstObject, 16) == 0xA60000C0);
  registry.RemoveObject(kFirstObject);
  CHECK(registry.Get(kFirstObject, 16) == 0);
  CHECK(registry.Get(kSecondObject, 16) == 0xA7000040);
}

TEST_CASE("GTA IV network object peer flags cover sparse high recipients",
          "[gta4][multiplayer][64-player]") {
  mp64::NetworkObjectPeerFlagsRegistry registry;
  constexpr uint32_t kFirstObject = 0xA5100000;
  constexpr uint32_t kSecondObject = 0xA5101000;
  CHECK_FALSE(registry.Set(kFirstObject, 15, {1, 0, 0}));
  REQUIRE(registry.Set(kFirstObject, 16, {1, 0, 0}));
  REQUIRE(registry.Set(kFirstObject, 63, {0, 1, 0}));
  REQUIRE(registry.Set(kSecondObject, 16, {0, 0, 1}));
  CHECK(registry.Get(kFirstObject, 16) == mp64::NetworkObjectPeerFlags{1, 0, 0});
  CHECK(registry.Get(kFirstObject, 63) == mp64::NetworkObjectPeerFlags{0, 1, 0});
  CHECK(registry.Get(kSecondObject, 16) == mp64::NetworkObjectPeerFlags{0, 0, 1});
  REQUIRE(registry.SetFlag(kFirstObject, 63, 2, true));
  CHECK(registry.Get(kFirstObject, 63) == mp64::NetworkObjectPeerFlags{0, 1, 1});
  CHECK_FALSE(registry.SetFlag(kFirstObject, 63, 3, true));

  registry.ClearPeer(kFirstObject, 16);
  CHECK(registry.Get(kFirstObject, 16) == mp64::NetworkObjectPeerFlags{});
  REQUIRE(registry.Set(kFirstObject, 16, {0, 0, 1}));
  CHECK(registry.Get(kFirstObject, 16) == mp64::NetworkObjectPeerFlags{0, 0, 1});
  registry.RemoveObject(kFirstObject);
  CHECK(registry.Get(kFirstObject, 16) == mp64::NetworkObjectPeerFlags{});
  CHECK(registry.Get(kSecondObject, 16) == mp64::NetworkObjectPeerFlags{0, 0, 1});
}

TEST_CASE("GTA IV ped network state sidecars isolate extended recipients",
          "[gta4][multiplayer][64-player]") {
  mp64::PedNetworkPeerStateRegistry registry;
  constexpr uint32_t kGuestObject = 0xA5200000;
  mp64::PedNetworkPeerState first{};
  mp64::PedNetworkPeerState last{};
  first.front() = 0x11;
  first.back() = 0x22;
  last.front() = 0x33;
  last.back() = 0x44;

  CHECK_FALSE(registry.Set(kGuestObject, 15, first));
  REQUIRE(registry.Set(kGuestObject, 16, first));
  REQUIRE(registry.Set(kGuestObject, 63, last));
  CHECK(registry.Get(kGuestObject, 16) == first);
  CHECK(registry.Get(kGuestObject, 63) == last);
  registry.RemoveObject(kGuestObject);
  CHECK(registry.Get(kGuestObject, 16) == mp64::PedNetworkPeerState{});
}

TEST_CASE("GTA IV extended network peer masks preserve peers 16 and 63 on wire",
          "[gta4][multiplayer][64-player]") {
  mp64::PeerMask64 low_only;
  REQUIRE(low_only.Set(0));
  REQUIRE(low_only.Set(15));
  const mp64::NetworkPeerMaskWireWords low_wire = mp64::EncodeNetworkPeerMask(low_only);
  CHECK_FALSE(low_wire.extended);
  CHECK(low_wire.header == 0x8001);
  CHECK(mp64::DecodeNetworkPeerMask(low_wire.header, low_wire.extension).bits() == low_only.bits());

  mp64::PeerMask64 extended = low_only;
  REQUIRE(extended.Set(16));
  REQUIRE(extended.Set(31));
  REQUIRE(extended.Set(32));
  REQUIRE(extended.Set(47));
  REQUIRE(extended.Set(48));
  REQUIRE(extended.Set(63));
  const mp64::NetworkPeerMaskWireWords wire = mp64::EncodeNetworkPeerMask(extended);
  CHECK(wire.extended);
  CHECK((wire.header & mp64::kNetworkPeerMaskExtensionMarker) != 0);
  CHECK(wire.extension[0] == 0x8001);
  CHECK(wire.extension[1] == 0x8001);
  CHECK(wire.extension[2] == 0x8001);
  const mp64::PeerMask64 decoded = mp64::DecodeNetworkPeerMask(wire.header, wire.extension);
  CHECK(decoded.bits() == extended.bits());
  CHECK(decoded.Contains(16));
  CHECK(decoded.Contains(63));
}

TEST_CASE("GTA IV event sidecars isolate high peer buffers and scopes",
          "[gta4][multiplayer][64-player]") {
  mp64::EventPeerBufferRegistry buffers;
  constexpr uint32_t kManager = 0xA5200000;
  CHECK_FALSE(buffers.Set(kManager, 15, {0xA5300000, 0xA5400000}));
  REQUIRE(buffers.Set(kManager, 16, {0xA5301000, 0xA5401000}));
  REQUIRE(buffers.Set(kManager, 63, {0xA5302000, 0xA5402000}));
  CHECK(buffers.Get(kManager, 16).guest_outbound == 0xA5301000);
  CHECK(buffers.Get(kManager, 63).guest_inbound == 0xA5402000);
  CHECK(buffers.RemovePeer(kManager, 16).guest_outbound == 0xA5301000);
  CHECK(buffers.Get(kManager, 16).guest_outbound == 0);

  mp64::EventScopeRegistry scopes;
  mp64::PeerMask64 first;
  REQUIRE(first.Set(16));
  REQUIRE(first.Set(63));
  REQUIRE(scopes.Set(0xA5500000, first));
  CHECK(scopes.Get(0xA5500000).Contains(16));
  CHECK(scopes.Get(0xA5500000).Contains(63));
  scopes.ResetPeer(16);
  CHECK_FALSE(scopes.Get(0xA5500000).Contains(16));
  CHECK(scopes.Get(0xA5500000).Contains(63));
  REQUIRE(scopes.Snapshot().size() == 1);
  scopes.Remove(0xA5500000);
  CHECK(scopes.Snapshot().empty());
}

TEST_CASE("GTA IV object-manager timing sidecars cover high peer channels",
          "[gta4][multiplayer][64-player]") {
  mp64::ObjectManagerPeerTimingRegistry timings;
  constexpr uint32_t kManager = 0xA5600000;
  CHECK_FALSE(timings.SetLastReceived(kManager, 15, 100));
  REQUIRE(timings.SetLastReceived(kManager, 16, 100));
  REQUIRE(timings.SetLastReceived(kManager, 63, 200));
  REQUIRE(timings.SetChannelReceived(kManager, 16, 0, 300));
  REQUIRE(timings.SetChannelReceived(kManager, 63, 3, 400));
  CHECK_FALSE(timings.SetChannelReceived(kManager, 63, 4, 500));
  CHECK(timings.Get(kManager, 16).last_received == 100);
  CHECK(timings.Get(kManager, 16).channel_received[0] == 300);
  CHECK(timings.Get(kManager, 63).last_received == 200);
  CHECK(timings.Get(kManager, 63).channel_received[3] == 400);
  timings.RemovePeer(kManager, 16);
  CHECK(timings.Get(kManager, 16).last_received == 0);
  CHECK(timings.Get(kManager, 63).last_received == 200);
  timings.RemoveManager(kManager);
  CHECK(timings.Get(kManager, 63).last_received == 0);
}

TEST_CASE("GTA IV player tick state isolates sparse high peers", "[gta4][multiplayer][64-player]") {
  mp64::PlayerTickStateRegistry states;
  constexpr uint32_t kManager = 0xA5650000;
  mp64::PlayerTickState first;
  first.first_byte = 1;
  first.second_byte = 2;
  first.eight_byte_record.fill(3);
  first.four_byte_record.fill(4);
  first.pointer_record.fill(5);
  first.large_record.fill(6);
  mp64::PlayerTickState last = first;
  last.first_byte = 7;
  last.large_record.fill(8);

  CHECK_FALSE(states.Set(kManager, 15, first));
  REQUIRE(states.Set(kManager, 16, first));
  REQUIRE(states.Set(kManager, 63, last));
  CHECK(states.Get(kManager, 16) == first);
  CHECK(states.Get(kManager, 63) == last);
  states.RemovePeer(kManager, 16);
  CHECK(states.Get(kManager, 16) == mp64::PlayerTickState{});
  CHECK(states.Get(kManager, 63) == last);
  states.RemoveManager(kManager);
  CHECK(states.Get(kManager, 63) == mp64::PlayerTickState{});
}

TEST_CASE("GTA IV proximity weights retain retail falloff semantics",
          "[gta4][multiplayer][64-player]") {
  CHECK(mp64::LinearProximityWeight(5.0f, 10.0f, 0.1f) == 1.0f);
  CHECK(mp64::LinearProximityWeight(15.0f, 10.0f, 0.1f) == 0.5f);
  CHECK(mp64::LinearProximityWeight(20.0f, 10.0f, 0.1f) == 0.0f);
}

TEST_CASE("GTA IV proximity status weights aggregate all 64 player categories",
          "[gta4][multiplayer][64-player]") {
  const std::array<uint32_t, 3> counts = {16, 16, 32};
  const std::array<float, 3> multipliers = {1.0f, 2.0f, 3.0f};
  const auto weights = mp64::ComputeGlobalProximityStatusWeights(counts, 1440, multipliers);
  CHECK(weights == std::array<uint32_t, 3>{10, 20, 30});
  CHECK(mp64::ComputeGlobalProximityStatusWeights({}, 1440, multipliers) ==
        std::array<uint32_t, 3>{});
}

TEST_CASE("GTA IV object-manager packet buffers isolate peers 16 and 63",
          "[gta4][multiplayer][64-player]") {
  mp64::ObjectManagerPeerBufferRegistry buffers;
  constexpr uint32_t kManager = 0xA5700000;
  CHECK_FALSE(buffers.Set(kManager, 15, {0xA5700000, 0xA5800000, 0xA5900000, 0xA5A00000, 15}));
  REQUIRE(buffers.Set(kManager, 16, {0xA5701000, 0xA5801000, 0xA5901000, 0xA5A01000, 16}));
  REQUIRE(buffers.Set(kManager, 63, {0xA5702000, 0xA5802000, 0xA5902000, 0xA5A02000, 63}));
  CHECK(buffers.Get(kManager, 16).guest_message == 0xA5701000);
  CHECK(buffers.Get(kManager, 16).guest_sync_ack == 0xA5801000);
  CHECK(buffers.Get(kManager, 16).sequence == 16);
  CHECK(buffers.Get(kManager, 63).guest_reliable == 0xA5902000);
  CHECK(buffers.RemovePeer(kManager, 16).guest_queue == 0xA5A01000);
  CHECK(buffers.Get(kManager, 16).guest_sync_ack == 0);
  const auto remaining = buffers.RemoveManager(kManager);
  CHECK(remaining[63].guest_queue == 0xA5A02000);
  CHECK(remaining[63].sequence == 63);
  CHECK(buffers.Get(kManager, 63).guest_reliable == 0);
}

TEST_CASE("GTA IV object owner lists preserve sparse high owners",
          "[gta4][multiplayer][64-player]") {
  mp64::ObjectOwnerListRegistry lists;
  constexpr uint32_t kManager = 0xA5B00000;
  CHECK_FALSE(lists.Set(kManager, 15, {0xA5B01000, 0xA5B02000}));
  REQUIRE(lists.Set(kManager, 16, {0xA5B11000, 0xA5B12000}));
  REQUIRE(lists.Set(kManager, 63, {0xA5B21000, 0xA5B22000}));
  CHECK(lists.Get(kManager, 16).guest_head == 0xA5B11000);
  CHECK(lists.Get(kManager, 63).guest_tail == 0xA5B22000);
  lists.ClearOwner(kManager, 16);
  CHECK(lists.Get(kManager, 16).empty());
  CHECK_FALSE(lists.Get(kManager, 63).empty());
  const auto remaining = lists.RemoveManager(kManager);
  CHECK(remaining[63].guest_head == 0xA5B21000);
  CHECK(lists.Get(kManager, 63).empty());
}

TEST_CASE("GTA IV object peer matrix isolates high peers and object reuse",
          "[gta4][multiplayer][64-player]") {
  mp64::ObjectPeerMatrixRegistry matrix;
  constexpr uint32_t kManager = 0xA5C00000;
  constexpr uint16_t kFirstObject = 1;
  constexpr uint16_t kLastObject = mp64::kObjectPeerMatrixObjectCapacity - 1;
  CHECK_FALSE(matrix.Set(kManager, kFirstObject, 15, 10));
  CHECK_FALSE(matrix.Set(kManager, mp64::kObjectPeerMatrixObjectCapacity, 16, 10));
  REQUIRE(matrix.Set(kManager, kFirstObject, 16, 100));
  REQUIRE(matrix.Set(kManager, kLastObject, 63, 200));
  CHECK(matrix.Get(kManager, kFirstObject, 16) == 100);
  CHECK(matrix.Get(kManager, kLastObject, 63) == 200);
  CHECK(matrix.Increment(kManager, kFirstObject, 16) == 101);
  matrix.ClearPeer(kManager, 16);
  CHECK(matrix.Get(kManager, kFirstObject, 16) == 0);
  CHECK(matrix.Get(kManager, kLastObject, 63) == 200);
  matrix.ClearObject(kManager, kLastObject);
  CHECK(matrix.Get(kManager, kLastObject, 63) == 0);
  REQUIRE(matrix.Set(kManager, kFirstObject, 63, 300));
  matrix.RemoveManager(kManager);
  CHECK(matrix.Get(kManager, kFirstObject, 63) == 0);
}

TEST_CASE("GTA IV voice cursor rotates bounded windows across 64 peers",
          "[gta4][multiplayer][64-player]") {
  mp64::RoundRobinCursor cursor;
  CHECK(cursor.start(63) == 0);
  cursor.Advance(63, 15);
  CHECK(cursor.start(63) == 15);
  cursor.Advance(63, 15);
  CHECK(cursor.start(63) == 30);
  cursor.Advance(63, 15);
  CHECK(cursor.start(63) == 45);
  cursor.Advance(63, 15);
  CHECK(cursor.start(63) == 60);
  cursor.Advance(63, 15);
  CHECK(cursor.start(63) == 12);
  cursor.Advance(0, 15);
  CHECK(cursor.start(0) == 0);
}

TEST_CASE("GTA IV relocated participant guest records preserve exact MP64 boundaries",
          "[gta4][multiplayer][64-player][guest-memory]") {
  constexpr uint32_t kGuestTable = 0x1000;
  constexpr std::array<std::size_t, 5> kBoundaries = {15, 16, 31, 32, 63};
  constexpr std::array<uint32_t, 5> kExpectedAddresses = {
      0x11E0, 0x1200, 0x13E0, 0x1400, 0x17E0};
  for (std::size_t index = 0; index < kBoundaries.size(); ++index) {
    CHECK(mp64::CheckedGuestArrayAddress(kGuestTable, kBoundaries[index],
                                         mp64::kSessionParticipantRecordSize) ==
          kExpectedAddresses[index]);
  }
  CHECK(mp64::CheckedGuestArrayAddress(kGuestTable, mp64::kExtendedPeerCapacity,
                                       mp64::kSessionParticipantRecordSize) == 0x1800);

  mp64::SessionParticipantRegistry participants;
  REQUIRE(participants.Register(0xA6000000, kGuestTable));
  for (std::size_t peer_id = 0; peer_id < mp64::kExtendedPeerCapacity; ++peer_id) {
    REQUIRE(participants.Add(0xA6000000, false));
    if (std::ranges::find(kBoundaries, peer_id) != kBoundaries.end()) {
      CHECK(participants.Get(0xA6000000).count == peer_id + 1);
    }
  }
  CHECK_FALSE(participants.Add(0xA6000000, false));
  CHECK(participants.Get(0xA6000000).count == mp64::kExtendedPeerCapacity);
}
