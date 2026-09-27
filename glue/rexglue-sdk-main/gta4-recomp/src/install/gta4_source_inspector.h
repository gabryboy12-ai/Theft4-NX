#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace gta4::install {

enum class GameSourceKind {
  kUnknown,
  kExtractedFolder,
  kDiscImage,
  kStfsPackage,
};

enum class GameSourceStatus {
  kSupported,
  kWrongGame,
  kWrongMediaId,
  kWrongRegion,
  kWrongRevision,
  kWrongSignature,
  kWrongExecutable,
  kCorruptImage,
  kMissingDefaultXex,
  kAmbiguousDefaultXex,
};

struct GameSourceMetadata {
  uint32_t module_flags = 0;
  uint32_t title_id = 0;
  uint32_t media_id = 0;
  uint32_t xex_version = 0;
  uint32_t base_version = 0;
  uint32_t region = 0;
  uint8_t disc_number = 0;
  uint8_t disc_count = 0;
  std::array<uint8_t, 20> rsa_signature_sha1{};
};

struct GameSourceInspection {
  GameSourceStatus status = GameSourceStatus::kCorruptImage;
  GameSourceKind source_kind = GameSourceKind::kUnknown;
  uint32_t title_id = 0;
  uint32_t media_id = 0;
  uint32_t xex_version = 0;
  uint32_t base_version = 0;
  uint32_t region = 0;
  uint8_t disc_number = 0;
  uint8_t disc_count = 0;
  std::array<uint8_t, 20> rsa_signature_sha1{};
  std::string display_name;
  std::string release_label;
  std::string rejection_reason;

  bool supported() const { return status == GameSourceStatus::kSupported; }
};

// Performs read-only source inspection. ISO, extracted-folder, and STFS/SVOD
// sources all pass through the same bounded default.xex parser and classifier.
GameSourceInspection InspectGameSource(const std::filesystem::path& path);

// Byte-level entry point used by the installer after it has mounted a source.
// Keeping classification here prevents the synchronous Install path from
// drifting from source-selection validation.
GameSourceInspection InspectGameXex(std::span<const uint8_t> bytes);

// Pure metadata classifier, exposed to keep the exact supported identity
// independently unit-testable from filesystem/container parsing.
GameSourceInspection ClassifyGameSourceMetadata(const GameSourceMetadata& metadata);

std::string FormatXexVersion(uint32_t version);
std::string FormatXexRegion(uint32_t region);
std::string FormatGameSourceInspection(const GameSourceInspection& inspection);
std::string FormatGameSourceDiagnostics(const GameSourceInspection& inspection);
const char* GameSourceKindName(GameSourceKind kind);
const char* GameSourceStatusName(GameSourceStatus status);

struct GameSourceInspectionSnapshot {
  uint64_t generation = 0;
  bool checking = false;
  std::optional<GameSourceInspection> result;
};

// A single persistent worker handles source checks without blocking the UI.
// Every request receives a generation; results from older generations are
// discarded if a newer source is selected or the selection is cleared.
class GameSourceInspectionWorker {
 public:
  using InspectFunction = std::function<GameSourceInspection(const std::filesystem::path&)>;

  explicit GameSourceInspectionWorker(InspectFunction inspect = {});
  ~GameSourceInspectionWorker();

  GameSourceInspectionWorker(const GameSourceInspectionWorker&) = delete;
  GameSourceInspectionWorker& operator=(const GameSourceInspectionWorker&) = delete;

  uint64_t Request(std::filesystem::path path);
  uint64_t Clear();
  GameSourceInspectionSnapshot Snapshot() const;
  void Stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace gta4::install
