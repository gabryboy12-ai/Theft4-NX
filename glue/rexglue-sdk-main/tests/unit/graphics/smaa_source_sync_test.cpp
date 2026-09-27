#include <catch2/catch_test_macros.hpp>

#include "graphics/gta4_native/smaa_source_sync.h"

namespace rex::graphics::gta4_native {
namespace {

TEST_CASE("SMAA reuses the published fragment-readable source") {
  const auto sync = GetSmaaSourceSynchronization(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  CHECK_FALSE(sync.needs_barrier);
  CHECK(sync.access == 0);
}

TEST_CASE("SMAA transitions wait only on known source producers") {
  auto sync = GetSmaaSourceSynchronization(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  CHECK(sync.needs_barrier);
  CHECK(sync.stages == VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
  CHECK((sync.access & VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT) != 0);

  sync = GetSmaaSourceSynchronization(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  CHECK(sync.needs_barrier);
  CHECK(sync.stages == VK_PIPELINE_STAGE_TRANSFER_BIT);
  CHECK(sync.access == VK_ACCESS_TRANSFER_WRITE_BIT);

  sync = GetSmaaSourceSynchronization(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  CHECK(sync.needs_barrier);
  CHECK(sync.stages == VK_PIPELINE_STAGE_TRANSFER_BIT);
  CHECK(sync.access == VK_ACCESS_TRANSFER_READ_BIT);
}

TEST_CASE("SMAA keeps conservative synchronization for an unknown general-layout writer") {
  const auto sync = GetSmaaSourceSynchronization(VK_IMAGE_LAYOUT_GENERAL);
  CHECK(sync.needs_barrier);
  CHECK(sync.stages == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  CHECK((sync.access & VK_ACCESS_MEMORY_WRITE_BIT) != 0);
}

}  // namespace
}  // namespace rex::graphics::gta4_native
