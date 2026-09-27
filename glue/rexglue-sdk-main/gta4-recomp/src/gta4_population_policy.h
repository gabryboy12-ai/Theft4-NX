#ifndef GTA4_POPULATION_POLICY_H_
#define GTA4_POPULATION_POLICY_H_

#include <cmath>
#include <limits>

namespace gta4::population {

// Apply a host-configured scale to one of GTA IV's population multipliers.
// Non-positive guest values are preserved because the game uses zero to
// suppress population during missions and transitions, and negative values
// may be sentinels. Invalid host configuration or overflow also falls back to
// the guest's request rather than injecting invalid state into the game.
inline float ScaleMultiplier(float requested, double configured_scale) noexcept {
  if (!std::isfinite(requested) || requested <= 0.0f) {
    return requested;
  }
  if (!std::isfinite(configured_scale) || configured_scale < 0.0) {
    return requested;
  }

  const double scaled = static_cast<double>(requested) * configured_scale;
  if (!std::isfinite(scaled) ||
      scaled > static_cast<double>(std::numeric_limits<float>::max())) {
    return requested;
  }
  return static_cast<float>(scaled);
}

}  // namespace gta4::population

#endif  // GTA4_POPULATION_POLICY_H_
