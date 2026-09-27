#include <os/version.h>
#include <UIKit/UIKit.h>

os::version::OSVersion os::version::GetOSVersion()
{
    NSOperatingSystemVersion v = [[NSProcessInfo processInfo] operatingSystemVersion];
    return { 
        static_cast<uint32_t>(v.majorVersion),
        static_cast<uint32_t>(v.minorVersion),
        static_cast<uint32_t>(v.patchVersion)
    };
}
