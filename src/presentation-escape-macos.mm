#include "presentation-escape.hpp"
#include <QDebug>
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#include <memory>

namespace obs_seeker {
namespace {
constexpr int64_t DeliveredEscape = 0x4f42535345454b;

bool windowValue(AXUIElementRef window, CFStringRef attribute, AXValueType type, void *result)
{
    CFTypeRef value = nullptr;
    const bool copied = AXUIElementCopyAttributeValue(window, attribute, &value) == kAXErrorSuccess;
    const bool valid = copied && CFGetTypeID(value) == AXValueGetTypeID()
        && AXValueGetValue(static_cast<AXValueRef>(value), type, result);
    if (value)
        CFRelease(value);
    return valid;
}

bool fullscreen(AXUIElementRef window)
{
    AXUIElementSetMessagingTimeout(window, 0.02f);
    CGPoint position{};
    CGSize size{};
    if (!windowValue(window, kAXPositionAttribute, kAXValueTypeCGPoint, &position)
        || !windowValue(window, kAXSizeAttribute, kAXValueTypeCGSize, &size))
        return false;
    CGDirectDisplayID displays[16];
    uint32_t count = 0;
    if (CGGetActiveDisplayList(16, displays, &count) != kCGErrorSuccess)
        return false;
    for (uint32_t index = 0; index < count; ++index) {
        const auto bounds = CGDisplayBounds(displays[index]);
        if (position.x <= bounds.origin.x + 2 && position.y <= bounds.origin.y + 2
            && position.x + size.width >= CGRectGetMaxX(bounds) - 2
            && position.y + size.height >= CGRectGetMaxY(bounds) - 2)
            return true;
    }
    return false;
}

AXUIElementRef focusedWindow(pid_t pid)
{
    auto app = AXUIElementCreateApplication(pid);
    AXUIElementSetMessagingTimeout(app, 0.02f);
    CFTypeRef window = nullptr;
    AXUIElementCopyAttributeValue(app, kAXFocusedWindowAttribute, &window);
    CFTypeRef focused = nullptr;
    CFTypeRef role = nullptr;
    if (AXUIElementCopyAttributeValue(app, kAXFocusedUIElementAttribute, &focused) == kAXErrorSuccess) {
        AXUIElementSetMessagingTimeout(static_cast<AXUIElementRef>(focused), 0.02f);
        AXUIElementCopyAttributeValue(static_cast<AXUIElementRef>(focused), kAXRoleAttribute, &role);
        CFRelease(focused);
    }
    CFRelease(app);
    const bool editing = role && (CFEqual(role, kAXTextFieldRole) || CFEqual(role, kAXTextAreaRole)
        || CFEqual(role, kAXComboBoxRole));
    if (role)
        CFRelease(role);
    if (window && (editing || !fullscreen(static_cast<AXUIElementRef>(window)))) {
        CFRelease(window);
        window = nullptr;
    }
    return static_cast<AXUIElementRef>(window);
}
}

void PresentationEscape::start()
{
    if (!enabled_) {
        stop();
        return;
    }
    if (tap_)
        return;
    if (!AXIsProcessTrusted()) {
        qWarning() << "[OBS-Seeker] Esc HOME needs OBS Accessibility permission; save settings again after granting access";
        return;
    }
    tap_ = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault,
        CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp),
        [](CGEventTapProxy, CGEventType type, CGEventRef event, void *context) -> CGEventRef {
            auto *self = static_cast<PresentationEscape *>(context);
            if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
                CGEventTapEnable(self->tap_, true);
                return event;
            }
            if (CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode) != 53
                || CGEventGetIntegerValueField(event, kCGEventSourceUserData) == DeliveredEscape)
                return event;
            if (self->held_) {
                if (type == kCGEventKeyUp)
                    self->held_ = false;
                return nullptr;
            }
            if (type != kCGEventKeyDown || !self->enabled_
                || (CGEventGetFlags(event) & (kCGEventFlagMaskShift | kCGEventFlagMaskControl
                    | kCGEventFlagMaskAlternate | kCGEventFlagMaskCommand)))
                return event;
            @autoreleasepool {
                auto *app = NSWorkspace.sharedWorkspace.frontmostApplication;
                if (![app.bundleIdentifier isEqualToString:@"com.microsoft.Powerpoint"]
                    && ![app.bundleIdentifier isEqualToString:@"com.apple.Keynote"]
                    && ![app.bundleIdentifier isEqualToString:@"com.apple.iWork.Keynote"])
                    return event;
                const auto pid = app.processIdentifier;
                auto focused = focusedWindow(pid);
                if (!focused)
                    return event;
                std::shared_ptr<const void> window(focused, CFRelease);
                self->held_ = true;
                self->captured([pid, window] {
                    auto current = focusedWindow(pid);
                    const bool same = current && CFEqual(current, window.get());
                    if (current)
                        CFRelease(current);
                    if (!same)
                        return;
                    for (const bool down : {true, false}) {
                        auto key = CGEventCreateKeyboardEvent(nullptr, 53, down);
                        CGEventSetFlags(key, 0);
                        CGEventSetIntegerValueField(key, kCGEventSourceUserData, DeliveredEscape);
                        CGEventPostToPid(pid, key);
                        CFRelease(key);
                    }
                });
                return nullptr;
            }
        }, this);
    if (!tap_) {
        qWarning() << "[OBS-Seeker] Cannot intercept presentation Esc on macOS";
        return;
    }
    source_ = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap_, 0);
    CFRunLoopAddSource(CFRunLoopGetMain(), source_, kCFRunLoopCommonModes);
}

void PresentationEscape::stop()
{
    enabled_ = false;
    held_ = false;
    if (tap_)
        CFMachPortInvalidate(tap_);
    if (source_) {
        CFRunLoopRemoveSource(CFRunLoopGetMain(), source_, kCFRunLoopCommonModes);
        CFRelease(source_);
        source_ = nullptr;
    }
    if (tap_) {
        CFRelease(tap_);
        tap_ = nullptr;
    }
}
}
