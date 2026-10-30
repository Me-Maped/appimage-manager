#pragma once
#include <filesystem>

#include "aimgr/util.hpp"

namespace aimgr {

/// Every path the tool touches, resolved from XDG variables and overridable
/// through $AIMGR_CONFIG / $AIMGR_STATE_DIR / $AIMGR_CACHE_DIR.
struct Paths {
  fs::path config;
  fs::path state_dir;
  fs::path cache_dir;
  fs::path backup_dir;  ///< root for versioned AppImage backups

  static Paths resolve();
  fs::path journal() const { return state_dir / "journal.json"; }
  fs::path lock() const { return state_dir / "aimgr.lock"; }
};

}  // namespace aimgr
