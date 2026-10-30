#!/usr/bin/env python3
"""Offline stand-in for the GitHub releases API plus a download host.

Serves (all on 127.0.0.1, plain HTTP - the CLI is run with
AIMGR_ALLOW_INSECURE_HTTP=1 during tests):

  GET /repos/<owner>/<repo>/releases          release list
  GET /repos/acme/missing/releases            404 (error-path coverage)
  GET /broken-api/...                         403 (API quota exhaustion)
  GET /<owner>/<repo>/releases/latest         302 -> /releases/tag/<tag>
  GET /<owner>/<repo>/releases/tag/<tag>      release page (web fallback)
  GET /<owner>/<repo>/releases/expanded_assets/<tag>  asset list (web fallback)
  GET /<owner>/<repo>/releases/download/<tag>/<name>  asset bytes
  GET /dl/<name>                              asset bytes
  GET /version.txt                            "stable=3.1.4"
  GET /sha/<name>.sha256                      checksum file
  GET /echo-auth                              {"authorization": ...}

Every request is appended to an optional log file as "<path>\tAUTH=<value|->"
so the test suite can prove the GitHub token is never sent to foreign hosts.
"""
import argparse
import hashlib
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LOG_FILE = None


def fake_appimage(payload: str) -> bytes:
    elf = b"\x7fELF\x02\x01\x01\x00" + b"AI\x02" + b"\x00" * 5
    return elf + b"PAYLOAD:" + payload.encode() + b"\n" + b"\x00" * 512


REPO_ASSETS = {
    "acme/demo": {
        "Demo-2.0.0.AppImage": fake_appimage("demo-2.0.0"),
        "Demo-2.0.0-bad.AppImage": b"<html>404 not found</html>",
        "Demo-2.1.0-beta.AppImage": fake_appimage("demo-2.1.0-beta"),
        "Demo-1.9.0.AppImage": fake_appimage("demo-1.9.0"),
        "Demo-3.0.0.AppImage": fake_appimage("demo-3.0.0"),
        "Plain-3.1.4.AppImage": fake_appimage("plain-3.1.4"),
    },
    "acme/aimgr": {
        "aimgr-x86_64.AppImage": fake_appimage("aimgr-2.0.0"),
        "aimgr-aarch64.AppImage": fake_appimage("aimgr-2.0.0-aarch64"),
    },
}

ALL_ASSETS = {}
for _repo, _assets in REPO_ASSETS.items():
    ALL_ASSETS.update(_assets)


def sha(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def releases_for(repo: str, base: str):
    if repo not in REPO_ASSETS:
        return None
    tags = {"acme/demo": ["v2.0.0"], "acme/aimgr": ["v2.0.0"]}[repo]
    entries = []
    ordered = list(REPO_ASSETS[repo].keys())
    for tag in tags:
        version = tag[1:] if tag.startswith("v") else tag
        assets = []
        for name in ordered:
            if repo != "acme/aimgr" and version not in name and "bad" not in name:
                continue
            body = REPO_ASSETS[repo][name]
            assets.append({
                "name": name,
                "browser_download_url": f"{base}/{repo}/releases/download/{tag}/{name}",
                "size": len(body),
                "digest": "sha256:" + sha(body),
            })
        if not assets:
            continue
        entries.append({
            "tag_name": tag,
            "name": version,
            "draft": False,
            "prerelease": False,
            "published_at": "2025-09-01T10:00:00Z",
            "html_url": f"{base}/{repo}/releases/tag/{tag}",
            "assets": assets,
        })
    if repo == "acme/demo":
        entries.append({
            "tag_name": "v2.1.0-beta",
            "name": "2.1.0 beta",
            "draft": False,
            "prerelease": True,
            "published_at": "2025-09-10T10:00:00Z",
            "html_url": f"{base}/{repo}/releases/tag/v2.1.0-beta",
            "assets": [{
                "name": "Demo-2.1.0-beta.AppImage",
                "browser_download_url": f"{base}/{repo}/releases/download/v2.1.0-beta/Demo-2.1.0-beta.AppImage",
                "size": len(REPO_ASSETS[repo]["Demo-2.1.0-beta.AppImage"]),
                "digest": "sha256:" + sha(REPO_ASSETS[repo]["Demo-2.1.0-beta.AppImage"]),
            }],
        })
        entries.append({
            "tag_name": "v1.9.0",
            "name": "1.9.0",
            "draft": False,
            "prerelease": False,
            "published_at": "2025-08-01T10:00:00Z",
            "html_url": f"{base}/{repo}/releases/tag/v1.9.0",
            "assets": [{
                "name": "Demo-1.9.0.AppImage",
                "browser_download_url": f"{base}/{repo}/releases/download/v1.9.0/Demo-1.9.0.AppImage",
                "size": len(REPO_ASSETS[repo]["Demo-1.9.0.AppImage"]),
                "digest": "sha256:" + sha(REPO_ASSETS[repo]["Demo-1.9.0.AppImage"]),
            }],
        })
    if repo == "acme/aimgr":
        # A pre-release that `self update` must ignore unless --prerelease is given.
        body = fake_appimage("aimgr-3.0.0-rc1")
        entries.append({
            "tag_name": "v3.0.0-rc1",
            "name": "3.0.0 rc1",
            "draft": False,
            "prerelease": True,
            "published_at": "2025-09-20T10:00:00Z",
            "html_url": f"{base}/releases/tag/v3.0.0-rc1",
            "assets": [{
                "name": "aimgr-x86_64.AppImage",
                "browser_download_url": f"{base}/acme/aimgr/releases/download/v3.0.0-rc1/aimgr-x86_64.AppImage",
                "size": len(body),
                "digest": "sha256:" + sha(body),
            }],
        })
    return entries


WEB_TAGS = ["v2.1.0-beta", "v2.0.0", "v1.9.0"]


def release_page(repo: str, tag: str) -> bytes:
    return f'<!DOCTYPE html><html><body><a href="/{repo}/releases/tag/{tag}">{tag}</a></body></html>'.encode()


def expanded_assets_page(repo: str, tag: str) -> bytes:
    assets = REPO_ASSETS.get(repo, {})
    # An off-origin absolute link must be ignored by the parser even when its
    # filename would otherwise rank first.
    poisoned = (
        f'<li><a href="https://evil.invalid/{repo}/releases/download/{tag}/Demo-2.0.0.AppImage">'
        "evil</a></li>"
    )
    links = "".join(
        f'<li><a href="/{repo}/releases/download/{tag}/{name}">{name}</a></li>' for name in assets
    )
    return f"<!DOCTYPE html><html><body><ul>{poisoned}{links}</ul></body></html>".encode()


def releases_index_page(repo: str) -> bytes:
    links = "".join(f'<li><a href="/{repo}/releases/tag/{t}">{t}</a></li>' for t in WEB_TAGS)
    return f"<!DOCTYPE html><html><body><ul>{links}</ul></body></html>".encode()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def _record(self, path: str):
        if not LOG_FILE:
            return
        auth = self.headers.get("Authorization") or "-"
        with open(LOG_FILE, "a") as f:
            f.write(f"{path}\tAUTH={auth}\n")

    def _send(self, code, body: bytes, ctype="application/octet-stream"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()
        try:
            self.wfile.write(body)
        except BrokenPipeError:
            pass

    def do_GET(self):
        host = self.headers.get("Host", "127.0.0.1")
        base = f"http://{host}"
        path = self.path.split("?")[0]
        self._record(path)

        if path.startswith("/broken-api/"):
            self._send(403, b'{"message":"API rate limit exceeded"}', "application/json")
            return
        if path == "/echo-auth":
            auth = self.headers.get("Authorization", "")
            self._send(200, json.dumps({"authorization": auth}).encode(), "application/json")
            return
        if path == "/version.txt":
            self._send(200, b"stable=3.1.4\n", "text/plain")
            return
        if path.startswith("/sha/") and path.endswith(".sha256"):
            name = path[len("/sha/"):-len(".sha256")]
            body = ALL_ASSETS.get(name)
            if body is None:
                self._send(404, b"no such asset\n", "text/plain")
            else:
                self._send(200, (sha(body) + "  " + name + "\n").encode(), "text/plain")
            return
        if path.startswith("/dl/"):
            name = path[len("/dl/"):]
            body = ALL_ASSETS.get(name)
            if body is None:
                self._send(404, b"no such asset")
            else:
                self._send(200, body)
            return
        if path == "/latest-appimage":
            # A direct URL that intentionally carries no version in its path.
            self._send(200, ALL_ASSETS["Plain-3.1.4.AppImage"])
            return

        # --- API
        if path.startswith("/repos/"):
            parts = [p for p in path.split("/") if p]
            repo = "/".join(parts[1:3]) if len(parts) >= 3 else ""
            if repo == "acme/missing":
                self._send(404, b'{"message":"Not Found"}', "application/json")
                return
            rels = releases_for(repo, base)
            if rels is None:
                self._send(404, b'{"message":"Not Found"}', "application/json")
            else:
                self._send(200, json.dumps(rels).encode(), "application/json")
            return

        # --- GitHub *web* pages (no API quota)
        if "/releases/" in path:
            parts = [p for p in path.split("/") if p]
            repo = "/".join(parts[:2])
            rest = parts[2:]
            if rest == ["releases", "latest"]:
                self.send_response(302)
                self.send_header("Location", f"{base}/{repo}/releases/tag/v2.0.0")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            if len(rest) == 3 and rest[0] == "releases" and rest[1] == "tag":
                self._send(200, release_page(repo, rest[2]), "text/html; charset=utf-8")
                return
            if len(rest) == 3 and rest[0] == "releases" and rest[1] == "expanded_assets":
                self._send(200, expanded_assets_page(repo, rest[2]), "text/html; charset=utf-8")
                return
            if rest == ["releases"]:
                self._send(200, releases_index_page(repo), "text/html; charset=utf-8")
                return
            if len(rest) == 4 and rest[0] == "releases" and rest[1] == "download":
                body = REPO_ASSETS.get(repo, {}).get(rest[3])
                if body is None:
                    self._send(404, b"no such asset")
                else:
                    self._send(200, body)
                return
            self._send(404, b"not found", "text/plain")
            return

        self._send(404, b"not found", "text/plain")


def main():
    global LOG_FILE
    ap = argparse.ArgumentParser()
    ap.add_argument("--port-file", required=True)
    ap.add_argument("--log-file", default=None)
    args = ap.parse_args()
    LOG_FILE = args.log_file
    srv = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    port = srv.server_address[1]
    with open(args.port_file, "w") as f:
        f.write(str(port))
    print(f"mock server on 127.0.0.1:{port}", file=sys.stderr, flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
