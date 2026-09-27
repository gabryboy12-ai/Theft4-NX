#pragma once

#include <cstdint>
#include <type_traits>

namespace rex::graphics::gta4_native {

// Execution provenance is independent of optional diagnostic phase markers.
enum class RenderExecutionStage : uint32_t {
  kUnknown,
  kSceneToGBuffer,
  kDeferredLighting,
  kForwardLighting,
  kRadarMap,
  kCompositePostFx,
};

enum class LightPassRole : uint32_t {
  kNone,
  kGlobalParameterUpload,
  kGlobalContribution,
  kLocalStencilSetup,
  kLocalContribution,
  kAmbientVolume,
  kShaft,
  kCorona,
  kBrightLight,
  kSmokeBoard,
  kGlobalStencil,
  kDepthCopy,
  kReflectionBlur,
  kWaterFx,
  kForwardLightSet,
  kUnresolvedDeferredEffect,
};

enum class LightSourceKind : uint32_t {
  kUnknown,
  kPrimaryRecord,
  kSecondaryRecord,
  kGlobalCommand,
  kEffectBatch,
};

inline constexpr uint32_t kUnknownLightSelection = UINT32_MAX;

struct LightingContext {
  // Host execution occurrences, never durable physical/source identities.
  uint64_t occurrence_id = 0;
  uint64_t view_id = 0;
  RenderExecutionStage stage = RenderExecutionStage::kUnknown;
  LightPassRole role = LightPassRole::kNone;
  LightSourceKind source = LightSourceKind::kUnknown;
  // Verified consuming executor; no world/vehicle producer is inferred.
  uint32_t source_function = 0;
  uint32_t record_address = 0;
  uint32_t view_address = 0;
  uint32_t selector_caller = 0;
  uint32_t requested_selector = kUnknownLightSelection;
  uint32_t requested_mode = kUnknownLightSelection;
  // Actual guest technique/pass objects. Zero means no proved object.
  uint32_t effective_technique = 0;
  uint32_t effective_mode = kUnknownLightSelection;
  uint32_t pass = 0;
  // Set only when this local execution selected a stencil pass. Retail may
  // bypass stencil entirely; such contributions must not require a predecessor.
  uint32_t stencil_setup_expected = 0;
  uint32_t reserved = 0;

  bool operator==(const LightingContext&) const = default;
};

// sub_824F6208 (generated .32) chooses these families. Call-site provenance
// separately distinguishes ordinary local stencil from the global copy path.
constexpr LightPassRole DeferredLightPassRole(uint32_t selector) {
  switch (selector) {
    case 0:
    case 1:
      return LightPassRole::kGlobalContribution;
    case 3:
      return LightPassRole::kUnresolvedDeferredEffect;
    case 4:
    case 5:
    case 6:
    case 7:
    case 8:
    case 9:
    case 10:
    case 11:
    case 12:
    case 19:
    case 20:
      return LightPassRole::kLocalContribution;
    case 13:
      return LightPassRole::kShaft;
    case 14:
    case 15:
      return LightPassRole::kCorona;
    case 16:
      return LightPassRole::kBrightLight;
    case 17:
      return LightPassRole::kSmokeBoard;
    case 18:
      return LightPassRole::kAmbientVolume;
    case 21:
    case 22:
      return LightPassRole::kDepthCopy;
    case 23:
      return LightPassRole::kReflectionBlur;
    case 24:
      return LightPassRole::kWaterFx;
    default:
      // Null selector handles can fall back in sub_828C6568; retain execution.
      return LightPassRole::kUnresolvedDeferredEffect;
  }
}

constexpr bool RequiresFullLightingConstants(const LightingContext& context) {
  // Every selected deferred-effect family uses the same mutable effect state,
  // including coronas, shafts, water and copy/debug variants. Do not classify
  // by a shader nickname, diagnostic ID, or disabled color writes.
  return context.role != LightPassRole::kNone;
}

constexpr bool IsLocalStencilSetup(const LightingContext& context) {
  return context.stage == RenderExecutionStage::kDeferredLighting &&
         context.role == LightPassRole::kLocalStencilSetup &&
         context.source == LightSourceKind::kPrimaryRecord &&
         context.source_function == 0x822B0F08 && context.record_address != 0 &&
         context.occurrence_id != 0 && context.requested_selector == 3;
}

constexpr bool IsSameLocalLightOccurrence(const LightingContext& setup,
                                         const LightingContext& contribution) {
  return IsLocalStencilSetup(setup) &&
         contribution.role == LightPassRole::kLocalContribution &&
         contribution.stage == setup.stage && contribution.source == setup.source &&
         contribution.source_function == setup.source_function &&
         contribution.stencil_setup_expected != 0 &&
         contribution.record_address == setup.record_address &&
         contribution.occurrence_id == setup.occurrence_id && setup.view_id != 0 &&
         contribution.view_id == setup.view_id &&
         contribution.view_address == setup.view_address;
}

// sub_822B0F08 can skip stencil at 0x822B1364. Join only the immediately
// preceding stencil occurrence in this loop invocation, consume it once, and
// give stencil-free contributions a fresh occurrence supplied by the caller.
class LocalLightOccurrenceTracker {
 public:
  bool HasPending(uint32_t record, uint32_t view) const {
    return record && record == pending_record_ && view == pending_view_ &&
           pending_occurrence_ != 0;
  }

  uint64_t Select(LightPassRole role, uint32_t record, uint32_t view,
                  uint64_t fresh_occurrence) {
    if (role == LightPassRole::kLocalStencilSetup) {
      pending_record_ = record;
      pending_view_ = view;
      pending_occurrence_ = fresh_occurrence;
      return fresh_occurrence;
    }
    if (role == LightPassRole::kLocalContribution) {
      const uint64_t occurrence = HasPending(record, view) ? pending_occurrence_ : 0;
      pending_record_ = 0;
      pending_view_ = 0;
      pending_occurrence_ = 0;
      return occurrence ? occurrence : fresh_occurrence;
    }
    return fresh_occurrence;
  }

 private:
  uint32_t pending_record_ = 0;
  uint32_t pending_view_ = 0;
  uint64_t pending_occurrence_ = 0;
};

static_assert(std::is_trivially_copyable_v<LightingContext>);

}  // namespace rex::graphics::gta4_native
