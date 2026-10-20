#import <AppKit/AppKit.h>
#import <OSAKit/OSAKit.h>
#import <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

// Expose application lookup and permissions to the presentation script.
@interface SeekerAutomation : NSObject
+ (NSString *)keynoteIdentifier;
+ (OSStatus)permissionFor:(NSString *)identifier askUser:(BOOL)askUser;
+ (NSInteger)retryAutomation;
@end

@implementation SeekerAutomation
+ (NSString *)keynoteIdentifier
{
    return [NSWorkspace.sharedWorkspace URLForApplicationWithBundleIdentifier:@"com.apple.Keynote"]
        ? @"com.apple.Keynote" : @"com.apple.iWork.Keynote";
}

+ (OSStatus)permissionFor:(NSString *)identifier askUser:(BOOL)askUser
{
    if (askUser) {
        auto *workspace = NSWorkspace.sharedWorkspace;
        auto *url = [workspace URLForApplicationWithBundleIdentifier:identifier];
        if (!url)
            return procNotFound;
        // The permission API requires a running target, not merely a launch request.
        auto *configuration = [NSWorkspaceOpenConfiguration configuration];
        configuration.activates = NO;
        auto ready = dispatch_semaphore_create(0);
        __block BOOL launched = NO;
        [workspace openApplicationAtURL:url configuration:configuration completionHandler:^(NSRunningApplication *app, NSError *error) {
            launched = app != nil;
            if (error)
                NSLog(@"[OBS-Seeker] Cannot launch %@: %@", identifier, error);
            dispatch_semaphore_signal(ready);
        }];
        dispatch_semaphore_wait(ready, DISPATCH_TIME_FOREVER);
        if (!launched)
            return procNotFound;
    }
    auto *target = [NSAppleEventDescriptor descriptorWithBundleIdentifier:identifier];
    return AEDeterminePermissionToAutomateTarget(target.aeDesc, typeWildCard, typeWildCard, askUser);
}

+ (NSInteger)retryAutomation
{
    auto *defaults = NSUserDefaults.standardUserDefaults;
    if (![defaults boolForKey:@"RetryAutomation"])
        return 0;
    // Nested helpers may not yet be registered, so tccutil cannot resolve their bundle ID.
    const auto registered = LSRegisterURL((__bridge CFURLRef)NSBundle.mainBundle.bundleURL, true);
    if (registered != noErr) {
        NSLog(@"[OBS-Seeker] Cannot register presentation helper: %d", static_cast<int>(registered));
        return -1;
    }
    // AppleEvents resets apply to this helper as a whole, including both presentation apps.
    auto *task = [NSTask new];
    task.executableURL = [NSURL fileURLWithPath:@"/usr/bin/tccutil"];
    task.arguments = @[@"reset", @"AppleEvents", NSBundle.mainBundle.bundleIdentifier];
    auto *output = [NSPipe pipe];
    task.standardOutput = output;
    task.standardError = output;
    NSError *error = nil;
    if (![task launchAndReturnError:&error]) {
        NSLog(@"[OBS-Seeker] Cannot reset automation: %@", error);
        return -1;
    }
    auto *message = [output.fileHandleForReading readDataToEndOfFile];
    [task waitUntilExit];
    if (task.terminationStatus != 0) {
        NSLog(@"[OBS-Seeker] Cannot reset automation: %@ (registered helper: %@)",
            [[NSString alloc] initWithData:message encoding:NSUTF8StringEncoding],
            [NSWorkspace.sharedWorkspace URLForApplicationWithBundleIdentifier:NSBundle.mainBundle.bundleIdentifier]);
        return -1;
    }
    [defaults removeObjectForKey:@"RetryAutomation"];
    [defaults synchronize];
    return 1;
}
@end

int main(int argc, const char **argv)
{
    @autoreleasepool {
        if (argc != 3)
            return 2;
        [NSApplication sharedApplication];
        [SeekerAutomation class];
        NSString *path = [NSString stringWithUTF8String:argv[1]];
        NSString *resultPath = [NSString stringWithUTF8String:argv[2]];
        const int descriptor = open(resultPath.fileSystemRepresentation, O_EVTONLY);
        if (descriptor < 0)
            return 2;
        // The request owns the result file; removing it cancels this helper, including a pending permission dialog.
        auto cancellation = dispatch_source_create(DISPATCH_SOURCE_TYPE_VNODE, descriptor,
            DISPATCH_VNODE_DELETE | DISPATCH_VNODE_RENAME, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
        dispatch_source_set_event_handler(cancellation, ^{ _exit(1); });
        dispatch_source_set_cancel_handler(cancellation, ^{ close(descriptor); });
        dispatch_resume(cancellation);
        [@"1\nPresentation did not complete" writeToFile:resultPath atomically:NO encoding:NSUTF8StringEncoding error:nil];
        // Permission requests may wait for the user; keep the app's main run loop available.
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            @autoreleasepool {
                NSError *readError = nil;
                NSString *scriptPath = [NSBundle.mainBundle pathForResource:@"presentation-macos" ofType:@"js"];
                NSString *source = scriptPath ? [NSString stringWithContentsOfFile:scriptPath encoding:NSUTF8StringEncoding error:&readError] : nil;
                NSString *output;
                if (source) {
                    NSData *arguments = [NSJSONSerialization dataWithJSONObject:@[path] options:0 error:nil];
                    NSString *json = [[NSString alloc] initWithData:arguments encoding:NSUTF8StringEncoding];
                    NSString *invocation = [path isEqualToString:@"--retry-automation"]
                        ? @"retryPermissions();" : [NSString stringWithFormat:@"present(%@[0]);", json];
                    source = [NSString stringWithFormat:@"ObjC.import('Foundation');\n%@\n%@", source, invocation];
                    OSAScript *script = [[OSAScript alloc] initWithSource:source language:[OSALanguage languageForName:@"JavaScript"]];
                    NSDictionary *error = nil;
                    NSAppleEventDescriptor *result = [script executeAndReturnError:&error];
                    output = result ? result.stringValue : [NSString stringWithFormat:@"1\n%@", error];
                } else {
                    output = [NSString stringWithFormat:@"2\nCannot load presentation script: %@", readError];
                }
                if ([output hasPrefix:@"3\n"]) {
                    auto *defaults = NSUserDefaults.standardUserDefaults;
                    [defaults setBool:YES forKey:@"RetryAutomation"];
                    [defaults synchronize];
                }
                const BOOL written = [output writeToFile:resultPath atomically:NO encoding:NSUTF8StringEncoding error:nil];
                dispatch_source_cancel(cancellation);
                std::exit(written ? 0 : 1);
            }
        });
        [NSApp run];
        return 0;
    }
}
