#!/usr/bin/env bash
# Package aimgr as a self-contained AppImage.
#
#   ./packaging/build-appimage.sh [output-dir]
#
# Needs: cmake, g++, appimagetool (https://github.com/AppImage/appimagetool).
# The script bundles aimgr + aimgr-gui and every non-core shared library they
# link against (SDL2, libcurl, ...). OpenGL/graphics drivers are deliberately
# left to the host, which is what Mesa/NVIDIA expect anyway.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/dist}"
BUILD="$ROOT/build"
APPDIR="$ROOT/build/AppDir"

if ! command -v appimagetool >/dev/null; then
  cat >&2 <<MSG
appimagetool not found.
  Arch:   sudo pacman -S appimagetool   (or: yay -S appimagetool-bin)
  Others: curl -L -o appimagetool https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
          chmod +x appimagetool && sudo mv appimagetool /usr/local/bin/
Then re-run: $0
MSG
  exit 1
fi

cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD" -j"$(nproc)"

rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" "$APPDIR/usr/share/applications" \
         "$APPDIR/usr/share/icons/hicolor/256x256/apps"
install -m755 "$BUILD/aimgr" "$BUILD/aimgr-gui" "$APPDIR/usr/bin/"
install -m644 "$ROOT/packaging/aimgr-gui.desktop" "$APPDIR/usr/share/applications/aimgr.desktop"
install -m644 "$ROOT/packaging/aimgr.png" "$APPDIR/usr/share/icons/hicolor/256x256/apps/aimgr.png"
cp "$APPDIR/usr/share/applications/aimgr.desktop" "$APPDIR/aimgr.desktop"
cp "$APPDIR/usr/share/icons/hicolor/256x256/apps/aimgr.png" "$APPDIR/aimgr.png"

echo "== bundling shared libraries"
bundle() {
  local bin="$1"
  ldd "$bin" | awk '/=> \// {print $3}' | while read -r lib; do
    case "$(basename "$lib")" in
      # glibc / loader / graphics stack stay on the host
      libc.so*|libm.so*|libdl.so*|libpthread.so*|librt.so*|ld-linux*|\
      libGL.so*|libGLX.so*|libEGL.so*|libGLdispatch.so*|libdrm.so*|\
      libX11.so*|libxcb.so*|libwayland-*.so*) continue ;;
    esac
    [[ -e "$APPDIR/usr/lib/$(basename "$lib")" ]] || cp -L "$lib" "$APPDIR/usr/lib/"
  done
}
bundle "$BUILD/aimgr"
bundle "$BUILD/aimgr-gui"

cat > "$APPDIR/AppRun" <<'RUN'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export AIMGR_APPIMAGE_LAUNCH=1
# No arguments + a display -> open the GUI; otherwise act as the CLI.
if [ $# -eq 0 ] && [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
  exec "$HERE/usr/bin/aimgr-gui" "$@"
fi
exec "$HERE/usr/bin/aimgr" "$@"
RUN
chmod +x "$APPDIR/AppRun"

mkdir -p "$OUT"
ARCH=x86_64 appimagetool "$APPDIR" "$OUT/aimgr-x86_64.AppImage"
echo
echo "built: $OUT/aimgr-x86_64.AppImage"
echo "try:   $OUT/aimgr-x86_64.AppImage --help"
