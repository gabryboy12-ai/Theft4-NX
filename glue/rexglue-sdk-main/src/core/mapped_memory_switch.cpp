/**
 * @file        core/mapped_memory_switch.cpp
 * @brief       Nintendo Switch (libnx) MappedMemory backend.
 *
 * Horizon has no file-backed mmap, so a "mapping" here is the requested byte
 * range read into a page-aligned heap buffer. Only Mode::kRead is supported:
 * a write-back mapping would need an explicit flush-to-file path that no
 * caller uses today (see docs/switch-port/01-first-build.md, "mapped_memory").
 *
 * Callers that map a whole disc image (DiscImageDevice) will allocate the full
 * image size; on Switch use extracted files via HostPathDevice/RomFS instead.
 */

#include <rex/platform.h>

static_assert(REX_PLATFORM_NX, "This file is Switch-only");

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include <sys/stat.h>

#include <rex/logging.h>
#include <rex/memory/mapped_memory.h>

namespace rex::memory {

namespace {

constexpr size_t kBufferAlignment = 0x1000;

class SwitchMappedMemory : public MappedMemory {
 public:
  SwitchMappedMemory(void* data, size_t size) : MappedMemory(data, size) {}

  ~SwitchMappedMemory() override { std::free(data_); }

  void Close(uint64_t truncate_size) override {
    // Read-only: there is nothing to write back or truncate.
    (void)truncate_size;
    std::free(data_);
    data_ = nullptr;
    size_ = 0;
  }

  void Flush() override {}
};

}  // namespace

std::unique_ptr<MappedMemory> MappedMemory::Open(const std::filesystem::path& path, Mode mode,
                                                 size_t offset, size_t length) {
  if (mode != Mode::kRead) {
    REXLOG_ERROR(
        "MappedMemory::Open({}): only read-only mappings are supported on Switch "
        "(Horizon has no file-backed mmap)",
        path.string());
    return nullptr;
  }

  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) {
    return nullptr;
  }

  struct stat file_stat;
  if (fstat(fileno(file), &file_stat) != 0 || file_stat.st_size < 0) {
    std::fclose(file);
    return nullptr;
  }
  const size_t file_size = size_t(file_stat.st_size);
  if (offset > file_size) {
    std::fclose(file);
    return nullptr;
  }
  const size_t map_length = length ? length : file_size - offset;
  if (map_length > file_size - offset) {
    // POSIX mmap would allow mapping past EOF (SIGBUS on access); refuse here.
    std::fclose(file);
    return nullptr;
  }

  // aligned_alloc requires a size that is a multiple of the alignment.
  const size_t alloc_size =
      ((map_length ? map_length : 1) + kBufferAlignment - 1) & ~(kBufferAlignment - 1);
  void* data = std::aligned_alloc(kBufferAlignment, alloc_size);
  if (!data) {
    REXLOG_ERROR("MappedMemory::Open({}): cannot allocate {} bytes", path.string(), alloc_size);
    std::fclose(file);
    return nullptr;
  }

  bool ok = fseeko(file, off_t(offset), SEEK_SET) == 0;
  if (ok && map_length) {
    ok = std::fread(data, 1, map_length, file) == map_length;
  }
  std::fclose(file);
  if (!ok) {
    std::free(data);
    return nullptr;
  }
  std::memset(static_cast<uint8_t*>(data) + map_length, 0, alloc_size - map_length);

  return std::make_unique<SwitchMappedMemory>(data, map_length);
}

std::unique_ptr<ChunkedMappedMemoryWriter> ChunkedMappedMemoryWriter::Open(
    const std::filesystem::path& path, size_t chunk_size, bool low_address_space) {
  // Same as the POSIX backend: not implemented, and no caller uses it.
  (void)path;
  (void)chunk_size;
  (void)low_address_space;
  return nullptr;
}

}  // namespace rex::memory
