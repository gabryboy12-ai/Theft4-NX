// Android process helpers — works within the APK/AAB sandbox
#include <os/process.h>
#include <unistd.h>
#include <limits.h>
#include <dlfcn.h>
#include <android/asset_manager.h>

// g_androidAppInternalPath is set by JNI glue in LibertySDLActivity.java
// before the native library initialises.
extern "C" const char* g_androidAppInternalPath;
extern "C" const char* g_androidObbPath;

std::filesystem::path os::process::GetExecutablePath()
{
    // The shared library IS the "executable" on Android
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&os::process::GetExecutablePath), &info) && info.dli_fname)
        return std::filesystem::path(info.dli_fname);
    return {};
}

std::filesystem::path os::process::GetExecutableRoot()
{
    // Internal app storage: /data/data/<package>/files/
    if (g_androidAppInternalPath)
        return std::filesystem::path(g_androidAppInternalPath);
    return {};
}

std::filesystem::path os::process::GetWorkingDirectory()
{
    char cwd[PATH_MAX] = {};
    if (getcwd(cwd, sizeof(cwd)))
        return std::filesystem::path(cwd);
    return {};
}

bool os::process::SetWorkingDirectory(const std::filesystem::path& path)
{
    return chdir(path.c_str()) == 0;
}

bool os::process::StartProcess(const std::filesystem::path&, const std::vector<std::string>&, std::filesystem::path)
{
    // Android sandbox does not allow launching arbitrary child processes
    return false;
}

void os::process::CheckConsole()
{
    g_consoleVisible = false;
}

void os::process::ShowConsole()
{
    // No-op on Android
}
