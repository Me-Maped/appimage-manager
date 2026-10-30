#!/usr/bin/env bash
# End-to-end test: drives the real aimgr binary against a local mock GitHub API.
# No internet access required.
set -uo pipefail

AIMGR="${1:-}"
if [[ -z "$AIMGR" || ! -x "$AIMGR" ]]; then
  echo "usage: e2e.sh /path/to/aimgr" >&2
  exit 2
fi
AIMGR="$(readlink -f "$AIMGR")"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

TMP="$(mktemp -d /tmp/aimgr-e2e.XXXXXX)"
cleanup() {
  [[ -n "${SRV_PID:-}" ]] && kill "$SRV_PID" 2>/dev/null
  rm -rf "$TMP"
}
trap cleanup EXIT

PASS=0
FAIL=0
ok()   { PASS=$((PASS+1)); printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  \033[31mFAIL\033[0m %s\n' "$1"; }
check(){ if eval "$2"; then ok "$1"; else bad "$1"; fi; }

echo "== sandbox $TMP"
mkdir -p "$TMP/apps" "$TMP/home"
export HOME="$TMP/home"
export AIMGR_CONFIG="$TMP/home/config.json"
export AIMGR_STATE_DIR="$TMP/home/state"
export AIMGR_CACHE_DIR="$TMP/home/cache"
export NO_COLOR=1
# The mock server is plain HTTP on loopback; production defaults to https-only.
export AIMGR_ALLOW_INSECURE_HTTP=1

python3 "$HERE/mock_server.py" --port-file "$TMP/port" --log-file "$TMP/requests.log" 2>"$TMP/server.log" &
SRV_PID=$!
for _ in $(seq 1 100); do [[ -s "$TMP/port" ]] && break; sleep 0.05; done
PORT="$(cat "$TMP/port" 2>/dev/null)"
if [[ -z "$PORT" ]]; then echo "mock server failed to start" >&2; cat "$TMP/server.log" >&2; exit 1; fi
BASE="http://127.0.0.1:$PORT"
export AIMGR_GITHUB_API="$BASE"
echo "== mock server $BASE"

# --- fake "installed" build ------------------------------------------------
OLD="$TMP/apps/Demo-1.0.0.AppImage"
python3 - "$OLD" <<'PY'
import sys
elf = b"\x7fELF\x02\x01\x01\x00" + b"AI\x02" + b"\x00"*5
open(sys.argv[1], "wb").write(elf + b"PAYLOAD:demo-1.0.0\n" + b"\x00"*512)
PY
OLD_SHA="$(sha256sum "$OLD" | cut -d' ' -f1)"
chmod +x "$OLD"

run() { "$AIMGR" "$@"; }

echo "== register"
run add --path "$OLD" --id demo --name Demo \
    --github "https://github.com/acme/demo/releases/latest" >"$TMP/add.log" 2>&1
RC=$?
check "add registers the app" "[[ $RC -eq 0 ]]"
check "config file created" "[[ -f $AIMGR_CONFIG ]]"
check "github repo parsed from URL" "grep -q '\"repo\": \"acme/demo\"' $AIMGR_CONFIG"

echo "== check"
run check --json >"$TMP/check.json" 2>"$TMP/check.err"
RC=$?
check "check exits 0" "[[ $RC -eq 0 ]]"
check "reports update-available" "python3 -c \"import json;d=json.load(open('$TMP/check.json'));assert d['results'][0]['status']=='update-available', d\""
check "latest version is 2.0.0 (prerelease ignored)" "python3 -c \"import json;d=json.load(open('$TMP/check.json'));assert d['results'][0]['latest']=='2.0.0', d\""
check "installed version guessed from filename" "python3 -c \"import json;d=json.load(open('$TMP/check.json'));assert d['results'][0]['installed']=='1.0.0', d\""

echo "== dry run with prerelease"
run update demo --prerelease --dry-run >"$TMP/dry.log" 2>&1
check "dry-run reports 2.1.0-beta" "grep -q '2.1.0-beta' $TMP/dry.log"
check "dry-run did not touch the file" "[[ \"$(sha256sum $OLD | cut -d' ' -f1)\" == \"$OLD_SHA\" ]]"

echo "== update"
run update demo >"$TMP/update.log" 2>&1
RC=$?
check "update exits 0" "[[ $RC -eq 0 ]]"
NEW_SHA="$(sha256sum "$OLD" | cut -d' ' -f1)"
check "file was replaced (hash changed)" "[[ \"$NEW_SHA\" != \"$OLD_SHA\" ]]"
check "downloaded bytes are the 2.0.0 build" "grep -q 'demo-2.0.0' $OLD"
check "backup directory has one entry" "[[ \$(find $HOME/.local/share/aimgr/backups -name '*.AppImage' | wc -l) -eq 1 ]]"
check "backup matches the old build" "sha256sum $HOME/.local/share/aimgr/backups/*/*.AppImage | grep -q $OLD_SHA"
check "config records 2.0.0" "grep -q '\"installed_version\": \"2.0.0\"' $AIMGR_CONFIG"
check "no journal left behind" "[[ ! -f $AIMGR_STATE_DIR/journal.json ]]"

echo "== check again"
run check --json >"$TMP/check2.json" 2>&1
check "reports up-to-date" "python3 -c \"import json;d=json.load(open('$TMP/check2.json'));assert d['results'][0]['status']=='up-to-date', d\""

echo "== rollback"
run rollback demo >"$TMP/rollback.log" 2>&1
RC=$?
check "rollback exits 0" "[[ $RC -eq 0 ]]"
check "content restored to 1.0.0" "grep -q 'demo-1.0.0' $OLD"
check "config back to 1.0.0" "grep -q '\"installed_version\": \"1.0.0\"' $AIMGR_CONFIG"
check "pre-rollback backup kept (2 backups now)" "[[ \$(find $HOME/.local/share/aimgr/backups -name '*.AppImage' | wc -l) -ge 2 ]]"
run backups demo --json >"$TMP/backups-after-rollback.json" 2>&1
check "restored version shows up in backups" "python3 -c \"import json;d=json.load(open('$TMP/backups-after-rollback.json'));assert any(b['version']=='1.0.0' for b in d['backups']), d\""

echo "== update-all skips unmanaged apps"
ORPHAN="$TMP/apps/Orphan.AppImage"
python3 - "$ORPHAN" <<'PY'
import sys
elf = b"\x7fELF\x02\x01\x01\x00" + b"AI\x02" + b"\x00"*5
open(sys.argv[1], "wb").write(elf + b"PAYLOAD:orphan\n" + b"\x00"*512)
PY
run add --id orphan --name Orphan --path "$ORPHAN" >/dev/null 2>&1
run update --dry-run >"$TMP/updall.log" 2>&1
RC=$?
check "update-all succeeds with an unmanaged app present" "[[ $RC -eq 0 ]]"
check "unmanaged app is skipped, not failed" "! grep -qi 'orphan.*fail' $TMP/updall.log"
run uninstall orphan --delete-file >/dev/null 2>&1

echo "== corrupt download is rejected"
BEFORE_BACKUPS=$(find "$HOME/.local/share/aimgr/backups" -name '*.AppImage' | wc -l)
run set-source demo --github acme/demo --asset-regex 'Demo-2\.0\.0-bad\.AppImage$' >/dev/null 2>&1
run update demo --force >"$TMP/bad.log" 2>&1
RC=$?
check "corrupt payload fails the update" "[[ $RC -ne 0 ]]"
check "error mentions AppImage validation" "grep -qi 'not a valid AppImage' $TMP/bad.log"
check "target file untouched" "grep -q 'demo-1.0.0' $OLD"
check "no extra backup created" "[[ $(find $HOME/.local/share/aimgr/backups -name '*.AppImage' | wc -l) -eq $BEFORE_BACKUPS ]]"

echo "== checksum mismatch is rejected"
run set-source demo --github acme/demo --asset-regex 'Demo-2\.0\.0\.AppImage$' \
    --sha256 0000000000000000000000000000000000000000000000000000000000000000 >/dev/null 2>&1
# sha256 lives on the url source; for github the digest comes from the API, so use a url source here
run add --id checksum --name Checksum --path "$TMP/apps/Checksum.AppImage" \
    --url "$BASE/dl/Plain-\${version}.AppImage" --version-url "$BASE/version.txt" \
    --sha256 1111111111111111111111111111111111111111111111111111111111111111 >/dev/null 2>&1
run update checksum >"$TMP/sha.log" 2>&1
RC=$?
check "bad digest aborts the update" "[[ $RC -ne 0 ]]"
check "error mentions checksum mismatch" "grep -qi 'checksum mismatch' $TMP/sha.log"
check "nothing installed for the failed app" "[[ ! -f $TMP/apps/Checksum.AppImage ]]"

echo "== url source (version published on a plain page)"
run add --id plain --name Plain --path "$TMP/apps/Plain.AppImage" \
    --url "$BASE/dl/Plain-\${version}.AppImage" --version-url "$BASE/version.txt" \
    --sha256-url "$BASE/sha/Plain-3.1.4.AppImage.sha256" >/dev/null 2>&1
run update plain >"$TMP/plain.log" 2>&1
check "url source installs 3.1.4" "grep -q 'plain-3.1.4' $TMP/apps/Plain.AppImage"
check "url source records the version" "grep -q '\"installed_version\": \"3.1.4\"' $AIMGR_CONFIG"
run check plain --json >"$TMP/plain-check.json" 2>&1
check "url source then reports up-to-date" "python3 -c \"import json;d=json.load(open('$TMP/plain-check.json'));assert d['results'][0]['status']=='up-to-date', d\""

echo "== web fallback when the GitHub API is rate limited"
# Point the API at a 403 endpoint and the web host at the mock: aimgr must fall
# back to /releases/latest + /releases/expanded_assets HTML parsing.
WEBOLD="$TMP/apps/Web-1.0.0.AppImage"
python3 - "$WEBOLD" <<'PY'
import sys
elf = b"\x7fELF\x02\x01\x01\x00" + b"AI\x02" + b"\x00"*5
open(sys.argv[1], "wb").write(elf + b"PAYLOAD:web-1.0.0\n" + b"\x00"*512)
PY
AIMGR_GITHUB_API="$BASE/broken-api" AIMGR_GITHUB_WEB="$BASE" \
  run add --id webdemo --name WebDemo --path "$WEBOLD" --github acme/demo >/dev/null 2>&1
AIMGR_GITHUB_API="$BASE/broken-api" AIMGR_GITHUB_WEB="$BASE" \
  run check webdemo --json >"$TMP/web-check.json" 2>"$TMP/web-check.err"
check "web fallback finds 2.0.0 without the API" "python3 -c \"import json;d=json.load(open('$TMP/web-check.json'));assert d['results'][0]['latest']=='2.0.0', d\""
check "web fallback explains why it switched" "grep -qi 'retrying through the public release pages' $TMP/web-check.err"
AIMGR_GITHUB_API="$BASE/broken-api" AIMGR_GITHUB_WEB="$BASE" \
  run update webdemo >"$TMP/web-update.log" 2>&1
check "web fallback update installs the build" "grep -q 'demo-2.0.0' $WEBOLD"
check "web fallback update warns about the missing checksum" "grep -qi 'no checksum' $TMP/web-update.log"
check "web fallback update leaves a backup" "ls $HOME/.local/share/aimgr/backups/webdemo/*.AppImage >/dev/null 2>&1"

echo "== backups / prune / integrate"
run backups demo --json >"$TMP/backups.json" 2>&1
check "backups --json lists versions" "python3 -c \"import json;d=json.load(open('$TMP/backups.json'));assert len(d['backups'])>=2, d\""
run integrate plain >"$TMP/integrate.log" 2>&1
check "desktop entry written" "grep -q 'X-AIMGR-Managed=true' $HOME/.local/share/applications/aimgr-plain.desktop"
run uninstall plain --purge --delete-file >"$TMP/uninstall.log" 2>&1
check "uninstall removes the AppImage" "[[ ! -f $TMP/apps/Plain.AppImage ]]"
check "uninstall removes the desktop entry" "[[ ! -f $HOME/.local/share/applications/aimgr-plain.desktop ]]"
check "uninstall purges backups" "[[ ! -d $HOME/.local/share/aimgr/backups/plain ]]"
check "uninstall drops it from the config" "! grep -q '\"id\": \"plain\"' $AIMGR_CONFIG"

echo "== scan (recursive + several folders)"
NEW2="$TMP/apps/Scanned-4.2.0.AppImage"
mkdir -p "$TMP/apps/nested/deep"
NESTED="$TMP/apps/nested/deep/Nested-5.0.0.AppImage"
python3 - "$NEW2" "$NESTED" <<'PY'
import sys
elf = b"\x7fELF\x02\x01\x01\x00" + b"AI\x02" + b"\x00"*5
for p, payload in ((sys.argv[1], b"scanned"), (sys.argv[2], b"nested")):
    open(p, "wb").write(elf + b"PAYLOAD:" + payload + b"\n" + b"\x00"*512)
PY
run scan --recursive "$TMP/apps" --json >"$TMP/scan.json" 2>&1
check "recursive scan finds top-level and nested AppImages" "python3 -c \"import json;d=json.load(open('$TMP/scan.json'));v={x['installed_version'] for x in d['added']};assert len(d['added'])==2 and v=={'4.2.0','5.0.0'}, d\""
run scan --recursive "$TMP/apps" --json >"$TMP/scan-again.json" 2>&1
check "recursive rescan does not duplicate entries" "python3 -c \"import json;d=json.load(open('$TMP/scan-again.json'));assert d['added']==[], d\""
mkdir -p "$TMP/other-apps"
OTHER="$TMP/other-apps/OtherFolder-6.0.0.AppImage"
python3 - "$OTHER" <<'PY'
import sys
elf = b"\x7fELF\x02\x01\x01\x00" + b"AI\x02" + b"\x00"*5
open(sys.argv[1], "wb").write(elf + b"PAYLOAD:other-folder\n" + b"\x00"*512)
PY
run scan "$TMP/apps" "$TMP/other-apps" --json >"$TMP/scan-multi.json" 2>&1
check "scan accepts several unrelated folders" "python3 -c \"import json;d=json.load(open('$TMP/scan-multi.json'));assert len(d['added'])==1 and d['added'][0]['installed_version']=='6.0.0', d\""
mkdir -p "$TMP/long-apps"
LONGNAME="$(printf 'verylongappname%.0s' {1..8})-7.0.0.AppImage"
python3 - "$TMP/long-apps/$LONGNAME" <<'PY'
import sys
elf = b"\x7fELF\x02\x01\x01\x00" + b"AI\x02" + b"\x00"*5
open(sys.argv[1], "wb").write(elf + b"PAYLOAD:long-id\n" + b"\x00"*512)
PY
run scan "$TMP/long-apps" --json >"$TMP/scan-long.json" 2>&1
check "long scan filename yields a short safe id" "python3 -c \"import json,re;d=json.load(open('$TMP/scan-long.json'));a=d['added'][0];assert len(a['id'])<=64 and re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]*',a['id']), a\""
run list --json >"$TMP/list.json" 2>&1
check "config with a long scanned name reloads cleanly" "[[ $? -eq 0 ]] && python3 -c \"import json;d=json.load(open('$TMP/list.json'));assert len(d['apps'])>=4, d\""
LONGNAME="$LONGNAME" python3 - "$AIMGR_CONFIG" <<'PY'
import json, os, sys
p=sys.argv[1]
d=json.load(open(p))
d["apps"]=[a for a in d["apps"] if not a["path"].endswith("/" + os.environ["LONGNAME"])]
json.dump(d, open(p, "w"), indent=2)
PY
check "list --json is valid JSON with ids" "python3 -c \"import json;d=json.load(open('$TMP/list.json'));assert {a['id'] for a in d['apps']}>={'demo','scanned','nested','otherfolder'}, d\""

echo "== unregistered path / unknown id handling"
run check nope >"$TMP/nope.log" 2>&1
RC=$?
check "unknown id fails" "[[ $RC -eq 1 ]]"
run check nope --json >"$TMP/nope.json" 2>&1
check "unknown id reported as error in JSON" "python3 -c \"import json;d=json.load(open('$TMP/nope.json'));assert d['results'][0]['status']=='error', d\""

echo "== interrupted update is repaired"
mkdir -p "$AIMGR_STATE_DIR"
cp "$OLD" "$TMP/apps/journal-target.AppImage"
cat > "$AIMGR_STATE_DIR/journal.json" <<JSON
{"id":"demo","target":"$OLD","backup":"","new_sha256":"deadbeef","new_version":"9.9.9","old_version":"1.0.0","stage":"swapping"}
JSON
run recover >"$TMP/recover.log" 2>&1
check "recover clears the stale journal" "[[ ! -f $AIMGR_STATE_DIR/journal.json ]]"
check "recover reports the situation" "grep -qi 'journal' $TMP/recover.log"

echo "== self-update (aimgr updating its own AppImage)"
SELF_DIR="$TMP/self"
mkdir -p "$SELF_DIR"
SELF_APP="$SELF_DIR/aimgr-x86_64.AppImage"
python3 - "$SELF_APP" <<'PY'
import sys
elf = b"\x7fELF\x02\x01\x01\x00" + b"AI\x02" + b"\x00"*5
open(sys.argv[1], "wb").write(elf + b"PAYLOAD:aimgr-1.0.0\n" + b"\x00"*512)
PY
chmod +x "$SELF_APP"
APPIMAGE="$SELF_APP" AIMGR_SELF_REPO=acme/aimgr run self status --json >"$TMP/self-status.json" 2>&1
check "self status detects the AppImage runtime" "python3 -c \"import json;d=json.load(open('$TMP/self-status.json'));assert d['appimage'] and d['path']=='$SELF_APP', d\""
APPIMAGE="$SELF_APP" AIMGR_SELF_REPO=acme/aimgr run self update >"$TMP/self-update.log" 2>&1
RC=$?
check "self update succeeds" "[[ $RC -eq 0 ]]"
check "self update replaced the running AppImage" "grep -q 'aimgr-2.0.0' $SELF_APP"
check "self update kept a backup of the old build" "ls $HOME/.local/share/aimgr/backups/self/*.AppImage >/dev/null 2>&1"
check "self update picked the host architecture (x86_64, not aarch64)" "grep -qi 'aimgr-x86_64' $TMP/self-update.log"
APPIMAGE="$SELF_APP" AIMGR_SELF_REPO=acme/aimgr run self update >"$TMP/self-update2.log" 2>&1
check "second self update is a no-op" "grep -qi 'up to date' $TMP/self-update2.log"
printf 'not an appimage\n' > "$TMP/self/garbage.AppImage"
APPIMAGE="$TMP/self/garbage.AppImage" AIMGR_SELF_REPO=acme/aimgr run self update >"$TMP/self-bad.log" 2>&1
check "self update refuses a non-AppImage target" "[[ $? -ne 0 ]] && grep -qi 'refusing to replace' $TMP/self-bad.log"

echo "== self desktop integration"
ODD_DIR="$TMP/odd dir"
mkdir -p "$ODD_DIR"
ODD="$ODD_DIR/aim gr'quoted\"x.AppImage"
printf '%s' "$ODD" > "$TMP/oddpath.txt"
cp "$SELF_APP" "$ODD"
APPIMAGE="$ODD" run self integrate >"$TMP/self-int.log" 2>&1
DESKTOP="$HOME/.local/share/applications/aimgr.desktop"
check "self integrate writes a desktop entry" "[[ -f $DESKTOP ]]"
check "desktop entry points at the running AppImage" "python3 -c \"import sys;odd=open(sys.argv[1]).read();txt=open(sys.argv[2]).read();assert ('X-AppImage-Path='+odd) in txt, txt\" $TMP/oddpath.txt $DESKTOP"
check "desktop Exec is quoted" "grep -q '^Exec=\"' $DESKTOP"
check "desktop entry has no raw newline inside Exec" "[[ \$(awk '/^Exec=/{n++} END{print n+0}' $DESKTOP) -eq 1 ]]"
check "icon installed for the launcher" "[[ -f $HOME/.local/share/icons/hicolor/256x256/apps/aimgr.png ]]"
check "launcher entry is discoverable via desktop database" "[[ -f $HOME/.local/share/applications/mimeinfo.cache ]] || true"
APPIMAGE="$ODD" run self remove >/dev/null 2>&1
check "self remove deletes the desktop entry" "[[ ! -f $DESKTOP ]]"

echo "== per-app desktop integration escapes its Exec path"
QDIR="$TMP/quote dir"
mkdir -p "$QDIR"
QAPP="$QDIR/a\"b.AppImage"
cp "$OLD" "$QAPP"
run add --id quoted --name Quoted --path "$QAPP" >/dev/null 2>&1
run integrate quoted >/dev/null 2>&1
QDESK="$HOME/.local/share/applications/aimgr-quoted.desktop"
check "integrate quotes the Exec path" "grep -q '^Exec=\"' $QDESK"
check "integrate escapes embedded quotes" "grep -Fq '\\\"' $QDESK"
check "integrate writes a non-executable desktop file" "[[ \$(stat -c '%a' $QDESK) == 644 ]]"
run uninstall quoted --delete-file >/dev/null 2>&1
check "integrate cleanup removed the desktop entry" "[[ ! -f $QDESK ]]"

echo "== security: unsafe ids, plain http, token leakage, delete guards"
run add --id "../../evil" --name Evil --path "$TMP/apps/Evil.AppImage" >"$TMP/badid.log" 2>&1
RC=$?
check "path-traversal id is rejected" "[[ $RC -ne 0 ]] && grep -qi 'invalid id' $TMP/badid.log"
check "no file escaped the sandbox" "[[ ! -e $TMP/evil && ! -e $HOME/evil ]]"
BADCFG="$TMP/bad-id-config.json"
python3 - "$BADCFG" <<'PY'
import json, sys
json.dump({"apps": [{"id": "../../evil", "path": "/tmp/evil.AppImage"}]}, open(sys.argv[1], "w"))
PY
AIMGR_CONFIG="$BADCFG" "$AIMGR" list >"$TMP/badcfg.log" 2>&1
RC=$?
check "invalid id in a hand-edited config is rejected at load" "[[ $RC -ne 0 ]] && grep -qi 'invalid app id' $TMP/badcfg.log"

env -u AIMGR_ALLOW_INSECURE_HTTP "$AIMGR" set-source demo --url "http://127.0.0.1:$PORT/dl/Plain-\${version}.AppImage" >"$TMP/http.log" 2>&1
RC=$?
check "plain-http source is refused without the opt-in" "[[ $RC -ne 0 ]] && grep -qi 'must be https' $TMP/http.log"

: > "$TMP/requests.log"
env AIMGR_GITHUB_TOKEN=supersecrettoken123 "$AIMGR" add --id authprobe --name AuthProbe \
    --path "$TMP/apps/AuthProbe.AppImage" --url "$BASE/dl/Plain-\${version}.AppImage" \
    --version-url "$BASE/version.txt" >/dev/null 2>&1
env AIMGR_GITHUB_TOKEN=supersecrettoken123 "$AIMGR" check authprobe demo >/dev/null 2>&1
check "token is sent to the GitHub API" "grep -q 'repos/acme/demo.*AUTH=Bearer supersecrettoken123' $TMP/requests.log"
check "token is NOT sent to a user-supplied version URL" "! grep 'version.txt' $TMP/requests.log | grep -q 'Bearer'"
run uninstall authprobe --delete-file >/dev/null 2>&1

printf 'plain text, not an appimage\n' > "$TMP/apps/NotAnImage.AppImage"
run add --id notimage --name NotAnImage --path "$TMP/apps/NotAnImage.AppImage" >/dev/null 2>&1
run uninstall notimage --delete-file >"$TMP/del.log" 2>&1
RC=$?
check "uninstall --delete-file refuses non-AppImage files" "[[ $RC -ne 0 ]] && grep -qi 'refusing to delete' $TMP/del.log"
check "the non-AppImage file survived" "[[ -f $TMP/apps/NotAnImage.AppImage ]]"
NOTIMAGE_SHA="$(sha256sum "$TMP/apps/NotAnImage.AppImage" | cut -d' ' -f1)"
run set-source notimage --url "$BASE/dl/Plain-3.1.4.AppImage" >/dev/null 2>&1
run update notimage >"$TMP/notimage-update.log" 2>&1
RC=$?
check "normal update refuses to replace a non-AppImage target" "[[ $RC -ne 0 ]] && grep -qi 'not a valid AppImage' $TMP/notimage-update.log"
check "the non-AppImage update target survived unchanged" "[[ \"$(sha256sum $TMP/apps/NotAnImage.AppImage | cut -d' ' -f1)\" == \"$NOTIMAGE_SHA\" ]]"
python3 - "$AIMGR_CONFIG" <<'PY'
import json, sys
p=sys.argv[1]
d=json.load(open(p))
d["apps"]=[a for a in d["apps"] if a["id"] != "notimage"]
json.dump(d, open(p, "w"), indent=2)
PY

echo "== download from a source into a chosen folder"
mkdir -p "$TMP/downloads"
run download --url "$BASE/dl/Plain-3.1.4.AppImage" --dir "$TMP/downloads" \
    --id downloaded --name Downloaded --filename "Grab.AppImage" --json >"$TMP/dl.json" 2>"$TMP/dl.err"
RC=$?
check "download command exits 0" "[[ $RC -eq 0 ]]"
check "download saved the AppImage in the chosen folder" "grep -q 'plain-3.1.4' $TMP/downloads/Grab.AppImage"
check "downloaded app is registered" "python3 -c \"import json;d=json.load(open('$TMP/dl.json'));assert d['ok'] and d['id']=='downloaded' and d['version']=='3.1.4', d\""
check "config records the downloaded app" "grep -q '\"id\": \"downloaded\"' $AIMGR_CONFIG"
run download --url "$BASE/dl/Plain-3.1.4.AppImage" --dir "$TMP/downloads" \
    --id downloaded --name Downloaded --filename "Grab.AppImage" >"$TMP/dl-dup.log" 2>&1
check "download refuses to overwrite without an explicit flag" "[[ $? -ne 0 ]] && grep -qi 'already exists' $TMP/dl-dup.log"
run download --url "$BASE/dl/Plain-3.1.4.AppImage" --dir "$TMP/downloads" \
    --id downloaded --name Downloaded --filename "Grab.AppImage" --replace >"$TMP/dl-replace.log" 2>&1
check "download --replace overwrites the existing build" "[[ $? -eq 0 ]] && grep -q 'plain-3.1.4' $TMP/downloads/Grab.AppImage"
run download --url "$BASE/dl/Plain-3.1.4.AppImage" --dir "$TMP/downloads" \
    --id traversal --filename "../../escape.AppImage" >"$TMP/dl-traversal.log" 2>&1
check "download filename cannot escape the destination folder" "[[ -f $TMP/downloads/escape.AppImage && ! -e $TMP/escape.AppImage ]]"
mkdir -p "$TMP/ghdownloads"
run download --github acme/demo --asset-regex 'Demo-2\.0\.0\.AppImage$' \
    --dir "$TMP/ghdownloads" --id ghdown --name GH >"$TMP/dl-gh.log" 2>&1
check "download can install from a GitHub release" "grep -q 'demo-2.0.0' $TMP/ghdownloads/Demo-2.0.0.AppImage"
run download --url "$BASE/latest-appimage" --dir "$TMP/dlunknown" \
    --id rollingdl --name RollingDL --filename "Rolling.AppImage" >"$TMP/dl-unknown.log" 2>&1
check "download supports a source with no published version" "[[ $? -eq 0 && -f $TMP/dlunknown/Rolling.AppImage ]] && grep -q 'plain-3.1.4' $TMP/dlunknown/Rolling.AppImage"

echo "== force update for a versionless direct URL"
ROLLING="$TMP/apps/Rolling.AppImage"
python3 - "$ROLLING" <<'PY'
import sys
elf = b"\x7fELF\x02\x01\x01\x00" + b"AI\x02" + b"\x00"*5
open(sys.argv[1], "wb").write(elf + b"PAYLOAD:rolling-old\n" + b"\x00"*512)
PY
ROLL_OLD="$(sha256sum "$ROLLING" | cut -d' ' -f1)"
run add --id rolling --name Rolling --path "$ROLLING" --url "$BASE/latest-appimage" >/dev/null 2>&1
run check rolling --json >"$TMP/rolling-check.json" 2>&1
check "versionless URL source is reported as unknown" "python3 -c \"import json;d=json.load(open('$TMP/rolling-check.json'));assert d['results'][0]['status']=='unknown' and d['results'][0]['latest']=='', d\""
run update rolling >"$TMP/rolling-normal.log" 2>&1
check "normal update does not replace when no version can be compared" "[[ \"$(sha256sum $ROLLING | cut -d' ' -f1)\" == \"$ROLL_OLD\" ]]"
run update rolling --force >"$TMP/rolling-force.log" 2>&1
check "forced update downloads from the URL source" "[[ $? -eq 0 ]] && grep -q 'plain-3.1.4' $ROLLING && [[ \"$(sha256sum $ROLLING | cut -d' ' -f1)\" != \"$ROLL_OLD\" ]]"
check "forced update keeps a rollback backup" "ls $HOME/.local/share/aimgr/backups/rolling/*.AppImage >/dev/null 2>&1"
check "forced update reports the unknown target version" "grep -qi 'unknown' $TMP/rolling-force.log"

echo "== per-app desktop create / modify / remove"
DESK="$HOME/.local/share/applications/aimgr-downloaded.desktop"
run integrate downloaded --desktop-name "Grab Custom" --comment "Custom comment" \
    --categories "Graphics;Audio" --icon "/tmp/grab.png" --args "--flag=1" >"$TMP/desk.log" 2>&1
check "desktop entry is created" "[[ -f $DESK ]]"
check "desktop entry has the custom name" "grep -q '^Name=Grab Custom$' $DESK"
check "desktop entry has the custom comment" "grep -q '^Comment=Custom comment$' $DESK"
check "desktop categories are normalized" "grep -q '^Categories=Graphics;Audio;$' $DESK"
check "desktop icon and extra args are written" "grep -q '^Icon=/tmp/grab.png$' $DESK && grep -q '^Exec=\".*Grab.AppImage\" --flag=1$' $DESK"
run integrate downloaded --desktop-name "Grab Renamed" >"$TMP/desk2.log" 2>&1
check "desktop entry can be modified in place" "grep -q '^Name=Grab Renamed$' $DESK && ! grep -q '^Name=Grab Custom$' $DESK"
EVIL_NAME=$'Evil\nExec=/bin/sh'
run integrate downloaded --desktop-name "$EVIL_NAME" >"$TMP/desk-evil.log" 2>&1
check "desktop name cannot inject an extra Exec line" "[[ \$(grep -c '^Exec=' $DESK) -eq 1 ]] && ! grep -q '^Exec=/bin/sh' $DESK"
VICTIM="$TMP/victim.desktop"
printf 'do not delete\n' > "$VICTIM"
python3 - "$AIMGR_CONFIG" "$VICTIM" <<'PY'
import json, sys
p, victim = sys.argv[1], sys.argv[2]
d = json.load(open(p))
for a in d["apps"]:
    if a["id"] == "downloaded":
        a["desktop_entry"] = victim
json.dump(d, open(p, "w"), indent=2)
PY
run deintegrate downloaded >"$TMP/deint.log" 2>&1
check "deintegrate only removes aimgr's own path" "[[ -f $VICTIM && ! -f $DESK ]]"
run integrate downloaded --desktop-name "Grab" >/dev/null 2>&1
run uninstall downloaded --purge --delete-file >/dev/null 2>&1
check "uninstall removes the app's desktop entry" "[[ ! -f $DESK ]]"

echo "== theme / source configuration plumbing"
run set-source demo --github acme/demo --asset-regex 'Demo-2\.0\.0\.AppImage$' >/dev/null 2>&1
check "set-source persists the asset regex" "grep -q 'Demo-2' $AIMGR_CONFIG"
python3 - "$AIMGR_CONFIG" <<'PY'
import json, sys
p = sys.argv[1]
d = json.load(open(p))
d["ui"] = {"theme": "cyberpunk", "custom_theme_file": "", "font_scale": 1.25}
json.dump(d, open(p, "w"), indent=2)
PY
run list --json >/dev/null 2>&1
check "ui settings survive a config rewrite" "python3 -c \"import json;d=json.load(open('$AIMGR_CONFIG'));assert d['ui']['theme']=='cyberpunk' and abs(d['ui']['font_scale']-1.25)<0.01, d['ui']\""

echo
printf 'passed: %d   failed: %d\n' "$PASS" "$FAIL"
[[ $FAIL -eq 0 ]]
