#include <rex/ui/vulkan/submission_completion.h>

#include <catch2/catch_test_macros.hpp>

namespace vulkan = rex::ui::vulkan;

TEST_CASE("Vulkan submission completion accepts the exact requested serial") {
  CHECK(vulkan::SubmissionCompletionReached(7, 7));
}

TEST_CASE("Vulkan submission completion accepts a surpassed requested serial") {
  CHECK(vulkan::SubmissionCompletionReached(8, 7));
}

TEST_CASE("Vulkan submission completion rejects a pending requested serial") {
  CHECK_FALSE(vulkan::SubmissionCompletionReached(6, 7));
}

TEST_CASE("Vulkan submission zero is the never-referenced completed sentinel") {
  CHECK(vulkan::SubmissionCompletionReached(0, 0));
  CHECK(vulkan::SubmissionCompletionReached(1, 0));
}
