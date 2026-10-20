// Exercise the production script without installing Office or granting automation access.
var calls = [];
var inputPath;
var permissionRequests = 0;
var authorized = false;
var installedApplications = [];
var realAutomation;
var retryDenied = false;
// Verify the compiled Objective-C bridge without requesting consent on the CI runner.
if (typeof $ === 'function') {
    realAutomation = $.NSClassFromString('SeekerAutomation');
    var keynoteIdentifier = realAutomation.keynoteIdentifier.js;
    if (keynoteIdentifier !== 'com.apple.Keynote' && keynoteIdentifier !== 'com.apple.iWork.Keynote')
        throw Error('Native Keynote lookup did not return a bundle identifier');
    retryDenied = Boolean($.NSUserDefaults.standardUserDefaults.boolForKey('SimulationRetryDenied'));
    ['com.microsoft.Powerpoint', 'com.apple.Keynote', 'com.apple.iWork.Keynote'].forEach(function (identifier) {
        if (typeof realAutomation.permissionForAskUser(identifier, false) !== 'number')
            throw Error('Native permission bridge did not return an OSStatus');
    });
}
var fakeNative = {SeekerAutomation: {permissionForAskUser: function (identifier, askUser) {
    calls.push(['permission', identifier, askUser]);
    if (installedApplications.indexOf(identifier) < 0) return -600;
    if (inputPath === '--retry-automation' && retryDenied) return -1743;
    if (inputPath.indexOf('permission-error') >= 0) return -600;
    if (inputPath.indexOf('permission-denied') >= 0) return -1743;
    if (!authorized) {
        ++permissionRequests;
        if (inputPath.indexOf('permission-refused') >= 0) return -1743;
        authorized = true;
    }
    return 0;
}}};
Object.defineProperty(fakeNative.SeekerAutomation, 'keynoteIdentifier', {get: function () {
    return {js: installedApplications.indexOf('com.apple.Keynote') >= 0
        ? 'com.apple.Keynote' : 'com.apple.iWork.Keynote'};
}});
Object.defineProperty(fakeNative.SeekerAutomation, 'retryAutomation', {get: function () {
    calls.push(['reset']);
    if (inputPath === '--retry-automation' && realAutomation)
        return Number(realAutomation.retryAutomation);
    if (inputPath.indexOf('retry-none') >= 0) return 0;
    if (inputPath.indexOf('retry-failed') >= 0) return -1;
    return inputPath.indexOf('retry-') >= 0 ? 1 : 0;
}});
fakeNative.NSClassFromString = function (name) {
    if (name !== 'SeekerAutomation') throw Error('Unexpected authorization class');
    return fakeNative.SeekerAutomation;
};
function fakeApplication(identifier) {
    calls.push(['application', identifier]);
    if (installedApplications.indexOf(identifier) < 0) throw Error('Application not installed');
    if (inputPath.indexOf('fail-app') >= 0) throw Error('Application unavailable');
    var settings = Object.freeze({});
    var document = Object.freeze({slideShowSettings: settings});
    return Object.freeze({
        activePresentation: document,
        documents: [document],
        launch: function () { calls.push(['launch']); },
        open: function (path) {
            calls.push(['open', path]);
            if (path !== inputPath) throw Error('Path changed');
            if (path.indexOf('fail-open') >= 0) throw Error('Open failed');
            if (path.indexOf('cancel') >= 0) {
                ObjC.import('Foundation');
                $.NSThread.sleepForTimeInterval(20);
            }
            // PowerPoint's open command returns nothing.
        },
        activate: function () {
            calls.push(['activate']);
            if (inputPath.indexOf('fail-activate') >= 0) throw Error('Activation failed');
        },
        runSlideShow: function (value) {
            if (value !== settings) throw Error('Wrong slideshow settings');
            calls.push(['show']);
            if (inputPath.indexOf('fail-show') >= 0) throw Error('Slideshow failed');
        },
        start: function (value) {
            if (value !== document) throw Error('Wrong Keynote document');
            calls.push(['start']);
            if (inputPath.indexOf('fail-show') >= 0) throw Error('Slideshow failed');
        }
    });
}
var production = (function (Application, Path, $) {
/* PRODUCTION_SCRIPT */
    return {present: present, retryPermissions: retryPermissions};
})(fakeApplication, function (path) { return path; }, fakeNative);

function present(path) {
    inputPath = path;
    installedApplications = ['com.microsoft.Powerpoint'];
    if (path.indexOf('no-keynote') < 0) {
        if (path.indexOf('classic') < 0 || path.indexOf('both-installed') >= 0)
            installedApplications.push('com.apple.Keynote');
        if (path.indexOf('classic') >= 0 || path.indexOf('both-installed') >= 0)
            installedApplications.push('com.apple.iWork.Keynote');
    }
    authorized = path.indexOf('already-allowed') >= 0;
    permissionRequests = 0;
    var count = path.indexOf('stress') >= 0 ? 10000 : 1;
    var output;
    for (var index = 0; index < count; ++index) {
        calls = [];
        output = /retry-|--retry-automation/.test(path) ? production.retryPermissions() : production.present(path);
        if (count > 1 && output !== '0') throw Error(output);
    }
    return output.split('\n')[0] + '\n' + JSON.stringify({calls: calls, cycles: count, permissionRequests: permissionRequests});
}

function retryPermissions() {
    return present('--retry-automation');
}
