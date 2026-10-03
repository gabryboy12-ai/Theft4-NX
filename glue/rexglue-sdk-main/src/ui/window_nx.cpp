/**
 * @file        ui/window_nx.cpp
 * @brief       See rex/ui/window_nx.h and rex/ui/windowed_app_context_nx.h
 */

#include <rex/ui/window_nx.h>
#include <rex/ui/windowed_app_context_nx.h>

#include <algorithm>

#include <switch.h>

#include <rex/logging.h>
#include <rex/ui/surface_switch.h>

namespace rex::ui {

// ── Context ────────────────────────────────────────────────────────────────

NxWindowedAppContext::~NxWindowedAppContext() {
  // What other threads posted last still runs, as the WindowedAppContext
  // shutdown contract asks.
  ExecutePendingFunctionsFromUIThread();
}

void NxWindowedAppContext::PumpFromUIThread() {
  ExecutePendingFunctionsFromUIThread();
  const std::vector<NxWindow*> windows = windows_;
  for (NxWindow* window : windows) {
    if (IsRegistered(window)) {
      window->HandlePendingPaint();
    }
  }
}

void NxWindowedAppContext::RegisterWindow(NxWindow* window) {
  if (!IsRegistered(window)) {
    windows_.push_back(window);
  }
}

void NxWindowedAppContext::UnregisterWindow(NxWindow* window) {
  windows_.erase(std::remove(windows_.begin(), windows_.end(), window), windows_.end());
}

bool NxWindowedAppContext::IsRegistered(const NxWindow* window) const {
  return std::find(windows_.begin(), windows_.end(), window) != windows_.end();
}

// ── Window ─────────────────────────────────────────────────────────────────

std::unique_ptr<Window> Window::Create(WindowedAppContext& app_context,
                                       const std::string_view title, uint32_t desired_logical_width,
                                       uint32_t desired_logical_height) {
  return std::make_unique<NxWindow>(app_context, title, desired_logical_width,
                                    desired_logical_height);
}

NxWindow::NxWindow(WindowedAppContext& app_context, const std::string_view title,
                   uint32_t desired_logical_width, uint32_t desired_logical_height)
    : Window(app_context, title, desired_logical_width, desired_logical_height) {}

NxWindow::~NxWindow() {
  EnterDestructor();
  if (nwindow_) {
    nx_app_context().UnregisterWindow(this);
    nwindow_ = nullptr;
  }
}

NxWindowedAppContext& NxWindow::nx_app_context() const {
  return static_cast<NxWindowedAppContext&>(app_context());
}

bool NxWindow::OpenImpl() {
  // The desired size is ignored: the application owns the whole display.
  nwindow_ = nwindowGetDefault();
  if (!nwindow_ || !nwindowIsValid(nwindow_)) {
    REXLOG_ERROR("NxWindow: no default NWindow");
    nwindow_ = nullptr;
    return false;
  }
  nx_app_context().RegisterWindow(this);
  REXLOG_INFO("NxWindow: default NWindow, {}x{}", kWidth, kHeight);
  WindowDestructionReceiver destruction_receiver(this);
  OnActualSizeUpdate(kWidth, kHeight, destruction_receiver);
  if (destruction_receiver.IsWindowDestroyed()) {
    return true;
  }
  OnFocusUpdate(true, destruction_receiver);
  return true;
}

void NxWindow::RequestCloseImpl() {
  PerformClose();
}

void NxWindow::PerformClose() {
  WindowDestructionReceiver destruction_receiver(this);
  OnBeforeClose(destruction_receiver);
  if (destruction_receiver.IsWindowDestroyed()) {
    return;
  }
  if (nwindow_) {
    nx_app_context().UnregisterWindow(this);
    nwindow_ = nullptr;
  }
  OnAfterClose();
}

std::unique_ptr<Surface> NxWindow::CreateSurfaceImpl(Surface::TypeFlags allowed_types) {
  if (!nwindow_ || !(allowed_types & Surface::kTypeFlag_ViNWindow)) {
    return nullptr;
  }
  return std::make_unique<ViNWindowSurface>(nwindow_, kWidth, kHeight);
}

void NxWindow::RequestPaintImpl() {
  // Coalesced: at most one paint pending; the next pump does it.
  paint_pending_.store(true, std::memory_order_release);
}

void NxWindow::HandlePendingPaint() {
  if (paint_pending_.exchange(false, std::memory_order_acq_rel)) {
    OnPaint();
  }
}

}  // namespace rex::ui
