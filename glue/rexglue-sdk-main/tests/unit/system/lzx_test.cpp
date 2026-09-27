#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/system/lzx.h>

namespace {

constexpr uint32_t kRetailWindowSize = 0x20000;
constexpr size_t kFullFrameSize = 0x8000;
constexpr std::array<uint8_t, 4> kReadableTail = {0xA5, 0x5A, 0xC3, 0x3C};

void AppendRepeatedOffsets(std::vector<uint8_t>& compressed) {
  // LZX uncompressed blocks store R0, R1, and R2 as little-endian u32s.
  constexpr std::array<uint8_t, 12> offsets = {
      1, 0, 0, 0,
      1, 0, 0, 0,
      1, 0, 0, 0,
  };
  compressed.insert(compressed.end(), offsets.begin(), offsets.end());
}

std::vector<uint8_t> MakeFirstFullFrame(std::vector<uint8_t>* expected) {
  // Stream header=0, block type=uncompressed, block length=0x8000. LZX bit
  // words are stored little-endian; these bytes were independently generated
  // from the 1+3+24-bit header and checked with Python.
  std::vector<uint8_t> compressed = {0x08, 0x30, 0x00, 0x00};
  AppendRepeatedOffsets(compressed);
  expected->resize(kFullFrameSize);
  for (size_t index = 0; index < expected->size(); ++index) {
    (*expected)[index] = static_cast<uint8_t>((index * 29u + 7u) & 0xFFu);
  }
  compressed.insert(compressed.end(), expected->begin(), expected->end());
  compressed.insert(compressed.end(), kReadableTail.begin(), kReadableTail.end());
  return compressed;
}

std::vector<uint8_t> MakeFinalShortFrame(std::vector<uint8_t>* expected) {
  // Header-read state persists, so this frame begins with only the 3+24-bit
  // uncompressed block header for a seven-byte final frame.
  std::vector<uint8_t> compressed = {0x00, 0x60, 0xE0, 0x00};
  AppendRepeatedOffsets(compressed);
  *expected = {0x4C, 0x5A, 0x58, 0x2D, 0x47, 0x54, 0x41};
  compressed.insert(compressed.end(), expected->begin(), expected->end());
  compressed.insert(compressed.end(), kReadableTail.begin(), kReadableTail.end());
  return compressed;
}

}  // namespace

TEST_CASE("persistent LZX decoder retains state across a full and short frame",
          "[system][lzx]") {
  auto decoder = rex::lzx::PersistentDecoder::Create(kRetailWindowSize);
  REQUIRE(decoder);

  std::vector<uint8_t> first_expected;
  const std::vector<uint8_t> first_compressed = MakeFirstFullFrame(&first_expected);
  std::vector<uint8_t> first_output(first_expected.size(), 0);
  const auto first = decoder->DecodeFrame(first_compressed, first_output);
  REQUIRE(first);
  CHECK(first.bytes_written == first_expected.size());
  CHECK(first.bytes_consumed == first_compressed.size() - kReadableTail.size());
  CHECK(first_output == first_expected);
  CHECK(decoder->total_output_bytes() == kFullFrameSize);

  std::vector<uint8_t> final_expected;
  const std::vector<uint8_t> final_compressed = MakeFinalShortFrame(&final_expected);
  std::vector<uint8_t> final_output(final_expected.size(), 0);
  const auto final = decoder->DecodeFrame(final_compressed, final_output);
  CAPTURE(static_cast<uint32_t>(final.status), final.bytes_written,
          final.bytes_consumed, decoder->total_output_bytes());
  REQUIRE(final);
  CHECK(final.bytes_written == final_expected.size());
  CHECK(final.bytes_consumed == final_compressed.size() - kReadableTail.size());
  CHECK(final_output == final_expected);
  CHECK(decoder->total_output_bytes() == kFullFrameSize + final_expected.size());
}

TEST_CASE("persistent LZX decoder rejects unsupported window configurations",
          "[system][lzx]") {
  CHECK_FALSE(rex::lzx::PersistentDecoder::Create(0));
  CHECK_FALSE(rex::lzx::PersistentDecoder::Create(0x4000));
  CHECK_FALSE(rex::lzx::PersistentDecoder::Create(0x30000));
  CHECK_FALSE(rex::lzx::PersistentDecoder::Create(0x400000));

  auto decoder = rex::lzx::PersistentDecoder::Create(kRetailWindowSize);
  REQUIRE(decoder);
  CHECK(decoder->window_size() == kRetailWindowSize);
}

TEST_CASE("persistent LZX decoder poisons malformed streams until reset",
          "[system][lzx]") {
  auto decoder = rex::lzx::PersistentDecoder::Create(kRetailWindowSize);
  REQUIRE(decoder);

  const std::array<uint8_t, 1> malformed = {0xFF};
  std::array<uint8_t, 32> output = {};
  const auto malformed_result = decoder->DecodeFrame(malformed, output);
  CHECK(malformed_result.status == rex::lzx::DecodeStatus::kDataError);
  CHECK(decoder->poisoned());

  std::vector<uint8_t> expected;
  const std::vector<uint8_t> valid = MakeFirstFullFrame(&expected);
  std::vector<uint8_t> valid_output(expected.size(), 0);
  CHECK(decoder->DecodeFrame(valid, valid_output).status ==
        rex::lzx::DecodeStatus::kPoisoned);

  REQUIRE(decoder->Reset());
  CHECK_FALSE(decoder->poisoned());
  const auto recovered = decoder->DecodeFrame(valid, valid_output);
  REQUIRE(recovered);
  CHECK(valid_output == expected);
}

TEST_CASE("persistent LZX decoder validates empty and missing input", "[system][lzx]") {
  auto decoder = rex::lzx::PersistentDecoder::Create(kRetailWindowSize);
  REQUIRE(decoder);

  const std::span<const uint8_t> no_input;
  const std::span<uint8_t> no_output;
  CHECK(decoder->DecodeFrame(no_input, no_output).status ==
        rex::lzx::DecodeStatus::kSuccess);
  CHECK(decoder->total_output_bytes() == 0);

  std::array<uint8_t, 1> output = {};
  CHECK(decoder->DecodeFrame(no_input, output).status ==
        rex::lzx::DecodeStatus::kInvalidArgument);
  CHECK_FALSE(decoder->poisoned());
}

TEST_CASE("one-shot LZX decoder rejects invalid windows and reference bounds",
          "[system][lzx]") {
  std::array<uint8_t, 4> input = {};
  std::array<uint8_t, 4> output = {};
  std::array<uint8_t, 1> reference = {};

  CHECK(lzx_decompress(input.data(), input.size(), output.data(), output.size(),
                       0x30000, nullptr, 0) == 1);
  CHECK(lzx_decompress(input.data(), input.size(), output.data(), output.size(),
                       kRetailWindowSize, reference.data(), kRetailWindowSize + 1) == 1);
  CHECK(lzx_decompress(nullptr, input.size(), output.data(), output.size(),
                       kRetailWindowSize, nullptr, 0) == 1);
}
