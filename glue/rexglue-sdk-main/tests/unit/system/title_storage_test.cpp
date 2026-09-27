#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ranges>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "kernel/xam/apps/xlivebase_title_storage_abi.h"

namespace {

using rex::X_HRESULT;
using rex::X_RESULT;
namespace storage = rex::kernel::xam::apps::detail;
namespace live = rex::system::xam;

class FakeAchievementStorageService final
    : public live::IGta4AchievementStorageService {
 public:
  bool ready() const override { return true; }
  std::string last_error() const override { return {}; }

  live::Gta4AchievementStorageFetchResult FetchGta4AchievementStorage() override {
    ++fetch_count;
    return fetch_result;
  }

  bool StoreGta4AchievementStorage(std::span<const uint8_t> blob) override {
    ++store_count;
    stored.assign(blob.begin(), blob.end());
    return store_success;
  }

  live::Gta4AchievementStorageFetchResult fetch_result;
  bool store_success = true;
  uint32_t fetch_count = 0;
  uint32_t store_count = 0;
  std::vector<uint8_t> stored;
};

TEST_CASE("Prog_ACH title storage ABI matches the generated callers",
          "[live][storage][abi]") {
  CHECK(sizeof(storage::Gta4TitleStorageBuildRequest) == 40);
  CHECK(sizeof(storage::Gta4TitleStorageRuntimeDescriptor) == 40);
  CHECK(sizeof(storage::Gta4TitleStorageResult) == 20);
  CHECK(storage::kGta4TitleStorageRuntimeRequestBytes == 1088);
  CHECK(storage::kGta4TitleStorageDynamicArgumentsBytes == 516);
  CHECK(storage::kGta4AchievementStorageValueCount == 151);
  CHECK(storage::kGta4AchievementStorageBytes == 604);
  CHECK(storage::kGta4AchievementStorageResultBytes == 20);
  CHECK(storage::kGta4AchievementStoragePath == u"Prog_ACH");
  CHECK(storage::kGta4AchievementStoragePathCodeUnitsWithNul == 9);
  CHECK(storage::kGta4AchievementStorageBytes ==
        live::kGta4AchievementStorageBlobBytes);
  CHECK(live::kGta4AchievementStorageEndpoint ==
        "/api/v3/storage/0x545407F2/3/Prog_ACH");
  CHECK(offsetof(storage::Gta4TitleStorageBuildRequest, owner_xuid) == 8);
  CHECK(offsetof(storage::Gta4TitleStorageBuildRequest, facility) == 16);
  CHECK(offsetof(storage::Gta4TitleStorageBuildRequest, item_path_ptr) == 28);
  CHECK(offsetof(storage::Gta4TitleStorageBuildRequest, server_path_ptr) == 32);
  CHECK(offsetof(storage::Gta4TitleStorageBuildRequest,
                 server_path_capacity_ptr) == 36);

  std::array<uint8_t, storage::kGta4TitleStorageDynamicArgumentsBytes> metadata{};
  constexpr std::array<size_t, 6> entry_offsets = {0, 16, 32, 48, 64, 80};
  constexpr std::array<uint32_t, 6> pointers = {
      0x10000100, 0x10000200, 0x10000300,
      0x10000400, 0x10000500, 0x10000600};
  for (size_t index = 0; index < entry_offsets.size(); ++index) {
    rex::memory::store_and_swap<uint32_t>(metadata.data() + entry_offsets[index], 4);
    rex::memory::store_and_swap<uint64_t>(metadata.data() + entry_offsets[index] + 8,
                                          pointers[index]);
  }
  rex::memory::store_and_swap<uint32_t>(
      metadata.data() + storage::kGta4TitleStorageDynamicArgumentCountOffset, 6);
  const auto parsed = storage::ReadTitleStorageDynamicArguments(metadata.data());
  REQUIRE(parsed);
  CHECK(*parsed == pointers);

  std::array<uint8_t, storage::kGta4TitleStorageRuntimeRequestBytes> request{};
  rex::memory::store_and_swap<uint32_t>(
      request.data() + storage::kGta4TitleStorageRuntimeResultPointerOffset,
      0x10000700);
  rex::memory::store_and_swap<uint32_t>(
      request.data() + storage::kGta4TitleStorageRuntimeResultSizeOffset, 20);
  CHECK(storage::ClassifyTitleStorageRuntimeRequest(request.data()) ==
        storage::Gta4TitleStorageOperation::kDownload);
  rex::memory::store_and_swap<uint32_t>(
      request.data() + storage::kGta4TitleStorageRuntimeResultPointerOffset, 0);
  rex::memory::store_and_swap<uint32_t>(
      request.data() + storage::kGta4TitleStorageRuntimeResultSizeOffset, 0);
  CHECK(storage::ClassifyTitleStorageRuntimeRequest(request.data()) ==
        storage::Gta4TitleStorageOperation::kUpload);
  CHECK(storage::IsRuntimeSchemaMessage(0x00050001));
  CHECK_FALSE(storage::IsRuntimeSchemaMessage(0x00040001));
}

TEST_CASE("Prog_ACH output writers preserve surrounding canaries",
          "[live][storage][abi][canary]") {
  std::array<rex::be<char16_t>, 11> path{};
  path.front() = 0x1357;
  path.back() = 0x2468;
  storage::WriteGta4AchievementStoragePath(path.data() + 1);
  CHECK(path.front() == 0x1357);
  CHECK(path.back() == 0x2468);
  for (size_t index = 0; index < storage::kGta4AchievementStoragePath.size(); ++index) {
    CHECK(path[index + 1] == storage::kGta4AchievementStoragePath[index]);
  }
  CHECK(path[9] == 0);

  struct GuardedResult {
    std::array<uint8_t, 4> before;
    storage::Gta4TitleStorageResult result;
    std::array<uint8_t, 4> after;
  } guarded;
  static_assert(sizeof(GuardedResult) == 28);
  std::memset(&guarded, 0xA5, sizeof(guarded));
  storage::CompleteTitleStorageResult(guarded.result, 0xE00000000000A501ULL);
  CHECK(std::ranges::all_of(guarded.before,
                            [](uint8_t byte) { return byte == 0xA5; }));
  CHECK(std::ranges::all_of(guarded.after,
                            [](uint8_t byte) { return byte == 0xA5; }));
  CHECK(rex::memory::load_and_swap<uint32_t>(guarded.result.bytes.data()) == 604);
  CHECK(rex::memory::load_and_swap<uint64_t>(guarded.result.bytes.data() + 4) ==
        0xE00000000000A501ULL);
  CHECK(rex::memory::load_and_swap<uint64_t>(guarded.result.bytes.data() + 12) == 0);
}

TEST_CASE("Prog_ACH fixed storage handles sync pending miss size and identity",
          "[live][storage][runtime]") {
  constexpr uint64_t local_xuid = 0xE00000000000B501ULL;
  FakeAchievementStorageService service;
  std::array<uint8_t, 604> destination{};
  destination.fill(0xCC);
  storage::Gta4TitleStorageResult result;
  std::memset(&result, 0xA5, sizeof(result));

  service.fetch_result = {
      .status = live::Gta4AchievementStorageFetchStatus::kNotFound};
  CHECK(storage::DownloadGta4AchievementStorage(service, local_xuid,
                                                 destination, result) ==
        storage::kGta4TitleStorageMissing);
  CHECK(std::ranges::all_of(result.bytes,
                            [](uint8_t byte) { return byte == 0; }));
  CHECK(std::ranges::all_of(destination,
                            [](uint8_t byte) { return byte == 0xCC; }));

  live::Gta4AchievementStorageRecord record{.xuid = local_xuid};
  record.blob.fill(0x5A);
  service.fetch_result = {
      .status = live::Gta4AchievementStorageFetchStatus::kFound,
      .record = record};
  CHECK(storage::DownloadGta4AchievementStorage(service, local_xuid,
                                                 destination, result) ==
        X_E_SUCCESS);
  CHECK(destination == record.blob);
  CHECK(rex::memory::load_and_swap<uint32_t>(result.bytes.data()) == 604);
  CHECK(rex::memory::load_and_swap<uint64_t>(result.bytes.data() + 4) ==
        local_xuid);

  destination.fill(0xCC);
  record.xuid = 0xE00000000000B502ULL;
  service.fetch_result.record = record;
  CHECK(storage::DownloadGta4AchievementStorage(service, local_xuid,
                                                 destination, result) ==
        X_E_FAIL);
  CHECK(std::ranges::all_of(result.bytes,
                            [](uint8_t byte) { return byte == 0; }));
  CHECK(std::ranges::all_of(destination,
                            [](uint8_t byte) { return byte == 0xCC; }));

  std::array<uint8_t, 603> short_blob{};
  std::array<uint8_t, 604> exact_blob{};
  std::array<uint8_t, 605> long_blob{};
  exact_blob.fill(0x3C);
  CHECK(storage::UploadGta4AchievementStorage(service, short_blob) ==
        X_E_INVALIDARG);
  CHECK(storage::UploadGta4AchievementStorage(service, long_blob) ==
        X_E_INVALIDARG);
  CHECK(service.store_count == 0);
  CHECK(storage::UploadGta4AchievementStorage(service, exact_blob) ==
        X_E_SUCCESS);
  CHECK(service.store_count == 1);
  CHECK(service.stored == std::vector<uint8_t>(exact_blob.begin(), exact_blob.end()));
  CHECK(storage::Gta4TitleStorageAsyncStartResult() == 997);
}

}  // namespace
