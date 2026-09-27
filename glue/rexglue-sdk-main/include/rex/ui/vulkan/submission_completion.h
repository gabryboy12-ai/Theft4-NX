#pragma once

#include <cstdint>

namespace rex::ui::vulkan {

// Submission serials form a monotonic queue-completion timeline. Completion
// of a later queue submission also proves completion of every earlier
// submission on the same ordered queue.
constexpr bool SubmissionCompletionReached(uint64_t completed_submission,
                                           uint64_t requested_submission) {
  return completed_submission >= requested_submission;
}

}  // namespace rex::ui::vulkan
