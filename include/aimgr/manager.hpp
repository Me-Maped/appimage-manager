#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "aimgr/config.hpp"
#include "aimgr/http.hpp"
#include "aimgr/paths.hpp"
#include "aimgr/source.hpp"
#include "aimgr/util.hpp"

namespace aimgr {

struct CheckResult {
  std::string id;
  std::string name;
  /// "up-to-date" | "update-available" | "pinned" | "no-source" | "unknown" | "error"
  std::string status;
  std::string installed;
  std::string latest;
  std::string asset;
  std::string url;
  std::string published;
  std::string notes_url;
  std::string error;
  uint64_t size = 0;
  bool update_available = false;
};

struct UpdateResult {
  bool ok = false;
  bool updated = false;       // false when already up to date
  std::string id;
  std::string from;
  std::string to;
  std::string backup;         // path of the backup taken before swapping
  std::string error;
};

struct BackupEntry {
  std::string file;           // full path
  std::string version;
  std::string sha256;
  uint64_t size = 0;
  std::string created_at;
};

struct UpdateOptions {
  bool force = false;             // reinstall even when versions match
  bool allow_prerelease = false;  // per-run override for github sources
  bool dry_run = false;
};

/// "Download a build from a source and register it" request. Used by the GUI
/// download dialog and by `aimgr install` (GitHub or a plain download URL).
struct InstallOptions {
  SourceType source = SourceType::None;
  SourceGithub github;
  SourceUrl url;
  std::string directory;   // destination folder; empty -> default_install_dir()
  std::string id;          // optional; derived from name/file name otherwise
  std::string name;        // optional display name
  std::string filename;    // optional destination file name
  bool replace = false;    // overwrite an existing file / app with the same id
  bool allow_prerelease = false;
  bool dry_run = false;
};

struct InstallResult {
  bool ok = false;
  std::string id;
  std::string path;
  std::string version;
  std::string error;
};

/// Orchestrates check / update / rollback / uninstall with crash-safe swap.
/// Not thread-safe: drive one instance from one thread at a time.
class Manager {
 public:
  Manager(Paths paths, Logger logger);

  /// Progress sink used by the GUI; the CLI leaves it empty and gets console
  /// output through the logger instead.
  std::function<void(const std::string& id, uint64_t done, uint64_t total)> on_progress;

  bool load(std::string* err);
  bool save(std::string* err);

  Config& config() { return cfg_; }
  const Config& config() const { return cfg_; }
  const Paths& paths() const { return paths_; }

  App* find(const std::string& id_or_path);
  std::vector<App*> all();

  /// Resolves the remote side without touching the disk.
  RemoteRelease resolve(App& app);

  CheckResult check(App& app);
  std::vector<CheckResult> check_all(const std::vector<std::string>& ids);

  UpdateResult update(App& app, const UpdateOptions& opts = {});
  std::vector<UpdateResult> update_all(const std::vector<std::string>& ids,
                                       const UpdateOptions& opts = {});

  std::vector<BackupEntry> backups(const App& app);
  bool rollback(App& app, std::string backup_selector, std::string* err,
                std::string* restored = nullptr);
  int prune(App& app, std::string* err);

  bool uninstall(App& app, bool purge_backups, bool delete_file, bool keep_config,
                 std::string* err);

  bool add_app(App app, bool replace, std::string* err);
  bool remove_app(const std::string& id, std::string* err);

  /// Resolves, downloads and atomically installs an AppImage from a configured
  /// source into `opts.directory`, then registers it in the managed app list.
  InstallResult install_from_source(const InstallOptions& opts);

  /// Re-points an existing app at a new update source (used by the GUI source
  /// dialog and by `aimgr set-source`). Validates and persists.
  bool set_source(App& app, SourceType type, const SourceGithub& gh, const SourceUrl& su,
                  std::string* err);
  /// Persists GUI preferences (theme, font scale).
  bool set_ui(const UiSettings& ui, std::string* err);

  /// Discovers *.AppImage files in `dir` and registers unknown ones.
  std::vector<App> scan(const std::string& dir, std::string* err);
  /// Same, optionally walking subdirectories (directory symlinks are not
  /// followed, so a scan cannot loop or escape through a crafted link).
  std::vector<App> scan_folder(const std::string& dir, bool recursive,
                               std::string* err);

  /// The .desktop path aimgr owns for this app (derived from the validated id).
  std::string desktop_path_for(const App& app) const;
  /// Generates the exact .desktop body from App::desktop. No file is written.
  std::string render_desktop(const App& app, std::string* err) const;
  /// Creates/updates (enabled=true) or removes (enabled=false) the managed
  /// desktop entry and persists the generation settings.
  bool set_desktop(App& app, const DesktopEntry& de, bool enabled, std::string* err);

  bool integrate(App& app, std::string* err);
  bool deintegrate(App& app, std::string* err);

  /// Returns a human readable message when a crashed run was repaired.
  std::string recover();

  std::string installed_version_of(const App& app) const;
  std::string backup_dir_of(const App& app) const;
  int backup_keep_of(const App& app) const;

 private:
  bool resize_and_swap(const fs::path& staged, const fs::path& target, std::string* err);
  bool restore_from_backup(const fs::path& backup, const fs::path& target, std::string* err);
  void touch_check(App& app, const std::string& status);
  fs::path cache_file_for(const App& app, const std::string& version,
                          const std::string& asset) const;

 public:
  /// Serialises config/journal mutations across processes (and across GUI
  /// worker jobs) with an advisory lock; recursive within one instance.
  bool begin_mutation(std::string* err);
  void end_mutation();

 private:
  Paths paths_;
  Logger log_;
  Config cfg_;
  Http http_;
  FileLock lock_;
  int lock_depth_ = 0;
};

}  // namespace aimgr
