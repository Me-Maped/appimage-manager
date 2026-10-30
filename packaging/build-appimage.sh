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

APPIMAGETOOL="${APPIMAGETOOL:-$(command -v appimagetool || true)}"
if [[ -z "$APPIMAGETOOL" ]]; then
  cat >&2 <<MSG
appimagetool not found.
  Arch:   sudo pacman -S appimagetool   (or: yay -S appimagetool-bin)
  Others: curl -L -o appimagetool https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
          chmod +x appimagetool && sudo mv appimagetool /usr/local/bin/
  Or point APPIMAGETOOL at a downloaded copy: APPIMAGETOOL=/path/to/appimagetool $0
Then re-run: $0
MSG
  exit 1
fi

# appimagetool is itself an AppImage; without FUSE it must unpack before running.
if [[ ! -e /dev/fuse ]]; then
  export APPIMAGE_EXTRACT_AND_RUN=1
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
      # glibc / loader, the GL stack (glvnd + Mesa + DRI) and the X11/Wayland
      # client stack stay on the host: drivers must match the running kernel and
      # GPU, and mixing a bundled libglvnd with the host's dispatcher breaks.
      libc.so*|libm.so*|libdl.so*|libpthread.so*|librt.so*|ld-linux*|\
      libGL.so*|libOpenGL.so*|libGLX.so*|libEGL.so*|libGLdispatch.so*|libdrm.so*|\
      libgbm.so*|libglapi.so*|libX11*.so*|libxcb*.so*|libXau.so*|libXdmcp.so*|\
      libXext.so*|libXrandr.so*|libXi.so*|libXcursor.so*|libXfixes.so*|\
      libXss.so*|libwayland-*.so*) continue ;;
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
APP="$OUT/aimgr-x86_64.AppImage"
ARCH=x86_64 "$APPIMAGETOOL" "$APPDIR" "$APP"
(cd "$OUT" && sha256sum "$(basename "$APP")" > "$(basename "$APP").sha256")

echo
echo "built: $APP"
echo "sha256: $(cut -d' ' -f1 "$APP.sha256")"
echo "try:   $APP --help"
