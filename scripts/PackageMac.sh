#!/bin/bash
set -euo pipefail
project="$(cd "$(dirname "$0")/.." && pwd)"
cd "$project"
version="$(python3 -c 'import json; print(json.load(open("buildspec.json"))["version"])')"
stage="$project/release/macos-package"
mkdir -p "$stage" "$project/dist"
ditto "release/install/OBS-Seeker.plugin" "$stage/OBS-Seeker.plugin"
helper="$stage/OBS-Seeker.plugin/Contents/Helpers/OBS-Seeker-present.app"
test -x "$helper/Contents/MacOS/OBS-Seeker-present"
codesign --force --sign - --options runtime --entitlements cmake/macos/presentation-helper.entitlements "$helper"
codesign --force --sign - "$stage/OBS-Seeker.plugin"
codesign --verify --deep --strict "$stage/OBS-Seeker.plugin"
test "$(lipo -archs "$stage/OBS-Seeker.plugin/Contents/MacOS/OBS-Seeker")" = arm64
printf '%s\n' 'Copy `OBS-Seeker.plugin` to `~/Library/Application Support/obs-studio/plugins/`.' > "$stage/README.MD"
cp scripts/InstallMac.sh "$stage/InstallMac.sh"
cp LICENSE "$stage/LICENSE"
chmod +x "$stage/InstallMac.sh"
zip="$project/dist/OBS-Seeker-$version-macos-arm64.zip"
ditto -c -k --sequesterRsrc --keepParent "$stage" "$zip"
shasum -a 256 "$zip" | sed "s|$project/dist/||" > dist/SHA256SUMS-macos.txt
echo "Created $zip"
