#pragma once
#include <string>

#include "aimgr/http.hpp"
#include "aimgr/paths.hpp"
#include "aimgr/util.hpp"

namespace aimgr {

/// How this process was started: as an AppImage, or as a plain binary.
struct SelfInfo {
  bool appimage = false;
  std::string appimage_path;  ///< $APPIMAGE (resolved) or /proc/self/exe
  std::string appdir;         ///< $APPDIR when mounted
  std::string version;        ///< AIMGR_VERSION baked in at build time

  static SelfInfo detect();
};

struct SelfUpdateOptions {
  std::string repo;          ///< empty -> $AIMGR_SELF_REPO or the built-in default
  std::string asset_regex;   ///< default: aimgr-.*\.AppImage$
  std::string target;        ///< empty -> the running AppImage
  bool force = false;
  bool dry_run = false;
};

struct SelfUpdateResult {
  bool ok = false;
  bool updated = false;
  std::string from;    ///< running version
  std::string to;      ///< release version
  std::string asset;
  std::string url;
  std::string backup;  ///< path of the previous build
  std::string error;
};

/// Default update source for aimgr itself (override with -DAIMGR_SELF_REPO or
/// $AIMGR_SELF_REPO).
std::string self_default_repo();

/// Checks the release feed and, when newer, replaces the running AppImage:
/// download -> verify (ELF/AppImage, size, sha256 when published) -> backup ->
/// atomic rename. Never executes the downloaded file.
SelfUpdateResult self_update(const SelfUpdateOptions& opts, Http& http, const Logger& log,
                             const Paths& paths);

/// Writes ~/.local/share/applications/aimgr.desktop (+ icon) pointing at the
/// running AppImage, so launchers and app menus find it.
bool self_integrate(std::string* err, std::string* desktop_path = nullptr);
bool self_deintegrate(std::string* err);

/// True when a desktop entry pointing at the running build already exists.
bool self_is_integrated(std::string* desktop_path = nullptr);

/// Version of the build that is actually on disk for the running AppImage.
/// Falls back to AIMGR_VERSION when nothing has been recorded yet, so a
/// replaced-but-not-restarted AppImage does not offer the same update twice.
std::string self_installed_version(const Paths& paths);

}  // namespace aimgr
