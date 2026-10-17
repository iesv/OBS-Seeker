"""Run the real signed helper and unchanged JXA script against simulated applications."""
import hashlib
import json
import plistlib
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def main():
    bundle, production = map(Path, sys.argv[1:])
    fixture = Path(__file__).with_name('macos-automation-fixture.js').read_text()
    fixture = fixture.replace('/* PRODUCTION_SCRIPT */', production.read_text())
    plugins = Path.home() / 'Library/Application Support/obs-studio/plugins'
    plugins.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='seeker-mac-test-', dir=plugins) as directory:
        root = Path(directory)
        app = root / 'OBS-Seeker.plugin/Contents/Helpers/Seeker Simulation.app'
        shutil.copytree(bundle, app)
        script = app / 'Contents/Resources/presentation-macos.js'
        script.write_text(fixture)
        plist = app / 'Contents/Info.plist'
        info = plistlib.loads(plist.read_bytes())
        identifier = 'com.andy.SeekerPresentation.Simulation.' + root.name
        info['CFBundleIdentifier'] = identifier
        plist.write_bytes(plistlib.dumps(info))
        subprocess.run(['codesign', '--force', '--sign', '-', '--preserve-metadata=entitlements,flags', str(app)], check=True)

        def pending_retry():
            value = subprocess.run(['/usr/bin/defaults', 'read', identifier, 'RetryAutomation'],
                                   capture_output=True, text=True)
            return value.returncode == 0 and value.stdout.strip() == '1'

        def launch(path, result):
            result.write_text('2\n')
            return subprocess.Popen(['/usr/bin/open', '-n', '-W', '-g', '-a', str(app),
                                     '--args', str(path), str(result)], stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE)

        for name, expected in [('诗歌 \' & %1.PPTX', 0), ('ordinary.ppt', 0),
                               ('keynote.key', 0), ('already-allowed.key', 0),
                               ('new-only.key', 0), ('classic-only.key', 0),
                               ('both-installed.key', 0), ('no-keynote.key', 2),
                               ('permission-denied.key', 3), ('permission-refused.key', 3),
                               ('permission-denied.pptx', 3), ('permission-refused.pptx', 3),
                               ('permission-error.key', 2), ('permission-error.pptx', 2), ('fail-app.pptx', 2),
                               ('fail-open.key', 2), ('fail-activate.pptx', 1),
                               ('fail-show.pptx', 1), ('fail-show.key', 1),
                               ('stress.pptx', 0), ('stress.key', 0), ('stress-classic.key', 0),
                               ('retry-none.key', 0), ('retry-allowed.key', 0),
                               ('retry-permission-denied.key', 3), ('retry-failed.key', 2)]:
            subprocess.run(['/usr/bin/defaults', 'delete', identifier, 'RetryAutomation'], capture_output=True)
            path = root / name
            path.write_bytes(b'Original presentation; never save or change this file.')
            digest = hashlib.sha256(path.read_bytes()).digest()
            result = root / 'result.txt'
            process = launch(path, result)
            stdout, stderr = process.communicate(timeout=15)
            # Like the plugin, check the result: open -W may miss an already exited helper.
            assert process.returncode == 0 or stderr == (
                b'Unable to block on applications (initial call to kevent() failed: No such process)\n'
            ), (name, process.returncode, stdout, stderr)
            code, details = result.read_text().split('\n', 1)
            assert int(code) == expected, (name, code, details)
            evidence = json.loads(details)
            calls = evidence['calls']
            keynote = path.suffix.lower() == '.key'
            target = 'com.microsoft.Powerpoint'
            if keynote:
                target = ('com.apple.iWork.Keynote' if 'classic' in name or name == 'no-keynote.key'
                          else 'com.apple.Keynote')
            if name.startswith('retry-'):
                assert calls[0] == ['reset'], calls
                assert len(calls) == (1 if name in ('retry-none.key', 'retry-failed.key') else 3), calls
                if len(calls) > 1:
                    assert calls[1:] == [['permission', app_id, True] for app_id in
                                         ('com.microsoft.Powerpoint', 'com.apple.Keynote')], calls
            else:
                assert calls[0] == ['permission', target, True], calls
                if expected == 0:
                    assert calls[1:] == [['application', target], ['open', str(path)], ['activate'],
                                        ['start' if keynote else 'show']], calls
                if name.startswith('permission-') or name == 'no-keynote.key':
                    assert calls == [['permission', target, True]], calls
            if not name.startswith('retry-') and not name.startswith(('permission-error', 'permission-denied', 'no-keynote')):
                assert evidence['permissionRequests'] == (0 if name.startswith('already-allowed') else 1), evidence
            if name == 'no-keynote.key':
                assert evidence['permissionRequests'] == 0, 'Unavailable Keynote requested consent'
            if name.startswith('permission-denied'):
                assert evidence['permissionRequests'] == 0, 'A previous denial triggered another consent request'
            assert pending_retry() == (expected == 3), (name, 'Denied access must persist across helper restarts')
            assert evidence['cycles'] == (10000 if 'stress' in name else 1)
            assert hashlib.sha256(path.read_bytes()).digest() == digest, name

        # Run the actual tccutil path, scoped only to this unique simulation app.
        for seed_retry, refused in ((False, False), (True, True), (False, True), (False, False), (False, False)):
            if seed_retry:
                subprocess.run(['/usr/bin/defaults', 'write', identifier, 'RetryAutomation', '-bool', 'true'], check=True)
            pending = pending_retry()
            subprocess.run(['/usr/bin/defaults', 'write', identifier, 'SimulationRetryDenied', '-bool',
                            'true' if refused else 'false'], check=True)
            result = root / 'retry-result.txt'
            result.write_text('2\n')
            process = subprocess.Popen([str(app / 'Contents/MacOS/OBS-Seeker-present'),
                                        '--retry-automation', str(result)],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            stdout, stderr = process.communicate(timeout=15)
            assert process.returncode == 0, (stdout, stderr)
            code, details = result.read_text().split('\n', 1)
            evidence = json.loads(details)
            assert code == ('3' if pending and refused else '0'), (pending, refused, code, details, stdout, stderr)
            assert len(evidence['calls']) == (3 if pending else 1), evidence
            assert pending_retry() == (pending and refused), 'Recovery did not retain another denial or clear a success'

        # Removing the request file must cancel even a blocked automation call.
        result = root / 'cancel-result.txt'
        process = launch(root / 'cancel.pptx', result)
        deadline = time.monotonic() + 8
        while not result.read_text().startswith('1\n') and time.monotonic() < deadline:
            time.sleep(0.02)
        assert result.read_text().startswith('1\n'), 'Helper never started'
        time.sleep(0.15)
        result.unlink()
        process.communicate(timeout=5)
        assert not result.exists(), 'Canceled helper recreated its request file'

        # Broken resource packaging must produce a safe default-open fallback.
        script.unlink()
        subprocess.run(['codesign', '--force', '--sign', '-', '--preserve-metadata=entitlements,flags', str(app)], check=True)
        result = root / 'missing-result.txt'
        process = launch(root / 'missing.pptx', result)
        process.communicate(timeout=10)
        assert result.read_text().startswith('2\n'), 'Missing script did not allow fallback'
        subprocess.run(['/usr/bin/defaults', 'delete', identifier], capture_output=True)
    print('STRESS: Mac helper/JXA: 30000 PowerPoint/new and classic Keynote cycles; literal paths, '
          'authorization states, persistent denial/restart recovery, scoped tccutil reset, '
          'open/show failures, missing resources, cancellation and unchanged files passed')


if __name__ == '__main__':
    main()
