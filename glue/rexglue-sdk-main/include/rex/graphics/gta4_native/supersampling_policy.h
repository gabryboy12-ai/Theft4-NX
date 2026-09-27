#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace rex::graphics::gta4_native {

struct SupersampledExtent {
  uint32_t width = 0;
  uint32_t height = 0;

  constexpr bool operator==(const SupersampledExtent&) const = default;
};

struct FragmentCoordinateScale {
  float x = 1.0f;
  float y = 1.0f;

  constexpr bool operator==(const FragmentCoordinateScale&) const = default;
};

constexpr FragmentCoordinateScale CalculateFragmentCoordinateScale(
    uint32_t logical_width, uint32_t logical_height, uint32_t physical_width,
    uint32_t physical_height) {
  if (!logical_width || !logical_height || !physical_width || !physical_height) {
    return {};
  }
  return {float(logical_width) / float(physical_width),
          float(logical_height) / float(physical_height)};
}

constexpr bool IsSupportedSupersamplingPixelFactor(uint32_t factor) {
  return factor >= 2u && factor <= 16u && (factor & 1u) == 0u;
}

inline std::optional<SupersampledExtent> CalculateSupersampledExtent(
    uint32_t logical_width, uint32_t logical_height, uint32_t pixel_factor,
    uint32_t maximum_dimension = std::numeric_limits<uint32_t>::max()) {
  if (!logical_width || !logical_height ||
      !IsSupportedSupersamplingPixelFactor(pixel_factor) || !maximum_dimension) {
    return std::nullopt;
  }

  const double axis_scale = std::sqrt(double(pixel_factor));
  const auto scale_axis = [axis_scale, maximum_dimension](uint32_t logical)
      -> std::optional<uint32_t> {
    const double unaligned = std::ceil(double(logical) * axis_scale);
    if (!std::isfinite(unaligned) || unaligned < 1.0 ||
        unaligned > double(maximum_dimension)) {
      return std::nullopt;
    }
    uint64_t aligned = uint64_t(unaligned);
    if (aligned & 1u) {
      ++aligned;
    }
    if (aligned > maximum_dimension) {
      return std::nullopt;
    }
    return uint32_t(aligned);
  };

  const std::optional<uint32_t> width = scale_axis(logical_width);
  const std::optional<uint32_t> height = scale_axis(logical_height);
  if (!width || !height) {
    return std::nullopt;
  }
  return SupersampledExtent{*width, *height};
}

}  // namespace rex::graphics::gta4_native
