/**
 * @file        rex/ui/window_nx.h
 * @brief       ui::Window over the default libnx NWindow (switch-game)
 *
 * One display, owned by the application: the window is the default NWindow
 * at 1280x720 (handheld), as switch-smoke's Vulkan probe uses it; its surface
 * is a ViNWindowSurface (VK_NN_vi_surface). Paints requested from any thread
 * are coalesced and done by NxWindowedAppContext::PumpFromUIThread. Reduced
 * from nfsmw-nx's WindowSwitch (06-game-boot-plan.md section 24).
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string_view>

#include <rex/ui/window.h>

struct NWindow;

namespace rex::ui {

class NxWindowedAppContext;

class NxWindow final : public Window {
 public:
  static constexpr uint32_t kWidth = 1280;
  static constexpr uint32_t kHeight = 720;

  NxWindow(WindowedAppContext& app_context, std::string_view title, uint32_t desired_logical_width,
           uint32_t desired_logical_height);
  ~NxWindow() override;

  // NxWindowedAppContext, UI thread.
  void HandlePendingPaint();

 protected:
  bool OpenImpl() override;
  void RequestCloseImpl() override;
  std::unique_ptr<Surface> CreateSurfaceImpl(Surface::TypeFlags allowed_types) override;
  void RequestPaintImpl() override;

 private:
  NxWindowedAppContext& nx_app_context() const;
  void PerformClose();

  NWindow* nwindow_ = nullptr;
  std::atomic<bool> paint_pending_{false};
};

}  // namespace rex::ui
