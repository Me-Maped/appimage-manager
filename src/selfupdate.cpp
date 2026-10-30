// Self-management: updating the aimgr AppImage itself and registering the
// running build with the desktop environment.
#include "aimgr/selfupdate.hpp"

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <vector>

#include "aimgr/json.hpp"
#include "aimgr/sha256.hpp"
#include "aimgr/source.hpp"
#include "aimgr/version.hpp"
#include "aimgr_icon_png.h"

namespace aimgr {
namespace {

std::string env_str(const char* name) {
  const char* v = ::getenv(name);
  return (v && *v) ? std::string(v) : "";
}

std::string read_link_exe() {
  char buf[4096];
  ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return "";
  buf[n] = '\0';
  return buf;
}

std::string xdg_data_home() {
  const std::string v = env_str("XDG_DATA_HOME");
  if (!v.empty()) return v;
  const std::string home = env_str("HOME");
  return home.empty() ? std::string() : home + "/.local/share";
}

/// Runs a helper (e.g. update-desktop-database) without a shell.
void run_helper(const char* program, const std::vector<std::string>& args) {
  // Absolute path only: execvp() would let a hijacked $PATH run arbitrary code.
  std::string exe;
  for (const char* dir : {"/usr/bin/", "/bin/", "/usr/local/bin/"}) {
    const std::string candidate = std::string(dir) + program;
    if (::access(candidate.c_str(), X_OK) == 0) {
      exe = candidate;
      break;
    }
  }
  if (exe.empty()) return;
  pid_t pid = ::fork();
  if (pid < 0) return;
  if (pid == 0) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(exe.c_str()));
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    ::execv(exe.c_str(), argv.data());
    ::_exit(127);
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
}

bool write_icon(const fs::path& dst, std::string* err) {
  std::string bytes;
  const std::string appdir = env_str("APPDIR");
  if (!appdir.empty()) {
    std::string from_pkg;
    if (read_file(fs::path(appdir) / "aimgr.png", from_pkg) && !from_pkg.empty()) bytes = from_pkg;
  }
  if (bytes.empty())
    bytes.assign(reinterpret_cast<const char*>(kAimgrIconPng),
                 static_cast<size_t>(kAimgrIconPngLen));
  return write_file_atomic(dst, bytes, err, 0644);
}

}  // namespace

namespace {

struct SelfState {
  std::string path, version, sha256;
};

SelfState read_self_state(const Paths& paths) {
  SelfState st;
  std::string txt, err;
  if (!read_file(paths.state_dir / "self.json", txt, &err)) return st;
  json::Value j;
  if (!json::parse(txt, j, err)) return st;
  st.path = j.get_str("path");
  st.version = j.get_str("version");
  st.sha256 = j.get_str("sha256");
  return st;
}

void write_self_state(const Paths& paths, const std::string& path, const std::string& version,
                      const std::string& sha) {
  json::Value j = json::Value::object();
  j.set("path", json::Value::make_str(path));
  j.set("version", json::Value::make_str(version));
  j.set("sha256", json::Value::make_str(sha));
  j.set("updated_at", json::Value::make_str(now_iso()));
  std::string err;
  write_file_atomic(paths.state_dir / "self.json", json::dump(j, 2), &err, 0600);
}

}  // namespace

std::string self_installed_version(const Paths& paths) {
  const SelfInfo info = SelfInfo::detect();
  const SelfState st = read_self_state(paths);
  if (!st.version.empty() && !info.appimage_path.empty() && st.path == info.appimage_path) {
    const std::string sha = sha256_file(info.appimage_path);
    if (!sha.empty() && sha == st.sha256) return st.version;
  }
  return info.version;
}

std::string self_default_repo() {
  const std::string from_env = env_str("AIMGR_SELF_REPO");
  if (!from_env.empty()) return from_env;
  return AIMGR_SELF_REPO;
}

SelfInfo SelfInfo::detect() {
  SelfInfo info;
  info.version = AIMGR_VERSION;
  const std::string appimage = env_str("APPIMAGE");
  info.appdir = env_str("APPDIR");
  if (!appimage.empty()) {
    std::error_code ec;
    fs::path p = fs::canonical(fs::path(appimage), ec);
    info.appimage_path = ec ? appimage : p.string();
    info.appimage = true;
  } else {
    info.appimage_path = read_link_exe();
  }
  return info;
}

SelfUpdateResult self_update(const SelfUpdateOptions& opts, Http& http, const Logger& log,
                             const Paths& paths) {
  SelfUpdateResult res;
  const SelfInfo info = SelfInfo::detect();
  const std::string target = opts.target.empty() ? info.appimage_path : opts.target;
  res.from = opts.target.empty() ? self_installed_version(paths) : info.version;

  if (target.empty()) {
    res.error = "cannot determine the running binary; pass --path";
    return res;
  }
  if (!path_is_regular(target)) {
    res.error = "target is not a regular file: " + target;
    return res;
  }
  {
    std::string why;
    if (!looks_like_appimage(target, &why)) {
      res.error = "refusing to replace " + target + ": " + why;
      return res;
    }
  }
  if (!info.appimage && opts.target.empty()) {
    res.error =
        "not running from an AppImage (no $APPIMAGE). Use `aimgr install`/package as AppImage, "
        "or pass --path";
    return res;
  }

  SourceGithub gh;
  gh.repo = opts.repo.empty() ? self_default_repo() : opts.repo;
  gh.asset_regex = opts.asset_regex.empty() ? std::string("aimgr-.*\\.AppImage$") : opts.asset_regex;
  gh.allow_prerelease = opts.allow_prerelease;
  gh.tag_prefix = "v";

  RemoteRelease rr = resolve_github(gh, http, log);
  if (!rr.ok) {
    res.error = rr.error;
    return res;
  }
  res.to = rr.version;
  res.asset = rr.asset;
  res.url = rr.url;

  const bool newer = is_newer(rr.version, res.from);
  if (!newer && !opts.force) {
    res.ok = true;
    res.updated = false;
    log.info("aimgr is up to date (" + res.from + ")");
    return res;
  }
  if (opts.dry_run) {
    res.ok = true;
    log.info("[dry-run] would update aimgr " + res.from + " -> " + rr.version + " from " + rr.url);
    return res;
  }

  // ---- download into the cache, verify, then swap atomically -------------
  const fs::path cache_dir = paths.cache_dir / "self" / sanitize_component(rr.version);
  fs::path cache = cache_dir / sanitize_component(rr.asset);
  fs::path part = cache;
  part += ".part";
  std::string err;
  if (!ensure_dir(cache_dir, &err)) {
    res.error = "cannot create cache dir: " + err;
    return res;
  }

  bool have = false;
  if (path_is_regular(cache)) {
    std::string why;
    if (looks_like_appimage(cache, &why) && (rr.size == 0 || path_size(cache) == rr.size)) {
      const std::string digest = sha256_file(cache);
      have = rr.sha256.empty() || digest == rr.sha256;
    }
    if (!have) {
      std::error_code ec;
      fs::remove(cache, ec);
    }
  }
  if (!have) {
    log.info("downloading " + rr.url);
    DownloadRequest req;
    req.url = rr.url;
    req.dest = part;
    req.resume = true;
    req.headers = {"Accept: application/octet-stream"};
    DownloadResult dr = http.download(req);
    if (!dr.ok) {
      res.error = "download failed: " + dr.error;
      return res;
    }
    std::string why;
    if (!looks_like_appimage(part, &why)) {
      std::error_code ec;
      fs::remove(part, ec);
      res.error = "downloaded file is not a valid AppImage: " + why;
      return res;
    }
    if (rr.size > 0 && path_size(part) != rr.size) {
      std::error_code ec;
      fs::remove(part, ec);
      res.error = "size mismatch for " + rr.asset;
      return res;
    }
    const std::string digest = sha256_file(part);
    if (!rr.sha256.empty() && digest != rr.sha256) {
      std::error_code ec;
      fs::remove(part, ec);
      res.error = "checksum mismatch: expected " + rr.sha256 + ", got " + digest;
      return res;
    }
    if (rr.sha256.empty())
      log.warn("release publishes no checksum; verified ELF/AppImage structure only");
    if (!copy_file_atomic(part, cache, &err, 0755)) {
      res.error = "cannot finalise download: " + err;
      return res;
    }
    std::error_code ec;
    fs::remove(part, ec);
  }
  const std::string new_sha = sha256_file(cache);

  // ---- backup the running build -----------------------------------------
  const fs::path bdir = paths.backup_dir / "self";
  if (!ensure_dir(bdir, &err)) {
    res.error = "cannot create backup dir: " + err;
    return res;
  }
  const fs::path bfile =
      bdir / (sanitize_component(res.from.empty() ? "unknown" : res.from) + "-" + utc_stamp() +
              ".AppImage");
  std::error_code ec;
  fs::create_hard_link(target, bfile, ec);
  if (ec && !copy_file_atomic(target, bfile, &err, 0755)) {
    res.error = "cannot back up the running build: " + err;
    return res;
  }
  res.backup = bfile.string();
  {
    json::Value meta = json::Value::object();
    meta.set("id", json::Value::make_str("self"));
    meta.set("version", json::Value::make_str(res.from));
    meta.set("sha256", json::Value::make_str(sha256_file(target)));
    meta.set("created_at", json::Value::make_str(now_iso()));
    meta.set("source_url", json::Value::make_str(rr.url));
    meta.set("kind", json::Value::make_str("self-update"));
    std::string merr;
    write_file_atomic(bfile.string() + ".json", json::dump(meta, 2), &merr, 0644);
  }
  log.info("backed up running build to " + bfile.string());

  // ---- journal + atomic replace -----------------------------------------
  json::Value journal = json::Value::object();
  journal.set("id", json::Value::make_str("self"));
  journal.set("target", json::Value::make_str(target));
  journal.set("backup", json::Value::make_str(bfile.string()));
  journal.set("new_sha256", json::Value::make_str(new_sha));
  journal.set("new_version", json::Value::make_str(rr.version));
  journal.set("old_version", json::Value::make_str(res.from));
  journal.set("stage", json::Value::make_str("self-swapping"));
  journal.set("started_at", json::Value::make_str(now_iso()));
  if (!write_file_atomic(paths.journal(), json::dump(journal, 2), &err, 0600)) {
    res.error = "cannot write journal: " + err;
    return res;
  }

  const fs::path staged =
      fs::path(target).parent_path() / (".aimgr-self-" + utc_stamp() + ".tmp");
  if (!copy_file_atomic(cache, staged, &err, 0755)) {
    fs::remove(paths.journal(), ec);
    res.error = "staging failed: " + err;
    return res;
  }
  if (::rename(staged.c_str(), target.c_str()) != 0) {
    fs::remove(staged, ec);
    // roll back from the backup we just made
    std::string rerr;
    if (copy_file_atomic(bfile, target, &rerr, 0755)) log.warn("restored previous build");
    fs::remove(paths.journal(), ec);
    res.error = std::string("atomic replace failed: ") + errno_str();
    return res;
  }
  fs::remove(paths.journal(), ec);
  write_self_state(paths, target, rr.version, new_sha);
  res.ok = true;
  res.updated = true;
  log.info("updated aimgr " + res.from + " -> " + rr.version);
  return res;
}

bool self_is_integrated(std::string* desktop_path) {
  const std::string data_home = xdg_data_home();
  if (data_home.empty()) {
    if (desktop_path) desktop_path->clear();
    return false;
  }
  const fs::path entry = fs::path(data_home) / "applications" / "aimgr.desktop";
  if (desktop_path) *desktop_path = entry.string();
  return path_is_regular(entry);
}

bool self_integrate(std::string* err, std::string* desktop_path) {
  const std::string data_home = xdg_data_home();
  if (data_home.empty()) {
    if (err) *err = "cannot determine the applications directory (set HOME or XDG_DATA_HOME)";
    return false;
  }
  const SelfInfo info = SelfInfo::detect();
  if (info.appimage_path.empty()) {
    if (err) *err = "cannot determine the running binary path";
    return false;
  }
  const fs::path apps_dir = fs::path(data_home) / "applications";
  const fs::path icon_dir = fs::path(xdg_data_home()) / "icons/hicolor/256x256/apps";
  if (!ensure_dir(apps_dir, err)) return false;
  if (!ensure_dir(icon_dir, err)) return false;
  if (!write_icon(icon_dir / "aimgr.png", err)) return false;

  std::string body;
  body += "[Desktop Entry]\n";
  body += "Type=Application\n";
  body += "Name=aimgr\n";
  body += "GenericName=AppImage Manager\n";
  body += "Comment=Check, update, back up and roll back AppImages\n";
  // Escape the path: a home directory with quotes/newlines must not be able to
  // inject extra Exec= arguments. No field codes: aimgr itself takes commands,
  // not URLs or file paths, so it must never be offered as an "open with" target.
  body += "Exec=" + escape_desktop_exec(info.appimage_path) + "\n";
  body += "TryExec=" + escape_desktop_exec(info.appimage_path) + "\n";
  body += "Icon=aimgr\n";
  body += "Terminal=false\n";
  body += "Categories=System;PackageManager;Utility;\n";
  body += "Keywords=appimage;update;package;manager;\n";
  body += "StartupWMClass=aimgr\n";
  body += "X-AppImage-Path=" + strip_control(info.appimage_path) + "\n";
  body += "X-AIMGR-Self=true\n";

  const fs::path entry = apps_dir / "aimgr.desktop";
  if (!write_file_atomic(entry, body, err, 0644)) return false;
  run_helper("update-desktop-database", {apps_dir.string()});
  if (desktop_path) *desktop_path = entry.string();
  return true;
}

bool self_deintegrate(std::string* err) {
  const std::string data_home = xdg_data_home();
  if (data_home.empty()) {
    if (err) *err = "cannot determine the applications directory (set HOME or XDG_DATA_HOME)";
    return false;
  }
  const fs::path apps_dir = fs::path(data_home) / "applications";
  const fs::path entry = apps_dir / "aimgr.desktop";
  const fs::path icon = fs::path(data_home) / "icons/hicolor/256x256/apps/aimgr.png";
  std::error_code ec;
  bool removed = false;
  if (path_is_regular(entry)) {
    fs::remove(entry, ec);
    removed = !ec;
  }
  if (path_is_regular(icon)) fs::remove(icon, ec);
  if (!removed) {
    if (err) *err = "no desktop entry to remove at " + entry.string();
    return false;
  }
  run_helper("update-desktop-database", {apps_dir.string()});
  return true;
}

}  // namespace aimgr
