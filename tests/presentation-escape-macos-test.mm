#include "presentation-escape.hpp"
#include <QCoreApplication>
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

// Replace OS input/accessibility boundaries, but execute the real native callback,
// Core Foundation ownership and Qt completion queue without system permissions.
namespace fixture {
NSString *identifier = @"com.microsoft.Powerpoint";
AXUIElementRef window = nullptr;
AXUIElementRef current = nullptr;
CFStringRef role = nullptr;
bool windowed = false;
CGEventTapCallBack callback = nullptr;
void *context = nullptr;
int queries = 0;
int posted = 0;

void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

AXError attribute(AXUIElementRef, CFStringRef name, CFTypeRef *value)
{
    ++queries;
    *value = nullptr;
    if (CFEqual(name, kAXFocusedWindowAttribute) || CFEqual(name, kAXFocusedUIElementAttribute)) {
        if (current)
            *value = CFRetain(current);
    } else if (CFEqual(name, kAXRoleAttribute)) {
        if (role)
            *value = CFRetain(role);
    } else if (CFEqual(name, kAXPositionAttribute)) {
        const CGPoint position{};
        *value = AXValueCreate(kAXValueTypeCGPoint, &position);
    } else if (CFEqual(name, kAXSizeAttribute)) {
        const CGSize size = windowed ? CGSize{800, 600} : CGSize{1920, 1080};
        *value = AXValueCreate(kAXValueTypeCGSize, &size);
    }
    return *value ? kAXErrorSuccess : kAXErrorNoValue;
}

Boolean trusted() { return true; }
AXError timeout(AXUIElementRef, float) { return kAXErrorSuccess; }
CGError displays(uint32_t, CGDirectDisplayID *list, uint32_t *count)
{
    list[0] = 1;
    *count = 1;
    return kCGErrorSuccess;
}
CGRect bounds(CGDirectDisplayID) { return CGRectMake(0, 0, 1920, 1080); }

CFMachPortRef tap(CGEventTapLocation, CGEventTapPlacement, CGEventTapOptions,
                 CGEventMask, CGEventTapCallBack call, void *info)
{
    callback = call;
    context = info;
    CFMachPortContext portContext{};
    return CFMachPortCreate(kCFAllocatorDefault, [](CFMachPortRef, void *, CFIndex, void *) {},
                          &portContext, nullptr);
}

void post(pid_t pid, CGEventRef event)
{
    require(pid == getpid(), "Delayed Esc changed its target process");
    require(CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode) == 53,
            "Delayed Esc sent a different key");
    require(CGEventGetFlags(event) == 0, "Delayed Esc kept modifier keys");
    require(CGEventGetType(event) == (posted % 2 ? kCGEventKeyUp : kCGEventKeyDown),
            "Delayed Esc key sequence changed");
    require(callback(reinterpret_cast<CGEventTapProxy>(&posted), CGEventGetType(event), event, context) == event,
            "Delivered Esc was intercepted again");
    ++posted;
}
}

@interface SeekerTestApplication : NSObject
@property(readonly) NSString *bundleIdentifier;
@property(readonly) pid_t processIdentifier;
@end
@implementation SeekerTestApplication
- (NSString *)bundleIdentifier { return fixture::identifier; }
- (pid_t)processIdentifier { return getpid(); }
@end

@interface SeekerTestWorkspace : NSObject
+ (SeekerTestWorkspace *)sharedWorkspace;
@property(readonly) SeekerTestApplication *frontmostApplication;
@end
@implementation SeekerTestWorkspace
+ (SeekerTestWorkspace *)sharedWorkspace
{
    static auto *workspace = [SeekerTestWorkspace new];
    return workspace;
}
- (SeekerTestApplication *)frontmostApplication { return [SeekerTestApplication new]; }
@end

#define NSWorkspace SeekerTestWorkspace
#define AXIsProcessTrusted fixture::trusted
#define AXUIElementCopyAttributeValue fixture::attribute
#define AXUIElementSetMessagingTimeout fixture::timeout
#define CGGetActiveDisplayList fixture::displays
#define CGDisplayBounds fixture::bounds
#define CGEventTapCreate fixture::tap
#define CGEventPostToPid fixture::post
#include "../src/presentation-escape-macos.mm"
#undef NSWorkspace
#undef AXIsProcessTrusted
#undef AXUIElementCopyAttributeValue
#undef AXUIElementSetMessagingTimeout
#undef CGGetActiveDisplayList
#undef CGDisplayBounds
#undef CGEventTapCreate
#undef CGEventPostToPid

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    @autoreleasepool {
        using namespace fixture;
        auto down = CGEventCreateKeyboardEvent(nullptr, 53, true);
        auto up = CGEventCreateKeyboardEvent(nullptr, 53, false);
        CGEventSetFlags(down, 0);
        CGEventSetFlags(up, 0);
        window = AXUIElementCreateApplication(getpid() + 1);
        auto other = AXUIElementCreateApplication(getpid() + 2);
        int requests = 0;
        obs_seeker::PresentationEscape::Completion completion;
        try {
            obs_seeker::PresentationEscape hook(&app, [&](auto done) {
                ++requests;
                completion = std::move(done);
            });
            hook.setEnabled(true);
            hook.start();
            require(callback, "Native Esc callback was not installed");
            const auto send = [&](CGEventRef event, bool captured) {
                require(callback(reinterpret_cast<CGEventTapProxy>(&posted), CGEventGetType(event), event, context)
                            == (captured ? nullptr : event), "Unexpected Esc interception");
            };
            const auto press = [&](bool captured) { send(down, captured); send(up, captured); };
            const auto retained = CFGetRetainCount(window);
            for (NSString *target in @[@"com.microsoft.Powerpoint", @"com.apple.Keynote", @"com.apple.iWork.Keynote"]) {
                identifier = target;
                for (int index = 0; index < 10000; ++index)
                    press(false); // A shared_ptr(nullptr, CFRelease) crashes on the first iteration.
            }
            require(requests == 0, "Missing windows triggered HOME");
            current = window;
            for (auto editing : {kAXTextFieldRole, kAXTextAreaRole, kAXComboBoxRole}) {
                role = editing;
                press(false);
            }
            role = nullptr;
            windowed = true;
            press(false);
            windowed = false;
            identifier = @"com.apple.TextEdit";
            const auto beforeQueries = queries;
            press(false);
            require(queries == beforeQueries, "Unrelated applications were queried");
            identifier = @"com.apple.Keynote";
            for (auto flags : {kCGEventFlagMaskShift, kCGEventFlagMaskControl,
                               kCGEventFlagMaskAlternate, kCGEventFlagMaskCommand}) {
                CGEventSetFlags(down, flags);
                press(false);
            }
            CGEventSetFlags(down, 0);
            for (int index = 0; index < 1000; ++index) {
                send(down, true);
                for (int repeat = 0; repeat < 10; ++repeat)
                    send(down, true);
                send(up, true);
                QCoreApplication::processEvents();
                require(requests == index + 1 && completion, "Repeated key requested HOME twice");
                require(posted == index * 2, "Esc was delivered before completion");
                require(CFGetRetainCount(window) == retained + 1, "Captured window ownership changed");
                press(true);
                QCoreApplication::processEvents();
                require(requests == index + 1, "Pending HOME was requested twice");
                completion(true);
                completion = {};
                require(posted == (index + 1) * 2, "Completed HOME did not deliver one Esc");
                require(CFGetRetainCount(window) == retained, "Completed Esc retained its window");
            }
            for (int state = 0; state < 3; ++state) {
                current = window;
                press(true);
                QCoreApplication::processEvents();
                current = state == 0 ? window : state == 1 ? other : nullptr;
                completion(state != 0);
                completion = {};
                require(posted == 2000, "Canceled or changed window received delayed Esc");
                require(CFGetRetainCount(window) == retained, "Canceled Esc retained its window");
            }
            hook.setEnabled(false);
            current = window;
            press(false);
            hook.stop();
            for (int index = 0; index < 100; ++index) {
                hook.setEnabled(true);
                hook.start();
                hook.stop();
            }
            require(CFGetRetainCount(window) == retained, "Native callback leaked its window");
            std::cout << "PASS: native Mac Esc: 30000 missing-window presses; editing/modifiers, "
                         "1000 delayed/repeated captures, cancellation, changed window and hook shutdown\n";
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
        CFRelease(other);
        CFRelease(window);
        CFRelease(up);
        CFRelease(down);
    }
}
