#include "aimgr/config.hpp"

#include <cstdlib>

#include "aimgr/json.hpp"
#include "aimgr/util.hpp"

namespace aimgr {
namespace {

json::Value app_to_json(const App& a) {
  json::Value j = json::Value::object();
  j.set("id", json::Value::make_str(a.id));
  j.set("name", json::Value::make_str(a.name));
  j.set("path", json::Value::make_str(a.path));

  json::Value src = json::Value::object();
  src.set("type", json::Value::make_str(source_type_name(a.source)));
  if (a.source == SourceType::Github) {
    src.set("repo", json::Value::make_str(a.github.repo));
    src.set("asset_regex", json::Value::make_str(a.github.asset_regex));
    src.set("tag_prefix", json::Value::make_str(a.github.tag_prefix));
    src.set("allow_prerelease", json::Value::make_bool(a.github.allow_prerelease));
  } else if (a.source == SourceType::Url) {
    src.set("url", json::Value::make_str(a.url.url));
    src.set("version_url", json::Value::make_str(a.url.version_url));
    src.set("version_regex", json::Value::make_str(a.url.version_regex));
    src.set("sha256", json::Value::make_str(a.url.sha256));
    src.set("sha256_url", json::Value::make_str(a.url.sha256_url));
  }
  j.set("source", std::move(src));

  j.set("version_regex", json::Value::make_str(a.version_regex));
  j.set("installed_version", json::Value::make_str(a.installed_version));
  j.set("installed_sha256", json::Value::make_str(a.installed_sha256));
  j.set("backup_keep", json::Value::make_num(a.backup_keep));
  j.set("pinned", json::Value::make_bool(a.pinned));
  j.set("last_check", json::Value::make_str(a.last_check));
  j.set("last_update", json::Value::make_str(a.last_update));
  j.set("last_status", json::Value::make_str(a.last_status));
  j.set("integrated", json::Value::make_bool(a.integrated));
  j.set("desktop_entry", json::Value::make_str(a.desktop_entry));
  json::Value desk = json::Value::object();
  desk.set("name", json::Value::make_str(a.desktop.name));
  desk.set("comment", json::Value::make_str(a.desktop.comment));
  desk.set("categories", json::Value::make_str(a.desktop.categories));
  desk.set("icon", json::Value::make_str(a.desktop.icon));
  desk.set("args", json::Value::make_str(a.desktop.args));
  desk.set("terminal", json::Value::make_bool(a.desktop.terminal));
  j.set("desktop", std::move(desk));
  return j;
}

App app_from_json(const json::Value& j) {
  App a;
  a.id = j.get_str("id");
  a.name = j.get_str("name");
  a.path = j.get_str("path");
  if (const json::Value* src = j.find("source")) {
    a.source = source_type_from_name(src->get_str("type", "none"));
    a.github.repo = src->get_str("repo");
    a.github.asset_regex = src->get_str("asset_regex");
    a.github.tag_prefix = src->get_str("tag_prefix");
    a.github.allow_prerelease = src->get_bool("allow_prerelease", false);
    a.url.url = src->get_str("url");
    a.url.version_url = src->get_str("version_url");
    a.url.version_regex = src->get_str("version_regex");
    a.url.sha256 = src->get_str("sha256");
    a.url.sha256_url = src->get_str("sha256_url");
  }
  a.version_regex = j.get_str("version_regex");
  a.installed_version = j.get_str("installed_version");
  a.installed_sha256 = j.get_str("installed_sha256");
  a.backup_keep = static_cast<int>(j.get_num("backup_keep", -1));
  a.pinned = j.get_bool("pinned", false);
  a.last_check = j.get_str("last_check");
  a.last_update = j.get_str("last_update");
  a.last_status = j.get_str("last_status");
  a.integrated = j.get_bool("integrated", false);
  a.desktop_entry = j.get_str("desktop_entry");
  if (const json::Value* desk = j.find("desktop")) {
    a.desktop.name = desk->get_str("name");
    a.desktop.comment = desk->get_str("comment", "AppImage managed by aimgr");
    a.desktop.categories = desk->get_str("categories", "Utility;");
    a.desktop.icon = desk->get_str("icon");
    a.desktop.args = desk->get_str("args", "%U");
    a.desktop.terminal = desk->get_bool("terminal", false);
  }
  if (a.name.empty()) a.name = a.id;
  return a;
}

}  // namespace

const char* source_type_name(SourceType t) {
  switch (t) {
    case SourceType::Github: return "github";
    case SourceType::Url: return "url";
    default: return "none";
  }
}

SourceType source_type_from_name(const std::string& name) {
  if (name == "github") return SourceType::Github;
  if (name == "url") return SourceType::Url;
  return SourceType::None;
}

namespace {
/// XDG base directory helper: returns "<base>/aimgr" for the given env var.
std::string xdg_base(const char* var, const char* fallback_rel) {
  const char* v = ::getenv(var);
  if (v && *v) return std::string(v) + "/aimgr";
  const char* home = ::getenv("HOME");
  return std::string(home ? home : "/tmp") + "/" + fallback_rel + "/aimgr";
}
}  // namespace

void apply_defaults(Config& cfg) {
  if (cfg.ui.theme.empty()) cfg.ui.theme = "bento";
  if (!(cfg.ui.font_scale >= 0.8f && cfg.ui.font_scale <= 2.0f)) cfg.ui.font_scale = 1.0f;
  if (cfg.defaults.backup_dir.empty())
    cfg.defaults.backup_dir = xdg_base("XDG_DATA_HOME", ".local/share") + "/backups";
  if (cfg.defaults.download_dir.empty())
    cfg.defaults.download_dir = xdg_base("XDG_CACHE_HOME", ".cache") + "/downloads";
  if (cfg.defaults.backup_keep < 0) cfg.defaults.backup_keep = 3;
}

bool load_config(const fs::path& path, Config& out, std::string* err) {
  out = Config{};
  if (!path_exists(path)) {
    apply_defaults(out);
    return true;
  }
  std::string text;
  if (!read_file(path, text, err)) return false;
  json::Value root;
  std::string jerr;
  if (!json::parse(text, root, jerr)) {
    if (err) *err = "config " + path.string() + " is not valid JSON: " + jerr;
    return false;
  }
  if (!root.is_obj()) {
    if (err) *err = "config root must be a JSON object";
    return false;
  }
  out.schema = static_cast<int>(root.get_num("schema", 1));
  if (const json::Value* d = root.find("defaults")) {
    out.defaults.backup_keep = static_cast<int>(d->get_num("backup_keep", 3));
    out.defaults.backup_dir = d->get_str("backup_dir");
    out.defaults.download_dir = d->get_str("download_dir");
  }
  if (const json::Value* ui = root.find("ui")) {
    out.ui.theme = ui->get_str("theme", "bento");
    out.ui.custom_theme_file = ui->get_str("custom_theme_file");
    out.ui.font_scale = static_cast<float>(ui->get_num("font_scale", 1.0));
    out.ui.launcher_prompt_done = ui->get_bool("launcher_prompt_done", false);
  }
  if (const json::Value* apps = root.find("apps")) {
    if (apps->is_arr()) {
      for (size_t i = 0; i < apps->size(); ++i) {
        App a = app_from_json(apps->at(i));
        // An id is used as a directory/file component everywhere. A hand-edited
        // or untrusted config must not be able to point backups/cache outside
        // their roots through "../../".
        if (!is_valid_id(a.id)) {
          if (err) *err = "config entry " + std::to_string(i) + " has an invalid app id '" +
                          a.id + "'";
          return false;
        }
        out.apps.push_back(std::move(a));
      }
    }
  }
  apply_defaults(out);
  return true;
}

bool save_config(const fs::path& path, const Config& cfg, std::string* err) {
  json::Value root = json::Value::object();
  root.set("schema", json::Value::make_num(1));
  json::Value d = json::Value::object();
  d.set("backup_keep", json::Value::make_num(cfg.defaults.backup_keep));
  d.set("backup_dir", json::Value::make_str(cfg.defaults.backup_dir));
  d.set("download_dir", json::Value::make_str(cfg.defaults.download_dir));
  root.set("defaults", std::move(d));
  json::Value ui = json::Value::object();
  ui.set("theme", json::Value::make_str(cfg.ui.theme));
  ui.set("custom_theme_file", json::Value::make_str(cfg.ui.custom_theme_file));
  ui.set("font_scale", json::Value::make_num(cfg.ui.font_scale));
  ui.set("launcher_prompt_done", json::Value::make_bool(cfg.ui.launcher_prompt_done));
  root.set("ui", std::move(ui));
  json::Value apps = json::Value::array();
  for (const auto& a : cfg.apps) apps.push(app_to_json(a));
  root.set("apps", std::move(apps));
  return write_file_atomic(path, json::dump(root, 2), err, 0644);
}

}  // namespace aimgr
