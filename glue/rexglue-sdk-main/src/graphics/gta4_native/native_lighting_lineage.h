#pragma once

#include <cstdint>
#include <unordered_map>

#include <rex/graphics/gta4_native/lighting_semantics.h>

#include "native_aspect_content.h"

namespace rex::graphics::gta4_native {

enum class NativeLightingBatchBoundary {
  kInternalFlush,
  kGuestPresent,
  kDeviceReset,
  kDiscardedRecording,
};

// Render-worker state, not command-buffer-local state. A texture lock may
// submit commands in the middle of a guest lighting occurrence.
class NativeLightingLineageLedger {
 public:
  struct Entry {
    LightingContext lighting{};
    NativeLightingAttachmentLineage attachment{};
  };

  const Entry* Find(uint64_t occurrence) const {
    const auto entry = entries_.find(occurrence);
    return entry == entries_.end() ? nullptr : &entry->second;
  }

  bool Matches(const LightingContext& contribution,
               const NativeLightingAttachmentLineage& attachment) const {
    const Entry* setup = Find(contribution.occurrence_id);
    return setup && IsSameLocalLightOccurrence(setup->lighting, contribution) &&
           NativeLightingAttachmentUnchanged(setup->attachment, attachment);
  }

  void RecordSetup(const LightingContext& setup,
                   const NativeLightingAttachmentLineage& attachment) {
    if (IsLocalStencilSetup(setup)) {
      entries_[setup.occurrence_id] = {setup, attachment};
    }
  }

  void RecordContribution(const LightingContext& contribution,
                          const NativeLightingAttachmentLineage& attachment) {
    const auto setup = entries_.find(contribution.occurrence_id);
    if (setup != entries_.end() &&
        IsSameLocalLightOccurrence(setup->second.lighting, contribution)) {
      setup->second.attachment = attachment;
    }
  }

  void OnBoundary(NativeLightingBatchBoundary boundary) {
    if (boundary != NativeLightingBatchBoundary::kInternalFlush) {
      entries_.clear();
    }
  }

 private:
  std::unordered_map<uint64_t, Entry> entries_;
};

}  // namespace rex::graphics::gta4_native
