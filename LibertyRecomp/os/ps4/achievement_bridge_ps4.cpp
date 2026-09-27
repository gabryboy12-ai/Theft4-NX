// achievement_bridge_ps4.cpp — Xbox achievement ID → PS4 NP trophy bridge
//
// Xbox GTA IV has 65 scoreable achievements (IDs 1..65, 1-indexed).
// PS4 trophy set LBTY00001_00 has 66 trophies (IDs 0..65, 0-indexed):
//   TROP000..TROP064  → non-platinum (maps 1:1 from xbox_id 1..65)
//   TROP065           → Platinum     (no Xbox equivalent; auto-granted here)
//
// Platinum logic:
//   Xbox 360 has no platinum trophy concept.  We implement it manually:
//   after every non-platinum unlock, query sceNpTrophyGetTrophyInfo on all
//   65 non-platinum trophies.  If every one is unlocked, we call
//   sceNpTrophyUnlockTrophy(platinum=65).  The SCE API is idempotent for
//   already-unlocked trophies (returns SCE_NP_TROPHY_ERROR_ALREADY_UNLOCKED),
//   so no double-grant is possible.
//
//   The 65-query scan is O(65) SCE calls but only runs on unlock events,
//   which are rare in gameplay.

#ifdef LIBERTY_RECOMP_PS4

#include <orbis/NpTrophy.h>
#include "achievement_bridge_ps4.h"
#include "np_conf_ps4.h"

// Globals defined in ps4_main.cpp
extern int32_t g_np_context;
extern int32_t g_np_handle;

// ── constants ────────────────────────────────────────────────────────────────

// Total non-platinum trophies (= total Xbox achievements).
static constexpr int kNonPlatinumCount = 65;
// PS4 trophy ID of the Platinum.
static constexpr int kPlatinumTrophyId = 65;
// SCE error returned when a trophy was already unlocked — not a hard error.
static constexpr int32_t kAlreadyUnlocked = static_cast<int32_t>(0x80551604);

// ── helpers ──────────────────────────────────────────────────────────────────

// Unlock a PS4 trophy by its 0-indexed ID.
// Returns true if the unlock succeeded or the trophy was already unlocked.
static bool UnlockPS4Trophy(int ps4_id)
{
    if (!g_np_context || !g_np_handle)
        return false;

    int32_t plat_id = -1;
    int32_t ret = sceNpTrophyUnlockTrophy(g_np_context, g_np_handle, ps4_id, &plat_id);
    return (ret == 0 || ret == kAlreadyUnlocked);
}

// Scan all 65 non-platinum trophies.  Returns true only if every one of them
// reports IsUnlocked == true (i.e., all 65 base/DLC trophies are done).
static bool AllNonPlatinumUnlocked()
{
    if (!g_np_context || !g_np_handle)
        return false;

    for (int i = 0; i < kNonPlatinumCount; ++i)
    {
        OrbisNpTrophyDetails details = {};
        details.size = sizeof(OrbisNpTrophyDetails);
        OrbisNpTrophyData data = {};
        data.size = sizeof(OrbisNpTrophyData);

        int32_t ret = sceNpTrophyGetTrophyInfo(g_np_context, g_np_handle, i, &details, &data);
        if (ret < 0 || !data.IsUnlocked)
            return false;
    }
    return true;
}

// ── public API ────────────────────────────────────────────────────────────────

void LibertyPS4OnXboxAchievementUnlocked(uint32_t xbox_id)
{
    // Xbox IDs are 1-indexed: valid range is 1..65.
    if (xbox_id < 1 || xbox_id > static_cast<uint32_t>(kNonPlatinumCount))
        return;

    // Map to 0-indexed PS4 trophy ID.
    const int ps4_id = static_cast<int>(xbox_id) - 1;

    // Attempt the unlock.  If it fails for a reason other than already-unlocked
    // (e.g. NP context not ready) we bail without checking platinum.
    bool just_unlocked = UnlockPS4Trophy(ps4_id);
    if (!just_unlocked)
        return;

    // --- Platinum check ---
    // Only bother scanning if the caller just completed what might be the last
    // non-platinum trophy.  We do a full scan to be certain (handles the case
    // where the session started with some already-unlocked from a previous run).
    if (AllNonPlatinumUnlocked())
        UnlockPS4Trophy(kPlatinumTrophyId);
}

#endif  // LIBERTY_RECOMP_PS4
