#ifndef REX_GRAPHICS_GTA4_NATIVE_NATIVE_MEMORY_SAMPLES_H_
#define REX_GRAPHICS_GTA4_NATIVE_NATIVE_MEMORY_SAMPLES_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rex::graphics::gta4_native::memory {

// Stable functional ownership categories. Host values are retained allocation
// capacities known to the renderer, while GPU values are VkDeviceMemory
// allocation sizes. Neither is silently treated as the process footprint.
enum class Category : uint8_t {
  kHostTexturePayloads,
  kHostTextureMetadata,
  kHostBufferPayloads,
  kHostVertexConversions,
  kHostIndexConversions,
  kHostCommandTransport,
  kHostDeviceSnapshots,
  kHostShadersAndDeclarations,
  kHostPipelineAndSamplerMetadata,
  kHostSurfaceAndPlacementMetadata,
  kHostProfiler,
  kGpuTextureImages,
  kGpuReflectionTextureImages,
  kGpuColorSurfaces,
  kGpuDepthSurfaces,
  kGpuReflectionSurfaces,
  kGpuDepthHandoffScratch,
  kGpuPostFxScene,
  kGpuPostFxSplit,
  kGpuSunShafts,
  kGpuSmaaExtent,
  kGpuSmaaLookup,
  kGpuSmaaStaging,
  kGpuUpload,
  kGpuPersistentGeometry,
  kGpuReadbackAndProbes,
  kGpuNullResources,
  kCount,
};

constexpr size_t kCategoryCount = size_t(Category::kCount);
constexpr size_t kMaximumSamples = 7200;
constexpr size_t kMaximumLifecycleEvents = 131072;
constexpr size_t kMaximumRetainedResources = 262144;

// macOS VM-region ownership tags reported independently from renderer-owned
// allocations. These categories overlap the process footprint and malloc-zone
// totals, so they are never included in tracked_host_bytes or
// tracked_gpu_bytes.
enum class ProcessVmCategory : uint8_t {
  kMallocSmall,
  kMallocLarge,
  kMallocTiny,
  kMallocNano,
  kMallocMedium,
  kMallocOther,
  kIosurface,
  kIokit,
  kStack,
  kUntaggedPrivate,
  kShared,
  kOther,
  kCount,
};

constexpr size_t kProcessVmCategoryCount = size_t(ProcessVmCategory::kCount);

struct ProcessVmRegionUsage {
  uint64_t virtual_bytes = 0;
  uint64_t resident_bytes = 0;
  uint64_t dirtied_bytes = 0;
  uint64_t swapped_bytes = 0;
  uint64_t region_count = 0;
};

struct CategoryUsage {
  // Bytes retained by the host allocator or allocated through VkDeviceMemory.
  uint64_t live_bytes = 0;
  // Useful payload bytes. This may be lower than live_bytes because vectors and
  // Vulkan allocations may retain spare capacity or alignment padding.
  uint64_t logical_bytes = 0;
  uint64_t live_count = 0;
  uint64_t peak_bytes = 0;
  uint64_t cumulative_growth_bytes = 0;
  uint64_t cumulative_shrink_bytes = 0;
};

struct Snapshot {
  uint64_t sample_index = 0;
  uint64_t host_tick = 0;
  uint64_t host_tick_frequency = 0;
  uint32_t submitted_frame = 0;
  uint32_t marker = 0;
  uint64_t process_physical_footprint_bytes = 0;
  uint64_t process_physical_footprint_peak_bytes = 0;
  uint64_t process_resident_bytes = 0;
  uint64_t process_resident_peak_bytes = 0;
  uint64_t process_virtual_bytes = 0;
  uint64_t process_internal_bytes = 0;
  uint64_t process_internal_peak_bytes = 0;
  uint64_t process_external_bytes = 0;
  uint64_t process_external_peak_bytes = 0;
  uint64_t process_reusable_bytes = 0;
  uint64_t process_reusable_peak_bytes = 0;
  uint64_t process_compressed_bytes = 0;
  uint64_t process_compressed_peak_bytes = 0;
  uint64_t process_device_bytes = 0;
  uint64_t process_device_peak_bytes = 0;
  uint64_t process_graphics_footprint_bytes = 0;
  uint64_t process_graphics_footprint_compressed_bytes = 0;
  uint64_t process_graphics_nofootprint_bytes = 0;
  uint64_t process_graphics_nofootprint_compressed_bytes = 0;
  uint64_t process_limit_bytes_remaining = 0;
  uint64_t process_region_count = 0;
  uint64_t process_page_size = 0;
  uint64_t process_malloc_blocks_in_use = 0;
  uint64_t process_malloc_in_use_bytes = 0;
  uint64_t process_malloc_peak_bytes = 0;
  uint64_t process_malloc_reserved_bytes = 0;
  uint64_t process_task_info_revision = 0;
  uint64_t process_vm_region_scan_complete = 0;
  uint64_t process_vm_region_scan_errors = 0;
  uint64_t process_vm_region_scan_host_tick = 0;
  uint64_t process_vm_region_scan_age_ticks = 0;
  uint64_t collection_duration_ticks = 0;
  uint64_t vulkan_heap_usage_bytes = 0;
  uint64_t vulkan_heap_budget_bytes = 0;
  uint64_t pending_texture_releases = 0;
  uint64_t pending_surface_releases = 0;
  uint64_t pipelines_live = 0;
  uint64_t samplers_live = 0;
  uint64_t descriptor_pools_live = 0;
  uint64_t frame_descriptor_draw_capacity = 0;
  uint64_t frame_descriptor_resolve_capacity = 0;
  uint64_t frame_descriptor_combined_set_capacity = 0;
  uint64_t frame_descriptor_draws_requested = 0;
  uint64_t frame_descriptor_unique_draws = 0;
  uint64_t frame_descriptor_sets_allocated = 0;
  uint64_t frame_descriptor_entries_written = 0;
  uint64_t frame_descriptor_pool_resets = 0;
  uint64_t frame_descriptor_pool_creates = 0;
  uint64_t command_pool_resets = 0;
  uint64_t buffer_cache_aged_count = 0;
  uint64_t buffer_cache_aged_bytes = 0;
  uint64_t vertex_conversion_aged_count = 0;
  uint64_t vertex_conversion_aged_bytes = 0;
  uint64_t maximum_vertex_variants_per_buffer = 0;
  uint64_t dropped_samples = 0;
  uint64_t dropped_events = 0;
  uint64_t dropped_retained = 0;
  std::array<ProcessVmRegionUsage, kProcessVmCategoryCount> process_vm_regions{};
  std::array<CategoryUsage, kCategoryCount> categories{};
};

struct DeltaEvent {
  uint64_t sample_index = 0;
  uint32_t submitted_frame = 0;
  uint32_t marker = 0;
  Category category = Category::kHostTexturePayloads;
  int64_t byte_delta = 0;
  int64_t count_delta = 0;
};

// Identity-level events are deliberately compact and numeric so capture can
// remain enabled for long leak investigations without adding strings or an
// allocation to the renderer hot path.
enum class ResourceKind : uint8_t {
  kBuffer,
  kVertexConversion,
  kIndexConversion,
  kTextureResource,
  kTextureImage,
  kColorSurface,
  kDepthSurface,
  kReflectionSurface,
  kPipeline,
  kSampler,
  kDescriptorPool,
  kCommandPool,
  kUploadBuffer,
  kPersistentBuffer,
  kCount,
};

enum class LifecycleAction : uint8_t {
  kBaseline,
  kCreate,
  kReplace,
  kReleaseRequested,
  kRetirePending,
  kEvict,
  kDestroy,
  kPoolReset,
  kPoolGrow,
  kCount,
};

enum class LifecycleReason : uint8_t {
  kNone,
  kCacheMiss,
  kContentChanged,
  kGuestRelease,
  kSuperseded,
  kUnused,
  kBudgetPressure,
  kAllocationRecovery,
  kCapacityGrowth,
  kFrameReuse,
  kShutdown,
  kCount,
};

struct LifecycleEvent {
  uint64_t event_index = 0;
  uint64_t host_tick = 0;
  uint64_t identity = 0;
  uint64_t generation = 0;
  uint64_t parent_identity = 0;
  uint64_t logical_bytes = 0;
  uint64_t retained_bytes = 0;
  uint64_t allocation_bytes = 0;
  uint32_t submitted_frame = 0;
  uint32_t marker = 0;
  uint32_t last_used_frame = 0;
  uint32_t auxiliary = 0;
  ResourceKind kind = ResourceKind::kBuffer;
  LifecycleAction action = LifecycleAction::kCreate;
  LifecycleReason reason = LifecycleReason::kNone;
};

static_assert(sizeof(LifecycleEvent) <= 96);

struct RetainedResource {
  uint64_t capture_index = 0;
  uint64_t host_tick = 0;
  uint64_t identity = 0;
  uint64_t generation = 0;
  uint64_t parent_identity = 0;
  uint64_t logical_bytes = 0;
  uint64_t retained_bytes = 0;
  uint64_t allocation_bytes = 0;
  uint32_t submitted_frame = 0;
  uint32_t marker = 0;
  uint32_t created_frame = 0;
  uint32_t last_used_frame = 0;
  uint32_t variant_count = 0;
  uint32_t auxiliary = 0;
  ResourceKind kind = ResourceKind::kBuffer;
  bool pending_release = false;
};

static_assert(sizeof(RetainedResource) <= 96);

class LifecycleEventSeries {
 public:
  void Begin(size_t capacity = kMaximumLifecycleEvents);
  void Clear();
  void Push(LifecycleEvent event);

  size_t size() const { return size_; }
  size_t capacity() const { return events_.size(); }
  uint64_t overwritten_events() const { return overwritten_events_; }
  bool CopyOldest(size_t offset, LifecycleEvent* event) const;

 private:
  std::vector<LifecycleEvent> events_;
  size_t head_ = 0;
  size_t size_ = 0;
  uint64_t next_event_index_ = 0;
  uint64_t overwritten_events_ = 0;
};

// Bounded capture storage. Push derives peaks and cumulative growth/shrink so
// category changes remain useful even when the oldest time-series rows roll
// out of a very long manual capture.
class SnapshotSeries {
 public:
  void Begin(size_t capacity = kMaximumSamples);
  void Clear();
  void Push(Snapshot snapshot);

  size_t size() const { return size_; }
  size_t capacity() const { return samples_.size(); }
  bool empty() const { return size_ == 0; }
  uint64_t overwritten_samples() const { return overwritten_samples_; }
  bool CopyOldest(size_t offset, Snapshot* snapshot) const;

 private:
  std::vector<Snapshot> samples_;
  Snapshot last_snapshot_{};
  std::array<uint64_t, kCategoryCount> peaks_{};
  std::array<uint64_t, kCategoryCount> growth_{};
  std::array<uint64_t, kCategoryCount> shrink_{};
  size_t head_ = 0;
  size_t size_ = 0;
  uint64_t next_sample_index_ = 0;
  uint64_t overwritten_samples_ = 0;
  bool has_last_snapshot_ = false;
};

const char* CategoryName(Category category);
const char* ProcessVmCategoryName(ProcessVmCategory category);
const char* ResourceKindName(ResourceKind kind);
const char* LifecycleActionName(LifecycleAction action);
const char* LifecycleReasonName(LifecycleReason reason);
bool CategoryIsGpu(Category category);
uint64_t SumLiveBytes(const Snapshot& snapshot, bool gpu);
std::vector<DeltaEvent> BuildDeltaEvents(const std::vector<Snapshot>& samples);

}  // namespace rex::graphics::gta4_native::memory

#endif  // REX_GRAPHICS_GTA4_NATIVE_NATIVE_MEMORY_SAMPLES_H_
