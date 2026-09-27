#include <array>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/memory.h>
#include <rex/system/xam/user_profile.h>

namespace rex::system::xam {
namespace {

std::vector<uint8_t> MakeBlob(std::initializer_list<std::array<uint8_t, 8>> entries) {
  std::vector<uint8_t> blob;
  for (const auto& entry : entries) {
    blob.insert(blob.end(), entry.begin(), entry.end());
  }
  return blob;
}

}  // namespace

TEST_CASE("GTA IV title profile validates opaque big-endian key/value records",
          "[xam][profile]") {
  CHECK(UserProfile::ValidateGta4TitleProfileBlob({}));

  const auto opaque = MakeBlob({
      {0x00, 0x00, 0x00, 0x01, 0xDE, 0xAD, 0xBE, 0xEF},
      {0x80, 0x00, 0x00, 0x02, 0x12, 0x34, 0x56, 0x78},
  });
  CHECK(UserProfile::ValidateGta4TitleProfileBlob(opaque));

  const auto duplicate = MakeBlob({
      {0x00, 0x00, 0x00, 0x01, 0xDE, 0xAD, 0xBE, 0xEF},
      {0x00, 0x00, 0x00, 0x01, 0x12, 0x34, 0x56, 0x78},
  });
  CHECK_FALSE(UserProfile::ValidateGta4TitleProfileBlob(duplicate));

  std::vector<uint8_t> misaligned(UserProfile::kGta4TitleProfileEntrySize - 1);
  CHECK_FALSE(UserProfile::ValidateGta4TitleProfileBlob(misaligned));

  std::vector<uint8_t> oversized(UserProfile::kGta4TitleProfileMaximumSize +
                                 UserProfile::kGta4TitleProfileEntrySize);
  CHECK_FALSE(UserProfile::ValidateGta4TitleProfileBlob(oversized));

  std::vector<uint8_t> maximum(UserProfile::kGta4TitleProfileMaximumSize);
  const size_t entry_count = maximum.size() / UserProfile::kGta4TitleProfileEntrySize;
  for (size_t index = 0; index < entry_count; ++index) {
    memory::store_and_swap<uint32_t>(
        maximum.data() + index * UserProfile::kGta4TitleProfileEntrySize,
        static_cast<uint32_t>(index));
  }
  CHECK(UserProfile::ValidateGta4TitleProfileBlob(maximum));
}

TEST_CASE("GTA IV guest title-profile writes preserve bytes and generation ordering",
          "[xam][profile]") {
  UserProfile profile;
  const auto local = MakeBlob({
      {0x00, 0x00, 0x00, 0x01, 0xFE, 0xDC, 0xBA, 0x98},
      {0x00, 0x00, 0x00, 0x02, 0x76, 0x54, 0x32, 0x10},
  });
  const auto remote = MakeBlob({
      {0x00, 0x00, 0x00, 0x03, 0xAA, 0xBB, 0xCC, 0xDD},
  });

  std::vector<uint8_t> callback_blob;
  uint64_t callback_generation = 0;
  profile.SetGta4TitleProfileWriteCallback(
      [&](std::vector<uint8_t> blob, uint64_t generation) {
        callback_blob = std::move(blob);
        callback_generation = generation;
      });

  REQUIRE(profile.WriteGuestBinarySetting(UserProfile::kGta4TitleId,
                                          UserProfile::kGta4TitleProfileSettingId, local));
  CHECK(callback_blob == local);
  CHECK(callback_generation == 1);
  CHECK(profile.gta4_title_profile_generation() == 1);
  REQUIRE(profile.SnapshotGta4TitleProfileBlob());
  CHECK(*profile.SnapshotGta4TitleProfileBlob() == local);

  CHECK_FALSE(profile.ImportGta4TitleProfileBlobIfGeneration(remote, 0));
  CHECK(*profile.SnapshotGta4TitleProfileBlob() == local);
  CHECK(profile.ImportGta4TitleProfileBlobIfGeneration(remote, callback_generation));
  CHECK(*profile.SnapshotGta4TitleProfileBlob() == remote);
  CHECK(profile.gta4_title_profile_generation() == callback_generation);
}

}  // namespace rex::system::xam
