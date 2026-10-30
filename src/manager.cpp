#include "aimgr/manager.hpp"
#include "aimgr/paths.hpp"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <set>

#include "aimgr/json.hpp"
#include "aimgr/sha256.hpp"
#include "aimgr/version.hpp"

namespace aimgr {
namespace {

std::string env_str(const char* name) {
  const char* v = ::getenv(name);
  return (v && *v) ? std::string(v) : "";
}

/// "<XDG base>/aimgr" for the given variable, falling back to $HOME/<rel>.
std::string xdg_dir(const char* var, const std::string& fallback_rel) {
  std::string base = env_str(var);
  if (base.empty()) {
    const std::string home = env_str("HOME");
    base = home.empty() ? std::string("/tmp") : home;
  }
  const std::string rel = env_str(var).empty() ? ("/" + fallback_rel) : std::string();
  return base + rel + "/aimgr";
}

/// Acquires the cross-process lock for the lifetime of one mutation. Recursive
/// calls (update -> prune -> save) reuse the already held lock.
struct MutationGuard {
  Manager& m;
  bool ok;
  std::string err;
  explicit MutationGuard(Manager& mm) : m(mm) { ok = m.begin_mutation(&err); }
  ~MutationGuard() { if (ok) m.end_mutation(); }
  MutationGuard(const MutationGuard&) = delete;
  MutationGuard& operator=(const MutationGuard&) = delete;
};

fs::path resolve_target(const fs::path& p) {
  std::error_code ec;
  if (fs::is_symlink(fs::symlink_status(p, ec))) {
    auto real = fs::canonical(p, ec);
    if (!ec) return real;
  }
  return p;
}

/// $XDG_DATA_HOME/applications (never the per-tool subdirectory).
std::string applications_dir() {
  const std::string base = env_str("XDG_DATA_HOME");
  if (!base.empty()) return base + "/applications";
  const std::string home = env_str("HOME");
  // Never fall back to a predictable /tmp path: desktop entries are written
  // into a user-controlled applications directory, not a shared scratch dir.
  return home.empty() ? std::string() : home + "/.local/share/applications";
}

std::string one_line(const std::string& s) { return sanitize_desktop_value(s); }

}  // namespace

Paths Paths::resolve() {
  Paths p;
  const std::string cfg = env_str("AIMGR_CONFIG");
  p.config = cfg.empty() ? fs::path(xdg_dir("XDG_CONFIG_HOME", ".config") + "/config.json")
                         : fs::path(expand_tilde(cfg));
  const std::string st = env_str("AIMGR_STATE_DIR");
  p.state_dir = st.empty() ? fs::path(xdg_dir("XDG_STATE_HOME", ".local/state"))
                           : fs::path(expand_tilde(st));
  const std::string ca = env_str("AIMGR_CACHE_DIR");
  p.cache_dir = ca.empty() ? fs::path(xdg_dir("XDG_CACHE_HOME", ".cache"))
                           : fs::path(expand_tilde(ca));
  const std::string bd = env_str("AIMGR_BACKUP_DIR");
  p.backup_dir = bd.empty() ? fs::path(xdg_dir("XDG_DATA_HOME", ".local/share") + "/backups")
                            : fs::path(expand_tilde(bd));
  return p;
}

Manager::Manager(Paths paths, Logger logger) : paths_(std::move(paths)), log_(std::move(logger)) {
  apply_defaults(cfg_);
}

bool Manager::load(std::string* err) {
  if (!load_config(paths_.config, cfg_, err)) return false;
  // Environment overrides win over the config file so the same config can be
  // reused with a different cache/backup location (containers, tests, USB).
  const std::string env_cache = env_str("AIMGR_CACHE_DIR");
  if (!env_cache.empty()) cfg_.defaults.download_dir = env_cache + "/downloads";
  const std::string env_backup = env_str("AIMGR_BACKUP_DIR");
  if (!env_backup.empty()) cfg_.defaults.backup_dir = env_backup;
  return true;
}

bool Manager::save(std::string* err) { return save_config(paths_.config, cfg_, err); }

bool Manager::begin_mutation(std::string* err) {
  if (lock_depth_ > 0) {
    ++lock_depth_;
    return true;
  }
  if (!lock_.acquire(paths_.lock(), err)) return false;
  lock_depth_ = 1;
  return true;
}

void Manager::end_mutation() {
  if (lock_depth_ <= 0) return;
  if (--lock_depth_ == 0) lock_.release();
}

App* Manager::find(const std::string& needle) {
  if (needle.empty()) return nullptr;
  for (auto& a : cfg_.apps)
    if (a.id == needle) return &a;
  for (auto& a : cfg_.apps)
    if (a.path == needle || expand_tilde(a.path).string() == needle) return &a;
  App* hit = nullptr;
  for (auto& a : cfg_.apps) {
    if (starts_with(a.id, needle)) {
      if (hit) return nullptr;  // ambiguous
      hit = &a;
    }
  }
  return hit;
}

std::vector<App*> Manager::all() {
  std::vector<App*> v;
  v.reserve(cfg_.apps.size());
  for (auto& a : cfg_.apps) v.push_back(&a);
  return v;
}

std::string Manager::installed_version_of(const App& app) const {
  if (!app.installed_version.empty()) return normalize_version(app.installed_version);
  const std::string fname = fs::path(app.path).filename().string();
  bool ok = false;
  std::string v = extract_version(fname, app.version_regex, &ok);
  return ok ? normalize_version(v) : std::string();
}

int Manager::backup_keep_of(const App& app) const {
  int keep = app.backup_keep >= 0 ? app.backup_keep : cfg_.defaults.backup_keep;
  return keep < 0 ? 3 : keep;
}

std::string Manager::backup_dir_of(const App& app) const {
  return (fs::path(expand_tilde(cfg_.defaults.backup_dir)) / sanitize_component(app.id)).string();
}

void Manager::touch_check(App& app, const std::string& status) {
  app.last_check = now_iso();
  app.last_status = status;
}

RemoteRelease Manager::resolve(App& app) { return resolve_source(app, http_, log_); }

CheckResult Manager::check(App& app) {
  MutationGuard guard(*this);
  if (!guard.ok) {
    CheckResult c;
    c.id = app.id;
    c.status = "error";
    c.error = guard.err;
    return c;
  }
  CheckResult c;
  c.id = app.id;
  c.name = app.name;
  c.installed = installed_version_of(app);

  if (app.pinned) {
    c.status = "pinned";
    c.latest = c.installed;
    touch_check(app, c.status);
    log_.info(app.id + ": pinned, skipping update check");
    return c;
  }
  if (app.source == SourceType::None) {
    c.status = "no-source";
    c.error = "no update source configured (aimgr set-source " + app.id + " --github owner/repo)";
    touch_check(app, c.status);
    return c;
  }

  RemoteRelease rr = resolve(app);
  if (!rr.ok) {
    c.status = "error";
    c.error = rr.error;
    touch_check(app, c.status);
    log_.error(app.id + ": " + rr.error);
    return c;
  }
  c.latest = rr.version;
  c.asset = rr.asset;
  c.url = rr.url;
  c.size = rr.size;
  c.published = rr.published;
  c.notes_url = rr.notes_url;

  if (rr.version.empty()) {
    c.status = "unknown";
    c.error = "source does not publish a comparable version (set version_url/version_regex)";
  } else if (is_newer(rr.version, c.installed)) {
    c.status = "update-available";
    c.update_available = true;
  } else {
    c.status = "up-to-date";
  }
  touch_check(app, c.status);
  log_.info(app.id + ": installed " + (c.installed.empty() ? "?" : c.installed) + ", latest " +
            (c.latest.empty() ? "?" : c.latest) + " -> " + c.status);
  return c;
}

std::vector<CheckResult> Manager::check_all(const std::vector<std::string>& ids) {
  std::vector<CheckResult> out;
  if (ids.empty()) {
    for (auto* a : all()) out.push_back(check(*a));
  } else {
    for (const auto& id : ids) {
      App* a = find(id);
      if (!a) {
        CheckResult c;
        c.id = id;
        c.status = "error";
        c.error = "unknown app '" + id + "'";
        out.push_back(c);
        continue;
      }
      out.push_back(check(*a));
    }
  }
  std::string err;
  if (!save(&err)) log_.warn("could not persist check results: " + err);
  return out;
}

fs::path Manager::cache_file_for(const App& app, const std::string& version,
                                 const std::string& asset) const {
  std::string name = asset.empty() ? std::string() : sanitize_component(fs::path(asset).filename().string());
  if (name.empty()) name = sanitize_component(app.id) + ".AppImage";
  // Cache is namespaced per app so two apps sharing an asset name can never
  // pick up each other's build.
  const std::string app_part = sanitize_component(app.id);
  const std::string ver_part = version.empty() ? "unknown" : sanitize_component(version);
  return fs::path(expand_tilde(cfg_.defaults.download_dir)) / app_part / ver_part / name;
}

bool Manager::resize_and_swap(const fs::path& staged, const fs::path& target, std::string* err) {
  std::error_code ec;
  fs::rename(staged, target, ec);
  if (ec) {
    if (err) *err = "atomic rename failed: " + ec.message();
    return false;
  }
  return true;
}

bool Manager::restore_from_backup(const fs::path& backup, const fs::path& target, std::string* err) {
  if (!path_is_regular(backup)) {
    if (err) *err = "backup missing: " + backup.string();
    return false;
  }
  std::string why;
  if (!looks_like_appimage(backup, &why)) {
    if (err) *err = "refusing to restore " + backup.string() + ": " + why;
    return false;
  }
  if (!copy_file_atomic(backup, target, err, 0755)) return false;
  log_.info("restored " + target.string() + " from " + backup.string());
  return true;
}

UpdateResult Manager::update(App& app, const UpdateOptions& opts) {
  MutationGuard guard(*this);
  if (!guard.ok) {
    UpdateResult u;
    u.id = app.id;
    u.error = guard.err;
    return u;
  }
  UpdateResult u;
  u.id = app.id;
  const std::string installed = installed_version_of(app);
  u.from = installed;

  if (app.pinned && !opts.force) {
    u.error = "pinned";
    return u;
  }
  if (app.source == SourceType::None) {
    u.error = "no update source configured for '" + app.id + "'";
    return u;
  }

  App probe = app;
  if (opts.allow_prerelease) probe.github.allow_prerelease = true;
  RemoteRelease rr = resolve(probe);
  if (!rr.ok) {
    u.error = rr.error;
    log_.error(app.id + ": " + rr.error);
    return u;
  }
  u.to = rr.version;

  const fs::path target = resolve_target(expand_tilde(app.path));
  const bool target_exists = path_is_regular(target);
  const bool newer = rr.version.empty() ? false : is_newer(rr.version, installed);
  // A source that publishes no comparable version (common for a plain download
  // URL) must not silently replace a build that already exists: that is a
  // forced update and needs an explicit opt-in/confirmation. A missing target
  // is still a fresh install and can proceed normally.
  const bool need = opts.force || !target_exists || (!rr.version.empty() && newer);
  if (!need) {
    u.ok = true;
    u.updated = false;
    app.last_status = rr.version.empty() ? "unknown" : "up-to-date";
    app.last_check = now_iso();
    if (rr.version.empty())
      log_.warn(app.id + ": source has no comparable version; keeping the current build "
                        "(use --force to download and replace it)");
    else
      log_.info(app.id + ": already up to date (" + installed + ")");
    return u;
  }

  if (target_exists) {
    std::string why;
    if (!looks_like_appimage(target, &why)) {
      // A hand-edited or stale config must not turn a forced/manual update into
      // a replacement of an unrelated file. A deliberate --force is still
      // allowed after the user has seen this explicit warning.
      if (!opts.force) {
        u.error = "refusing to replace " + target.string() +
                  ": existing target is not a valid AppImage (" + why +
                  "); use --force only if you are sure";
        log_.error(app.id + ": " + u.error);
        return u;
      }
      log_.warn(app.id + ": replacing non-AppImage target " + target.string() + " (" + why +
                ") because force was requested");
    }
  }
  if (opts.dry_run) {
    u.ok = true;
    u.error.clear();
    log_.info("[dry-run] " + app.id + ": would download " + rr.url + " and replace " +
              target.string() + (installed.empty() ? "" : " (backup of " + installed + " kept)"));
    return u;
  }
  if (!ensure_dir(target.parent_path(), &u.error)) return u;

  // ---------------------------------------------------------------- download
  const fs::path cache = cache_file_for(app, rr.version, rr.asset);
  fs::path part = cache;
  part += ".part";
  {
    std::string derr;
    if (!ensure_dir(cache.parent_path(), &derr)) {
      u.error = "cannot create download directory: " + derr;
      log_.error(app.id + ": " + u.error);
      return u;
    }
  }
  bool cache_ok = false;
  if (path_is_regular(cache)) {
    std::string why;
    const bool magic = looks_like_appimage(cache, &why);
    const bool size_ok = rr.size == 0 || path_size(cache) == rr.size;
    std::string digest;
    if (magic && size_ok && !rr.sha256.empty()) digest = sha256_file(cache);
    const bool digest_ok = rr.sha256.empty() || digest == rr.sha256;
    cache_ok = magic && size_ok && digest_ok;
    if (cache_ok) {
      log_.info(app.id + ": reusing cached download " + cache.string());
    } else {
      log_.warn(app.id + ": cached download is stale, refetching");
      std::error_code ec;
      fs::remove(cache, ec);
    }
  }

  if (!cache_ok) {
    log_.info(app.id + ": downloading " + rr.url);
    DownloadRequest req;
    req.url = rr.url;
    req.dest = part;
    req.resume = true;
    req.headers = {"Accept: application/octet-stream"};
    const std::string id = app.id;
    if (on_progress) {
      req.progress = [this, id](uint64_t done, uint64_t total) {
        on_progress(id, done, total);
        return true;
      };
    }
    DownloadResult dr = http_.download(req);
    if (!dr.ok) {
      u.error = "download failed: " + dr.error;
      log_.error(app.id + ": " + u.error);
      return u;
    }
    log_.info(app.id + ": downloaded " + format_size(dr.bytes));

    std::string why;
    if (!looks_like_appimage(part, &why)) {
      std::error_code ec;
      fs::remove(part, ec);
      u.error = "downloaded file is not a valid AppImage: " + why;
      log_.error(app.id + ": " + u.error);
      return u;
    }
    if (rr.size > 0 && path_size(part) != rr.size) {
      std::error_code ec;
      fs::remove(part, ec);
      u.error = "size mismatch: expected " + format_size(rr.size) + ", got " +
                format_size(path_size(part));
      log_.error(app.id + ": " + u.error);
      return u;
    }
    const std::string digest = sha256_file(part);
    if (!rr.sha256.empty() && digest != rr.sha256) {
      std::error_code ec;
      fs::remove(part, ec);
      u.error = "checksum mismatch: expected " + rr.sha256 + ", got " + digest;
      log_.error(app.id + ": " + u.error);
      return u;
    }
    if (rr.sha256.empty())
      log_.warn(app.id + ": source published no checksum, verified ELF/AppImage marker only");
    if (!resize_and_swap(part, cache, &u.error)) return u;
  }
  const std::string new_sha = sha256_file(cache);

  // ------------------------------------------------------------------ backup
  std::string backup_path;
  if (path_is_regular(target)) {
    const std::string bdir = backup_dir_of(app);
    std::string berr;
    if (!ensure_dir(bdir, &berr)) {
      u.error = "cannot create backup dir: " + berr;
      return u;
    }
    const std::string ver = installed.empty() ? std::string("unknown") : sanitize_component(installed);
    fs::path bfile = fs::path(bdir) / (ver + "-" + utc_stamp() + ".AppImage");
    std::error_code ec;
    fs::create_hard_link(target, bfile, ec);
    if (ec) {
      log_.debug("hardlink not possible (" + ec.message() + "), copying instead");
      if (!copy_file_atomic(target, bfile, &u.error, 0755)) return u;
    }
    backup_path = bfile.string();
    const std::string old_sha = sha256_file(target);
    json::Value meta = json::Value::object();
    meta.set("id", json::Value::make_str(app.id));
    meta.set("version", json::Value::make_str(ver));
    meta.set("sha256", json::Value::make_str(old_sha));
    meta.set("size", json::Value::make_num(static_cast<double>(path_size(target))));
    meta.set("created_at", json::Value::make_str(now_iso()));
    meta.set("source_url", json::Value::make_str(rr.url));
    meta.set("kind", json::Value::make_str("pre-update"));
    std::string merr;
    write_file_atomic(bfile.string() + ".json", json::dump(meta, 2), &merr, 0644);
    log_.info(app.id + ": backed up " + (installed.empty() ? "current build" : installed) + " -> " +
              backup_path);
  }
  u.backup = backup_path;

  // ------------------------------------------------------------------- swap
  json::Value journal = json::Value::object();
  journal.set("id", json::Value::make_str(app.id));
  journal.set("target", json::Value::make_str(target.string()));
  journal.set("backup", json::Value::make_str(backup_path));
  journal.set("new_sha256", json::Value::make_str(new_sha));
  journal.set("new_version", json::Value::make_str(rr.version));
  journal.set("old_version", json::Value::make_str(installed));
  journal.set("stage", json::Value::make_str("swapping"));
  journal.set("started_at", json::Value::make_str(now_iso()));
  std::string jerr;
  if (!write_file_atomic(paths_.journal(), json::dump(journal, 2), &jerr, 0600)) {
    u.error = "cannot write journal: " + jerr;
    return u;
  }

  auto fail_swap = [&](const std::string& why) {
    log_.error(app.id + ": " + why + " - rolling back");
    std::string rerr;
    if (!backup_path.empty() && path_is_regular(backup_path)) {
      if (!restore_from_backup(backup_path, target, &rerr))
        log_.error(app.id + ": ROLLBACK FAILED: " + rerr + " (backup kept at " + backup_path + ")");
    } else {
      std::error_code ec;
      fs::remove(target, ec);
    }
    std::error_code ec;
    fs::remove(paths_.journal(), ec);
    u.error = why;
    u.ok = false;
    return u;
  };

  fs::path staged = target.parent_path() / (".aimgr-" + app.id + "-" + utc_stamp() + ".tmp");
  std::string serr;
  if (!copy_file_atomic(cache, staged, &serr, 0755)) return fail_swap("staging failed: " + serr);
  if (!path_is_regular(staged)) return fail_swap("staged file vanished");

  std::error_code ec;
  fs::rename(staged, target, ec);
  if (ec) {
    fs::remove(staged, ec);
    return fail_swap("atomic replace failed: " + ec.message());
  }
  log_.info(app.id + ": replaced " + target.string() + " atomically");

  app.installed_version = rr.version.empty() ? installed : rr.version;
  app.installed_sha256 = new_sha;
  app.last_update = now_iso();
  app.last_status = "up-to-date";
  app.last_check = now_iso();
  std::string cerr;
  if (!save(&cerr)) log_.warn("config save failed (state on disk is fine): " + cerr);

  fs::remove(paths_.journal(), ec);
  prune(app, nullptr);

  u.ok = true;
  u.updated = true;
  log_.info(app.id + ": updated " + (installed.empty() ? "fresh install" : installed) + " -> " +
            (rr.version.empty() ? "unknown" : rr.version));
  return u;
}

std::vector<UpdateResult> Manager::update_all(const std::vector<std::string>& ids,
                                              const UpdateOptions& opts) {
  std::vector<UpdateResult> out;
  std::vector<App*> targets;
  if (ids.empty()) {
    // "update everything" must not fail because some entries are deliberately
    // unmanaged (scanned but no source) or pinned: those are simply skipped.
    for (auto* a : all()) {
      if (a->pinned) {
        log_.debug(a->id + ": pinned, skipped");
        continue;
      }
      if (a->source == SourceType::None) {
        log_.debug(a->id + ": no update source, skipped");
        continue;
      }
      targets.push_back(a);
    }
  } else {
    for (const auto& id : ids) {
      App* a = find(id);
      if (!a) {
        UpdateResult r;
        r.id = id;
        r.error = "unknown app '" + id + "'";
        out.push_back(r);
        continue;
      }
      targets.push_back(a);
    }
  }
  for (auto* a : targets) out.push_back(update(*a, opts));
  return out;
}

std::vector<BackupEntry> Manager::backups(const App& app) {
  std::vector<BackupEntry> out;
  const fs::path dir = backup_dir_of(app);
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return out;
  std::vector<fs::path> files;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (ec) break;
    if (e.is_symlink() || !e.is_regular_file()) continue;
    if (e.path().extension() == ".AppImage") files.push_back(e.path());
  }
  for (const auto& f : files) {
    BackupEntry b;
    b.file = f.string();
    b.size = path_size(f);
    const fs::path meta = f.string() + ".json";
    if (path_is_regular(meta)) {
      std::string text, ignored;
      if (read_file(meta, text, &ignored)) {
        json::Value j;
        if (json::parse(text, j, ignored)) {
          b.version = j.get_str("version");
          b.sha256 = j.get_str("sha256");
          b.created_at = j.get_str("created_at");
        }
      }
    }
    if (b.version.empty()) b.version = f.stem().string();
    out.push_back(b);
  }
  std::sort(out.begin(), out.end(), [](const BackupEntry& a, const BackupEntry& b) {
    if (a.created_at != b.created_at) return a.created_at > b.created_at;
    return a.file > b.file;
  });
  return out;
}

int Manager::prune(App& app, std::string* err) {
  MutationGuard guard(*this);
  if (!guard.ok) { if (err) *err = guard.err; return 0; }
  const int keep = backup_keep_of(app);
  if (keep <= 0) return 0;
  auto list = backups(app);
  if (static_cast<int>(list.size()) <= keep) return 0;
  int removed = 0;
  for (size_t i = static_cast<size_t>(keep); i < list.size(); ++i) {
    std::error_code ec;
    fs::remove(list[i].file, ec);
    if (!ec) ++removed;
    std::error_code ec2;
    fs::remove(list[i].file + ".json", ec2);
  }
  if (removed) log_.info(app.id + ": pruned " + std::to_string(removed) + " old backup(s)");
  (void)err;
  return removed;
}

bool Manager::rollback(App& app, std::string selector, std::string* err, std::string* restored) {
  MutationGuard guard(*this);
  if (!guard.ok) { if (err) *err = guard.err; return false; }
  auto list = backups(app);
  if (list.empty()) {
    if (err) *err = "no backups for '" + app.id + "'";
    return false;
  }
  const BackupEntry* pick = nullptr;
  if (selector.empty() || selector == "latest") {
    pick = &list.front();
  } else {
    for (const auto& b : list) {
      if (b.file == selector || fs::path(b.file).filename().string() == selector ||
          b.version == selector) {
        pick = &b;
        break;
      }
    }
    if (!pick) {
      for (const auto& b : list)
        if (b.file.find(selector) != std::string::npos) {
          pick = &b;
          break;
        }
    }
  }
  if (!pick) {
    if (err) *err = "no backup matching '" + selector + "' for '" + app.id + "'";
    return false;
  }
  if (!path_is_within(pick->file, backup_dir_of(app))) {
    if (err) *err = "refusing to restore " + pick->file + ": outside the backup directory";
    return false;
  }
  if (!path_is_regular(pick->file)) {
    if (err) *err = "backup file disappeared: " + pick->file;
    return false;
  }
  const fs::path target = resolve_target(expand_tilde(app.path));
  if (path_is_regular(target)) {
    std::string why;
    if (!looks_like_appimage(target, &why)) {
      if (err) *err = "refusing to roll back over non-AppImage target " + target.string() + ": " + why;
      return false;
    }
  }
  const std::string cur = installed_version_of(app);

  json::Value journal = json::Value::object();
  journal.set("id", json::Value::make_str(app.id));
  journal.set("target", json::Value::make_str(target.string()));
  journal.set("backup", json::Value::make_str(pick->file));
  journal.set("new_sha256", json::Value::make_str(pick->sha256));
  journal.set("new_version", json::Value::make_str(pick->version));
  journal.set("old_version", json::Value::make_str(cur));
  journal.set("stage", json::Value::make_str("rollback"));
  std::string jerr;
  if (!write_file_atomic(paths_.journal(), json::dump(journal, 2), &jerr, 0600)) {
    if (err) *err = "cannot write journal: " + jerr;
    return false;
  }

  // Preserve the current build so the rollback itself is reversible.
  if (path_is_regular(target)) {
    const std::string bdir = backup_dir_of(app);
    std::string berr;
    if (ensure_dir(bdir, &berr)) {
      fs::path keep = fs::path(bdir) / ((cur.empty() ? "unknown" : sanitize_component(cur)) +
                                        "-pre-rollback-" + utc_stamp() + ".AppImage");
      std::error_code ec;
      fs::create_hard_link(target, keep, ec);
      if (ec) copy_file_atomic(target, keep, &berr, 0755);
      json::Value meta = json::Value::object();
      meta.set("id", json::Value::make_str(app.id));
      meta.set("version", json::Value::make_str(cur));
      meta.set("sha256", json::Value::make_str(sha256_file(target)));
      meta.set("created_at", json::Value::make_str(now_iso()));
      meta.set("kind", json::Value::make_str("pre-rollback"));
      std::string merr;
      write_file_atomic(keep.string() + ".json", json::dump(meta, 2), &merr, 0644);
    }
  }

  if (!restore_from_backup(pick->file, target, err)) {
    std::error_code ec;
    fs::remove(paths_.journal(), ec);
    return false;
  }
  app.installed_version = pick->version;
  app.installed_sha256 = pick->sha256;
  app.last_update = now_iso();
  app.last_status = "rolled-back";
  std::string cerr;
  if (!save(&cerr)) log_.warn("config save failed after rollback: " + cerr);
  std::error_code ec;
  fs::remove(paths_.journal(), ec);
  log_.info(app.id + ": rolled back " + (cur.empty() ? "current" : cur) + " -> " + pick->version);
  if (restored) *restored = pick->version;
  return true;
}

bool Manager::uninstall(App& app, bool purge_backups, bool delete_file, bool keep_config,
                        std::string* err) {
  MutationGuard guard(*this);
  if (!guard.ok) { if (err) *err = guard.err; return false; }
  const fs::path target = resolve_target(expand_tilde(app.path));
  if (path_is_regular(target)) {
    std::string why;
    if (!looks_like_appimage(target, &why)) {
      if (err)
        *err = std::string(delete_file ? "refusing to delete " : "refusing to uninstall ") +
               target.string() + ": " + why;
      return false;
    }
  }
  deintegrate(app, nullptr);
  if (path_is_regular(target)) {
    if (delete_file) {
      std::error_code ec;
      fs::remove(target, ec);
      if (ec) {
        if (err) *err = "cannot delete " + target.string() + ": " + ec.message();
        return false;
      }
      log_.info(app.id + ": deleted " + target.string());
    } else {
      const std::string bdir = backup_dir_of(app);
      std::string berr;
      if (!ensure_dir(bdir, &berr)) {
        if (err) *err = berr;
        return false;
      }
      fs::path keep = fs::path(bdir) / ("uninstall-" + utc_stamp() + ".AppImage");
      std::error_code ec;
      fs::create_hard_link(target, keep, ec);
      if (ec) {
        if (!copy_file_atomic(target, keep, err, 0755)) return false;
      }
      std::error_code ec2;
      fs::remove(target, ec2);
      log_.info(app.id + ": moved " + target.string() + " to " + keep.string());
    }
  } else {
    log_.warn(app.id + ": AppImage file already missing at " + target.string());
  }
  if (purge_backups) {
    std::error_code ec;
    fs::remove_all(backup_dir_of(app), ec);
    log_.info(app.id + ": removed backups in " + backup_dir_of(app));
  }
  if (!keep_config) {
    if (!remove_app(app.id, err)) return false;
  }
  return true;
}

bool Manager::add_app(App app, bool replace, std::string* err) {
  MutationGuard guard(*this);
  if (!guard.ok) { if (err) *err = guard.err; return false; }
  if (app.path.empty()) {
    if (err) *err = "an AppImage path is required (--path)";
    return false;
  }
  app.path = expand_tilde(app.path).string();
  if (!app.path.empty() && app.path[0] != '/') {
    std::error_code ec;
    app.path = fs::absolute(app.path, ec).string();
  }
  if (app.id.empty()) app.id = slugify(app.name.empty() ? fs::path(app.path).stem().string() : app.name);
  if (app.id.empty()) {
    if (err) *err = "cannot derive an id, pass --id";
    return false;
  }
  // The id becomes a directory name and a file-name component: it must never be
  // able to escape those directories (../../etc, absolute paths, hidden files).
  if (!is_valid_id(app.id)) {
    if (err) *err = "invalid id '" + app.id +
                    "': use 1-64 chars of A-Z a-z 0-9 . _ - (no slashes, no leading '-' or '.')";
    return false;
  }
  if (app.name.empty()) app.name = app.id;
  if (app.source == SourceType::Github) {
    const std::string repo = parse_github_repo(app.github.repo);
    if (repo.empty()) {
      if (err) *err = "invalid GitHub repo '" + app.github.repo + "'";
      return false;
    }
    app.github.repo = repo;
  } else if (app.source == SourceType::Url) {
    if (app.url.url.empty()) {
      if (err) *err = "a download URL is required";
      return false;
    }
    const std::string scheme = url_scheme(app.url.url);
    if (scheme != "https" && !(scheme == "http" && Http::insecure_http_allowed())) {
      if (err) *err = "URL must be https (got '" + scheme + "://'); plain http needs "
                      "AIMGR_ALLOW_INSECURE_HTTP=1";
      return false;
    }
    if (!app.url.version_url.empty()) {
      const std::string vs = url_scheme(app.url.version_url);
      if (vs != "https" && !(vs == "http" && Http::insecure_http_allowed())) {
        if (err) *err = "version URL must be https too";
        return false;
      }
    }
    if (!app.url.sha256_url.empty()) {
      const std::string ss = url_scheme(app.url.sha256_url);
      if (ss != "https" && !(ss == "http" && Http::insecure_http_allowed())) {
        if (err) *err = "checksum URL must be https too";
        return false;
      }
    }
  }
  if (app.installed_version.empty() && path_is_regular(app.path)) {
    bool ok = false;
    std::string v = extract_version(fs::path(app.path).filename().string(), app.version_regex, &ok);
    if (ok) app.installed_version = normalize_version(v);
  }
  const fs::path existing_target = resolve_target(expand_tilde(app.path));
  if (path_is_regular(existing_target)) {
    std::string why;
    if (!looks_like_appimage(existing_target, &why)) log_.warn(app.id + ": " + why);
  } else {
    std::error_code ec;
    if (!ensure_dir(fs::path(app.path).parent_path(), err)) return false;
    log_.info(app.id + ": no existing file yet, update will install a fresh copy");
  }
  App* existing = nullptr;
  for (auto& a : cfg_.apps)
    if (a.id == app.id) {
      existing = &a;
      break;
    }
  if (existing && existing->path != app.path) {
    if (!replace) {
      if (err) *err = "id '" + app.id + "' already used by " + existing->path + " (use --replace)";
      return false;
    }
  }
  if (existing) {
    *existing = app;
  } else {
    cfg_.apps.push_back(app);
  }
  return save(err);
}

InstallResult Manager::install_from_source(const InstallOptions& opts) {
  MutationGuard guard(*this);
  InstallResult res;
  if (!guard.ok) {
    res.error = guard.err;
    return res;
  }
  if (opts.source == SourceType::None) {
    res.error = "an update source is required (GitHub release or download URL)";
    return res;
  }

  SourceGithub gh = opts.github;
  SourceUrl su = opts.url;
  if (opts.source == SourceType::Github) {
    gh.repo = parse_github_repo(gh.repo);
    if (gh.repo.empty()) {
      res.error = "invalid GitHub repository '" + opts.github.repo + "' (expected owner/repo)";
      return res;
    }
    if (opts.allow_prerelease) gh.allow_prerelease = true;
  } else {
    if (su.url.empty()) {
      res.error = "a download URL is required";
      return res;
    }
    const std::string scheme = url_scheme(su.url);
    if (scheme != "https" && !(scheme == "http" && Http::insecure_http_allowed())) {
      res.error = "URL must be https (got '" + scheme + "://'); plain http needs "
                  "AIMGR_ALLOW_INSECURE_HTTP=1";
      return res;
    }
    if (!su.version_url.empty()) {
      const std::string vs = url_scheme(su.version_url);
      if (vs != "https" && !(vs == "http" && Http::insecure_http_allowed())) {
        res.error = "version URL must be https too";
        return res;
      }
    }
    if (!su.sha256_url.empty()) {
      const std::string ss = url_scheme(su.sha256_url);
      if (ss != "https" && !(ss == "http" && Http::insecure_http_allowed())) {
        res.error = "checksum URL must be https too";
        return res;
      }
    }
  }

  App probe;
  probe.source = opts.source;
  probe.github = gh;
  probe.url = su;
  RemoteRelease rr = resolve(probe);
  if (!rr.ok) {
    res.error = rr.error;
    return res;
  }
  if (rr.url.empty()) {
    res.error = "source resolved a release but no download URL";
    return res;
  }
  if (rr.version.empty() && su.url.find("${version}") != std::string::npos) {
    res.error = "the URL template needs ${version}, but the source did not publish a version";
    return res;
  }

  fs::path dir = opts.directory.empty() ? default_install_dir() : expand_tilde(opts.directory);
  if (dir.is_relative()) {
    std::error_code ec;
    dir = fs::absolute(dir, ec);
    if (ec) {
      res.error = "cannot resolve destination directory: " + ec.message();
      return res;
    }
  }
  std::string derr;
  if (!ensure_dir(dir, &derr)) {
    res.error = "cannot create destination directory: " + derr;
    return res;
  }

  std::string id = opts.id;
  std::string name = opts.name;
  std::string filename = opts.filename;
  if (filename.empty()) {
    filename = rr.asset.empty() ? std::string() : fs::path(rr.asset).filename().string();
    if (filename.empty()) {
      id = id.empty() ? slugify(name.empty() ? "appimage" : name) : id;
      filename = id + (rr.version.empty() ? "" : "-" + rr.version) + ".AppImage";
    }
  }
  filename = sanitize_component(fs::path(filename).filename().string());
  if (filename.empty() || filename == "unnamed") filename = "download.AppImage";
  if (!ends_with(to_lower(filename), ".appimage")) filename += ".AppImage";
  if (id.empty()) {
    std::string stem = fs::path(filename).stem().string();
    bool ok = false;
    const std::string v = extract_version(stem, "", &ok);
    if (ok) {
      auto pos = stem.find(v);
      if (pos != std::string::npos) stem = stem.substr(0, pos);
    }
    id = slugify(name.empty() ? stem : name);
  }
  if (id.empty()) id = "appimage";
  if (name.empty()) name = id;

  const fs::path target = dir / filename;
  if (!path_is_within(target, dir)) {
    res.error = "destination escapes the selected directory";
    return res;
  }
  const bool target_existed = path_exists(target);
  if (target_existed) {
    if (!path_is_regular(target)) {
      res.error = "destination exists but is not a regular file: " + target.string();
      return res;
    }
    if (!opts.replace) {
      res.error = target.string() + " already exists (enable replace/overwrite)";
      return res;
    }
  }

  res.id = id;
  res.path = target.string();
  res.version = rr.version;
  if (opts.dry_run) {
    res.ok = true;
    log_.info("[dry-run] would download " + rr.url + " to " + res.path);
    return res;
  }

  std::optional<App> previous;
  for (const auto& a : cfg_.apps)
    if (a.id == id) {
      previous = a;
      break;
    }

  App app;
  app.id = id;
  app.name = name;
  app.path = target.string();
  app.source = opts.source;
  app.github = gh;
  app.url = su;
  app.installed_version = rr.version;
  std::string aerr;
  if (!add_app(app, opts.replace, &aerr)) {
    res.error = aerr;
    return res;
  }
  App* saved = find(id);
  if (!saved) {
    res.error = "installed app could not be registered";
    return res;
  }
  UpdateOptions uo;
  uo.force = true;
  uo.allow_prerelease = opts.allow_prerelease;
  UpdateResult u = update(*saved, uo);
  if (!u.ok) {
    res.error = u.error;
    // Do not leave a freshly-created list entry pointing at a file that was
    // never installed. Restore the previous record when one existed.
    if (previous) {
      for (auto& a : cfg_.apps) {
        if (a.id == id) {
          a = *previous;
          break;
        }
      }
      std::string e;
      save(&e);
    } else if (!target_existed && !path_is_regular(target)) {
      remove_app(id, nullptr);
    }
    return res;
  }
  res.ok = true;
  res.version = u.to.empty() ? rr.version : u.to;
  res.path = saved->path;
  log_.info(id + ": downloaded and registered " + res.path);
  return res;
}

bool Manager::remove_app(const std::string& id, std::string* err) {
  MutationGuard guard(*this);
  if (!guard.ok) { if (err) *err = guard.err; return false; }
  auto it = std::find_if(cfg_.apps.begin(), cfg_.apps.end(), [&](const App& a) { return a.id == id; });
  if (it == cfg_.apps.end()) {
    if (err) *err = "unknown app '" + id + "'";
    return false;
  }
  cfg_.apps.erase(it);
  return save(err);
}

bool Manager::set_source(App& app, SourceType type, const SourceGithub& gh, const SourceUrl& su,
                         std::string* err) {
  MutationGuard guard(*this);
  if (!guard.ok) {
    if (err) *err = guard.err;
    return false;
  }
  if (type == SourceType::Github) {
    const std::string repo = parse_github_repo(gh.repo);
    if (repo.empty()) {
      if (err) *err = "invalid GitHub repository '" + gh.repo + "' (expected owner/repo)";
      return false;
    }
    app.source = SourceType::Github;
    app.github = gh;
    app.github.repo = repo;
    app.url = SourceUrl{};
  } else if (type == SourceType::Url) {
    if (su.url.empty()) {
      if (err) *err = "a download URL is required";
      return false;
    }
    const std::string scheme = url_scheme(su.url);
    if (scheme != "https" && !(scheme == "http" && Http::insecure_http_allowed())) {
      if (err) *err = "URL must be https (got '" + scheme + "://'); plain http needs "
                      "AIMGR_ALLOW_INSECURE_HTTP=1";
      return false;
    }
    if (!su.version_url.empty()) {
      const std::string vs = url_scheme(su.version_url);
      if (vs != "https" && !(vs == "http" && Http::insecure_http_allowed())) {
        if (err) *err = "version URL must be https too";
        return false;
      }
    }
    if (!su.sha256_url.empty()) {
      const std::string ss = url_scheme(su.sha256_url);
      if (ss != "https" && !(ss == "http" && Http::insecure_http_allowed())) {
        if (err) *err = "checksum URL must be https too";
        return false;
      }
    }
    app.source = SourceType::Url;
    app.url = su;
    app.github = SourceGithub{};
  } else {
    app.source = SourceType::None;
    app.github = SourceGithub{};
    app.url = SourceUrl{};
  }
  if (app.installed_version.empty() && path_is_regular(expand_tilde(app.path))) {
    bool ok = false;
    const std::string v =
        extract_version(fs::path(app.path).filename().string(), app.version_regex, &ok);
    if (ok) app.installed_version = normalize_version(v);
  }
  log_.info(app.id + ": update source set to " + source_type_name(app.source) +
            (app.source == SourceType::Github ? ":" + app.github.repo : ""));
  return save(err);
}

bool Manager::set_ui(const UiSettings& ui, std::string* err) {
  MutationGuard guard(*this);
  if (!guard.ok) {
    if (err) *err = guard.err;
    return false;
  }
  cfg_.ui = ui;
  apply_defaults(cfg_);
  return save(err);
}

std::vector<App> Manager::scan(const std::string& dir, std::string* err) {
  return scan_folder(dir, false, err);
}

std::vector<App> Manager::scan_folder(const std::string& dir, bool recursive,
                                      std::string* err) {
  MutationGuard guard(*this);
  if (!guard.ok) { if (err) *err = guard.err; return {}; }
  std::vector<App> added;
  const fs::path root = expand_tilde(dir);
  std::error_code ec;
  if (!fs::is_directory(root, ec)) {
    if (err) *err = root.string() + " is not a directory";
    return added;
  }
  std::set<std::string> known;
  for (const auto& a : cfg_.apps) known.insert(resolve_target(expand_tilde(a.path)).string());

  // Collect candidate paths first so a huge directory tree cannot keep the
  // config half-mutated, and so the resulting registration order is stable.
  std::vector<fs::path> files;
  constexpr size_t kMaxEntries = 100000;
  size_t seen_entries = 0;
  if (recursive) {
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    while (!ec && it != end) {
      if (++seen_entries > kMaxEntries) {
        log_.warn("scan: stopped after " + std::to_string(kMaxEntries) + " directory entries");
        break;
      }
      std::error_code fec;
      if (it->is_regular_file(fec) && !fec) {
        const std::string name = it->path().filename().string();
        if (ends_with(to_lower(name), ".appimage")) files.push_back(it->path());
      }
      it.increment(ec);
      if (ec) {
        log_.debug("scan: skipping unreadable directory entry: " + ec.message());
        ec.clear();
      }
    }
  } else {
    for (const auto& e : fs::directory_iterator(root, ec)) {
      if (ec) break;
      if (++seen_entries > kMaxEntries) break;
      if (!e.is_regular_file()) continue;
      const std::string name = e.path().filename().string();
      if (ends_with(to_lower(name), ".appimage")) files.push_back(e.path());
    }
  }
  std::sort(files.begin(), files.end());

  auto id_taken = [&](const std::string& id) {
    for (const auto& a : cfg_.apps)
      if (a.id == id) return true;
    return std::any_of(added.begin(), added.end(), [&](const App& a) { return a.id == id; });
  };

  for (const auto& file : files) {
    const std::string name = file.filename().string();
    const std::string abs = resolve_target(file).string();
    if (known.count(abs)) continue;
    std::string why;
    if (!looks_like_appimage(abs, &why)) {
      log_.debug("scan: skipping " + name + " (" + why + ")");
      continue;
    }
    App a;
    a.path = abs;
    bool ok = false;
    const std::string v = extract_version(name, "", &ok);
    a.installed_version = ok ? normalize_version(v) : "";
    std::string base = name;
    if (ok) {
      auto pos = name.find(v);
      if (pos != std::string::npos) base = name.substr(0, pos);
    } else {
      base = fs::path(name).stem().string();
    }
    base = trim(base);
    while (!base.empty() && (base.back() == '-' || base.back() == '_' || base.back() == '.'))
      base.pop_back();
    a.name = base.empty() ? fs::path(name).stem().string() : base;
    std::string base_id = slugify(a.name);
    if (base_id.empty()) base_id = slugify(fs::path(name).stem().string());
    if (base_id.empty()) base_id = "appimage";
    if (base_id.size() > 64) base_id.resize(64);
    while (!base_id.empty() && base_id.back() == '-') base_id.pop_back();
    if (base_id.empty()) base_id = "appimage";
    // avoid collisions with existing ids and with files found earlier in the
    // same scan (two different folders may contain the same file name).
    a.id = base_id;
    int n = 2;
    while (id_taken(a.id)) {
      const std::string suffix = "-" + std::to_string(n++);
      const size_t keep = base_id.size() + suffix.size() <= 64
                              ? base_id.size()
                              : 64 - std::min<size_t>(suffix.size(), 64);
      a.id = base_id.substr(0, keep) + suffix;
    }
    a.source = SourceType::None;
    added.push_back(a);
    known.insert(abs);
  }
  for (auto& a : added) {
    cfg_.apps.push_back(a);
    log_.info("registered " + a.id + " (" + (a.installed_version.empty() ? "version unknown" : a.installed_version) +
              ") -> " + a.path);
  }
  if (!added.empty()) {
    std::string serr;
    if (!save(&serr) && err) *err = serr;
  }
  return added;
}


std::string Manager::desktop_path_for(const App& app) const {
  if (!is_valid_id(app.id)) return "";
  const std::string dir = applications_dir();
  if (dir.empty()) return "";
  return (fs::path(dir) / ("aimgr-" + app.id + ".desktop")).string();
}

std::string Manager::render_desktop(const App& app, std::string* err) const {
  const std::string& id = app.id;
  if (!is_valid_id(id)) {
    if (err) *err = "invalid app id for a desktop file";
    return "";
  }
  const fs::path target = resolve_target(expand_tilde(app.path));
  const std::string name = one_line(app.desktop.name.empty() ? app.name : app.desktop.name);
  if (name.empty()) {
    if (err) *err = "desktop entry needs a non-empty name";
    return "";
  }
  std::string comment = one_line(app.desktop.comment);
  if (comment.empty()) comment = "AppImage managed by aimgr";
  std::string categories = one_line(app.desktop.categories);
  if (categories.empty()) categories = "Utility;";
  if (!categories.empty() && categories.back() != ';') categories += ';';
  const std::string args = one_line(app.desktop.args);
  const std::string icon = one_line(app.desktop.icon);

  std::string body;
  body += "[Desktop Entry]\n";
  body += "Type=Application\n";
  body += "Version=1.0\n";
  body += "Name=" + name + "\n";
  body += "Comment=" + comment + "\n";
  // The managed executable is always the AppImage path; extra arguments are
  // appended after it. Neither field is passed through a shell.
  body += "Exec=" + escape_desktop_exec(target.string());
  if (!args.empty()) body += " " + args;
  body += "\n";
  body += "Terminal=";
  body += app.desktop.terminal ? "true" : "false";
  body += "\n";
  body += "Categories=" + categories + "\n";
  if (!icon.empty()) body += "Icon=" + icon + "\n";
  body += "X-AppImage-Path=" + one_line(target.string()) + "\n";
  body += "X-AIMGR-Managed=true\n";
  body += "X-AIMGR-ID=" + id + "\n";
  return body;
}

bool Manager::set_desktop(App& app, const DesktopEntry& de, bool enabled,
                          std::string* err) {
  MutationGuard guard(*this);
  if (!guard.ok) { if (err) *err = guard.err; return false; }
  const std::string entry = desktop_path_for(app);
  if (entry.empty()) {
    if (err) *err = "cannot determine the applications directory (set HOME or XDG_DATA_HOME)";
    return false;
  }
  DesktopEntry clean = de;
  clean.name = one_line(de.name);
  clean.comment = one_line(de.comment);
  clean.categories = one_line(de.categories);
  clean.icon = one_line(de.icon);
  clean.args = one_line(de.args);
  if (clean.comment.empty()) clean.comment = "AppImage managed by aimgr";
  if (clean.categories.empty()) clean.categories = "Utility;";
  if (!clean.categories.empty() && clean.categories.back() != ';') clean.categories += ';';

  if (!enabled) {
    std::error_code ec;
    // Only ever delete the path aimgr derives for this exact id. Never trust a
    // path copied from a (possibly attacker-edited) config file. fs::remove on
    // a symlink removes the link itself, never its target.
    if (path_exists(entry) || path_is_symlink(entry)) fs::remove(entry, ec);
    if (ec) {
      if (err) *err = "cannot remove " + entry + ": " + ec.message();
      return false;
    }
    app.integrated = false;
    app.desktop_entry.clear();
    app.desktop = clean;
    return save(err);
  }

  const fs::path target = resolve_target(expand_tilde(app.path));
  if (!path_is_regular(target)) {
    if (err) *err = "AppImage not found: " + target.string();
    return false;
  }
  std::string why;
  if (!looks_like_appimage(target, &why)) {
    if (err) *err = "refusing to create a desktop entry for " + target.string() + ": " + why;
    return false;
  }
  app.desktop = clean;
  std::string body = render_desktop(app, err);
  if (body.empty()) return false;
  if (!write_file_atomic(entry, body, err, 0644)) return false;
  app.integrated = true;
  app.desktop_entry = entry;
  log_.info(app.id + ": wrote desktop entry " + entry);
  return save(err);
}

bool Manager::integrate(App& app, std::string* err) {
  return set_desktop(app, app.desktop, true, err);
}

bool Manager::deintegrate(App& app, std::string* err) {
  return set_desktop(app, app.desktop, false, err);
}

std::string Manager::recover() {
  MutationGuard guard(*this);
  if (!guard.ok) return guard.err;
  if (!path_exists(paths_.journal())) return "";
  std::string text, err;
  if (!read_file(paths_.journal(), text, &err)) return "";
  json::Value j;
  if (!json::parse(text, j, err)) {
    std::error_code ec;
    fs::remove(paths_.journal(), ec);
    return "journal was unreadable, discarded (" + err + ")";
  }
  const std::string id = j.get_str("id");
  const std::string target = j.get_str("target");
  const std::string backup = j.get_str("backup");
  const std::string new_sha = j.get_str("new_sha256");
  const std::string new_version = j.get_str("new_version");
  const std::string old_version = j.get_str("old_version");
  const std::string stage = j.get_str("stage");
  std::string msg;

  App* app = find(id);
  if (app && !target.empty() && !path_is_within(target, fs::path(expand_tilde(app->path)).parent_path())) {
    std::error_code ec;
    fs::remove(paths_.journal(), ec);
    const std::string msg2 =
        "recovery refused for '" + id + "': journal target is outside the app directory";
    log_.error(msg2);
    return msg2;
  }
  if (!app && id == "self" && !backup.empty() && !path_is_within(backup, paths_.backup_dir)) {
    std::error_code ec;
    fs::remove(paths_.journal(), ec);
    const std::string msg2 = "recovery refused for self-update: backup outside the backup root";
    log_.error(msg2);
    return msg2;
  }
  if (app && !backup.empty() && !path_is_within(backup, backup_dir_of(*app))) {
    std::error_code ec;
    fs::remove(paths_.journal(), ec);
    const std::string msg2 =
        "recovery refused for '" + id + "': journal backup is outside the backup directory";
    log_.error(msg2);
    return msg2;
  }
  if (!target.empty() && path_is_regular(target) && !new_sha.empty() &&
      sha256_file(target) == new_sha) {
    if (app && !new_version.empty()) {
      app->installed_version = new_version;
      app->last_status = "up-to-date";
      app->last_update = now_iso();
      std::string serr;
      save(&serr);
    }
    msg = "recovered: interrupted " + stage + " of '" + id + "' actually completed (" +
          (new_version.empty() ? "new build" : new_version) + ")";
  } else if (!backup.empty() && path_is_regular(backup)) {
    std::string rerr;
    if (restore_from_backup(backup, target, &rerr)) {
      if (app) {
        app->installed_version = old_version;
        app->last_status = "restored";
        std::string serr;
        save(&serr);
      }
      msg = "recovered: interrupted " + stage + " of '" + id + "', restored " +
            (old_version.empty() ? "previous build" : old_version);
    } else {
      msg = "recovery FAILED for '" + id + "': " + rerr + " (backup: " + backup + ")";
    }
  } else {
    msg = "journal for '" + id + "' found but neither the new file nor a backup is present";
  }
  std::error_code ec;
  fs::remove(paths_.journal(), ec);
  if (app) save(nullptr);
  log_.warn(msg);
  return msg;
}

}  // namespace aimgr
