#!/usr/bin/env bash
# Package four programs at once with the pinned AppImage runtime.
. "$(dirname "$0")/lib.sh"

grep -q "pub const TAG: &str = \"$APPIMAGE_RUNTIME_TAG\";" relift/src/appimage_runtime.rs

dir="$CI_WORK/appimagetool-$APPIMAGETOOL_VERSION"
fetch "https://github.com/AppImage/appimagetool/releases/download/$APPIMAGETOOL_VERSION/appimagetool-x86_64.AppImage" \
  "$APPIMAGETOOL_SHA256" "$dir/appimagetool.AppImage"
if [ ! -x "$dir/bin/appimagetool" ]; then
  chmod +x "$dir/appimagetool.AppImage"
  (cd "$dir" && ./appimagetool.AppImage --appimage-extract >/dev/null)
  mkdir -p "$dir/bin"
  ln -sf "$dir/squashfs-root/AppRun" "$dir/bin/appimagetool"
fi
PATH="$dir/bin:$PATH" cargo test --locked -p relift --test appimage_parallel -- --nocapture
