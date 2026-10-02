// switch-smoke step 7: what Mesa's NVK (mesa-switch libvulkan.a in portlibs)
// can do on this console, before the renderer is built
// (docs/switch-port/11-vulkan-probe.md).
//
//   a) vkCreateInstance, the physical devices, API version, driver name and
//      version;
//   b) every instance and device extension, the features ReXGlue's Vulkan
//      backend asks for (and the ones nfsmw-nx works around), the limits,
//      the memory heaps and types (with VK_EXT_memory_budget if present);
//   c) a VI surface on the default NWindow (the console is closed for it:
//      both cannot own the window), a swapchain, and 5 s of frames cleared
//      to a changing colour, with a small square drawn by a test pipeline;
//   d) the time to create a simple graphics pipeline cold, from a filled
//      VkPipelineCache, from that cache's data loaded into a new cache, and
//      through the driver's own cache; the first vkQueueSubmit with a fence;
//   e) everything destroyed in order, then "VULKAN OK" or "VULKAN FAIL at
//      <operation>: <VkResult>".
//
// Every risky call is preceded by an "about to" line, durable before the
// call (Report fsyncs each line), so a crash leaves the operation that
// crashed as the last line of smoke.log. Mesa's own shader cache on the SD
// card is disabled (MESA_SHADER_CACHE_DISABLE), as nfsmw-nx does, so the
// cold time is cold.

#include "vulkan_probe.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <switch.h>

#define VK_USE_PLATFORM_VI_NN
#include <vulkan/vulkan.h>

#include "probes.h"

// The SDK's fullscreen triangle vertex shader (glslang output, no inputs, no
// descriptors): the vertex side of the test pipeline.
#include "../../glue/rexglue-sdk-main/src/graphics/shaders/vulkan_spirv/fullscreen_cw_vs.h"

namespace {

// layout(location = 0) out vec4 color; void main() { color = vec4(1, 0, 1, 1); }
// assembled by hand (no shader compiler on the build machine): magenta.
const uint32_t kMagentaFs[] = {
    0x07230203, 0x00010000, 0x00000000, 0x0000000C, 0x00000000, 0x00020011, 0x00000001, 0x0003000E,
    0x00000000, 0x00000001, 0x0006000F, 0x00000004, 0x00000001, 0x6E69616D, 0x00000000, 0x00000002,
    0x00030010, 0x00000001, 0x00000007, 0x00040047, 0x00000002, 0x0000001E, 0x00000000, 0x00020013,
    0x00000003, 0x00030021, 0x00000004, 0x00000003, 0x00030016, 0x00000005, 0x00000020, 0x00040017,
    0x00000006, 0x00000005, 0x00000004, 0x00040020, 0x00000007, 0x00000003, 0x00000006, 0x0004003B,
    0x00000007, 0x00000002, 0x00000003, 0x0004002B, 0x00000005, 0x00000008, 0x3F800000, 0x0004002B,
    0x00000005, 0x00000009, 0x00000000, 0x0007002C, 0x00000006, 0x0000000A, 0x00000008, 0x00000009,
    0x00000008, 0x00000008, 0x00050036, 0x00000003, 0x00000001, 0x00000000, 0x00000004, 0x000200F8,
    0x0000000B, 0x0003003E, 0x00000002, 0x0000000A, 0x000100FD, 0x00010038,
};

const char* ResultName(VkResult r) {
  switch (r) {
#define R(x) \
  case x:    \
    return #x;
    R(VK_SUCCESS)
    R(VK_NOT_READY)
    R(VK_TIMEOUT)
    R(VK_EVENT_SET)
    R(VK_EVENT_RESET)
    R(VK_INCOMPLETE)
    R(VK_ERROR_OUT_OF_HOST_MEMORY)
    R(VK_ERROR_OUT_OF_DEVICE_MEMORY)
    R(VK_ERROR_INITIALIZATION_FAILED)
    R(VK_ERROR_DEVICE_LOST)
    R(VK_ERROR_MEMORY_MAP_FAILED)
    R(VK_ERROR_LAYER_NOT_PRESENT)
    R(VK_ERROR_EXTENSION_NOT_PRESENT)
    R(VK_ERROR_FEATURE_NOT_PRESENT)
    R(VK_ERROR_INCOMPATIBLE_DRIVER)
    R(VK_ERROR_TOO_MANY_OBJECTS)
    R(VK_ERROR_FORMAT_NOT_SUPPORTED)
    R(VK_ERROR_FRAGMENTED_POOL)
    R(VK_ERROR_UNKNOWN)
    R(VK_ERROR_SURFACE_LOST_KHR)
    R(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR)
    R(VK_SUBOPTIMAL_KHR)
    R(VK_ERROR_OUT_OF_DATE_KHR)
    R(VK_ERROR_VALIDATION_FAILED_EXT)
#undef R
    default:
      return "?";
  }
}

u64 NowNs() {
  return armTicksToNs(armGetSystemTick());
}

double Ms(u64 ns) {
  return double(ns) / 1e6;
}

// Names packed several to a line (one durable write per line).
void LogNames(const char* title, const std::vector<std::string>& names) {
  SMOKE_INFO("  %s (%zu):", title, names.size());
  std::string line;
  for (const std::string& n : names) {
    if (!line.empty() && line.size() + n.size() > 150) {
      SMOKE_INFO("    %s", line.c_str());
      line.clear();
    }
    if (!line.empty()) line += ' ';
    line += n;
  }
  if (!line.empty()) SMOKE_INFO("    %s", line.c_str());
}

bool Has(const std::vector<std::string>& names, const char* name) {
  for (const std::string& n : names) {
    if (n == name) return true;
  }
  return false;
}

class Probe {
 public:
  Probe(void (*close_console)(), void (*reopen_console)())
      : close_console_(close_console), reopen_console_(reopen_console) {}
  ~Probe() { Destroy(); }

  bool Run();
  const std::string& failure() const { return failure_; }

 private:
  // Logs the failure and returns false: VK(Check(...)) at each call.
  bool Check(const char* what, VkResult r) {
    if (r == VK_SUCCESS) return true;
    failure_ = std::string(what) + ": VkResult " + std::to_string(int(r)) + " (" +
               ResultName(r) + ")";
    Report(true, "  FAILED %s", failure_.c_str());
    return false;
  }
  bool Fail(const char* what) {
    failure_ = what;
    Report(true, "  FAILED %s", what);
    return false;
  }

  bool StepInstance();
  bool StepDevice();
  void LogFeatures();
  void LogLimitsAndMemory();
  bool StepSurface();
  bool StepPipelines();
  bool StepFirstSubmit();
  bool StepFrames();
  void Destroy();
  bool CreatePipeline(VkPipelineCache cache, VkPipeline* out, double* ms);

  void (*close_console_)();
  void (*reopen_console_)();
  bool console_closed_ = false;
  std::string failure_;

  VkInstance instance_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties props_{};
  std::vector<std::string> instance_exts_;
  std::vector<std::string> device_exts_;
  uint32_t queue_family_ = 0;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkFormat format_ = VK_FORMAT_UNDEFINED;
  VkExtent2D extent_{};
  std::vector<VkImage> images_;
  std::vector<VkImageView> views_;
  std::vector<VkFramebuffer> framebuffers_;
  VkRenderPass render_pass_ = VK_NULL_HANDLE;
  VkShaderModule vs_ = VK_NULL_HANDLE;
  VkShaderModule fs_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  std::vector<VkPipeline> extra_pipelines_;
  std::vector<VkPipelineCache> caches_;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  static constexpr int kFrames = 2;
  VkCommandBuffer cmd_[kFrames] = {};
  VkFence fence_[kFrames] = {};
  VkSemaphore acquired_[kFrames] = {};
  std::vector<VkSemaphore> rendered_;  // one per swapchain image
};

bool Probe::Run() {
  // Mesa's disk cache on the SD card would make the cold pipeline warm on the
  // second run, and costs a thread with an 8 MiB stack (nfsmw-nx).
  setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
  return StepInstance() && StepDevice() && StepSurface() && StepPipelines() &&
         StepFirstSubmit() && StepFrames();
}

// ── a) instance and devices ────────────────────────────────────────────────

bool Probe::StepInstance() {
  uint32_t api = 0;
  vkEnumerateInstanceVersion(&api);
  SMOKE_INFO("  loader instance version %u.%u.%u", VK_API_VERSION_MAJOR(api),
             VK_API_VERSION_MINOR(api), VK_API_VERSION_PATCH(api));
  uint32_t n = 0;
  if (!Check("vkEnumerateInstanceExtensionProperties", vkEnumerateInstanceExtensionProperties(
                                                          nullptr, &n, nullptr))) {
    return false;
  }
  std::vector<VkExtensionProperties> exts(n);
  vkEnumerateInstanceExtensionProperties(nullptr, &n, exts.data());
  for (const auto& e : exts) instance_exts_.emplace_back(e.extensionName);
  LogNames("instance extensions", instance_exts_);
  uint32_t layers = 0;
  vkEnumerateInstanceLayerProperties(&layers, nullptr);
  SMOKE_INFO("  instance layers: %u", layers);

  std::vector<const char*> enable;
  for (const char* want : {VK_KHR_SURFACE_EXTENSION_NAME, VK_NN_VI_SURFACE_EXTENSION_NAME,
                           VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME}) {
    if (Has(instance_exts_, want)) enable.push_back(want);
  }
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "theft4-switch-smoke";
  app.apiVersion = api ? api : VK_API_VERSION_1_0;
  VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ci.pApplicationInfo = &app;
  ci.enabledExtensionCount = uint32_t(enable.size());
  ci.ppEnabledExtensionNames = enable.data();
  SMOKE_INFO("  about to vkCreateInstance (API %u.%u, %zu extensions)", VK_API_VERSION_MAJOR(api),
             VK_API_VERSION_MINOR(api), enable.size());
  const u64 t0 = NowNs();
  if (!Check("vkCreateInstance", vkCreateInstance(&ci, nullptr, &instance_))) return false;
  SMOKE_INFO("  vkCreateInstance: %.2f ms", Ms(NowNs() - t0));

  n = 0;
  SMOKE_INFO("  about to vkEnumeratePhysicalDevices");
  if (!Check("vkEnumeratePhysicalDevices", vkEnumeratePhysicalDevices(instance_, &n, nullptr))) {
    return false;
  }
  if (n == 0) return Fail("vkEnumeratePhysicalDevices: no device");
  std::vector<VkPhysicalDevice> devices(n);
  vkEnumeratePhysicalDevices(instance_, &n, devices.data());
  physical_ = devices[0];
  for (uint32_t i = 0; i < n; ++i) {
    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &driver;
    vkGetPhysicalDeviceProperties2(devices[i], &p2);
    const auto& p = p2.properties;
    SMOKE_INFO("  device %u: '%s', type %d, API %u.%u.%u, driver version 0x%X, vendor 0x%04X, "
               "device 0x%04X",
               i, p.deviceName, int(p.deviceType), VK_API_VERSION_MAJOR(p.apiVersion),
               VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion),
               p.driverVersion, p.vendorID, p.deviceID);
    SMOKE_INFO("    driver id %d, name '%s', info '%s', conformance %u.%u.%u.%u", int(driver.driverID),
               driver.driverName, driver.driverInfo, driver.conformanceVersion.major,
               driver.conformanceVersion.minor, driver.conformanceVersion.subminor,
               driver.conformanceVersion.patch);
    if (i == 0) props_ = p;
  }
  return true;
}

// ── b) device: extensions, features, limits, memory ────────────────────────

bool Probe::StepDevice() {
  uint32_t n = 0;
  vkEnumerateDeviceExtensionProperties(physical_, nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> exts(n);
  vkEnumerateDeviceExtensionProperties(physical_, nullptr, &n, exts.data());
  for (const auto& e : exts) device_exts_.emplace_back(e.extensionName);
  LogNames("device extensions", device_exts_);
  LogFeatures();
  LogLimitsAndMemory();

  uint32_t families = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical_, &families, nullptr);
  std::vector<VkQueueFamilyProperties> qf(families);
  vkGetPhysicalDeviceQueueFamilyProperties(physical_, &families, qf.data());
  bool found = false;
  for (uint32_t i = 0; i < families; ++i) {
    SMOKE_INFO("  queue family %u: flags 0x%X, %u queues, timestamp bits %u", i, qf[i].queueFlags,
               qf[i].queueCount, qf[i].timestampValidBits);
    if (!found && (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
      queue_family_ = i;
      found = true;
    }
  }
  if (!found) return Fail("no graphics queue family");

  const float priority = 1.0f;
  VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  q.queueFamilyIndex = queue_family_;
  q.queueCount = 1;
  q.pQueuePriorities = &priority;
  std::vector<const char*> enable;
  if (Has(device_exts_, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
    enable.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  }
  VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  ci.queueCreateInfoCount = 1;
  ci.pQueueCreateInfos = &q;
  ci.enabledExtensionCount = uint32_t(enable.size());
  ci.ppEnabledExtensionNames = enable.data();
  SMOKE_INFO("  about to vkCreateDevice (queue family %u, swapchain %s)", queue_family_,
             enable.empty() ? "NOT supported" : "enabled");
  const u64 t0 = NowNs();
  if (!Check("vkCreateDevice", vkCreateDevice(physical_, &ci, nullptr, &device_))) return false;
  SMOKE_INFO("  vkCreateDevice: %.2f ms", Ms(NowNs() - t0));
  vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
  if (enable.empty()) return Fail("VK_KHR_swapchain not supported");
  return true;
}

void Probe::LogFeatures() {
  VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
  VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT fsi{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
  VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT roaa{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT};
  VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
  VkPhysicalDeviceRobustness2FeaturesEXT rob2{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
  VkPhysicalDeviceDepthClipControlFeaturesEXT dcc{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
  VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT nscm{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NON_SEAMLESS_CUBE_MAP_FEATURES_EXT};

  // Core 1.1-1.3 structures only up to the device's version; extension
  // structures only when the extension is listed.
  VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  void** next = &f2.pNext;
  auto chain = [&next](void* s, void** s_next) {
    *next = s;
    next = s_next;
  };
  const uint32_t api = props_.apiVersion;
  if (api >= VK_API_VERSION_1_2) {
    chain(&f11, &f11.pNext);
    chain(&f12, &f12.pNext);
  }
  if (api >= VK_API_VERSION_1_3) chain(&f13, &f13.pNext);
  const bool has_fsi = Has(device_exts_, "VK_EXT_fragment_shader_interlock");
  const bool has_roaa = Has(device_exts_, "VK_EXT_rasterization_order_attachment_access") ||
                        Has(device_exts_, "VK_ARM_rasterization_order_attachment_access");
  const bool has_cbc = Has(device_exts_, "VK_EXT_custom_border_color");
  const bool has_rob2 = Has(device_exts_, "VK_EXT_robustness2") ||
                        Has(device_exts_, "VK_KHR_robustness2");
  const bool has_dcc = Has(device_exts_, "VK_EXT_depth_clip_control");
  const bool has_nscm = Has(device_exts_, "VK_EXT_non_seamless_cube_map");
  if (has_fsi) chain(&fsi, &fsi.pNext);
  if (has_roaa) chain(&roaa, &roaa.pNext);
  if (has_cbc) chain(&cbc, &cbc.pNext);
  if (has_rob2) chain(&rob2, &rob2.pNext);
  if (has_dcc) chain(&dcc, &dcc.pNext);
  if (has_nscm) chain(&nscm, &nscm.pNext);
  vkGetPhysicalDeviceFeatures2(physical_, &f2);
  const VkPhysicalDeviceFeatures& f = f2.features;

  struct Item {
    const char* name;
    int value;  // 1 yes, 0 no, -1 not queryable (extension absent)
  };
  auto ext = [](bool present, VkBool32 v) { return present ? int(v) : -1; };
  const Item items[] = {
      // VkPhysicalDeviceFeatures ReXGlue enables (vulkan_device.cpp), and more.
      {"robustBufferAccess", int(f.robustBufferAccess)},
      {"fullDrawIndexUint32", int(f.fullDrawIndexUint32)},
      {"imageCubeArray", int(f.imageCubeArray)},
      {"independentBlend", int(f.independentBlend)},
      {"geometryShader", int(f.geometryShader)},
      {"tessellationShader", int(f.tessellationShader)},
      {"sampleRateShading", int(f.sampleRateShading)},
      {"dualSrcBlend", int(f.dualSrcBlend)},
      {"logicOp", int(f.logicOp)},
      {"multiDrawIndirect", int(f.multiDrawIndirect)},
      {"drawIndirectFirstInstance", int(f.drawIndirectFirstInstance)},
      {"depthClamp", int(f.depthClamp)},
      {"depthBiasClamp", int(f.depthBiasClamp)},
      {"fillModeNonSolid", int(f.fillModeNonSolid)},
      {"depthBounds", int(f.depthBounds)},
      {"wideLines", int(f.wideLines)},
      {"largePoints", int(f.largePoints)},
      {"alphaToOne", int(f.alphaToOne)},
      {"multiViewport", int(f.multiViewport)},
      {"samplerAnisotropy", int(f.samplerAnisotropy)},
      {"textureCompressionBC", int(f.textureCompressionBC)},
      {"textureCompressionETC2", int(f.textureCompressionETC2)},
      {"textureCompressionASTC_LDR", int(f.textureCompressionASTC_LDR)},
      {"occlusionQueryPrecise", int(f.occlusionQueryPrecise)},
      {"pipelineStatisticsQuery", int(f.pipelineStatisticsQuery)},
      {"vertexPipelineStoresAndAtomics", int(f.vertexPipelineStoresAndAtomics)},
      {"fragmentStoresAndAtomics", int(f.fragmentStoresAndAtomics)},
      {"shaderStorageImageExtendedFormats", int(f.shaderStorageImageExtendedFormats)},
      {"shaderStorageImageWriteWithoutFormat", int(f.shaderStorageImageWriteWithoutFormat)},
      {"shaderClipDistance", int(f.shaderClipDistance)},
      {"shaderCullDistance", int(f.shaderCullDistance)},
      {"shaderFloat64", int(f.shaderFloat64)},
      {"shaderInt64", int(f.shaderInt64)},
      {"shaderInt16", int(f.shaderInt16)},
      {"sparseBinding", int(f.sparseBinding)},
      {"sparseResidencyBuffer", int(f.sparseResidencyBuffer)},
      {"sparseResidencyImage2D", int(f.sparseResidencyImage2D)},
      // 1.2 / 1.3
      {"samplerMirrorClampToEdge", api >= VK_API_VERSION_1_2 ? int(f12.samplerMirrorClampToEdge) : -1},
      {"drawIndirectCount", api >= VK_API_VERSION_1_2 ? int(f12.drawIndirectCount) : -1},
      {"shaderFloat16", api >= VK_API_VERSION_1_2 ? int(f12.shaderFloat16) : -1},
      {"descriptorIndexing", api >= VK_API_VERSION_1_2 ? int(f12.descriptorIndexing) : -1},
      {"runtimeDescriptorArray", api >= VK_API_VERSION_1_2 ? int(f12.runtimeDescriptorArray) : -1},
      {"descriptorBindingPartiallyBound",
       api >= VK_API_VERSION_1_2 ? int(f12.descriptorBindingPartiallyBound) : -1},
      {"descriptorBindingSampledImageUpdateAfterBind",
       api >= VK_API_VERSION_1_2 ? int(f12.descriptorBindingSampledImageUpdateAfterBind) : -1},
      {"descriptorBindingVariableDescriptorCount",
       api >= VK_API_VERSION_1_2 ? int(f12.descriptorBindingVariableDescriptorCount) : -1},
      {"scalarBlockLayout", api >= VK_API_VERSION_1_2 ? int(f12.scalarBlockLayout) : -1},
      {"uniformBufferStandardLayout",
       api >= VK_API_VERSION_1_2 ? int(f12.uniformBufferStandardLayout) : -1},
      {"timelineSemaphore", api >= VK_API_VERSION_1_2 ? int(f12.timelineSemaphore) : -1},
      {"bufferDeviceAddress", api >= VK_API_VERSION_1_2 ? int(f12.bufferDeviceAddress) : -1},
      {"shaderDemoteToHelperInvocation",
       api >= VK_API_VERSION_1_3 ? int(f13.shaderDemoteToHelperInvocation) : -1},
      {"dynamicRendering", api >= VK_API_VERSION_1_3 ? int(f13.dynamicRendering) : -1},
      {"synchronization2", api >= VK_API_VERSION_1_3 ? int(f13.synchronization2) : -1},
      {"maintenance4", api >= VK_API_VERSION_1_3 ? int(f13.maintenance4) : -1},
      // Extensions
      {"fragmentShaderSampleInterlock", ext(has_fsi, fsi.fragmentShaderSampleInterlock)},
      {"fragmentShaderPixelInterlock", ext(has_fsi, fsi.fragmentShaderPixelInterlock)},
      {"fragmentShaderShadingRateInterlock", ext(has_fsi, fsi.fragmentShaderShadingRateInterlock)},
      {"rasterizationOrderColorAttachmentAccess",
       ext(has_roaa, roaa.rasterizationOrderColorAttachmentAccess)},
      {"rasterizationOrderDepthAttachmentAccess",
       ext(has_roaa, roaa.rasterizationOrderDepthAttachmentAccess)},
      {"customBorderColors", ext(has_cbc, cbc.customBorderColors)},
      {"customBorderColorWithoutFormat", ext(has_cbc, cbc.customBorderColorWithoutFormat)},
      {"nullDescriptor (robustness2)", ext(has_rob2, rob2.nullDescriptor)},
      {"depthClipControl", ext(has_dcc, dcc.depthClipControl)},
      {"nonSeamlessCubeMap", ext(has_nscm, nscm.nonSeamlessCubeMap)},
  };
  std::vector<std::string> yes, no, absent;
  for (const Item& i : items) {
    (i.value > 0 ? yes : i.value == 0 ? no : absent).emplace_back(i.name);
  }
  LogNames("features YES", yes);
  LogNames("features NO", no);
  LogNames("features not queryable (extension absent)", absent);
}

void Probe::LogLimitsAndMemory() {
  const VkPhysicalDeviceLimits& l = props_.limits;
  SMOKE_INFO("  limits: maxImageDimension2D %u, 3D %u, Cube %u, maxImageArrayLayers %u",
             l.maxImageDimension2D, l.maxImageDimension3D, l.maxImageDimensionCube,
             l.maxImageArrayLayers);
  SMOKE_INFO("  limits: maxStorageBufferRange %u, maxUniformBufferRange %u, maxPushConstantsSize %u, "
             "maxMemoryAllocationCount %u, maxSamplerAllocationCount %u",
             l.maxStorageBufferRange, l.maxUniformBufferRange, l.maxPushConstantsSize,
             l.maxMemoryAllocationCount, l.maxSamplerAllocationCount);
  SMOKE_INFO("  limits: maxPerStageDescriptor Samplers %u, UniformBuffers %u, StorageBuffers %u, "
             "SampledImages %u, StorageImages %u, InputAttachments %u, maxPerStageResources %u",
             l.maxPerStageDescriptorSamplers, l.maxPerStageDescriptorUniformBuffers,
             l.maxPerStageDescriptorStorageBuffers, l.maxPerStageDescriptorSampledImages,
             l.maxPerStageDescriptorStorageImages, l.maxPerStageDescriptorInputAttachments,
             l.maxPerStageResources);
  SMOKE_INFO("  limits: maxDescriptorSet Samplers %u, SampledImages %u, StorageBuffers %u; "
             "maxBoundDescriptorSets %u",
             l.maxDescriptorSetSamplers, l.maxDescriptorSetSampledImages,
             l.maxDescriptorSetStorageBuffers, l.maxBoundDescriptorSets);
  SMOKE_INFO("  limits: maxVertexInputAttributes %u, bindings %u, maxVertexOutputComponents %u, "
             "maxFragmentInputComponents %u, maxColorAttachments %u, maxFramebuffer %ux%u",
             l.maxVertexInputAttributes, l.maxVertexInputBindings, l.maxVertexOutputComponents,
             l.maxFragmentInputComponents, l.maxColorAttachments, l.maxFramebufferWidth,
             l.maxFramebufferHeight);
  SMOKE_INFO("  limits: maxComputeSharedMemorySize %u, maxComputeWorkGroupInvocations %u, "
             "timestampPeriod %.3f ns, min UBO/SSBO offset alignment %" PRIu64 "/%" PRIu64
             ", nonCoherentAtomSize %" PRIu64 ", sample counts color 0x%X depth 0x%X",
             l.maxComputeSharedMemorySize, l.maxComputeWorkGroupInvocations,
             double(l.timestampPeriod), uint64_t(l.minUniformBufferOffsetAlignment),
             uint64_t(l.minStorageBufferOffsetAlignment), uint64_t(l.nonCoherentAtomSize),
             l.framebufferColorSampleCounts, l.framebufferDepthSampleCounts);

  VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
  VkPhysicalDeviceMemoryProperties2 m2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
  const bool has_budget = Has(device_exts_, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
  if (has_budget) m2.pNext = &budget;
  vkGetPhysicalDeviceMemoryProperties2(physical_, &m2);
  const auto& m = m2.memoryProperties;
  for (uint32_t i = 0; i < m.memoryHeapCount; ++i) {
    if (has_budget) {
      SMOKE_INFO("  memory heap %u: %" PRIu64 " MiB, flags 0x%X; budget %" PRIu64
                 " MiB, usage %" PRIu64 " MiB",
                 i, uint64_t(m.memoryHeaps[i].size >> 20), m.memoryHeaps[i].flags,
                 uint64_t(budget.heapBudget[i] >> 20), uint64_t(budget.heapUsage[i] >> 20));
    } else {
      SMOKE_INFO("  memory heap %u: %" PRIu64 " MiB, flags 0x%X (no VK_EXT_memory_budget)", i,
                 uint64_t(m.memoryHeaps[i].size >> 20), m.memoryHeaps[i].flags);
    }
  }
  for (uint32_t i = 0; i < m.memoryTypeCount; ++i) {
    const VkMemoryPropertyFlags p = m.memoryTypes[i].propertyFlags;
    SMOKE_INFO("  memory type %u: heap %u, flags 0x%X%s%s%s%s", i, m.memoryTypes[i].heapIndex, p,
               (p & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? " DEVICE_LOCAL" : "",
               (p & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? " HOST_VISIBLE" : "",
               (p & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? " HOST_COHERENT" : "",
               (p & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? " HOST_CACHED" : "");
  }
}

// ── c) surface and swapchain ───────────────────────────────────────────────

bool Probe::StepSurface() {
  if (!Has(instance_exts_, VK_NN_VI_SURFACE_EXTENSION_NAME)) {
    return Fail("VK_NN_vi_surface not supported");
  }
  NWindow* win = nwindowGetDefault();
  if (!win || !nwindowIsValid(win)) return Fail("no default NWindow");
  SMOKE_INFO("  about to close the console: the default NWindow goes to Vulkan; until the console "
             "reopens, lines go to smoke.log only");
  close_console_();
  console_closed_ = true;
  u32 w = 0, h = 0;
  nwindowGetDimensions(win, &w, &h);
  SMOKE_INFO("  default NWindow %ux%u", w, h);

  auto create_vi = reinterpret_cast<PFN_vkCreateViSurfaceNN>(
      vkGetInstanceProcAddr(instance_, "vkCreateViSurfaceNN"));
  if (!create_vi) return Fail("vkGetInstanceProcAddr(vkCreateViSurfaceNN) = null");
  VkViSurfaceCreateInfoNN si{VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN};
  si.window = win;
  SMOKE_INFO("  about to vkCreateViSurfaceNN");
  if (!Check("vkCreateViSurfaceNN", create_vi(instance_, &si, nullptr, &surface_))) return false;

  VkBool32 supported = VK_FALSE;
  vkGetPhysicalDeviceSurfaceSupportKHR(physical_, queue_family_, surface_, &supported);
  VkSurfaceCapabilitiesKHR caps{};
  if (!Check("vkGetPhysicalDeviceSurfaceCapabilitiesKHR",
             vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps))) {
    return false;
  }
  SMOKE_INFO("  surface: present support %u, images %u-%u, current extent %ux%u, min %ux%u, max "
             "%ux%u, transforms 0x%X, composite alpha 0x%X, usage 0x%X",
             supported, caps.minImageCount, caps.maxImageCount, caps.currentExtent.width,
             caps.currentExtent.height, caps.minImageExtent.width, caps.minImageExtent.height,
             caps.maxImageExtent.width, caps.maxImageExtent.height, caps.supportedTransforms,
             caps.supportedCompositeAlpha, caps.supportedUsageFlags);
  uint32_t n = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &n, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(n);
  vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &n, formats.data());
  std::string fl;
  for (const auto& f : formats) {
    char item[40];
    std::snprintf(item, sizeof(item), " %d/%d", int(f.format), int(f.colorSpace));
    fl += item;
  }
  SMOKE_INFO("  surface formats (format/colour space):%s", fl.c_str());
  n = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &n, nullptr);
  std::vector<VkPresentModeKHR> modes(n);
  vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &n, modes.data());
  std::string ml;
  for (VkPresentModeKHR m : modes) {
    ml += m == VK_PRESENT_MODE_IMMEDIATE_KHR      ? " IMMEDIATE"
          : m == VK_PRESENT_MODE_MAILBOX_KHR      ? " MAILBOX"
          : m == VK_PRESENT_MODE_FIFO_KHR         ? " FIFO"
          : m == VK_PRESENT_MODE_FIFO_RELAXED_KHR ? " FIFO_RELAXED"
                                                  : " other(" + std::to_string(int(m)) + ")";
  }
  SMOKE_INFO("  present modes:%s", ml.c_str());
  if (formats.empty()) return Fail("no surface format");

  VkSurfaceFormatKHR chosen = formats[0];
  for (const auto& f : formats) {
    if (f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_UNORM) {
      chosen = f;
      break;
    }
  }
  format_ = chosen.format;
  extent_ = caps.currentExtent.width != 0xFFFFFFFFu ? caps.currentExtent : VkExtent2D{w, h};
  uint32_t images = 3;  // nfsmw-nx: exactly three on this WSI
  if (images < caps.minImageCount) images = caps.minImageCount;
  if (caps.maxImageCount && images > caps.maxImageCount) images = caps.maxImageCount;

  VkSwapchainCreateInfoKHR sc{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  sc.surface = surface_;
  sc.minImageCount = images;
  sc.imageFormat = chosen.format;
  sc.imageColorSpace = chosen.colorSpace;
  sc.imageExtent = extent_;
  sc.imageArrayLayers = 1;
  sc.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  sc.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  sc.preTransform = caps.currentTransform;
  sc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  sc.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  sc.clipped = VK_TRUE;
  SMOKE_INFO("  about to vkCreateSwapchainKHR (%u images, format %d, %ux%u, FIFO)", images,
             int(chosen.format), extent_.width, extent_.height);
  const u64 t0 = NowNs();
  if (!Check("vkCreateSwapchainKHR", vkCreateSwapchainKHR(device_, &sc, nullptr, &swapchain_))) {
    return false;
  }
  SMOKE_INFO("  vkCreateSwapchainKHR: %.2f ms", Ms(NowNs() - t0));
  n = 0;
  vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
  images_.resize(n);
  vkGetSwapchainImagesKHR(device_, swapchain_, &n, images_.data());
  SMOKE_INFO("  swapchain images: %u", n);

  VkAttachmentDescription color{};
  color.format = format_;
  color.samples = VK_SAMPLE_COUNT_1_BIT;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &ref;
  VkSubpassDependency dep{};
  dep.srcSubpass = VK_SUBPASS_EXTERNAL;
  dep.dstSubpass = 0;
  dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rp.attachmentCount = 1;
  rp.pAttachments = &color;
  rp.subpassCount = 1;
  rp.pSubpasses = &sub;
  rp.dependencyCount = 1;
  rp.pDependencies = &dep;
  if (!Check("vkCreateRenderPass", vkCreateRenderPass(device_, &rp, nullptr, &render_pass_))) {
    return false;
  }
  for (VkImage image : images_) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format_;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (!Check("vkCreateImageView", vkCreateImageView(device_, &vi, nullptr, &view))) return false;
    views_.push_back(view);
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass = render_pass_;
    fb.attachmentCount = 1;
    fb.pAttachments = &view;
    fb.width = extent_.width;
    fb.height = extent_.height;
    fb.layers = 1;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    if (!Check("vkCreateFramebuffer", vkCreateFramebuffer(device_, &fb, nullptr, &framebuffer))) {
      return false;
    }
    framebuffers_.push_back(framebuffer);
    VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore s = VK_NULL_HANDLE;
    if (!Check("vkCreateSemaphore", vkCreateSemaphore(device_, &semi, nullptr, &s))) return false;
    rendered_.push_back(s);
  }
  return true;
}

// ── d) pipelines and the first submit ──────────────────────────────────────

bool Probe::CreatePipeline(VkPipelineCache cache, VkPipeline* out, double* ms) {
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs_;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fs_;
  stages[1].pName = "main";
  VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1;
  vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms_state{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms_state.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState blend{};
  blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &blend;
  const VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dyn.dynamicStateCount = 2;
  dyn.pDynamicStates = dyn_states;
  VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gp.stageCount = 2;
  gp.pStages = stages;
  gp.pVertexInputState = &vin;
  gp.pInputAssemblyState = &ia;
  gp.pViewportState = &vp;
  gp.pRasterizationState = &rs;
  gp.pMultisampleState = &ms_state;
  gp.pColorBlendState = &cb;
  gp.pDynamicState = &dyn;
  gp.layout = layout_;
  gp.renderPass = render_pass_;
  const u64 t0 = NowNs();
  const VkResult r = vkCreateGraphicsPipelines(device_, cache, 1, &gp, nullptr, out);
  *ms = Ms(NowNs() - t0);
  return Check("vkCreateGraphicsPipelines", r);
}

bool Probe::StepPipelines() {
  VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  sm.codeSize = sizeof(fullscreen_cw_vs);
  sm.pCode = fullscreen_cw_vs;
  if (!Check("vkCreateShaderModule (vertex)", vkCreateShaderModule(device_, &sm, nullptr, &vs_))) {
    return false;
  }
  sm.codeSize = sizeof(kMagentaFs);
  sm.pCode = kMagentaFs;
  if (!Check("vkCreateShaderModule (fragment)", vkCreateShaderModule(device_, &sm, nullptr, &fs_))) {
    return false;
  }
  VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  if (!Check("vkCreatePipelineLayout", vkCreatePipelineLayout(device_, &pl, nullptr, &layout_))) {
    return false;
  }

  // 1. cold: an empty application cache (the driver then looks nowhere else).
  VkPipelineCacheCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  VkPipelineCache cache0 = VK_NULL_HANDLE;
  if (!Check("vkCreatePipelineCache", vkCreatePipelineCache(device_, &pci, nullptr, &cache0))) {
    return false;
  }
  caches_.push_back(cache0);
  double cold = 0, same = 0, loaded = 0, driver = 0;
  SMOKE_INFO("  about to vkCreateGraphicsPipelines (cold: empty VkPipelineCache)");
  if (!CreatePipeline(cache0, &pipeline_, &cold)) return false;
  // 2. the same cache, now holding it.
  VkPipeline p = VK_NULL_HANDLE;
  SMOKE_INFO("  about to vkCreateGraphicsPipelines (same cache, filled)");
  if (!CreatePipeline(cache0, &p, &same)) return false;
  extra_pipelines_.push_back(p);
  // 3. the cache's data in a new cache, as it would come back from the SD card.
  size_t size = 0;
  vkGetPipelineCacheData(device_, cache0, &size, nullptr);
  std::vector<uint8_t> data(size);
  vkGetPipelineCacheData(device_, cache0, &size, data.data());
  pci.initialDataSize = size;
  pci.pInitialData = data.data();
  VkPipelineCache cache1 = VK_NULL_HANDLE;
  if (!Check("vkCreatePipelineCache (from data)",
             vkCreatePipelineCache(device_, &pci, nullptr, &cache1))) {
    return false;
  }
  caches_.push_back(cache1);
  SMOKE_INFO("  about to vkCreateGraphicsPipelines (new cache from %zu bytes of data)", size);
  if (!CreatePipeline(cache1, &p, &loaded)) return false;
  extra_pipelines_.push_back(p);
  // 4. no application cache: the driver's own in-memory cache.
  SMOKE_INFO("  about to vkCreateGraphicsPipelines (VK_NULL_HANDLE cache)");
  if (!CreatePipeline(VK_NULL_HANDLE, &p, &driver)) return false;
  extra_pipelines_.push_back(p);
  SMOKE_INFO("  pipeline creation: cold %.3f ms, same cache %.3f ms, cache from data (%zu bytes) "
             "%.3f ms, driver cache %.3f ms (a trivial shader pair; nfsmw-nx measured ~59 ms "
             "per game pipeline)",
             cold, same, size, loaded, driver);
  return true;
}

bool Probe::StepFirstSubmit() {
  VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cp.queueFamilyIndex = queue_family_;
  if (!Check("vkCreateCommandPool", vkCreateCommandPool(device_, &cp, nullptr, &pool_))) {
    return false;
  }
  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ai.commandPool = pool_;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = kFrames;
  if (!Check("vkAllocateCommandBuffers", vkAllocateCommandBuffers(device_, &ai, cmd_))) {
    return false;
  }
  for (int i = 0; i < kFrames; ++i) {
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (!Check("vkCreateFence", vkCreateFence(device_, &fi, nullptr, &fence_[i])) ||
        !Check("vkCreateSemaphore", vkCreateSemaphore(device_, &si, nullptr, &acquired_[i]))) {
      return false;
    }
  }
  for (int round = 0; round < 2; ++round) {
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetFences(device_, 1, &fence_[0]);
    vkBeginCommandBuffer(cmd_[0], &bi);
    vkEndCommandBuffer(cmd_[0]);
    VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    s.commandBufferCount = 1;
    s.pCommandBuffers = &cmd_[0];
    SMOKE_INFO("  about to vkQueueSubmit (%s, empty command buffer, fence)",
               round == 0 ? "first" : "second");
    const u64 t0 = NowNs();
    if (!Check("vkQueueSubmit", vkQueueSubmit(queue_, 1, &s, fence_[0]))) return false;
    const u64 t1 = NowNs();
    if (!Check("vkWaitForFences", vkWaitForFences(device_, 1, &fence_[0], VK_TRUE, 1'000'000'000))) {
      return false;
    }
    const u64 t2 = NowNs();
    SMOKE_INFO("  %s vkQueueSubmit: %.3f ms, fence wait %.3f ms", round == 0 ? "first" : "second",
               Ms(t1 - t0), Ms(t2 - t1));
  }
  return true;
}

// ── c) 5 s of frames ───────────────────────────────────────────────────────

bool Probe::StepFrames() {
  SMOKE_INFO("  about to present for 5 s: the screen is cleared to a changing colour, with a "
             "magenta square from the test pipeline");
  const u64 start = NowNs();
  const u64 duration = 5'000'000'000ull;
  u64 frames = 0, suboptimal = 0, worst_ns = 0, last = start;
  u64 acquire_ns = 0, submit_ns = 0, present_ns = 0, fence_ns = 0;
  int slot = 0;
  while (NowNs() - start < duration) {
    if (!appletMainLoop()) return Fail("the system asked the applet to exit during the frames");
    u64 t = NowNs();
    vkWaitForFences(device_, 1, &fence_[slot], VK_TRUE, 1'000'000'000);
    fence_ns += NowNs() - t;
    uint32_t index = 0;
    t = NowNs();
    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, 1'000'000'000, acquired_[slot],
                                       VK_NULL_HANDLE, &index);
    acquire_ns += NowNs() - t;
    if (r == VK_SUBOPTIMAL_KHR) {
      ++suboptimal;
    } else if (!Check("vkAcquireNextImageKHR", r)) {
      return false;
    }
    vkResetFences(device_, 1, &fence_[slot]);
    const double phase = double(NowNs() - start) / 1e9;
    const float red = float((frames % 120) / 120.0);
    const float green = float(phase / 5.0);
    const float blue = 1.0f - red;
    VkClearValue clear{};
    clear.color = {{red, green, blue, 1.0f}};
    VkCommandBuffer cb = cmd_[slot];
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = render_pass_;
    rb.framebuffer = framebuffers_[index];
    rb.renderArea = {{0, 0}, extent_};
    rb.clearValueCount = 1;
    rb.pClearValues = &clear;
    vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    const float side = 200.0f;
    VkViewport viewport{(float(extent_.width) - side) / 2, (float(extent_.height) - side) / 2,
                        side, side, 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetViewport(cb, 0, 1, &viewport);
    vkCmdSetScissor(cb, 0, 1, &scissor);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    vkEndCommandBuffer(cb);
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    s.waitSemaphoreCount = 1;
    s.pWaitSemaphores = &acquired_[slot];
    s.pWaitDstStageMask = &wait_stage;
    s.commandBufferCount = 1;
    s.pCommandBuffers = &cb;
    s.signalSemaphoreCount = 1;
    s.pSignalSemaphores = &rendered_[index];
    t = NowNs();
    if (!Check("vkQueueSubmit (frame)", vkQueueSubmit(queue_, 1, &s, fence_[slot]))) return false;
    submit_ns += NowNs() - t;
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &rendered_[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &index;
    t = NowNs();
    r = vkQueuePresentKHR(queue_, &pi);
    present_ns += NowNs() - t;
    if (r == VK_SUBOPTIMAL_KHR) {
      ++suboptimal;
    } else if (!Check("vkQueuePresentKHR", r)) {
      return false;
    }
    if (frames == 0) SMOKE_INFO("  first frame presented");
    ++frames;
    const u64 now = NowNs();
    if (now - last > worst_ns) worst_ns = now - last;
    last = now;
    slot = (slot + 1) % kFrames;
  }
  const double seconds = double(NowNs() - start) / 1e9;
  const double f = frames ? double(frames) : 1.0;
  SMOKE_INFO("  frames: %" PRIu64 " in %.2f s = %.1f fps, worst frame %.2f ms, %" PRIu64
             " suboptimal; per frame: fence wait %.3f ms, acquire %.3f ms, submit %.3f ms, "
             "present %.3f ms",
             frames, seconds, double(frames) / seconds, Ms(worst_ns), suboptimal,
             Ms(fence_ns) / f, Ms(acquire_ns) / f, Ms(submit_ns) / f, Ms(present_ns) / f);
  return true;
}

// ── e) teardown ────────────────────────────────────────────────────────────

void Probe::Destroy() {
  if (device_) {
    SMOKE_INFO("  about to vkDeviceWaitIdle and destroy the device objects");
    vkDeviceWaitIdle(device_);
    for (int i = 0; i < kFrames; ++i) {
      if (fence_[i]) vkDestroyFence(device_, fence_[i], nullptr);
      if (acquired_[i]) vkDestroySemaphore(device_, acquired_[i], nullptr);
      fence_[i] = VK_NULL_HANDLE;
      acquired_[i] = VK_NULL_HANDLE;
    }
    for (VkSemaphore s : rendered_) vkDestroySemaphore(device_, s, nullptr);
    rendered_.clear();
    if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
    pool_ = VK_NULL_HANDLE;
    for (VkPipeline p : extra_pipelines_) vkDestroyPipeline(device_, p, nullptr);
    extra_pipelines_.clear();
    if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    for (VkPipelineCache c : caches_) vkDestroyPipelineCache(device_, c, nullptr);
    caches_.clear();
    if (layout_) vkDestroyPipelineLayout(device_, layout_, nullptr);
    if (vs_) vkDestroyShaderModule(device_, vs_, nullptr);
    if (fs_) vkDestroyShaderModule(device_, fs_, nullptr);
    layout_ = VK_NULL_HANDLE;
    vs_ = fs_ = VK_NULL_HANDLE;
    for (VkFramebuffer fb : framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
    framebuffers_.clear();
    for (VkImageView v : views_) vkDestroyImageView(device_, v, nullptr);
    views_.clear();
    if (render_pass_) vkDestroyRenderPass(device_, render_pass_, nullptr);
    render_pass_ = VK_NULL_HANDLE;
    if (swapchain_) {
      SMOKE_INFO("  about to vkDestroySwapchainKHR");
      vkDestroySwapchainKHR(device_, swapchain_, nullptr);
      swapchain_ = VK_NULL_HANDLE;
    }
    SMOKE_INFO("  about to vkDestroyDevice");
    vkDestroyDevice(device_, nullptr);
    device_ = VK_NULL_HANDLE;
  }
  if (instance_) {
    if (surface_) {
      SMOKE_INFO("  about to vkDestroySurfaceKHR");
      vkDestroySurfaceKHR(instance_, surface_, nullptr);
      surface_ = VK_NULL_HANDLE;
    }
    SMOKE_INFO("  about to vkDestroyInstance");
    vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
  }
  if (console_closed_) {
    SMOKE_INFO("  about to reopen the console on the default NWindow");
    reopen_console_();
    console_closed_ = false;
    SMOKE_INFO("  console reopened");
  }
}

}  // namespace

bool RunVulkanProbe(void (*close_console)(), void (*reopen_console)()) {
  SMOKE_INFO("step 7 Vulkan (mesa-switch NVK, static libvulkan.a): instance, device, features, "
             "surface, swapchain, 5 s of frames, pipeline and submit timings");
  std::string failure;
  bool ok = false;
  {
    Probe probe(close_console, reopen_console);
    ok = probe.Run();
    failure = probe.failure();
  }  // destroyed here, in order, success or not
  if (ok) {
    Report(false, "VULKAN OK");
  } else {
    Report(true, "VULKAN FAIL at %s", failure.c_str());
  }
  return ok;
}
