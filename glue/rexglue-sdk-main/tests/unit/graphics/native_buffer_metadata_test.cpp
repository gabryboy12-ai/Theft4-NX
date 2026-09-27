#include "graphics/gta4_native/native_buffer_metadata.h"
#include "graphics/gta4_native/native_frame_scheduling.h"

#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace gta4 = rex::graphics::gta4_native;

TEST_CASE("retail vertex header flags do not extend the captured allocation") {
  const auto metadata = gta4::DecodeNativeBufferMetadata(0x00100001, 0x2003, 0x18000042);
  REQUIRE(metadata);
  REQUIRE(metadata->guest_address == 0x2000);
  REQUIRE(metadata->guest_size == 64);
  REQUIRE(metadata->HasValidPayload(64));
  REQUIRE_FALSE(metadata->guest_locked);

  // The previous 0x0FFFFFFF mask retained packed bits and read beyond this
  // exact allocation. ASan verifies the copy against the allocation boundary.
  std::vector<uint8_t> guest(64, 7);
  const std::vector<uint8_t> captured(guest.data(), guest.data() + metadata->guest_size);
  REQUIRE(captured == guest);
}

TEST_CASE("vertex shadow validation ignores neighbors but detects payload writes") {
  const auto metadata = gta4::DecodeNativeBufferMetadata(0x00100001, 0x2003, 0x10000042);
  REQUIRE(metadata);
  std::array<uint8_t, 66> guest{};
  const std::vector<uint8_t> captured(guest.data(), guest.data() + metadata->guest_size);

  // The old decoder treated these neighboring bytes as vertex payload.
  guest[64] = 1;
  guest[65] = 2;
  size_t offset = 0;
  do {
    const auto range = gta4::GetNativeBufferShadowValidationRange(metadata->guest_size, offset);
    REQUIRE(gta4::NativeBufferShadowPayloadRangeMatches(
        guest.data(), captured.data(), metadata->guest_size, range));
    offset = range.next_offset;
    if (range.completes_sweep) {
      break;
    }
  } while (true);

  guest[63] = 3;
  const auto last_range = gta4::GetNativeBufferShadowValidationRange(metadata->guest_size, 63);
  const bool matches = gta4::NativeBufferShadowPayloadRangeMatches(
      guest.data(), captured.data(), metadata->guest_size, last_range);
  REQUIRE_FALSE(matches);
  REQUIRE(gta4::ShouldDisableNativeBufferFastPath(true, matches));
}

TEST_CASE("retail index sizes retain their complete unmodified byte count") {
  const auto metadata = gta4::DecodeNativeBufferMetadata(0x20100002, 0x2002, 6);
  REQUIRE(metadata);
  REQUIRE(metadata->guest_address == 0x2002);
  REQUIRE(metadata->guest_size == 6);
  REQUIRE(metadata->HasValidPayload(64));
  std::array<uint8_t, 6> indices{0, 1, 0, 2, 0, 3};
  const std::vector<uint8_t> captured(indices.data(), indices.data() + metadata->guest_size);
  REQUIRE(captured.back() == 3);

  const auto oversized = gta4::DecodeNativeBufferMetadata(0x00100002, 0x2000, 0x10000006);
  REQUIRE(oversized);
  REQUIRE_FALSE(oversized->HasValidPayload(64));
}

TEST_CASE("buffer payload validation rejects wrapping ranges and other resource kinds") {
  REQUIRE_FALSE(gta4::DecodeNativeBufferMetadata(0x00100004, 0x2000, 64));
  REQUIRE_FALSE(gta4::DecodeNativeBufferMetadata(0x00100001, 3, 0x10000042)
                    ->HasValidPayload(64));
  REQUIRE_FALSE(gta4::DecodeNativeBufferMetadata(0x00100001, 0x2003, 0x10000002)
                    ->HasValidPayload(64));
  REQUIRE(gta4::DecodeNativeBufferMetadata(0x00100002, 0xFFFFFFFC, 4)->HasValidPayload(64));
  REQUIRE_FALSE(
      gta4::DecodeNativeBufferMetadata(0x00100002, 0xFFFFFFFC, 8)->HasValidPayload(64));
}

TEST_CASE("retail buffer lock nesting prevents an immutable capture assumption") {
  for (uint32_t flags : {0x00100101u, 0x00100201u, 0x00100F01u}) {
    const auto metadata = gta4::DecodeNativeBufferMetadata(flags, 0x2003, 0x10000042);
    REQUIRE(metadata);
    REQUIRE(metadata->guest_locked);
  }
  REQUIRE_FALSE(gta4::DecodeNativeBufferMetadata(0x00100001, 0x2003, 0x10000042)
                    ->guest_locked);
}
