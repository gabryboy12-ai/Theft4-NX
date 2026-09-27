// iOS process helpers — sandboxed app environment
#include <os/process.h>

#include <dlfcn.h>
#include <unistd.h>
#include <sys/param.h>
#include <Foundation/Foundation.h>

std::filesystem::path os::process::GetExecutablePath()
{
    // On iOS the "executable" is inside the .app bundle
    NSString* execPath = [[NSBundle mainBundle] executablePath];
    if (execPath)
        return std::filesystem::path([execPath UTF8String]);
    return {};
}

std::filesystem::path os::process::GetExecutableRoot()
{
    // Resources are bundled inside the .ipa; return the bundle path
    NSString* bundlePath = [[NSBundle mainBundle] bundlePath];
    if (bundlePath)
        return std::filesystem::path([bundlePath UTF8String]);
    return {};
}

std::filesystem::path os::process::GetWorkingDirectory()
{
    // On iOS the working directory is the app's documents directory
    NSArray<NSString*>* paths = NSSearchPathForDirectoriesInDomains(
        NSDocumentDirectory, NSUserDomainMask, YES);
    if ([paths count] > 0)
        return std::filesystem::path([paths[0] UTF8String]);

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
    // iOS sandbox does not allow launching child processes
    return false;
}

void os::process::CheckConsole()
{
    g_consoleVisible = false; // No console window on iOS
}

void os::process::ShowConsole()
{
    // No-op on iOS
}
