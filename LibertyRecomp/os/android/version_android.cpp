#include <os/version.h>
#include <sys/system_properties.h>
#include <cstdlib>

os::version::OSVersion os::version::GetOSVersion()
{
    char release[PROP_VALUE_MAX] = {};
    __system_property_get("ro.build.version.release", release);

    uint32_t major = 0, minor = 0, patch = 0;
    sscanf(release, "%u.%u.%u", &major, &minor, &patch);
    return { major, minor, patch };
}
