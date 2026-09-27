#include <catch2/catch_test_macros.hpp>

#include <string_view>
#include <vector>

#include "graphics/gta4_native/native_memory_samples.h"

namespace rex::graphics::gta4_native::memory {
namespace {

TEST_CASE("GTA IV native memory categories preserve host and GPU domains") {
  CHECK_FALSE(CategoryIsGpu(Category::kHostTexturePayloads));
  CHECK_FALSE(CategoryIsGpu(Category::kHostProfiler));
  CHECK(CategoryIsGpu(Category::kGpuTextureImages));
  CHECK(CategoryIsGpu(Category::kGpuNullResources));
  CHECK(std::string_view(CategoryName(Category::kGpuPostFxSplit)) == "gpu-postfx-split");
  CHECK(std::string_view(ProcessVmCategoryName(ProcessVmCategory::kMallocSmall)) == "malloc-small");
  CHECK(std::string_view(ProcessVmCategoryName(ProcessVmCategory::kIosurface)) == "iosurface");
}

TEST_CASE("GTA IV native memory series derives peaks and cumulative churn") {
  SnapshotSeries series;
  series.Begin(4);

  Snapshot first;
  first.categories[size_t(Category::kHostBufferPayloads)].live_bytes = 100;
  series.Push(first);

  Snapshot second;
  second.categories[size_t(Category::kHostBufferPayloads)].live_bytes = 160;
  series.Push(second);

  Snapshot third;
  third.categories[size_t(Category::kHostBufferPayloads)].live_bytes = 120;
  series.Push(third);

  Snapshot copied;
  REQUIRE(series.CopyOldest(2, &copied));
  const CategoryUsage& usage = copied.categories[size_t(Category::kHostBufferPayloads)];
  CHECK(usage.live_bytes == 120);
  CHECK(usage.peak_bytes == 160);
  CHECK(usage.cumulative_growth_bytes == 60);
  CHECK(usage.cumulative_shrink_bytes == 40);
}

TEST_CASE("GTA IV native memory series bounds long captures deterministically") {
  SnapshotSeries series;
  series.Begin(2);
  for (uint32_t frame = 1; frame <= 3; ++frame) {
    Snapshot sample;
    sample.submitted_frame = frame;
    series.Push(sample);
  }

  CHECK(series.size() == 2);
  CHECK(series.overwritten_samples() == 1);
  Snapshot copied;
  REQUIRE(series.CopyOldest(0, &copied));
  CHECK(copied.submitted_frame == 2);
  REQUIRE(series.CopyOldest(1, &copied));
  CHECK(copied.submitted_frame == 3);
  CHECK(copied.dropped_samples == 1);
}

TEST_CASE("GTA IV native memory delta events report changed categories only") {
  std::vector<Snapshot> samples(2);
  samples[0].sample_index = 7;
  samples[0].categories[size_t(Category::kGpuColorSurfaces)] = {100, 80, 2};
  samples[1].sample_index = 8;
  samples[1].submitted_frame = 44;
  samples[1].marker = 3;
  samples[1].categories[size_t(Category::kGpuColorSurfaces)] = {140, 120, 3};

  const std::vector<DeltaEvent> events = BuildDeltaEvents(samples);
  REQUIRE(events.size() == 1);
  CHECK(events[0].sample_index == 8);
  CHECK(events[0].submitted_frame == 44);
  CHECK(events[0].marker == 3);
  CHECK(events[0].category == Category::kGpuColorSurfaces);
  CHECK(events[0].byte_delta == 40);
  CHECK(events[0].count_delta == 1);
}

TEST_CASE("GTA IV native memory tracked totals do not mix allocation domains") {
  Snapshot sample;
  sample.process_physical_footprint_bytes = 1000;
  sample.process_malloc_reserved_bytes = 500;
  sample.process_vm_regions[size_t(ProcessVmCategory::kMallocSmall)].resident_bytes = 250;
  sample.categories[size_t(Category::kHostTexturePayloads)].live_bytes = 11;
  sample.categories[size_t(Category::kHostProfiler)].live_bytes = 7;
  sample.categories[size_t(Category::kGpuTextureImages)].live_bytes = 31;
  sample.categories[size_t(Category::kGpuUpload)].live_bytes = 13;
  CHECK(SumLiveBytes(sample, false) == 18);
  CHECK(SumLiveBytes(sample, true) == 44);
}

TEST_CASE("GTA IV native lifecycle events are bounded and ordered") {
  LifecycleEventSeries events;
  events.Begin(2);
  for (uint64_t generation = 1; generation <= 3; ++generation) {
    LifecycleEvent event;
    event.generation = generation;
    event.kind = ResourceKind::kBuffer;
    event.action = LifecycleAction::kCreate;
    events.Push(event);
  }
  CHECK(events.size() == 2);
  CHECK(events.overwritten_events() == 1);
  LifecycleEvent copied;
  REQUIRE(events.CopyOldest(0, &copied));
  CHECK(copied.event_index == 1);
  CHECK(copied.generation == 2);
  REQUIRE(events.CopyOldest(1, &copied));
  CHECK(copied.event_index == 2);
  CHECK(copied.generation == 3);
  CHECK(std::string_view(ResourceKindName(copied.kind)) == "buffer");
  CHECK(std::string_view(LifecycleActionName(copied.action)) == "create");
}

}  // namespace
}  // namespace rex::graphics::gta4_native::memory
