#pragma once

#include <rex/ui/imgui_dialog.h>

namespace gta4::input {

class ContextTouchOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit ContextTouchOverlay(rex::ui::ImGuiDrawer* drawer);

 protected:
  void OnDraw(ImGuiIO& io) override;
};

}  // namespace gta4::input
