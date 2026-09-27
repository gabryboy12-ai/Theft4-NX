/**
 ******************************************************************************
 * @file        online_services_test.cpp
 * @brief       Tests for LAN-safe social, progression, and voice services.
 ******************************************************************************
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/system/xam/social_cache.h>

#include "kernel/xam/apps/xlivebase_social_abi.h"
#include "system/xam/live_compatibility_internal.h"

namespace {

using rex::X_RESULT;
using rex::system::xam::StatValueType;
using rex::system::xam::StatView;
using rex::system::xam::VoiceChannel;
using rex::system::xam::VoiceRoute;
using rex::system::xam::InMemorySessionDirectory;
using rex::system::xam::QosListenerUpdate;
using rex::system::xam::QosTarget;
using rex::system::xam::SessionRecord;
namespace live_detail = rex::system::xam::detail;
namespace social_abi = rex::kernel::xam::apps::detail;

auto MakeServices(uint64_t local_xuid,
                  const std::shared_ptr<InMemorySessionDirectory>& directory) {
  return live_detail::CreateInMemoryOnlineServices(local_xuid, directory);
}

TEST_CASE("LAN QoS carries exact title data and rejects unknown exchange keys", "[live][qos]") {
  constexpr uint64_t local_xuid = 0xE000000000000123ULL;
  constexpr uint64_t session_id = 0xAE00000000000001ULL;
  const std::array<uint8_t, 16> exchange_key = {
      0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  const std::array<uint8_t, rex::system::xam::kQosTitleDataSize> title_data = {
      16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27};
  auto directory = std::make_shared<InMemorySessionDirectory>();
  SessionRecord session{.title_id = 0x545407F2,
                        .session_id = session_id,
                        .exchange_key = exchange_key,
                        .max_public_slots = 1,
                        .open_public_slots = 1,
                        .host_xuid = local_xuid,
                        .host_ipv4 = 0x0100007F};
  REQUIRE(directory->Create(session));
  auto services = MakeServices(local_xuid, directory);
  REQUIRE(services.qos_service);
  QosListenerUpdate initial_listener{.enabled = true, .title_data = title_data};
  REQUIRE(services.qos_service->UpdateListener(session_id, exchange_key, initial_listener));
  REQUIRE(services.qos_service->UpdateListener(
      session_id, exchange_key,
      QosListenerUpdate{.enabled = true, .bits_per_second = 16384}));
  const std::array targets = {QosTarget{.session_id = session_id,
                                        .exchange_key = exchange_key}};
  const auto results = services.qos_service->Lookup(targets);
  REQUIRE(results.size() == 1);
  CHECK(results.front().reachable);
  REQUIRE(results.front().title_data);
  CHECK(*results.front().title_data == title_data);
  CHECK(results.front().probes_xmit == 1);
  CHECK(results.front().probes_recv == 1);

  auto wrong_key = exchange_key;
  wrong_key.front() = 255;
  const std::array wrong_targets = {
      QosTarget{.session_id = session_id, .exchange_key = wrong_key}};
  const auto wrong = services.qos_service->Lookup(wrong_targets);
  REQUIRE(wrong.size() == 1);
  CHECK_FALSE(wrong.front().reachable);

  REQUIRE(services.qos_service->UpdateListener(
      session_id, exchange_key, QosListenerUpdate{.enabled = false}));
  CHECK_FALSE(services.qos_service->Lookup(targets).front().reachable);
  REQUIRE(services.qos_service->UpdateListener(
      session_id, exchange_key, QosListenerUpdate{.enabled = true}));
  const auto reenabled = services.qos_service->Lookup(targets);
  REQUIRE(reenabled.front().reachable);
  REQUIRE(reenabled.front().title_data);
  CHECK(*reenabled.front().title_data == title_data);

  REQUIRE(services.qos_service->Close(session_id, exchange_key));
  CHECK_FALSE(services.qos_service->Lookup(targets).front().reachable);
}

TEST_CASE("LAN invitations are received before explicit acceptance", "[live][social]") {
  constexpr uint64_t local_xuid = 0xE000000000000123ULL;
  constexpr uint64_t session_id = 0xAE00000000000001ULL;
  auto directory = std::make_shared<InMemorySessionDirectory>();
  SessionRecord session{.title_id = 0x545407F2,
                        .session_id = session_id,
                        .max_public_slots = 1,
                        .open_public_slots = 1,
                        .host_xuid = local_xuid};
  REQUIRE(directory->Create(session));
  auto services = MakeServices(local_xuid, directory);
  REQUIRE(services.social_service);
  const std::array recipients = {local_xuid};
  const std::array<uint8_t, 3> custom_data = {1, 2, 3};
  REQUIRE(services.social_service->SendInvitations(session_id, recipients, custom_data));
  const auto received = services.social_service->AcceptedInvitation();
  REQUIRE(received);
  REQUIRE(received->session);
  CHECK(received->session->session_id == session_id);
  CHECK(received->recipient_xuid == local_xuid);
  CHECK(received->custom_data == std::vector<uint8_t>(custom_data.begin(), custom_data.end()));
  CHECK_FALSE(services.social_service->AcceptedInvitation());
  const auto accepted = services.social_service->AcceptInvitation(session_id, local_xuid);
  REQUIRE(accepted);
  CHECK(accepted->id == received->id);
  CHECK_FALSE(services.social_service->AcceptInvitation(session_id, local_xuid));
  CHECK(services.social_service->QueryMute(0xE000000000000456ULL) ==
        rex::system::xam::CachedMuteState::kNotMuted);
}

TEST_CASE("pending invite cache waits for explicit title acceptance and refreshes migration",
          "[live][social][invite][cache]") {
  live_detail::SocialCacheState cache;
  rex::system::xam::InvitationRecord pending{
      .id = "invite-a",
      .sender_xuid = 0xE000000000000101ULL,
      .recipient_xuid = 0xE000000000000102ULL,
      .session_id = 0xAE00000000000001ULL,
      .revision = 1,
  };
  pending.session = SessionRecord{.title_id = 0x545407F2,
                                  .session_id = pending.session_id,
                                  .max_public_slots = 1,
                                  .open_public_slots = 1,
                                  .host_xuid = pending.sender_xuid};
  REQUIRE(cache.ObservePending(pending, "idem-first"));
  CHECK(cache.pending_invitation_count() == 1);
  CHECK_FALSE(cache.HasAcceptedInvitation());
  CHECK_FALSE(cache.PrepareExplicitAcceptance(0xAE00000000000002ULL,
                                              pending.sender_xuid));

  uint32_t notifications = 0;
  bool cache_preceded_notification = false;
  cache.SetInviteNotificationHandler([&] {
    ++notifications;
    cache_preceded_notification = cache.HasAcceptedInvitation();
  });
  REQUIRE(cache.PresentNextPending());
  CHECK(cache_preceded_notification);
  CHECK(notifications == 1);
  cache.SetInviteNotificationHandler([&] { ++notifications; });
  CHECK(notifications == 2);
  const auto presented = cache.ConsumeAcceptedInvitation();
  REQUIRE(presented);
  CHECK(presented->id == pending.id);
  CHECK_FALSE(cache.ConsumeAcceptedInvitation());
  CHECK(cache.delivered_invitation_count() == 1);

  auto migrated = pending;
  migrated.session_id = 0xAE00000000000002ULL;
  migrated.session = SessionRecord{.title_id = 0x545407F2,
                                   .session_id = migrated.session_id,
                                   .previous_session_id = pending.session_id,
                                   .max_public_slots = 1,
                                   .open_public_slots = 1,
                                   .host_xuid = pending.sender_xuid};
  REQUIRE(cache.ObservePending(migrated, "idem-replacement"));
  const auto attempt =
      cache.PrepareExplicitAcceptance(pending.session_id, pending.sender_xuid);
  REQUIRE(attempt);
  CHECK(attempt->idempotency == "idem-first");
  CHECK(attempt->invitation.session_id == migrated.session_id);

  auto mismatched = migrated;
  mismatched.sender_xuid = 0xE000000000000199ULL;
  CHECK_FALSE(cache.PublishExplicitAcceptance(migrated.id, mismatched));
  cache.AbandonExplicitAcceptance(migrated.id);
  REQUIRE(cache.PrepareExplicitAcceptance(migrated.session_id, migrated.sender_xuid));
  REQUIRE(cache.PublishExplicitAcceptance(migrated.id, migrated));
  CHECK(cache.pending_invitation_count() == 0);
}

TEST_CASE("GTA IV episode presence maps the retail episode index",
          "[live][social][presence]") {
  CHECK(live_detail::Gta4EpisodePresenceName(0) == "base");
  CHECK(live_detail::Gta4EpisodePresenceName(1) == "tlad");
  CHECK(live_detail::Gta4EpisodePresenceName(2) == "tbogt");
  CHECK(live_detail::Gta4EpisodePresenceName(3) == "base");
}

TEST_CASE("mute cache uses only the local blocked direction",
          "[live][social][mute][cache]") {
  live_detail::SocialCacheState cache;
  constexpr uint64_t blocked = 0xE000000000000201ULL;
  constexpr uint64_t blocked_by = 0xE000000000000202ULL;
  constexpr uint64_t accepted_friend = 0xE000000000000203ULL;
  constexpr uint64_t not_friend = 0xE000000000000204ULL;

  CHECK(live_detail::IsLocallyBlockedRelationship("blocked"));
  CHECK_FALSE(live_detail::IsLocallyBlockedRelationship("blocked_by"));
  CHECK_FALSE(live_detail::IsLocallyBlockedRelationship("accepted"));
  CHECK_FALSE(live_detail::IsLocallyBlockedRelationship("removed"));

  CHECK(cache.QueryMute(blocked) == rex::system::xam::CachedMuteState::kMissing);
  const std::array relationships = {
      rex::system::xam::FriendRecord{.xuid = blocked, .blocked = true},
      rex::system::xam::FriendRecord{.xuid = blocked_by, .blocked = false},
      rex::system::xam::FriendRecord{.xuid = accepted_friend, .blocked = false},
  };
  cache.ReplaceRelationships(relationships);
  CHECK(cache.QueryMute(blocked) == rex::system::xam::CachedMuteState::kMuted);
  CHECK(cache.QueryMute(blocked_by) == rex::system::xam::CachedMuteState::kNotMuted);
  CHECK(cache.QueryMute(accepted_friend) == rex::system::xam::CachedMuteState::kNotMuted);
  CHECK(cache.QueryMute(not_friend) == rex::system::xam::CachedMuteState::kNotMuted);
}

TEST_CASE("XLive social ABI is exact and deterministic", "[live][social][abi]") {
  CHECK(sizeof(social_abi::XLiveFriendInfo) == 196);
  CHECK(social_abi::kFriendStateOffset == 24);
  CHECK(social_abi::kFriendTitleIdOffset == 36);
  CHECK(sizeof(social_abi::XLiveMuteQuery) == 20);
  CHECK(sizeof(social_abi::XLiveAcceptedInviteInfo) == 96);
  CHECK(offsetof(social_abi::XLiveAcceptedInviteInfo, session_info) == 20);
  CHECK(offsetof(social_abi::XLiveAcceptedInviteInfo, flags) == 80);

  social_abi::XLiveMuteQuery query{};
  rex::memory::store_and_swap<uint32_t>(
      query.bytes.data() + social_abi::kMuteQueryUserIndexOffset, 0);
  rex::memory::store_and_swap<uint64_t>(
      query.bytes.data() + social_abi::kMuteQueryXuidOffset,
      0xE000000000000301ULL);
  rex::be<uint32_t> output = 0xFFFFFFFF;
  social_abi::CompleteMuteQuery(query, output, X_ERROR_NOT_FOUND, true);
  CHECK(static_cast<uint32_t>(output) == 0);
  CHECK(rex::memory::load_and_swap<uint32_t>(
            query.bytes.data() + social_abi::kMuteQueryStatusOffset) ==
        X_ERROR_NOT_FOUND);
  CHECK(social_abi::MuteQueryUserIndex(query) == 0);
  CHECK(social_abi::MuteQueryXuid(query) == 0xE000000000000301ULL);

  social_abi::CompleteMuteQuery(query, output, X_ERROR_SUCCESS, true);
  CHECK(static_cast<uint32_t>(output) == 1);
  CHECK(rex::memory::load_and_swap<uint32_t>(
            query.bytes.data() + social_abi::kMuteQueryStatusOffset) ==
        X_ERROR_SUCCESS);

  social_abi::XLiveAcceptedInviteInfo invite;
  std::memset(&invite, 0xA5, sizeof(invite));
  std::memset(&invite, 0, sizeof(invite));
  CHECK(std::ranges::all_of(
      std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&invite), sizeof(invite)),
      [](uint8_t byte) { return byte == 0; }));
  invite.recipient_xuid = 0xE000000000000311ULL;
  invite.sender_xuid = 0xE000000000000312ULL;
  invite.title_id = 0x545407F2;
  invite.flags = 1;
  const auto* invite_bytes = reinterpret_cast<const uint8_t*>(&invite);
  CHECK(rex::memory::load_and_swap<uint64_t>(
            invite_bytes + offsetof(social_abi::XLiveAcceptedInviteInfo,
                                    recipient_xuid)) ==
        0xE000000000000311ULL);
  CHECK(rex::memory::load_and_swap<uint64_t>(
            invite_bytes + offsetof(social_abi::XLiveAcceptedInviteInfo,
                                    sender_xuid)) ==
        0xE000000000000312ULL);
  CHECK(rex::memory::load_and_swap<uint32_t>(
            invite_bytes + offsetof(social_abi::XLiveAcceptedInviteInfo,
                                    title_id)) == 0x545407F2);
  CHECK(rex::memory::load_and_swap<uint32_t>(
            invite_bytes + offsetof(social_abi::XLiveAcceptedInviteInfo,
                                    flags)) == 1);

  social_abi::XLiveFriendInfo friend_entry;
  std::memset(&friend_entry, 0xA5, sizeof(friend_entry));
  social_abi::EncodeFriendInfo(friend_entry, 0xE000000000000313ULL,
                               "LongPlayerNameThatMustBeBounded", true, true,
                               0xAE00000000000313ULL, 0x545407F2);
  CHECK(rex::memory::load_and_swap<uint64_t>(
            friend_entry.bytes.data() + social_abi::kFriendXuidOffset) ==
        0xE000000000000313ULL);
  const uint32_t friend_state = rex::memory::load_and_swap<uint32_t>(
      friend_entry.bytes.data() + social_abi::kFriendStateOffset);
  CHECK((friend_state & social_abi::kFriendStateOnline) != 0);
  CHECK((friend_state & social_abi::kFriendStateExcludedMask) == 0);
  CHECK(rex::memory::load_and_swap<uint64_t>(
            friend_entry.bytes.data() + social_abi::kFriendSessionIdOffset) ==
        0xAE00000000000313ULL);
  CHECK(rex::memory::load_and_swap<uint32_t>(
            friend_entry.bytes.data() + social_abi::kFriendTitleIdOffset) ==
        0x545407F2);
  CHECK(friend_entry.bytes[social_abi::kFriendGamertagOffset +
                           social_abi::kFriendGamertagMaximumBytes] == 0);
  CHECK(friend_entry.bytes[social_abi::kFriendRichPresenceOffset] == 0);
}

TEST_CASE("friend enumeration distinguishes empty success from request failure",
          "[live][social][friends][errors]") {
  auto services = MakeServices(0xE000000000000321ULL,
                               std::make_shared<InMemorySessionDirectory>());
  REQUIRE(services.social_service);
  const auto empty = services.social_service->EnumerateFriends(100);
  CHECK(empty.succeeded());
  CHECK(empty.friends.empty());
  const auto invalid = services.social_service->EnumerateFriends(0);
  CHECK_FALSE(invalid.succeeded());
  CHECK(invalid.status == rex::system::xam::SocialServiceStatus::kInvalidRequest);
  CHECK(invalid.friends.empty());
}

TEST_CASE("LAN social shutdown is idempotent and detaches notifications",
          "[live][social][shutdown]") {
  constexpr uint64_t local_xuid = 0xE000000000000401ULL;
  constexpr uint64_t session_id = 0xAE00000000000004ULL;
  auto directory = std::make_shared<InMemorySessionDirectory>();
  REQUIRE(directory->Create(SessionRecord{.title_id = 0x545407F2,
                                          .session_id = session_id,
                                          .max_public_slots = 1,
                                          .open_public_slots = 1,
                                          .host_xuid = local_xuid}));
  auto services = MakeServices(local_xuid, directory);
  uint32_t notifications = 0;
  services.social_service->SetInviteNotificationHandler([&] { ++notifications; });
  services.social_service->Shutdown();
  services.social_service->Shutdown();
  services.social_service->SetInviteNotificationHandler([&] { ++notifications; });
  const std::array recipients = {local_xuid};
  REQUIRE(services.social_service->SendInvitations(session_id, recipients,
                                                   std::span<const uint8_t>{}));
  CHECK(notifications == 0);
  CHECK(services.social_service->HasAcceptedInvitation());
}

TEST_CASE("LAN stat reads and leaderboards use intrinsic rating and preserve projections",
          "[live][stats]") {
  constexpr uint64_t local_xuid = 0xE000000000000123ULL;
  constexpr uint64_t other_xuid = 0xE000000000000456ULL;
  constexpr uint64_t session_id = 0xAE00000000000001ULL;
  constexpr uint32_t view_id = 0x00000100;
  constexpr uint32_t score_id = 0x00000200;
  constexpr uint32_t games_id = 0x00000300;
  auto services = MakeServices(local_xuid, std::make_shared<InMemorySessionDirectory>());
  REQUIRE(services.stats_service);

  StatView valid{
      .id = view_id,
      .rows = {{.rating = 100,
                .columns = {{.id = score_id,
                             .type = StatValueType::kInt64,
                             .value = int64_t{10}},
                            {.id = games_id,
                             .type = StatValueType::kInt32,
                             .value = int32_t{50}}}}}};
  StatView invalid{.id = 0, .rows = valid.rows};
  const std::array atomic_batch = {valid, invalid};
  CHECK_FALSE(services.stats_service->Write(session_id, local_xuid, atomic_batch));
  const std::array xuids = {local_xuid};
  const std::array views = {view_id};
  const std::array columns = {score_id};
  const auto after_rejected_write = services.stats_service->Read(xuids, views, columns);
  REQUIRE(after_rejected_write.size() == 1);
  REQUIRE(after_rejected_write.front().rows.size() == 1);
  CHECK(after_rejected_write.front().rows.front().xuid == local_xuid);
  CHECK(after_rejected_write.front().rows.front().columns.empty());

  const std::array local_write = {valid};
  REQUIRE(services.stats_service->Write(session_id, local_xuid, local_write));
  valid.rows.front().rating = 200;
  valid.rows.front().columns.front().value = int64_t{25};
  valid.rows.front().columns.back().value = int32_t{1};
  const std::array remote_write = {valid};
  REQUIRE(services.stats_service->Write(session_id, other_xuid, remote_write));
  const std::array projected_columns = {games_id, score_id};
  const auto leaderboard = services.stats_service->Leaderboard(
      view_id, projected_columns, 0, 10, false);
  REQUIRE(leaderboard.succeeded());
  REQUIRE(leaderboard.page.rows.size() == 2);
  CHECK(leaderboard.page.total == 2);
  CHECK(leaderboard.page.rows[0].xuid == other_xuid);
  CHECK(leaderboard.page.rows[0].rank == 1);
  CHECK(leaderboard.page.rows[0].rating == 200);
  REQUIRE(leaderboard.page.rows[0].columns.size() == 2);
  CHECK(leaderboard.page.rows[0].columns[0].id == games_id);
  CHECK(leaderboard.page.rows[0].columns[1].id == score_id);
  CHECK(leaderboard.page.rows[1].xuid == local_xuid);
  CHECK(leaderboard.page.rows[1].rank == 2);
  CHECK(leaderboard.page.rows[1].rating == 100);

  const auto rating_only = services.stats_service->Leaderboard(
      view_id, std::span<const uint32_t>{}, 0, 10, false);
  REQUIRE(rating_only.succeeded());
  REQUIRE(rating_only.page.rows.size() == 2);
  CHECK(rating_only.page.rows[0].columns.empty());

  const std::array invalid_projection = {uint32_t{0}};
  const auto invalid_leaderboard = services.stats_service->Leaderboard(
      view_id, invalid_projection, 0, 10, false);
  CHECK_FALSE(invalid_leaderboard.succeeded());
  CHECK(invalid_leaderboard.status == rex::system::xam::StatsServiceStatus::kInvalidRequest);
  CHECK(invalid_leaderboard.page.rows.empty());

  const auto ranked_direct_read =
      services.stats_service->Read(xuids, views, projected_columns);
  REQUIRE(ranked_direct_read.size() == 1);
  REQUIRE(ranked_direct_read.front().rows.size() == 1);
  const auto& direct_row = ranked_direct_read.front().rows.front();
  CHECK(direct_row.rank == 2);
  CHECK(direct_row.rating == 100);
  REQUIRE(direct_row.columns.size() == 2);
  CHECK(direct_row.columns[0].id == games_id);
  CHECK(direct_row.columns[1].id == score_id);

  valid.rows.front().rating = 100;
  valid.rows.front().columns.front().type = StatValueType::kUnicode;
  const std::u16string unicode_value = u"Player \U0001F3AE \u6F22\u5B57";
  valid.rows.front().columns.front().value = unicode_value;
  const std::array text_write = {valid};
  REQUIRE(services.stats_service->Write(session_id, local_xuid, text_write));
  const auto unicode_read = services.stats_service->Read(xuids, views, columns);
  REQUIRE(unicode_read.size() == 1);
  REQUIRE(unicode_read.front().rows.size() == 1);
  REQUIRE(unicode_read.front().rows.front().columns.size() == 1);
  CHECK(std::get<std::u16string>(unicode_read.front().rows.front().columns.front().value) ==
        unicode_value);
  const std::array unicode_projection = {score_id};
  const auto unicode_leaderboard = services.stats_service->Leaderboard(
      view_id, unicode_projection, 0, 10, false);
  REQUIRE(unicode_leaderboard.succeeded());
  REQUIRE(unicode_leaderboard.page.rows.size() == 2);
}

TEST_CASE("LAN voice routing honors all, private target, and mute policy", "[live][voice]") {
  constexpr uint64_t local_xuid = 0xE000000000000123ULL;
  constexpr uint64_t session_id = 0xAE00000000000001ULL;
  auto services = MakeServices(local_xuid, std::make_shared<InMemorySessionDirectory>());
  REQUIRE(services.voice_transport);
  const std::array<uint8_t, 3> payload = {4, 5, 6};

  REQUIRE(services.voice_transport->Configure(
      {.session_id = session_id, .channel = VoiceChannel::kAll}));
  REQUIRE(services.voice_transport->Send(7, payload));
  auto packet = services.voice_transport->Receive(4096);
  REQUIRE(packet);
  CHECK(packet->source_xuid == local_xuid);
  CHECK(packet->session_id == session_id);
  CHECK(packet->sequence == 7);
  CHECK(packet->payload == std::vector<uint8_t>(payload.begin(), payload.end()));

  REQUIRE(services.voice_transport->Configure({.session_id = session_id,
                                               .channel = VoiceChannel::kPrivate,
                                               .muted_xuids = {local_xuid}}));
  REQUIRE(services.voice_transport->Send(8, payload));
  CHECK_FALSE(services.voice_transport->Receive(4096));

  REQUIRE(services.voice_transport->Configure({.session_id = session_id,
                                               .channel = VoiceChannel::kPrivate,
                                               .target_xuids = {local_xuid}}));
  REQUIRE(services.voice_transport->Send(9, payload));
  CHECK(services.voice_transport->Receive(4096));
}

}  // namespace
