#!/bin/bash
set -euo pipefail
source_dir="$(cd "$(dirname "$0")" && pwd)"
target_dir="$HOME/Library/Application Support/obs-studio/plugins"
if pgrep -x OBS >/dev/null; then
  echo 'Please close OBS before installing or updating Seeker.' >&2
  exit 1
fi
test -d "$source_dir/OBS-Seeker.plugin"
mkdir -p "$target_dir"
PLUGIN="$target_dir/OBS-Seeker.plugin"
if [[ "$source_dir/OBS-Seeker.plugin" -ef "$PLUGIN" ]]; then
  echo 'Extract the ZIP to a different folder before installing.' >&2
  exit 1
fi
for path in "$PLUGIN" "$PLUGIN/Contents" "$PLUGIN/Contents/Helpers" "$PLUGIN/Contents/Helpers/OBS-Seeker-present.app"; do
  if [[ -L "$path" ]]; then
    echo "Cannot update a symbolic link: $path" >&2
    exit 1
  fi
done
# 整体替换插件包，清除旧版本残留。
rm -rf "$PLUGIN"
ditto "$source_dir/OBS-Seeker.plugin" "$target_dir/OBS-Seeker.plugin"
# 清除隔离属性，解除系统加载拦截
xattr -dr com.apple.quarantine "$PLUGIN"
codesign --force --sign - "$PLUGIN"
codesign --verify --deep --strict "$PLUGIN"
echo "Installed to $target_dir/OBS-Seeker.plugin"
echo 'Start OBS and enable the Seeker dock.'
if [[ "${1:-}" != '--quiet' ]]; then
  for seconds in 5 4 3 2 1; do
    printf '\rExiting in %ss... Press any key to exit. ' "$seconds"
    if [[ ! -t 0 ]]; then
      sleep 1
    elif IFS= read -r -s -n 1 -t 1; then
      break
    fi
  done
  printf '\n'
fi
