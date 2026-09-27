#include <os/user.h>
#include <UIKit/UIKit.h>

bool os::user::IsDarkTheme()
{
    // iOS 13+ supports dark mode
    if (@available(iOS 13.0, *))
    {
        return [UITraitCollection currentTraitCollection].userInterfaceStyle
               == UIUserInterfaceStyleDark;
    }
    return false;
}
