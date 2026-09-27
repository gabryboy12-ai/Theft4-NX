#pragma once

#include <algorithm>
#include <cstdint>

namespace rex::input::mnk {

// GTA IV stores an action's logical magnitude as the XOR of a per-action
// polarity byte and its current encoded byte. Negative and positive halves of
// an axis consequently use different encoded bytes for the same magnitude.
constexpr uint8_t DecodeActionMagnitude(uint8_t polarity, uint8_t encoded_value) noexcept {
  return static_cast<uint8_t>(polarity ^ encoded_value);
}

constexpr uint8_t EncodeActionMagnitude(uint8_t polarity, uint8_t magnitude) noexcept {
  return static_cast<uint8_t>(polarity ^ magnitude);
}

constexpr uint8_t MergeActionMagnitude(uint8_t polarity, uint8_t encoded_value,
                                       uint8_t requested_magnitude) noexcept {
  const uint8_t current_magnitude = DecodeActionMagnitude(polarity, encoded_value);
  return requested_magnitude > current_magnitude
             ? EncodeActionMagnitude(polarity, requested_magnitude)
             : encoded_value;
}

// GTA IV represents each signed controller axis as a byte centered between
// 127 and 128. The title registers two action records for the same raw axis:
// its selector examines the first record's sign, then returns either the
// negative or positive record. Both records therefore have to describe the
// same signed value.
constexpr int32_t kSignedActionExtent = 255;

constexpr uint8_t SignedActionValueToRaw(int32_t value) noexcept {
  const int32_t clamped = std::clamp(value, -kSignedActionExtent, kSignedActionExtent);
  // Only odd centered values are exactly representable. Round even values
  // toward zero on both sides so equal pointer deltas remain symmetric.
  const int32_t center = clamped < 0 ? kSignedActionExtent + 1 : kSignedActionExtent;
  return static_cast<uint8_t>((clamped + center) / 2);
}

// Returns the signed distance from the 127.5 center in doubled-byte units.
// This avoids floating-point rounding while preserving both endpoints.
constexpr int32_t DecodeSignedActionRaw(uint8_t raw_value) noexcept {
  return static_cast<int32_t>(raw_value) * 2 - kSignedActionExtent;
}

constexpr int32_t SignedActionMagnitude(uint8_t raw_value) noexcept {
  const int32_t value = DecodeSignedActionRaw(raw_value);
  return value < 0 ? -value : value;
}

struct SignedActionPairMerge {
  uint8_t negative_encoded = 0;
  uint8_t positive_encoded = 0;
  uint8_t effective_raw = 0;
  bool changed = false;
};

constexpr SignedActionPairMerge MergeSignedActionPair(uint8_t negative_polarity,
                                                      uint8_t negative_encoded,
                                                      uint8_t positive_polarity,
                                                      uint8_t positive_encoded,
                                                      int32_t requested_value) noexcept {
  const uint8_t negative_raw = DecodeActionMagnitude(negative_polarity, negative_encoded);
  const uint8_t positive_raw = DecodeActionMagnitude(positive_polarity, positive_encoded);

  // GTA's pair selector chooses the negative record only when the first
  // record decodes below center. Otherwise the positive record is effective.
  const uint8_t current_raw = DecodeSignedActionRaw(negative_raw) < 0 ? negative_raw : positive_raw;
  const uint8_t requested_raw = SignedActionValueToRaw(requested_value);
  if (requested_value == 0 ||
      SignedActionMagnitude(requested_raw) <= SignedActionMagnitude(current_raw)) {
    return {negative_encoded, positive_encoded, current_raw, false};
  }

  return {EncodeActionMagnitude(negative_polarity, requested_raw),
          EncodeActionMagnitude(positive_polarity, requested_raw), requested_raw, true};
}

struct SignedActionMerge {
  uint8_t encoded = 0;
  uint8_t effective_raw = 0;
  bool changed = false;
};

// Some GTA IV actions (notably the map pan axes) are independent centered
// signed records rather than the two-record selector used by sticks. Merge
// them without inventing a companion action or discarding stronger pad input.
constexpr SignedActionMerge MergeSignedAction(uint8_t polarity, uint8_t encoded,
                                              int32_t requested_value) noexcept {
  const uint8_t current_raw = DecodeActionMagnitude(polarity, encoded);
  const uint8_t requested_raw = SignedActionValueToRaw(requested_value);
  if (requested_value == 0 ||
      SignedActionMagnitude(requested_raw) <= SignedActionMagnitude(current_raw)) {
    return {encoded, current_raw, false};
  }

  return {EncodeActionMagnitude(polarity, requested_raw), requested_raw, true};
}

}  // namespace rex::input::mnk
