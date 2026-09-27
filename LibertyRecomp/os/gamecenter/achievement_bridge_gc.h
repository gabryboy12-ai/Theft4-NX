// achievement_bridge_gc.h — Game Center achievement bridge for iOS & macOS
//
// Provides two entry points callable from plain C++:
//   LibertyGCInit()                         — call once at startup (authenticates GKLocalPlayer)
//   LibertyGCOnXboxAchievementUnlocked(id)  — call from the XAM message hook
//
// Achievement ID mapping:
//   Xbox achievement IDs are 1-indexed (1..65).
//   Game Center identifiers: "ach_NNN" where NNN = zero-padded xbox_id (e.g. "ach_001").
//   A bonus "ach_platinum" achievement is auto-granted when all 65 are complete
//   (Game Center has no built-in platinum concept, so we handle it the same
//   way as the PS4 bridge — by scanning completion after every unlock).
//
// This header is included on both iOS and macOS builds.

#pragma once

#ifdef LIBERTY_RECOMP_GAMECENTER

#include <stdint.h>

// Authenticate GKLocalPlayer and set up the completion handler.
// Safe to call multiple times — internally guarded with dispatch_once.
void LibertyGCInit();

// Called from the XAM kernel shim for every achievement write.
// xbox_id: 1-indexed Xbox 360 achievement ID (1..65).
void LibertyGCOnXboxAchievementUnlocked(uint32_t xbox_id);

#endif  // LIBERTY_RECOMP_GAMECENTER
