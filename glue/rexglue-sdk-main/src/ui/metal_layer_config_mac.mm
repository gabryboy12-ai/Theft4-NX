/**
 ******************************************************************************
 * LibertyRecomp : GTA IV for modern platforms                               *
 ******************************************************************************
 * Copyright 2026 LibertyRecomp contributors. All rights reserved.
 * Released under the BSD license - see LICENSE in the root for more details.
 */

#import <QuartzCore/CAMetalLayer.h>

#include <rex/ui/surface_mac.h>

namespace rex::ui {

void ConfigureMetalLayerForPresentation(void* layer_pointer) {
  CAMetalLayer* layer = (__bridge CAMetalLayer*)layer_pointer;
  if (!layer) {
    return;
  }

  // Core Animation owns a finite drawable pool. A permanent nextDrawable
  // wait can otherwise strand MoltenVK's asynchronous queue and anything
  // waiting on its Vulkan fence. Keep Apple's bounded behavior explicit.
  layer.allowsNextDrawableTimeout = YES;
  // Vulkan presentation is committed independently of Core Animation
  // transactions. This is the normal low-latency CAMetalLayer mode.
  layer.presentsWithTransaction = NO;
}

}  // namespace rex::ui
