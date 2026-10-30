#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace aimgr {

enum class SourceType { None, Github, Url };

/// "Look at the latest GitHub release of `repo`, pick the asset matching
/// `asset_regex`, and treat its tag as the version."
struct SourceGithub {
  std::string repo;              // "owner/name" (full URLs are parsed too)
  std::string asset_regex;       // default: ".*\\.AppImage$"
  std::string tag_prefix;        // e.g. "v" -> tag v1.2.3 becomes 1.2.3
  bool allow_prerelease = false;
};

/// "Fetch `url` (optionally derived from a version published elsewhere) and
/// treat it as the current build."
struct SourceUrl {
  std::string url;               // may contain ${version}
  std::string version_url;       // plain-text page that publishes the version
  std::string version_regex;     // regex used on version_url (or on the url)
  std::string sha256;            // expected digest, optional
  std::string sha256_url;        // or fetch it from here (first 64-hex token)
};

/// GUI preferences, persisted next to the app list so both front-ends agree.
struct UiSettings {
  std::string theme = "bento";   ///< built-in preset slug, see ImGuiThemes::Slug()
  std::string custom_theme_file; ///< optional .ini produced by the theme studio
  float font_scale = 1.0f;       ///< 0.8 .. 2.0
  bool launcher_prompt_done = false;  ///< user answered the "add to launcher" banner
};

/// User-editable values used to generate the per-app .desktop file. The
/// executable path itself is never editable here: it always comes from the
/// managed AppImage, so a .desktop entry cannot be turned into an arbitrary
/// command launcher through the config file.
struct DesktopEntry {
  std::string name;                  // empty -> App::name
  std::string comment = "AppImage managed by aimgr";
  std::string categories = "Utility;";
  std::string icon;                  // icon name or absolute path, optional
  std::string args = "%U";           // extra Exec arguments / field codes
  bool terminal = false;
};

struct App {
  std::string id;                // stable handle used by every command
  std::string name;              // display name
  std::string path;              // absolute AppImage path (target to replace)
  SourceType source = SourceType::None;
  SourceGithub github;
  SourceUrl url;
  DesktopEntry desktop;          // persisted .desktop generation settings

  std::string version_regex;     // fallback: derive version from file name
  std::string installed_version;
  std::string installed_sha256;
  int backup_keep = -1;          // -1 -> Defaults::backup_keep
  bool pinned = false;

  std::string last_check;
  std::string last_update;
  std::string last_status;
  std::string desktop_entry;     // managed .desktop file, when integrated
  bool integrated = false;
};

struct Defaults {
  int backup_keep = 3;
  std::string backup_dir;
  std::string download_dir;
};

struct Config {
  int schema = 1;
  Defaults defaults;
  UiSettings ui;
  std::vector<App> apps;
};

/// Fills in XDG defaults for anything left blank.
void apply_defaults(Config& cfg);

/// Loads (or returns an empty config when the file does not exist yet).
bool load_config(const std::filesystem::path& path, Config& out, std::string* err);
bool save_config(const std::filesystem::path& path, const Config& cfg, std::string* err);

const char* source_type_name(SourceType t);
SourceType source_type_from_name(const std::string& name);

}  // namespace aimgr
