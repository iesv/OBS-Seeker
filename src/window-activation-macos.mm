#import <AppKit/AppKit.h>

namespace obs_seeker {
void activateMacApplication()
{
    if (@available(macOS 14.0, *))
        [NSApp activate];
    else
        [NSApp activateIgnoringOtherApps:YES];
}
}
