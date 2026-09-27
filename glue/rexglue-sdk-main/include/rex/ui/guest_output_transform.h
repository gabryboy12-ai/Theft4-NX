#pragma once

#include <cstdint>

namespace rex::ui {

// Exact geometry used by the common Presenter for the final guest-output pass.
// Host input is expressed in physical window pixels; the final render target
// may have a different size, so both coordinate spaces are retained.
struct GuestOutputTransform {
  uint64_t revision = 0;
  uint32_t surface_width = 0;
  uint32_t surface_height = 0;
  uint32_t host_render_target_width = 0;
  uint32_t host_render_target_height = 0;
  int32_t output_x = 0;
  int32_t output_y = 0;
  uint32_t output_width = 0;
  uint32_t output_height = 0;
  uint32_t guest_width = 0;
  uint32_t guest_height = 0;

  bool IsValid() const {
    return surface_width && surface_height && host_render_target_width &&
           host_render_target_height && output_width && output_height && guest_width &&
           guest_height;
  }
};

}  // namespace rex::ui
