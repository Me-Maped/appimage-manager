#!/usr/bin/env bash
# Headless GUI smoke test.
#
# Renders the real ImGui window twice with different themes (software GL inside
# Xvfb), checks that a screenshot is produced, that the two themes actually
# render differently, and that the theme name is reported. Skips (exit 77) when
# the host has no Xvfb/OpenGL so CI without a display stays green.
set -uo pipefail

AIMGR_GUI="${1:-}"
if [[ -z "$AIMGR_GUI" || ! -x "$AIMGR_GUI" ]]; then
  echo "usage: gui_smoke.sh /path/to/aimgr-gui" >&2
  exit 2
fi
AIMGR_GUI="$(readlink -f "$AIMGR_GUI")"
AIMGR_BIN="$(dirname "$AIMGR_GUI")/aimgr"

if ! command -v xvfb-run >/dev/null || ! command -v python3 >/dev/null; then
  echo "SKIP: xvfb-run or python3 not available"
  exit 77
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMP="$(mktemp -d /tmp/aimgr-gui-smoke.XXXXXX)"
cleanup() {
  [[ -n "${SRV_PID:-}" ]] && kill "$SRV_PID" 2>/dev/null
  rm -rf "$TMP"
}
trap cleanup EXIT
export HOME="$TMP/home"
mkdir -p "$HOME" "$TMP/apps"
export AIMGR_CONFIG="$TMP/home/config.json" AIMGR_STATE_DIR="$TMP/home/state" \
       AIMGR_CACHE_DIR="$TMP/home/cache" NO_COLOR=1
export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe MESA_LOADER_DRIVER_OVERRIDE=llvmpipe
export __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json

python3 - "$TMP/apps/DemoOne-1.2.3.AppImage" "$TMP/apps/DemoTwo-4.5.6.AppImage" <<'PY'
import sys
elf = b"\x7fELF\x02\x01\x01\x00" + b"AI\x02" + b"\x00" * 5
for path in sys.argv[1:]:
    open(path, "wb").write(elf + b"PAYLOAD:demo\n" + b"\x00" * 512)
PY
"$AIMGR_BIN" scan "$TMP/apps" >/dev/null 2>&1 || true

# A source that would be hit by a startup auto-check. The mock server records
# every request; the GUI must leave this log empty until the user clicks Check.
python3 "$HERE/mock_server.py" --port-file "$TMP/port" --log-file "$TMP/requests.log" \
  2>"$TMP/server.log" &
SRV_PID=$!
for _ in $(seq 1 100); do [[ -s "$TMP/port" ]] && break; sleep 0.05; done
PORT="$(cat "$TMP/port" 2>/dev/null)"
if [[ -z "$PORT" ]]; then
  echo "  mock server failed to start"; cat "$TMP/server.log"; exit 1
fi
export AIMGR_ALLOW_INSECURE_HTTP=1 AIMGR_GITHUB_API="http://127.0.0.1:$PORT"
"$AIMGR_BIN" add --id smoke --name Smoke --path "$TMP/apps/DemoOne-1.2.3.AppImage" \
  --github acme/demo >/dev/null 2>&1
: > "$TMP/requests.log"

shoot() {  # $1 = theme, $2 = output png
  timeout 90 xvfb-run -a --server-args="-screen 0 1400x900x24" \
    "$AIMGR_GUI" --theme "$1" --autoquit-ms 2200 --screenshot "$2" 2>"$TMP/$1.log"
  local rc=$?
  grep -q "screenshot written" "$TMP/$1.log" || { echo "  no screenshot for $1"; cat "$TMP/$1.log"; return 1; }
  return $rc
}

fail=0
shoot cyberpunk "$TMP/a.png" || fail=1
shoot brutalist "$TMP/b.png" || fail=1
grep -q "theme: 01 / Cyberpunk" "$TMP/cyberpunk.log" || { echo "  theme label missing (cyberpunk)"; fail=1; }
grep -q "theme: 07 / Neo Brutalism" "$TMP/brutalist.log" || { echo "  theme label missing (brutalist)"; fail=1; }
if [[ -s "$TMP/requests.log" ]]; then
  echo "  startup unexpectedly performed an update check:"; cat "$TMP/requests.log"; fail=1
fi

python3 - "$TMP/a.png" "$TMP/b.png" <<'PY' || exit 1
import sys
from PIL import Image
import numpy as np
a = np.asarray(Image.open(sys.argv[1]).convert("RGB"), dtype=int)
b = np.asarray(Image.open(sys.argv[2]).convert("RGB"), dtype=int)
if a.shape != b.shape:
    print("  screenshot sizes differ"); raise SystemExit(1)
diff = float(np.abs(a - b).mean())
print(f"  mean abs pixel difference between themes: {diff:.2f}")
if a.std() < 1.0:
    print("  screenshot looks blank"); raise SystemExit(1)
if diff < 20:
    print("  themes render identically - theme switching is broken"); raise SystemExit(1)
PY
rc=$?
if [[ $fail -ne 0 || $rc -ne 0 ]]; then
  echo "GUI smoke test FAILED"
  exit 1
fi
echo "GUI smoke test passed"
