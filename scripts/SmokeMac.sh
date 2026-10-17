#!/bin/bash
set -euo pipefail
project="$(cd "$(dirname "$0")/.." && pwd)"
cd "$project"
runtime="$project/.runtime"
mkdir -p "$runtime/mount" "$runtime/logs"
version="$(python3 -c 'import json; print(json.load(open("buildspec.json"))["version"])')"
dmg="$runtime/OBS-32.0.4-Apple.dmg"
curl --fail --location --retry 2 'https://github.com/obsproject/obs-studio/releases/download/32.0.4/OBS-Studio-32.0.4-macOS-Apple.dmg' -o "$dmg"
echo "7c056644636a794cb18281af98d54b5795ad8b0985e950d93b8c510b645b6298  $dmg" | shasum -a 256 -c -
hdiutil attach "$dmg" -readonly -nobrowse -mountpoint "$runtime/mount"
trap 'hdiutil detach "$runtime/mount" >/dev/null 2>&1 || true' EXIT
ditto "$runtime/mount/OBS.app" "$runtime/OBS.app"
hdiutil detach "$runtime/mount"
trap - EXIT
package="$(find "$runtime/artifact" -name '*-macos-arm64.zip' -print -quit)"
test -n "$package"
ditto -x -k "$package" "$runtime/plugin"
config="$HOME/Library/Application Support/obs-studio"
mkdir -p "$config/plugins" "$config/logs"
# 同时验证安装脚本可清除隔离属性，并可重复运行。
source_bundle="$runtime/plugin/macos-package/OBS-Seeker.plugin"
installed_bundle="$config/plugins/OBS-Seeker.plugin"
mkdir -p "$installed_bundle/Contents/Helpers/OBS-Seeker-present.app"
printf 'legacy helper\n' > "$installed_bundle/Contents/Helpers/OBS-Seeker-present.app/obsolete"
xattr -w com.apple.quarantine '0081;00000000;OBS-Seeker-test;' "$source_bundle"
xattr -w com.apple.quarantine '0081;00000000;OBS-Seeker-test;' "$source_bundle/Contents/MacOS/OBS-Seeker"
bash "$runtime/plugin/macos-package/InstallMac.sh" --quiet
xattr -dr com.apple.quarantine "$source_bundle"
bash "$runtime/plugin/macos-package/InstallMac.sh" --quiet
for item in "$installed_bundle" "$installed_bundle/Contents/MacOS/OBS-Seeker"; do
  if xattr -p com.apple.quarantine "$item" >/dev/null 2>&1; then
    echo 'Installer left a quarantine attribute.' >&2
    exit 1
  fi
done
helper="$installed_bundle/Contents/Helpers/OBS-Seeker-present.app"
test -x "$helper/Contents/MacOS/OBS-Seeker-present"
test ! -e "$helper/obsolete"
test -n "$(/usr/libexec/PlistBuddy -c 'Print :NSAppleEventsUsageDescription' "$helper/Contents/Info.plist")"
codesign --verify --strict "$helper"
codesign --verify --deep --strict "$installed_bundle"
if /usr/libexec/PlistBuddy -c 'Print :NSAppleEventsUsageDescription' "$installed_bundle/Contents/Info.plist" >/dev/null 2>&1; then
  echo 'Obsolete automation permission remains.' >&2
  exit 1
fi
printf '[General]\nFirstRun=true\nLanguage=en-US\n' > "$config/user.ini"
printf '[General]\nLastVersion=536870912\nMacOSPermissionsDialogLastShown=1\n' > "$config/global.ini"
"$runtime/OBS.app/Contents/MacOS/OBS" --multi --disable-updater > "$runtime/console.txt" 2>&1 &
obs_pid=$!
cleanup() {
  kill -TERM "$obs_pid" 2>/dev/null || true
  find "$config/logs" -name '*.txt' -exec cp {} "$runtime/logs/" \; || true
}
trap cleanup EXIT
started=0
for ((i=0; i<120; ++i)); do
  log="$(find "$config/logs" -name '*.txt' -print | sort | tail -1)"
  if [[ -n "$log" ]] && grep -q '\[OBS-Seeker\] Search dock registered' "$log" && grep -q '==== Startup complete' "$log"; then
    started=1
    break
  fi
  kill -0 "$obs_pid" 2>/dev/null || break
  sleep 0.5
done
test "$started" = 1
grep -Fq "[OBS-Seeker] Loaded version $version" "$log"
grep -Fq '[OBS-Seeker] UI title: Seeker' "$log"
grep -Fq '[OBS-Seeker] Search and refresh hotkeys registered' "$log"
grep -Fq '[OBS-Seeker] Favorites hotkeys registered' "$log"
kill -INT "$obs_pid"
for ((i=0; i<40; ++i)); do
  kill -0 "$obs_pid" 2>/dev/null || break
  sleep 0.5
done
if kill -0 "$obs_pid" 2>/dev/null; then
  cp "$log" "$runtime/logs/OBS.txt"
  echo 'FAIL: OBS did not exit normally.' > "$runtime/validation.txt"
  exit 1
fi
wait "$obs_pid"
trap - EXIT
cp "$log" "$runtime/logs/OBS.txt"
grep -q '==== Shutting down' "$log"
echo 'PASS: Apple Silicon OBS 32 loaded OBS-Seeker, registered its dock/hotkeys, and exited normally.' | tee "$runtime/validation.txt"
