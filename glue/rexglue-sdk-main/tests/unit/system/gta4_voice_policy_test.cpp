#include <catch2/catch_test_macros.hpp>

#include "input/voice_policy.h"
#include "network/community_voice_recovery.h"
#include "network/gta4_voice_audio.h"

namespace {

using gta4::input::ResolveVoiceRoute;
using gta4::input::VoicePolicyState;
using gta4::network::detail::IsAuthoritativeVoiceRouteLoss;
using gta4::network::detail::IsCanonicalVoiceCursor;
using gta4::network::detail::VoiceRouteRecoveryGate;
using gta4::voice::detail::EvaluateVoiceDeviceLifecycle;
using rex::system::xam::SessionMember;
using rex::system::xam::VoiceChannel;

TEST_CASE("GTA IV focused talker takes precedence over team voice") {
  VoicePolicyState policy{.session_id = 1, .team_only = true, .focused_peer_id = 2};
  const std::vector<SessionMember> members = {
      {.xuid = 10, .multiplayer_peer_id = 1},
      {.xuid = 20, .multiplayer_peer_id = 2},
  };
  const std::optional<std::vector<uint64_t>> team_targets = std::vector<uint64_t>{10};
  const auto route = ResolveVoiceRoute(policy, members, team_targets, {});

  REQUIRE(route.channel == VoiceChannel::kPrivate);
  REQUIRE(route.target_xuids == std::vector<uint64_t>{20});
}

TEST_CASE("GTA IV unresolved targeted voice remains fail closed") {
  VoicePolicyState policy{.session_id = 1, .focused_peer_id = 9};
  const std::vector<SessionMember> members = {
      {.xuid = 10, .multiplayer_peer_id = 1},
  };
  const auto route = ResolveVoiceRoute(policy, members, std::nullopt, {});

  REQUIRE(route.channel == VoiceChannel::kPrivate);
  REQUIRE(route.target_xuids.empty());
}

TEST_CASE("GTA IV empty team voice remains a team route") {
  VoicePolicyState policy{.session_id = 1, .team_only = true};
  const std::vector<SessionMember> members;
  const std::optional<std::vector<uint64_t>> no_teammates = std::vector<uint64_t>{};
  const auto route = ResolveVoiceRoute(policy, members, no_teammates, {});

  REQUIRE(route.channel == VoiceChannel::kTeam);
  REQUIRE(route.target_xuids.empty());
}

TEST_CASE("GTA IV voice mutes merge manual and relationship policy") {
  VoicePolicyState policy{.session_id = 1, .manually_muted_xuids = {30, 20}};
  const std::vector<uint64_t> relationship_mutes = {20, 40};
  const auto route = ResolveVoiceRoute(policy, {}, std::nullopt, relationship_mutes);

  REQUIRE(route.muted_xuids == std::vector<uint64_t>{20, 30, 40});
}

TEST_CASE("GTA IV voice policy survives migration but not unrelated sessions") {
  VoicePolicyState policy{
      .session_id = 1, .team_only = true, .focused_peer_id = 2, .manually_muted_xuids = {20}};
  policy.ObserveSession(3, 1);
  REQUIRE(policy.session_id == 3);
  REQUIRE(policy.team_only);
  REQUIRE(policy.focused_peer_id == 2);
  REQUIRE(policy.manually_muted_xuids == std::vector<uint64_t>{20});

  policy.ObserveSession(4, 0);
  REQUIRE(policy.session_id == 4);
  REQUIRE_FALSE(policy.team_only);
  REQUIRE_FALSE(policy.focused_peer_id.has_value());
  REQUIRE(policy.manually_muted_xuids.empty());
}

TEST_CASE("Community voice recovers only from authoritative route loss") {
  REQUIRE(IsAuthoritativeVoiceRouteLoss(403));
  REQUIRE(IsAuthoritativeVoiceRouteLoss(404));
  REQUIRE_FALSE(IsAuthoritativeVoiceRouteLoss(0));
  REQUIRE_FALSE(IsAuthoritativeVoiceRouteLoss(400));
  REQUIRE_FALSE(IsAuthoritativeVoiceRouteLoss(401));
  REQUIRE_FALSE(IsAuthoritativeVoiceRouteLoss(500));
}

TEST_CASE("Community voice accepts only canonical cursor tokens") {
  REQUIRE(IsCanonicalVoiceCursor("cursor_1"));
  REQUIRE(IsCanonicalVoiceCursor("cursor_18446744073709551615"));
  REQUIRE_FALSE(IsCanonicalVoiceCursor(""));
  REQUIRE_FALSE(IsCanonicalVoiceCursor("cursor_"));
  REQUIRE_FALSE(IsCanonicalVoiceCursor("cursor_0"));
  REQUIRE_FALSE(IsCanonicalVoiceCursor("cursor_01"));
  REQUIRE_FALSE(IsCanonicalVoiceCursor("cursor_-1"));
  REQUIRE_FALSE(IsCanonicalVoiceCursor("cursor_18446744073709551616"));
  REQUIRE_FALSE(IsCanonicalVoiceCursor("voice_1"));
}

TEST_CASE("Community voice permits one recovery request for the current route") {
  VoiceRouteRecoveryGate gate;
  gate.OnConfigured();

  REQUIRE(gate.Begin(404, true, true));
  REQUIRE_FALSE(gate.Begin(404, true, true));
  REQUIRE(gate.CanCommit(true, true));
  REQUIRE_FALSE(gate.CanCommit(false, true));
  REQUIRE_FALSE(gate.CanCommit(true, false));

  gate.Finish();
  REQUIRE(gate.Begin(403, true, true));
  gate.Finish();
}

TEST_CASE("Community voice rejects stale or throttled recovery and closes cleanly") {
  VoiceRouteRecoveryGate gate;
  REQUIRE_FALSE(gate.Begin(404, true, true));

  gate.OnConfigured();
  REQUIRE_FALSE(gate.Begin(404, false, true));
  REQUIRE_FALSE(gate.Begin(404, true, false));
  REQUIRE_FALSE(gate.Begin(500, true, true));

  gate.OnClosed();
  REQUIRE_FALSE(gate.Begin(404, true, true));
  REQUIRE_FALSE(gate.CanCommit(true, true));
}

TEST_CASE("GTA IV voice lifecycle keeps playback while microphone permission is denied") {
  const auto actions = EvaluateVoiceDeviceLifecycle({
      .active = true,
      .permission_granted = false,
      .playback_device_available = true,
      .capture_device_available = true,
      .playback_stream_open = false,
      .capture_stream_open = true,
  });

  REQUIRE(actions.open_playback);
  REQUIRE(actions.close_capture);
  REQUIRE_FALSE(actions.close_playback);
  REQUIRE_FALSE(actions.open_capture);
}

TEST_CASE("GTA IV voice lifecycle recovers after permission and device return") {
  const auto actions = EvaluateVoiceDeviceLifecycle({
      .active = true,
      .permission_granted = true,
      .playback_device_available = true,
      .capture_device_available = true,
  });

  REQUIRE(actions.open_playback);
  REQUIRE(actions.open_capture);
  REQUIRE_FALSE(actions.close_playback);
  REQUIRE_FALSE(actions.close_capture);
}

TEST_CASE("GTA IV voice lifecycle replaces invalidated SDL streams") {
  const auto actions = EvaluateVoiceDeviceLifecycle({
      .active = true,
      .permission_granted = true,
      .playback_device_available = true,
      .capture_device_available = true,
      .playback_stream_open = true,
      .capture_stream_open = true,
      .playback_stream_invalidated = true,
      .capture_stream_invalidated = true,
  });

  REQUIRE(actions.close_playback);
  REQUIRE(actions.close_capture);
  REQUIRE(actions.open_playback);
  REQUIRE(actions.open_capture);
}

TEST_CASE("GTA IV voice lifecycle withdraws headset state while the microphone is absent") {
  const auto actions = EvaluateVoiceDeviceLifecycle({
      .active = true,
      .permission_granted = true,
      .playback_device_available = true,
      .capture_device_available = false,
      .playback_stream_open = true,
      .capture_stream_open = true,
  });

  REQUIRE_FALSE(actions.close_playback);
  REQUIRE(actions.close_capture);
  REQUIRE_FALSE(actions.open_playback);
  REQUIRE_FALSE(actions.open_capture);
}

TEST_CASE("GTA IV voice lifecycle closes devices after the final client") {
  const auto actions = EvaluateVoiceDeviceLifecycle({
      .active = false,
      .permission_granted = true,
      .playback_device_available = true,
      .capture_device_available = true,
      .playback_stream_open = true,
      .capture_stream_open = true,
  });

  REQUIRE(actions.close_playback);
  REQUIRE(actions.close_capture);
  REQUIRE_FALSE(actions.open_playback);
  REQUIRE_FALSE(actions.open_capture);
}

}  // namespace
