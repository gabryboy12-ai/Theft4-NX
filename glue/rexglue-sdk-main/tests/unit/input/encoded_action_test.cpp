#include <catch2/catch_test_macros.hpp>

#include <rex/input/mnk/encoded_action.h>

namespace {

using rex::input::mnk::DecodeActionMagnitude;
using rex::input::mnk::DecodeSignedActionRaw;
using rex::input::mnk::EncodeActionMagnitude;
using rex::input::mnk::MergeActionMagnitude;
using rex::input::mnk::MergeSignedAction;
using rex::input::mnk::MergeSignedActionPair;
using rex::input::mnk::SignedActionValueToRaw;

TEST_CASE("action magnitude encoding preserves both polarities", "[input][encoded_action]") {
  for (const uint8_t polarity : {uint8_t{0}, uint8_t{255}}) {
    for (const uint8_t magnitude : {uint8_t{0}, uint8_t{1}, uint8_t{127}, uint8_t{255}}) {
      CHECK(DecodeActionMagnitude(polarity, EncodeActionMagnitude(polarity, magnitude)) ==
            magnitude);
    }
  }
}

TEST_CASE("action merge compares decoded magnitudes", "[input][encoded_action]") {
  CHECK(MergeActionMagnitude(0, 0, 255) == 255);
  CHECK(MergeActionMagnitude(255, 255, 255) == 0);

  const uint8_t positive_existing = EncodeActionMagnitude(0, 200);
  const uint8_t negative_existing = EncodeActionMagnitude(255, 200);
  CHECK(MergeActionMagnitude(0, positive_existing, 100) == positive_existing);
  CHECK(MergeActionMagnitude(255, negative_existing, 100) == negative_existing);
  CHECK(DecodeActionMagnitude(255, MergeActionMagnitude(255, negative_existing, 250)) == 250);
}

TEST_CASE("signed action bytes preserve both endpoints and center sides",
          "[input][encoded_action]") {
  CHECK(SignedActionValueToRaw(-255) == 0);
  CHECK(SignedActionValueToRaw(-2) == 127);
  CHECK(SignedActionValueToRaw(-1) == 127);
  CHECK(SignedActionValueToRaw(1) == 128);
  CHECK(SignedActionValueToRaw(2) == 128);
  CHECK(SignedActionValueToRaw(255) == 255);
  CHECK(DecodeSignedActionRaw(0) == -255);
  CHECK(DecodeSignedActionRaw(127) == -1);
  CHECK(DecodeSignedActionRaw(128) == 1);
  CHECK(DecodeSignedActionRaw(255) == 255);
  CHECK(DecodeSignedActionRaw(SignedActionValueToRaw(-2)) ==
        -DecodeSignedActionRaw(SignedActionValueToRaw(2)));
}

TEST_CASE("signed action pair writes the same raw axis through both polarities",
          "[input][encoded_action]") {
  const auto left = MergeSignedActionPair(0, 128, 255, 127, -255);
  REQUIRE(left.changed);
  CHECK(DecodeActionMagnitude(0, left.negative_encoded) == 0);
  CHECK(DecodeActionMagnitude(255, left.positive_encoded) == 0);
  CHECK(DecodeSignedActionRaw(left.effective_raw) < 0);

  const auto right = MergeSignedActionPair(0, 128, 255, 127, 255);
  REQUIRE(right.changed);
  CHECK(DecodeActionMagnitude(0, right.negative_encoded) == 255);
  CHECK(DecodeActionMagnitude(255, right.positive_encoded) == 255);
  CHECK(DecodeSignedActionRaw(right.effective_raw) > 0);
}

TEST_CASE("signed action pair preserves stronger controller input and ties",
          "[input][encoded_action]") {
  const uint8_t controller_raw = SignedActionValueToRaw(-200);
  const uint8_t negative_encoded = EncodeActionMagnitude(0, controller_raw);
  const uint8_t positive_encoded = EncodeActionMagnitude(255, controller_raw);

  const auto weaker = MergeSignedActionPair(0, negative_encoded, 255, positive_encoded, 100);
  CHECK_FALSE(weaker.changed);
  CHECK(weaker.effective_raw == controller_raw);

  const auto tie = MergeSignedActionPair(0, negative_encoded, 255, positive_encoded, 200);
  CHECK_FALSE(tie.changed);
  CHECK(tie.effective_raw == controller_raw);

  const auto stronger = MergeSignedActionPair(0, negative_encoded, 255, positive_encoded, 255);
  REQUIRE(stronger.changed);
  CHECK(DecodeActionMagnitude(0, stronger.negative_encoded) == 255);
  CHECK(DecodeActionMagnitude(255, stronger.positive_encoded) == 255);
}

TEST_CASE("independent signed action merges without a synthetic companion record",
          "[input][encoded_action]") {
  const uint8_t controller_raw = SignedActionValueToRaw(101);
  const uint8_t encoded = EncodeActionMagnitude(255, controller_raw);

  const auto weaker = MergeSignedAction(255, encoded, -51);
  CHECK_FALSE(weaker.changed);
  CHECK(weaker.effective_raw == controller_raw);

  const auto stronger = MergeSignedAction(255, encoded, -255);
  REQUIRE(stronger.changed);
  CHECK(DecodeActionMagnitude(255, stronger.encoded) == SignedActionValueToRaw(-255));
}

}  // namespace
