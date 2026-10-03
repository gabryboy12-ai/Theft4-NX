/**
 * @file        rex/ui/windowed_app_context_nx.h
 * @brief       WindowedAppContext for switch-game (Horizon, no SDL)
 *
 * Phase (a) of the renderer (06-game-boot-plan.md section 24): the UI thread
 * is switch-game's main loop, which already polls the pad and the applet at
 * 60 Hz. Each pass calls PumpFromUIThread: the functions other threads posted
 * run, then the windows paint if asked to. No applet hook, no dock tracking:
 * reduced from nfsmw-nx's SwitchWindowedAppContext.
 */

#pragma once

#include <atomic>
#include <vector>

#include <rex/ui/windowed_app_context.h>

namespace rex::ui {

class NxWindow;

class NxWindowedAppContext final : public WindowedAppContext {
 public:
  NxWindowedAppContext() = default;
  ~NxWindowedAppContext() override;

  // Other threads post through CallInUIThread*; the next pump runs them.
  void NotifyUILoopOfPendingFunctions() override {}
  void PlatformQuitFromUIThread() override {}

  // UI thread (switch-game's main loop), once per pass.
  void PumpFromUIThread();

  // UI thread only.
  void RegisterWindow(NxWindow* window);
  void UnregisterWindow(NxWindow* window);

 private:
  bool IsRegistered(const NxWindow* window) const;

  std::vector<NxWindow*> windows_;
};

}  // namespace rex::ui
