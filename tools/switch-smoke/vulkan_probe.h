// switch-smoke step 7: Mesa NVK on Horizon (vulkan_probe.cpp).
#pragma once

// Runs the Vulkan probe and logs "VULKAN OK" or "VULKAN FAIL at ...". The
// probe takes the default NWindow from the console: close_console is called
// before the surface is created, reopen_console after the teardown. Returns
// true on success; the SMOKE verdict does not depend on it.
bool RunVulkanProbe(void (*close_console)(), void (*reopen_console)());

// Threads of this process (svcGetThreadList); a failure gives -(result code).
int ProcessThreadCount();
