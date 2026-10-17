function present(path) {
    var opened = false;
    try {
        var keynote = /\.key$/i.test(path);
        var automation = $.NSClassFromString('SeekerAutomation');
        var identifier = keynote ? automation.keynoteIdentifier.js : 'com.microsoft.Powerpoint';
        var permission = automation.permissionForAskUser(identifier, true);
        if (permission !== 0)
            return (permission === -1743 ? '3' : '2') + '\n' + identifier + ' automation authorization: ' + permission;
        var app = Application(identifier);
        app.open(Path(path));
        opened = true;
        app.activate();
        if (keynote) {
            app.start(app.documents[0]);
        } else {
            app.runSlideShow(app.activePresentation.slideShowSettings);
        }
        return '0';
    } catch (error) {
        return (opened ? '1' : error.errorNumber === -1743 ? '3' : '2') + '\n' + String(error);
    }
}

function retryPermissions() {
    var automation = $.NSClassFromString('SeekerAutomation');
    var retry = automation.retryAutomation;
    if (retry === 0) return '0';
    if (retry !== 1) return '2\nCannot reset presentation automation';
    var denied = false;
    var details = [];
    ['com.microsoft.Powerpoint', automation.keynoteIdentifier.js].forEach(function (identifier) {
        var permission = automation.permissionForAskUser(identifier, true);
        denied = denied || permission === -1743;
        details.push(identifier + ': ' + permission);
    });
    return (denied ? '3' : '0') + '\n' + details.join('\n');
}
