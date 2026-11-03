#!/usr/bin/env bash
set -euo pipefail

SCRIPT="${1:-packaging/build-appimage.sh}"
SCRIPT="$(readlink -f "$SCRIPT")"
ROOT="$(cd "$(dirname "$SCRIPT")/.." && pwd)"
TMP="$(mktemp -d /tmp/aimgr-package-smoke.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/bin" "$TMP/build" "$TMP/out"
cat > "$TMP/build/CMakeCache.txt" <<CACHE
CMAKE_HOME_DIRECTORY:INTERNAL=/home/maped/Apps/appimage-manager
CMAKE_CACHEFILE_DIR:INTERNAL=/home/maped/Apps/appimage-manager/build
CACHE
printf 'stale sentinel\n' > "$TMP/build/stale-sentinel"

cat > "$TMP/bin/cmake" <<'FAKE_CMAKE'
#!/usr/bin/env bash
set -euo pipefail
if [[ "${1:-}" == "--build" ]]; then
  exit 0
fi
src=""
build=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    -S) src="$2"; shift 2 ;;
    -B) build="$2"; shift 2 ;;
    *) shift ;;
  esac
done
[[ -n "$src" && -n "$build" ]]
mkdir -p "$build"
printf '#!/bin/sh\nexit 0\n' > "$build/aimgr"
printf '#!/bin/sh\nexit 0\n' > "$build/aimgr-gui"
chmod +x "$build/aimgr" "$build/aimgr-gui"
cat > "$build/CMakeCache.txt" <<CACHE
CMAKE_HOME_DIRECTORY:INTERNAL=$src
CMAKE_CACHEFILE_DIR:INTERNAL=$build
CACHE
FAKE_CMAKE

cat > "$TMP/bin/ldd" <<'FAKE_LDD'
#!/bin/sh
exit 0
FAKE_LDD

cat > "$TMP/bin/appimagetool" <<'FAKE_APPIMAGETOOL'
#!/bin/sh
printf 'fake AppImage\n' > "$2"
chmod +x "$2"
FAKE_APPIMAGETOOL
chmod +x "$TMP/bin/"*

export PATH="$TMP/bin:$PATH"
export AIMGR_APPIMAGE_BUILD_DIR="$TMP/build"

"$SCRIPT" "$TMP/out" > "$TMP/first.log" 2>&1
grep -q 'resetting stale CMake cache' "$TMP/first.log"
test ! -e "$TMP/build/stale-sentinel"
test -x "$TMP/out/aimgr-x86_64.AppImage"
test -s "$TMP/out/aimgr-x86_64.AppImage.sha256"

"$SCRIPT" "$TMP/out" > "$TMP/second.log" 2>&1
if grep -q 'resetting stale CMake cache' "$TMP/second.log"; then
  echo "valid package build cache was reset on the second run" >&2
  exit 1
fi

if AIMGR_APPIMAGE_BUILD_DIR="$ROOT" "$SCRIPT" "$TMP/out" > "$TMP/unsafe.log" 2>&1; then
  echo "script accepted the source tree as a build directory" >&2
  exit 1
fi
grep -q 'refusing unsafe AppImage build directory' "$TMP/unsafe.log"
echo "package script smoke test passed"
