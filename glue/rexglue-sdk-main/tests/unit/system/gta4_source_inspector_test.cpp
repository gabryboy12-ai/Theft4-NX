#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/memory/utils.h>
#include <rex/system/util/xex2_info.h>

#include "install/gta4_source_inspector.h"

namespace {

using gta4::install::ClassifyGameSourceMetadata;
using gta4::install::GameSourceInspection;
using gta4::install::GameSourceInspectionWorker;
using gta4::install::GameSourceKind;
using gta4::install::GameSourceMetadata;
using gta4::install::GameSourceStatus;

constexpr std::array<uint8_t, 20> kRetailSignatureDigest = {
    0x19, 0x2B, 0x3F, 0x56, 0x7C, 0x59, 0x36, 0x0C, 0x6C, 0xE2,
    0x11, 0x82, 0x0D, 0x77, 0x6F, 0x6B, 0x25, 0x25, 0x1A, 0x89,
};

GameSourceMetadata RetailMetadata() {
  GameSourceMetadata metadata;
  metadata.module_flags = rex::XEX_MODULE_TITLE;
  metadata.title_id = 0x545407F2;
  metadata.media_id = 0x6AC07221;
  metadata.xex_version = 0x00000005;
  metadata.base_version = 0x00000005;
  metadata.region = rex::XEX_REGION_NTSCU;
  metadata.disc_number = 1;
  metadata.disc_count = 1;
  metadata.rsa_signature_sha1 = kRetailSignatureDigest;
  return metadata;
}

void StoreBe32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
  REQUIRE(offset <= bytes.size());
  REQUIRE(sizeof(value) <= bytes.size() - offset);
  rex::memory::store_and_swap<uint32_t>(bytes.data() + offset, value);
}

struct SyntheticXex {
  std::vector<uint8_t> bytes;
  size_t optional_offset = 0;
  size_t security_offset = 0;
  size_t execution_offset = 0;
};

SyntheticXex MakeSyntheticXex() {
  constexpr size_t kHeaderPrefixSize = offsetof(rex::xex2_header, headers);
  constexpr size_t kOptionalHeaderSize = sizeof(rex::xex2_opt_header);
  constexpr size_t kSecurityPrefixSize = offsetof(rex::xex2_security_info, page_descriptors);
  constexpr size_t kExecutionSize = sizeof(rex::xex2_opt_execution_info);

  SyntheticXex xex;
  xex.optional_offset = kHeaderPrefixSize;
  xex.security_offset = kHeaderPrefixSize + kOptionalHeaderSize;
  xex.execution_offset = xex.security_offset + kSecurityPrefixSize;
  xex.bytes.resize(xex.execution_offset + kExecutionSize);

  std::memcpy(xex.bytes.data(), "XEX2", 4);
  StoreBe32(xex.bytes, offsetof(rex::xex2_header, module_flags), rex::XEX_MODULE_TITLE);
  StoreBe32(xex.bytes, offsetof(rex::xex2_header, header_size),
            static_cast<uint32_t>(xex.bytes.size()));
  StoreBe32(xex.bytes, offsetof(rex::xex2_header, security_offset),
            static_cast<uint32_t>(xex.security_offset));
  StoreBe32(xex.bytes, offsetof(rex::xex2_header, header_count), 1);

  StoreBe32(xex.bytes, xex.optional_offset + offsetof(rex::xex2_opt_header, key),
            rex::XEX_HEADER_EXECUTION_INFO);
  StoreBe32(xex.bytes, xex.optional_offset + offsetof(rex::xex2_opt_header, offset),
            static_cast<uint32_t>(xex.execution_offset));

  StoreBe32(xex.bytes, xex.security_offset + offsetof(rex::xex2_security_info, header_size),
            static_cast<uint32_t>(kSecurityPrefixSize));
  StoreBe32(xex.bytes, xex.security_offset + offsetof(rex::xex2_security_info, region),
            rex::XEX_REGION_NTSCU);
  StoreBe32(xex.bytes,
            xex.security_offset + offsetof(rex::xex2_security_info, page_descriptor_count), 0);

  StoreBe32(xex.bytes, xex.execution_offset + offsetof(rex::xex2_opt_execution_info, media_id),
            0x6AC07221);
  StoreBe32(xex.bytes, xex.execution_offset + offsetof(rex::xex2_opt_execution_info, version_value),
            0x00000005);
  StoreBe32(xex.bytes,
            xex.execution_offset + offsetof(rex::xex2_opt_execution_info, base_version_value),
            0x00000005);
  StoreBe32(xex.bytes, xex.execution_offset + offsetof(rex::xex2_opt_execution_info, title_id),
            0x545407F2);
  xex.bytes[xex.execution_offset + offsetof(rex::xex2_opt_execution_info, disc_number)] = 1;
  xex.bytes[xex.execution_offset + offsetof(rex::xex2_opt_execution_info, disc_count)] = 1;
  return xex;
}

class TempDirectory {
 public:
  explicit TempDirectory(std::string_view tag) {
    static std::atomic<uint64_t> next_id{0};
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            (std::string(tag) + "_" + std::to_string(timestamp) + "_" +
             std::to_string(next_id.fetch_add(1, std::memory_order_relaxed)));
    std::filesystem::create_directories(path_);
  }

  ~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

void WriteBytes(const std::filesystem::path& path, std::span<const uint8_t> bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  REQUIRE(output);
  output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  REQUIRE(output.good());
}

GameSourceInspection SupportedResult(uint32_t title_id) {
  GameSourceInspection result;
  result.status = GameSourceStatus::kSupported;
  result.title_id = title_id;
  result.display_name = "Grand Theft Auto IV";
  result.release_label = "Retail 1.00";
  return result;
}

template <typename Predicate>
bool WaitUntil(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

}  // namespace

TEST_CASE("GTA IV source classifier accepts only canonical USA retail 1.00 identity",
          "[gta4][installer][source-inspection]") {
  const GameSourceInspection result = ClassifyGameSourceMetadata(RetailMetadata());
  REQUIRE(result.supported());
  CHECK(result.display_name == "Grand Theft Auto IV");
  CHECK(result.release_label == "Retail 1.00");
  CHECK(result.title_id == 0x545407F2);
  CHECK(result.media_id == 0x6AC07221);
  CHECK(result.xex_version == 0x00000005);
  CHECK(result.base_version == 0x00000005);
  CHECK(result.region == rex::XEX_REGION_NTSCU);
}

TEST_CASE("GTA IV source classifier rejects mismatched identity fields",
          "[gta4][installer][source-inspection]") {
  SECTION("title") {
    auto metadata = RetailMetadata();
    metadata.title_id = 0x545407F3;
    CHECK(ClassifyGameSourceMetadata(metadata).status == GameSourceStatus::kWrongGame);
  }
  SECTION("media ID") {
    auto metadata = RetailMetadata();
    metadata.media_id = 0x6AC07220;
    CHECK(ClassifyGameSourceMetadata(metadata).status == GameSourceStatus::kWrongMediaId);
  }
  SECTION("XEX version") {
    auto metadata = RetailMetadata();
    metadata.xex_version = 0x00000006;
    CHECK(ClassifyGameSourceMetadata(metadata).status == GameSourceStatus::kWrongRevision);
  }
  SECTION("already-patched v8 executable") {
    auto metadata = RetailMetadata();
    metadata.xex_version = 0x00000805;
    metadata.base_version = 0x00000805;
    CHECK(ClassifyGameSourceMetadata(metadata).status == GameSourceStatus::kWrongRevision);
  }
  SECTION("base version") {
    auto metadata = RetailMetadata();
    metadata.base_version = 0x00000004;
    CHECK(ClassifyGameSourceMetadata(metadata).status == GameSourceStatus::kWrongRevision);
  }
  SECTION("source signature digest") {
    auto metadata = RetailMetadata();
    metadata.rsa_signature_sha1.front() ^= 0xFF;
    CHECK(ClassifyGameSourceMetadata(metadata).status == GameSourceStatus::kWrongSignature);
  }
  SECTION("title-module flags") {
    auto metadata = RetailMetadata();
    metadata.module_flags = rex::XEX_MODULE_MODULE_PATCH | rex::XEX_MODULE_PATCH_DELTA;
    CHECK(ClassifyGameSourceMetadata(metadata).status == GameSourceStatus::kCorruptImage);
  }
  SECTION("extra module flags") {
    auto metadata = RetailMetadata();
    metadata.module_flags |= rex::XEX_MODULE_DLL_MODULE;
    CHECK(ClassifyGameSourceMetadata(metadata).status == GameSourceStatus::kCorruptImage);
  }
}

TEST_CASE("GTA IV source classifier rejects non-exact region flags",
          "[gta4][installer][source-inspection]") {
  SECTION("PAL") {
    auto metadata = RetailMetadata();
    metadata.region = rex::XEX_REGION_PAL;
    CHECK(ClassifyGameSourceMetadata(metadata).status == GameSourceStatus::kWrongRegion);
  }
  SECTION("region free") {
    auto metadata = RetailMetadata();
    metadata.region = rex::XEX_REGION_ALL;
    const auto result = ClassifyGameSourceMetadata(metadata);
    CHECK(result.status == GameSourceStatus::kWrongRegion);
    CHECK(result.rejection_reason.find("Region-free") != std::string::npos);
  }
  SECTION("multi-region") {
    auto metadata = RetailMetadata();
    metadata.region = rex::XEX_REGION_NTSCU | rex::XEX_REGION_PAL;
    const auto result = ClassifyGameSourceMetadata(metadata);
    CHECK(result.status == GameSourceStatus::kWrongRegion);
    CHECK(result.rejection_reason.find("multi-region") != std::string::npos);
  }
}

TEST_CASE("GTA IV bounded XEX parser reports identity and rejects malformed headers",
          "[gta4][installer][source-inspection]") {
  SECTION("valid structure reaches signature classification") {
    const SyntheticXex xex = MakeSyntheticXex();
    const auto result = gta4::install::InspectGameXex(xex.bytes);
    CHECK(result.status == GameSourceStatus::kWrongSignature);
    CHECK(result.title_id == 0x545407F2);
    CHECK(result.media_id == 0x6AC07221);
    CHECK(result.xex_version == 0x00000005);
    CHECK(result.base_version == 0x00000005);
    CHECK(result.disc_number == 1);
    CHECK(result.disc_count == 1);
  }
  SECTION("bad magic") {
    auto xex = MakeSyntheticXex();
    xex.bytes.front() = 0;
    CHECK(gta4::install::InspectGameXex(xex.bytes).status == GameSourceStatus::kCorruptImage);
  }
  SECTION("truncated security data") {
    constexpr size_t kSecurityPrefixSize = offsetof(rex::xex2_security_info, page_descriptors);
    auto xex = MakeSyntheticXex();
    xex.bytes.resize(xex.security_offset + kSecurityPrefixSize - 1);
    StoreBe32(xex.bytes, offsetof(rex::xex2_header, header_size),
              static_cast<uint32_t>(xex.bytes.size()));
    const auto result = gta4::install::InspectGameXex(xex.bytes);
    CHECK(result.status == GameSourceStatus::kCorruptImage);
    CHECK(result.rejection_reason.find("security") != std::string::npos);
  }
  SECTION("excessive optional-header count") {
    auto xex = MakeSyntheticXex();
    StoreBe32(xex.bytes, offsetof(rex::xex2_header, header_count),
              std::numeric_limits<uint32_t>::max());
    const auto result = gta4::install::InspectGameXex(xex.bytes);
    CHECK(result.status == GameSourceStatus::kCorruptImage);
    CHECK(result.rejection_reason.find("optional-header") != std::string::npos);
  }
  SECTION("invalid execution-info offset") {
    auto xex = MakeSyntheticXex();
    StoreBe32(xex.bytes, xex.optional_offset + offsetof(rex::xex2_opt_header, offset),
              static_cast<uint32_t>(xex.bytes.size()));
    const auto result = gta4::install::InspectGameXex(xex.bytes);
    CHECK(result.status == GameSourceStatus::kCorruptImage);
    CHECK(result.rejection_reason.find("execution-information") != std::string::npos);
  }
  SECTION("invalid security offset") {
    auto xex = MakeSyntheticXex();
    StoreBe32(xex.bytes, offsetof(rex::xex2_header, security_offset),
              static_cast<uint32_t>(xex.bytes.size()));
    const auto result = gta4::install::InspectGameXex(xex.bytes);
    CHECK(result.status == GameSourceStatus::kCorruptImage);
    CHECK(result.rejection_reason.find("security") != std::string::npos);
  }
}

TEST_CASE("GTA IV source inspection distinguishes missing and ambiguous default.xex layouts",
          "[gta4][installer][source-inspection]") {
  SECTION("missing") {
    TempDirectory temp("gta4_source_inspector_missing");
    const auto result = gta4::install::InspectGameSource(temp.path());
    CHECK(result.status == GameSourceStatus::kMissingDefaultXex);
  }
  SECTION("ambiguous") {
    TempDirectory temp("gta4_source_inspector_ambiguous");
    const std::array<uint8_t, 1> byte{0};
    WriteBytes(temp.path() / "disc-one" / "default.xex", byte);
    WriteBytes(temp.path() / "disc-two" / "DEFAULT.XEX", byte);
    const auto result = gta4::install::InspectGameSource(temp.path());
    CHECK(result.status == GameSourceStatus::kAmbiguousDefaultXex);
  }
}

TEST_CASE("GTA IV source inspection mounts folders read-only and rejects corrupt disc images",
          "[gta4][installer][source-inspection]") {
  SECTION("single extracted default.xex") {
    TempDirectory temp("gta4_source_inspector_extracted");
    const SyntheticXex xex = MakeSyntheticXex();
    WriteBytes(temp.path() / "default.xex", xex.bytes);
    const auto result = gta4::install::InspectGameSource(temp.path());
    CHECK(result.source_kind == GameSourceKind::kExtractedFolder);
    CHECK(result.status == GameSourceStatus::kWrongSignature);
  }
  SECTION("corrupt ISO") {
    TempDirectory temp("gta4_source_inspector_corrupt_iso");
    const std::array<uint8_t, 4> bytes{'n', 'o', 'p', 'e'};
    const auto path = temp.path() / "corrupt.iso";
    WriteBytes(path, bytes);
    const auto result = gta4::install::InspectGameSource(path);
    CHECK(result.source_kind == GameSourceKind::kDiscImage);
    CHECK(result.status == GameSourceStatus::kCorruptImage);
  }
}

TEST_CASE("GTA IV source version and region display formatting is diagnostic",
          "[gta4][installer][source-inspection]") {
  CHECK(gta4::install::FormatXexVersion(0x00000005) == "0.0.0.5");
  CHECK(gta4::install::FormatXexVersion(0x00000805) == "0.0.8.5");
  CHECK(gta4::install::FormatXexRegion(rex::XEX_REGION_NTSCU) == "USA");
  CHECK(gta4::install::FormatXexRegion(rex::XEX_REGION_ALL) == "Region Free");
  CHECK(gta4::install::FormatXexRegion(rex::XEX_REGION_NTSCU | rex::XEX_REGION_PAL) == "USA + PAL");

  const auto supported = ClassifyGameSourceMetadata(RetailMetadata());
  CHECK(gta4::install::FormatGameSourceInspection(supported) ==
        "Detected: Grand Theft Auto IV — USA — Retail 1.00 — Supported");
}

TEST_CASE("GTA IV inspection worker suppresses stale rapid reselection results",
          "[gta4][installer][source-inspection][worker]") {
  std::mutex mutex;
  std::condition_variable condition;
  bool first_started = false;
  bool release_first = false;

  GameSourceInspectionWorker worker([&](const std::filesystem::path& path) -> GameSourceInspection {
    if (path == "first") {
      std::unique_lock lock(mutex);
      first_started = true;
      condition.notify_all();
      condition.wait(lock, [&] { return release_first; });
      return SupportedResult(1);
    }
    return SupportedResult(2);
  });

  worker.Request("first");
  {
    std::unique_lock lock(mutex);
    REQUIRE(condition.wait_for(lock, std::chrono::seconds(2), [&] { return first_started; }));
  }
  const uint64_t latest_generation = worker.Request("second");
  {
    std::lock_guard lock(mutex);
    release_first = true;
  }
  condition.notify_all();

  REQUIRE(WaitUntil([&] {
    const auto snapshot = worker.Snapshot();
    return snapshot.generation == latest_generation && !snapshot.checking && snapshot.result;
  }));
  const auto snapshot = worker.Snapshot();
  REQUIRE(snapshot.result);
  CHECK(snapshot.result->title_id == 2);
}

TEST_CASE("GTA IV inspection worker clear invalidates an in-flight result",
          "[gta4][installer][source-inspection][worker]") {
  std::mutex mutex;
  std::condition_variable condition;
  bool started = false;
  bool release = false;

  GameSourceInspectionWorker worker([&](const std::filesystem::path&) -> GameSourceInspection {
    std::unique_lock lock(mutex);
    started = true;
    condition.notify_all();
    condition.wait(lock, [&] { return release; });
    return SupportedResult(1);
  });
  worker.Request("source");
  {
    std::unique_lock lock(mutex);
    REQUIRE(condition.wait_for(lock, std::chrono::seconds(2), [&] { return started; }));
  }
  const uint64_t clear_generation = worker.Clear();
  {
    const auto snapshot = worker.Snapshot();
    CHECK(snapshot.generation == clear_generation);
    CHECK_FALSE(snapshot.checking);
    CHECK_FALSE(snapshot.result);
  }
  {
    std::lock_guard lock(mutex);
    release = true;
  }
  condition.notify_all();
  worker.Stop();
  const auto stopped = worker.Snapshot();
  CHECK_FALSE(stopped.checking);
  CHECK_FALSE(stopped.result);
}

TEST_CASE("GTA IV inspection worker stop joins an active inspection safely",
          "[gta4][installer][source-inspection][worker]") {
  std::mutex mutex;
  std::condition_variable condition;
  bool started = false;
  bool release = false;

  GameSourceInspectionWorker worker([&](const std::filesystem::path&) -> GameSourceInspection {
    std::unique_lock lock(mutex);
    started = true;
    condition.notify_all();
    condition.wait(lock, [&] { return release; });
    return SupportedResult(1);
  });
  worker.Request("source");
  {
    std::unique_lock lock(mutex);
    REQUIRE(condition.wait_for(lock, std::chrono::seconds(2), [&] { return started; }));
  }

  auto stopped = std::async(std::launch::async, [&] { worker.Stop(); });
  {
    std::lock_guard lock(mutex);
    release = true;
  }
  condition.notify_all();
  REQUIRE(stopped.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  stopped.get();
  const auto snapshot = worker.Snapshot();
  CHECK_FALSE(snapshot.checking);
  CHECK_FALSE(snapshot.result);
}
