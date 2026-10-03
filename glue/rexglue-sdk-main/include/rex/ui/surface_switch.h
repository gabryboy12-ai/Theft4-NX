/**
 * @file        rex/ui/surface_switch.h
 * @brief       Presentation surface over a libnx NWindow (VK_NN_vi_surface)
 *
 * From nfsmw-nx's SDK (06-game-boot-plan.md section 24): the window is the
 * default NWindow switch-game takes from the console, as switch-smoke's
 * Vulkan probe does.
 */

#pragma once

#include <cstdint>

#include <rex/ui/surface.h>

// libnx; declared by tag so switch.h stays out of the UI headers.
struct NWindow;

namespace rex::ui {

class ViNWindowSurface final : public Surface {
 public:
  ViNWindowSurface(NWindow* window, uint32_t width, uint32_t height)
      : window_(window), width_(width), height_(height) {}

  TypeIndex GetType() const override { return kTypeIndex_ViNWindow; }
  NWindow* window() const { return window_; }

 protected:
  // The size the presenter creates the first swapchain with (1280x720 for
  // now: the window does not follow the dock yet).
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override {
    width_out = width_;
    height_out = height_;
    return true;
  }

 private:
  NWindow* window_;
  uint32_t width_;
  uint32_t height_;
};

}  // namespace rex::ui
