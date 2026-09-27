// achievement_bridge_ps4.h — Xbox 360 achievement → PS4 NP trophy bridge
//
// Call LibertyPS4OnXboxAchievementUnlocked() from the XAM message handler
// whenever the game fires an "AWARD_ACHIEVEMENT" write (XMsg 0xFB/0xB0008).
//
// Trophy mapping (TROP.ESFM, set 0, NP comm ID LBTY00001_00):
//   Xbox achievement IDs are 1-indexed (1..65, base + DLC).
//   PS4 trophy IDs are 0-indexed  (0..64, non-platinum).
//   Platinum lives at trophy 65 — no Xbox equivalent; auto-granted here
//   once all 65 non-platinum trophies are confirmed unlocked.
//
//   xbox_id → ps4_trophy_id = xbox_id - 1
//
// This header is only meaningful on PS4/OpenOrbis builds.

#pragma once

#ifdef LIBERTY_RECOMP_PS4

#include <stdint.h>

// Called from the XAM kernel shim (xam_msg.cpp) for every achievement write.
// xbox_id: 1-indexed Xbox 360 achievement ID (1..65).
void LibertyPS4OnXboxAchievementUnlocked(uint32_t xbox_id);

#endif  // LIBERTY_RECOMP_PS4
