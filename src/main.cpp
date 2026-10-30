// aimgr - AppImage manager: check / update / roll back / uninstall AppImages
// that are published through GitHub releases or plain download URLs.
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "aimgr/config.hpp"
#include "aimgr/json.hpp"
#include "aimgr/manager.hpp"
#include "aimgr/selfupdate.hpp"
#include "aimgr/source.hpp"
#include "aimgr/util.hpp"
#include "aimgr/version.hpp"

using namespace aimgr;

namespace {

const char* kUsage = R"(aimgr - AppImage 管理器（检查更新 / 更新 / 回滚 / 卸载）

用法:
  aimgr [全局选项] <命令> [参数]

命令:
  list                          列出已登记的应用
  add --path P [--id ID] [--name N] [--github owner/repo | --url U]
                                登记一个已有 AppImage
  install | download --github owner/repo [--dir D] [--id ID] [--name N]
                     | --url U [--version-url U] [--version-regex R]
                                从 GitHub release 或下载 URL 直接下载到 --dir 并登记
  set-source <id> --github owner/repo | --url U [--version-url U] ...
                                给已登记的应用配置/更换更新源
  check [id...]                 检查更新（不带 id 表示全部）
  update [id...] [--force] [--prerelease] [--dry-run]
                                下载并原子替换，旧版本自动备份
  backups <id>                  列出备份版本
  rollback <id> [版本|latest]   还原到某个备份版本
  prune <id>                    按 backup_keep 清理旧备份
  uninstall <id> [--purge] [--delete-file] [--keep-config]
                                卸载（默认把 AppImage 移进备份目录，可再还原）
  scan [--recursive] [目录...]  扫描目录里未登记的 *.AppImage（可递归）
  integrate <id> [--desktop-name N] [--comment C] [--categories C]
              [--icon P] [--args A] [--terminal] [--remove]
                                创建 / 修改 / 删除该应用的 .desktop 文件
  deintegrate <id>              删除该应用的 .desktop 文件
  self <status|update|integrate|remove>
                                管理 aimgr 自身：状态 / 自更新（GitHub 发布）/ 创建或移除启动器项
  recover                       修复上次中断的更新（启动时也会自动执行）
  doctor                        打印环境/路径诊断
  gui                           启动图形界面（aimgr-gui）

全局选项:
  -c, --config PATH   指定配置文件（默认 ~/.config/aimgr/config.json）
      --json          机器可读的 JSON 输出
  -q, --quiet         只输出错误
  -v, --verbose       调试输出
  -h, --help          显示帮助
  -V, --version       显示版本

退出码: 0 成功 / 1 失败 / 2 用法错误 / 3 有可用更新（配合 check --fail-on-update）

示例:
  aimgr install --github agalwood/Motrix
  aimgr add --path ~/Apps/Motrix-1.8.19.AppImage --github agalwood/Motrix
  aimgr add --path ~/Apps/foo.AppImage --url https://example.com/foo-${version}.AppImage \
            --version-url https://example.com/latest.txt
  aimgr check && aimgr update motrix && aimgr rollback motrix
)";

bool g_json = false;
bool g_quiet = false;
bool g_verbose = false;

bool use_color() { return ::isatty(STDERR_FILENO) && ::getenv("NO_COLOR") == nullptr; }

void log_sink(LogLevel level, const std::string& msg) {
  if (g_quiet && level != LogLevel::Error) return;
  if (g_verbose == false && level == LogLevel::Debug) return;
  if (g_json) {
    if (level == LogLevel::Error || level == LogLevel::Warn) std::fprintf(stderr, "%s\n", msg.c_str());
    return;
  }
  const char* c = "";
  const char* reset = "";
  const char* tag = "";
  if (use_color()) {
    reset = "\033[0m";
    switch (level) {
      case LogLevel::Warn: c = "\033[33m"; tag = "warn: "; break;
      case LogLevel::Error: c = "\033[31m"; tag = "error: "; break;
      case LogLevel::Debug: c = "\033[90m"; tag = "debug: "; break;
      default: c = "\033[36m"; tag = ""; break;
    }
  }
  const std::string safe = strip_control(msg);
  std::fprintf(stderr, "%s%s%s%s\n", c, tag, safe.c_str(), reset);
}

struct Args {
  std::vector<std::string> pos;
  std::map<std::string, std::string> opt;
  std::set<std::string> flags;

  bool has(const std::string& k) const {
    return flags.count(k) || opt.count(k);
  }
  std::string get(const std::string& k, const std::string& def = "") const {
    auto it = opt.find(k);
    return it == opt.end() ? def : it->second;
  }
  int get_int(const std::string& k, int def) const {
    auto it = opt.find(k);
    if (it == opt.end() || it->second.empty()) return def;
    return std::atoi(it->second.c_str());
  }
};

const std::set<std::string> kValueOpts = {
    "config", "id", "name", "path", "github", "url", "version-url", "version-regex",
    "asset-regex", "tag-prefix", "backup-keep", "dir", "sha256", "sha256-url",
    "autoquit-ms", "screenshot", "repo", "filename", "desktop-name", "comment",
    "categories", "icon", "args"};

Args parse_args(const std::vector<std::string>& argv, std::string* err) {
  Args a;
  for (size_t i = 0; i < argv.size(); ++i) {
    const std::string& s = argv[i];
    if (s.size() > 2 && s[0] == '-' && s[1] == '-') {
      std::string body = s.substr(2);
      auto eq = body.find('=');
      if (eq != std::string::npos) {
        a.opt[body.substr(0, eq)] = body.substr(eq + 1);
      } else if (kValueOpts.count(body)) {
        if (i + 1 >= argv.size()) {
          if (err) *err = "option --" + body + " needs a value";
          return a;
        }
        a.opt[body] = argv[++i];
      } else {
        a.flags.insert(body);
      }
    } else if (s.size() == 2 && s[0] == '-' && s[1] == 'c') {
      if (i + 1 >= argv.size()) {
        if (err) *err = "option -c needs a value";
        return a;
      }
      a.opt["config"] = argv[++i];
    } else if (s == "-h") {
      a.flags.insert("help");
    } else if (s == "-V") {
      a.flags.insert("version");
    } else if (s == "-q") {
      a.flags.insert("quiet");
    } else if (s == "-v") {
      a.flags.insert("verbose");
    } else if (!s.empty() && s[0] == '-' && s != "-") {
      if (err) *err = "unknown option '" + s + "'";
      return a;
    } else {
      a.pos.push_back(s);
    }
  }
  return a;
}

void print_json(const json::Value& v) { std::printf("%s", json::dump(v, 2).c_str()); }

json::Value check_to_json(const CheckResult& c) {
  json::Value j = json::Value::object();
  j.set("id", json::Value::make_str(c.id));
  j.set("name", json::Value::make_str(c.name));
  j.set("status", json::Value::make_str(c.status));
  j.set("installed", json::Value::make_str(c.installed));
  j.set("latest", json::Value::make_str(c.latest));
  j.set("update_available", json::Value::make_bool(c.update_available));
  j.set("asset", json::Value::make_str(c.asset));
  j.set("url", json::Value::make_str(c.url));
  j.set("size", json::Value::make_num(static_cast<double>(c.size)));
  j.set("published", json::Value::make_str(c.published));
  j.set("notes_url", json::Value::make_str(c.notes_url));
  j.set("error", json::Value::make_str(c.error));
  return j;
}

json::Value update_to_json(const UpdateResult& u) {
  json::Value j = json::Value::object();
  j.set("id", json::Value::make_str(u.id));
  j.set("ok", json::Value::make_bool(u.ok));
  j.set("updated", json::Value::make_bool(u.updated));
  j.set("from", json::Value::make_str(u.from));
  j.set("to", json::Value::make_str(u.to));
  j.set("backup", json::Value::make_str(u.backup));
  j.set("error", json::Value::make_str(u.error));
  return j;
}

std::string pad(const std::string& s, size_t w) {
  if (s.size() >= w) return s;
  return s + std::string(w - s.size(), ' ');
}

void print_table(const std::vector<std::vector<std::string>>& rows) {
  if (rows.empty()) return;
  std::vector<size_t> w(rows[0].size(), 0);
  for (const auto& r : rows)
    for (size_t i = 0; i < r.size() && i < w.size(); ++i) w[i] = std::max(w[i], r[i].size());
  for (size_t ri = 0; ri < rows.size(); ++ri) {
    std::string line;
    for (size_t i = 0; i < rows[ri].size(); ++i) {
      line += pad(rows[ri][i], w[i]);
      if (i + 1 < rows[ri].size()) line += "  ";
    }
    while (!line.empty() && line.back() == ' ') line.pop_back();
    std::printf("%s\n", line.c_str());
    if (ri == 0) {
      std::string sep;
      for (size_t i = 0; i < w.size(); ++i) {
        sep += std::string(w[i], '-');
        if (i + 1 < w.size()) sep += "  ";
      }
      std::printf("%s\n", sep.c_str());
    }
  }
}

void print_progress(const std::string& id, uint64_t done, uint64_t total) {
  if (g_json || g_quiet) return;
  static std::map<std::string, int> last;
  const int pct = total > 0 ? static_cast<int>(done * 100 / total) : -1;
  if (pct >= 0 && last[id] == pct) return;
  last[id] = pct;
  if (!use_color()) return;  // keep non-tty logs clean
  if (pct >= 0)
    std::fprintf(stderr, "\r\033[36m%s\033[0m downloading %3d%%  %s / %s\033[K",
                 id.c_str(), pct, format_size(done).c_str(), format_size(total).c_str());
  else
    std::fprintf(stderr, "\r\033[36m%s\033[0m downloading %s\033[K", id.c_str(),
                 format_size(done).c_str());
  if (pct >= 100) std::fprintf(stderr, "\n");
}

std::string source_summary(const App& a) {
  switch (a.source) {
    case SourceType::Github: return "github:" + a.github.repo;
    case SourceType::Url: return "url:" + a.url.url;
    default: return "none";
  }
}

int cmd_list(Manager& m, const Args&) {
  auto apps = m.all();
  if (g_json) {
    json::Value arr = json::Value::array();
    for (auto* a : apps) {
      json::Value j = json::Value::object();
      j.set("id", json::Value::make_str(a->id));
      j.set("name", json::Value::make_str(a->name));
      j.set("path", json::Value::make_str(a->path));
      j.set("installed_version", json::Value::make_str(m.installed_version_of(*a)));
      j.set("source", json::Value::make_str(source_type_name(a->source)));
      j.set("source_detail", json::Value::make_str(source_summary(*a)));
      j.set("pinned", json::Value::make_bool(a->pinned));
      j.set("integrated", json::Value::make_bool(path_is_regular(m.desktop_path_for(*a))));
      j.set("desktop_entry", json::Value::make_str(a->desktop_entry));
      j.set("last_check", json::Value::make_str(a->last_check));
      j.set("last_status", json::Value::make_str(a->last_status));
      arr.push(j);
    }
    json::Value root = json::Value::object();
    root.set("ok", json::Value::make_bool(true));
    root.set("count", json::Value::make_num(static_cast<double>(apps.size())));
    root.set("apps", std::move(arr));
    print_json(root);
    return 0;
  }
  if (apps.empty()) {
    std::printf("还没有登记任何 AppImage。用 `aimgr scan ~/Apps` 或 `aimgr add --path ...` 开始。\n");
    return 0;
  }
  std::vector<std::vector<std::string>> rows = {{"ID", "NAME", "INSTALLED", "SOURCE", "PATH"}};
  for (auto* a : apps) {
    std::string v = m.installed_version_of(*a);
    rows.push_back({a->id, a->name, v.empty() ? "?" : v, source_summary(*a), a->path});
  }
  print_table(rows);
  return 0;
}

int cmd_add(Manager& m, const Args& a) {
  if (!a.has("path")) {
    std::fprintf(stderr, "add 需要 --path（已有 AppImage 的路径）。全新安装请用 `aimgr install --github owner/repo`。\n");
    return 2;
  }
  App app;
  app.id = a.get("id");
  app.name = a.get("name");
  app.path = a.get("path");
  app.version_regex = a.get("version-regex");
  app.backup_keep = a.get_int("backup-keep", -1);
  if (a.has("github")) {
    app.source = SourceType::Github;
    app.github.repo = a.get("github");
    app.github.asset_regex = a.get("asset-regex");
    app.github.tag_prefix = a.get("tag-prefix");
    app.github.allow_prerelease = a.has("prerelease");
  } else if (a.has("url")) {
    app.source = SourceType::Url;
    app.url.url = a.get("url");
    app.url.version_url = a.get("version-url");
    app.url.version_regex = a.get("version-regex");
    app.url.sha256 = a.get("sha256");
    app.url.sha256_url = a.get("sha256-url");
  }
  std::string err;
  if (!m.add_app(app, a.has("replace"), &err)) {
    std::fprintf(stderr, "add 失败: %s\n", err.c_str());
    return 1;
  }
  App* saved = m.find(app.id.empty() ? slugify(a.get("name")) : app.id);
  if (!g_json)
    std::printf("已登记 %s -> %s\n", saved ? saved->id.c_str() : "?", app.path.c_str());
  else {
    json::Value root = json::Value::object();
    root.set("ok", json::Value::make_bool(true));
    root.set("id", json::Value::make_str(saved ? saved->id : ""));
    print_json(root);
  }
  return 0;
}

int cmd_install(Manager& m, const Args& a) {
  if (!a.has("github") && !a.has("url")) {
    std::fprintf(stderr,
                 "install/download 需要 --github owner/repo 或 --url URL"
                 "（已有文件请用 add）。\n");
    return 2;
  }
  if (a.has("github") && a.has("url")) {
    std::fprintf(stderr, "--github 和 --url 只能选一个。\n");
    return 2;
  }
  InstallOptions opts;
  opts.directory = a.get("dir");
  opts.id = a.get("id");
  opts.name = a.get("name");
  opts.filename = a.get("filename");
  opts.replace = a.has("replace") || a.has("force");
  opts.dry_run = a.has("dry-run");
  opts.allow_prerelease = a.has("prerelease");
  if (a.has("github")) {
    opts.source = SourceType::Github;
    opts.github.repo = a.get("github");
    opts.github.asset_regex = a.get("asset-regex");
    opts.github.tag_prefix = a.get("tag-prefix");
    opts.github.allow_prerelease = a.has("prerelease");
  } else {
    opts.source = SourceType::Url;
    opts.url.url = a.get("url");
    opts.url.version_url = a.get("version-url");
    opts.url.version_regex = a.get("version-regex");
    opts.url.sha256 = a.get("sha256");
    opts.url.sha256_url = a.get("sha256-url");
  }
  InstallResult r = m.install_from_source(opts);
  if (g_json) {
    json::Value j = json::Value::object();
    j.set("ok", json::Value::make_bool(r.ok));
    j.set("id", json::Value::make_str(r.id));
    j.set("path", json::Value::make_str(r.path));
    j.set("version", json::Value::make_str(r.version));
    j.set("error", json::Value::make_str(r.error));
    print_json(j);
    return r.ok ? 0 : 1;
  }
  if (!r.ok) {
    std::fprintf(stderr, "下载/安装失败: %s\n", r.error.c_str());
    return 1;
  }
  if (opts.dry_run)
    std::printf("· dry-run: %s %s -> %s\n", r.id.c_str(),
                r.version.empty() ? "unknown" : r.version.c_str(), r.path.c_str());
  else
    std::printf("已下载并登记 %s (%s) -> %s\n", r.id.c_str(),
                r.version.empty() ? "version unknown" : r.version.c_str(), r.path.c_str());
  return 0;
}

int cmd_set_source(Manager& m, const Args& a) {
  if (a.pos.size() < 2) {
    std::fprintf(stderr, "用法: aimgr set-source <id> --github owner/repo | --url U [--clear]\n");
    return 2;
  }
  App* app = m.find(a.pos[1]);
  if (!app) {
    std::fprintf(stderr, "未知应用: %s\n", a.pos[1].c_str());
    return 1;
  }
  SourceType type = SourceType::None;
  SourceGithub gh;
  SourceUrl su;
  if (a.has("github")) {
    type = SourceType::Github;
    gh.repo = a.get("github");
    gh.asset_regex = a.get("asset-regex");
    gh.tag_prefix = a.get("tag-prefix");
    gh.allow_prerelease = a.has("prerelease");
  } else if (a.has("url")) {
    type = SourceType::Url;
    su.url = a.get("url");
    su.version_url = a.get("version-url");
    su.version_regex = a.get("version-regex");
    su.sha256 = a.get("sha256");
    su.sha256_url = a.get("sha256-url");
  } else if (!a.has("clear")) {
    std::fprintf(stderr, "需要 --github、--url 或 --clear\n");
    return 2;
  }
  std::string err;
  if (!m.set_source(*app, type, gh, su, &err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }
  if (g_json) {
    json::Value j = json::Value::object();
    j.set("ok", json::Value::make_bool(true));
    j.set("id", json::Value::make_str(app->id));
    j.set("source", json::Value::make_str(source_type_name(app->source)));
    print_json(j);
  } else {
    std::printf("%s 的更新源已设为 %s\n", app->id.c_str(),
                app->source == SourceType::Github ? ("github:" + app->github.repo).c_str()
                : app->source == SourceType::Url ? ("url:" + app->url.url).c_str()
                                                 : "none");
  }
  return 0;
}

int cmd_check(Manager& m, const Args& a) {
  std::vector<std::string> ids(a.pos.begin() + 1, a.pos.end());
  auto results = m.check_all(ids);
  bool any = std::any_of(results.begin(), results.end(),
                         [](const CheckResult& c) { return c.update_available; });
  if (g_json) {
    json::Value arr = json::Value::array();
    for (const auto& c : results) arr.push(check_to_json(c));
    json::Value root = json::Value::object();
    root.set("ok", json::Value::make_bool(true));
    root.set("update_available", json::Value::make_bool(any));
    root.set("results", std::move(arr));
    print_json(root);
  } else {
    std::vector<std::vector<std::string>> rows = {{"ID", "INSTALLED", "LATEST", "STATUS"}};
    for (const auto& c : results) {
      std::string status = c.status;
      if (c.status == "error") status += ": " + c.error;
      rows.push_back({c.id, c.installed.empty() ? "?" : c.installed,
                      c.latest.empty() ? "?" : c.latest, status});
    }
    print_table(rows);
    if (!any) std::printf("\n没有可用更新。\n");
    else std::printf("\n有可用更新，运行 `aimgr update` 安装。\n");
  }
  if (a.has("fail-on-update") && any) return 3;
  bool errored = std::any_of(results.begin(), results.end(),
                             [](const CheckResult& c) { return c.status == "error"; });
  return errored ? 1 : 0;
}

int cmd_update(Manager& m, const Args& a) {
  UpdateOptions opts;
  opts.force = a.has("force");
  opts.allow_prerelease = a.has("prerelease");
  opts.dry_run = a.has("dry-run");
  std::vector<std::string> ids(a.pos.begin() + 1, a.pos.end());
  auto results = m.update_all(ids, opts);
  bool ok = true;
  for (const auto& r : results) ok = ok && r.ok;
  if (g_json) {
    json::Value arr = json::Value::array();
    for (const auto& r : results) arr.push(update_to_json(r));
    json::Value root = json::Value::object();
    root.set("ok", json::Value::make_bool(ok));
    root.set("results", std::move(arr));
    print_json(root);
  } else {
    int updated = 0;
    for (const auto& r : results) {
      if (r.updated) {
        ++updated;
        std::printf("✔ %s %s -> %s (备份: %s)\n", r.id.c_str(),
                    r.from.empty() ? "?" : r.from.c_str(),
                    r.to.empty() ? "?" : r.to.c_str(),
                    r.backup.empty() ? "-" : r.backup.c_str());
      } else if (!r.ok) {
        std::printf("✘ %s 失败: %s\n", r.id.c_str(), r.error.c_str());
      } else if (r.error == "dry-run") {
        std::printf("· %s 可更新 %s -> %s (dry-run)\n", r.id.c_str(), r.from.c_str(), r.to.c_str());
      }
    }
    if (updated == 0) std::printf("没有需要更新的应用。\n");
  }
  return ok ? 0 : 1;
}

int cmd_backups(Manager& m, const Args& a) {
  if (a.pos.size() < 2) {
    std::fprintf(stderr, "用法: aimgr backups <id>\n");
    return 2;
  }
  App* app = m.find(a.pos[1]);
  if (!app) {
    std::fprintf(stderr, "未知应用: %s\n", a.pos[1].c_str());
    return 1;
  }
  auto list = m.backups(*app);
  if (g_json) {
    json::Value arr = json::Value::array();
    for (const auto& b : list) {
      json::Value j = json::Value::object();
      j.set("file", json::Value::make_str(b.file));
      j.set("version", json::Value::make_str(b.version));
      j.set("sha256", json::Value::make_str(b.sha256));
      j.set("size", json::Value::make_num(static_cast<double>(b.size)));
      j.set("created_at", json::Value::make_str(b.created_at));
      arr.push(j);
    }
    json::Value root = json::Value::object();
    root.set("ok", json::Value::make_bool(true));
    root.set("backups", std::move(arr));
    print_json(root);
    return 0;
  }
  if (list.empty()) {
    std::printf("%s 还没有备份。\n", app->id.c_str());
    return 0;
  }
  std::vector<std::vector<std::string>> rows = {{"VERSION", "CREATED", "SIZE", "FILE"}};
  for (const auto& b : list)
    rows.push_back({b.version, b.created_at, format_size(b.size), b.file});
  print_table(rows);
  return 0;
}

int cmd_rollback(Manager& m, const Args& a) {
  if (a.pos.size() < 2) {
    std::fprintf(stderr, "用法: aimgr rollback <id> [版本|latest]\n");
    return 2;
  }
  App* app = m.find(a.pos[1]);
  if (!app) {
    std::fprintf(stderr, "未知应用: %s\n", a.pos[1].c_str());
    return 1;
  }
  const std::string sel = a.pos.size() > 2 ? a.pos[2] : "latest";
  std::string err, restored;
  if (!m.rollback(*app, sel, &err, &restored)) {
    std::fprintf(stderr, "回滚失败: %s\n", err.c_str());
    return 1;
  }
  if (g_json) {
    json::Value root = json::Value::object();
    root.set("ok", json::Value::make_bool(true));
    root.set("id", json::Value::make_str(app->id));
    root.set("restored", json::Value::make_str(restored));
    print_json(root);
  } else {
    std::printf("✔ %s 已回滚到 %s\n", app->id.c_str(), restored.c_str());
  }
  return 0;
}

int cmd_prune(Manager& m, const Args& a) {
  if (a.pos.size() < 2) {
    std::fprintf(stderr, "用法: aimgr prune <id>\n");
    return 2;
  }
  App* app = m.find(a.pos[1]);
  if (!app) {
    std::fprintf(stderr, "未知应用: %s\n", a.pos[1].c_str());
    return 1;
  }
  int removed = m.prune(*app, nullptr);
  std::printf("清理了 %d 个旧备份（保留 %d 个）。\n", removed, m.backup_keep_of(*app));
  return 0;
}

int cmd_uninstall(Manager& m, const Args& a) {
  if (a.pos.size() < 2) {
    std::fprintf(stderr, "用法: aimgr uninstall <id> [--purge] [--delete-file] [--keep-config]\n");
    return 2;
  }
  App* app = m.find(a.pos[1]);
  if (!app) {
    std::fprintf(stderr, "未知应用: %s\n", a.pos[1].c_str());
    return 1;
  }
  const std::string id = app->id;
  std::string err;
  if (!m.uninstall(*app, a.has("purge"), a.has("delete-file"), a.has("keep-config"), &err)) {
    std::fprintf(stderr, "卸载失败: %s\n", err.c_str());
    return 1;
  }
  if (g_json) {
    json::Value root = json::Value::object();
    root.set("ok", json::Value::make_bool(true));
    root.set("id", json::Value::make_str(id));
    print_json(root);
  } else {
    std::printf("✔ 已卸载 %s\n", id.c_str());
  }
  return 0;
}

int cmd_scan(Manager& m, const Args& a) {
  std::vector<std::string> dirs;
  for (size_t i = 1; i < a.pos.size(); ++i) dirs.push_back(a.pos[i]);
  if (dirs.empty()) dirs.push_back(default_install_dir().string());
  const bool recursive = a.has("recursive");
  std::vector<App> added;
  bool failed = false;
  for (const auto& dir : dirs) {
    std::string err;
    auto part = m.scan_folder(dir, recursive, &err);
    if (!err.empty()) {
      std::fprintf(stderr, "%s: %s\n", dir.c_str(), err.c_str());
      failed = true;
    }
    added.insert(added.end(), part.begin(), part.end());
  }
  if (g_json) {
    json::Value arr = json::Value::array();
    for (const auto& x : added) {
      json::Value j = json::Value::object();
      j.set("id", json::Value::make_str(x.id));
      j.set("path", json::Value::make_str(x.path));
      j.set("installed_version", json::Value::make_str(x.installed_version));
      arr.push(j);
    }
    json::Value root = json::Value::object();
    root.set("ok", json::Value::make_bool(!failed));
    root.set("added", std::move(arr));
    print_json(root);
  } else {
    std::printf("新登记 %zu 个 AppImage。\n", added.size());
    if (!added.empty())
      std::printf("提示: 用 `aimgr set-source <id> --github owner/repo` 给它们配置更新源。\n");
  }
  return failed ? 1 : 0;
}

int cmd_integrate(Manager& m, const Args& a) {
  const bool remove = a.has("remove") || a.pos[0] == "deintegrate";
  if (a.pos.size() < 2) {
    std::fprintf(stderr, "用法: aimgr %s <id> [--desktop-name N] [--comment C] "
                         "[--categories C] [--icon P] [--args A] [--terminal]%s\n",
                 a.pos[0].c_str(), remove ? "" : " [--remove]");
    return 2;
  }
  App* app = m.find(a.pos[1]);
  if (!app) {
    std::fprintf(stderr, "未知应用: %s\n", a.pos[1].c_str());
    return 1;
  }
  DesktopEntry de = app->desktop;
  if (a.has("desktop-name")) de.name = a.get("desktop-name");
  if (a.has("comment")) de.comment = a.get("comment");
  if (a.has("categories")) de.categories = a.get("categories");
  if (a.has("icon")) de.icon = a.get("icon");
  if (a.has("args")) de.args = a.get("args");
  if (a.has("terminal")) de.terminal = true;
  std::string err;
  const bool ok = remove ? m.deintegrate(*app, &err) : m.set_desktop(*app, de, true, &err);
  if (!ok) {
    std::fprintf(stderr, "%s失败: %s\n", remove ? "删除桌面项" : "生成桌面项", err.c_str());
    return 1;
  }
  if (g_json) {
    json::Value j = json::Value::object();
    j.set("ok", json::Value::make_bool(true));
    j.set("id", json::Value::make_str(app->id));
    j.set("integrated", json::Value::make_bool(app->integrated));
    j.set("desktop_entry", json::Value::make_str(app->desktop_entry));
    print_json(j);
  } else if (remove) {
    std::printf("✔ 已删除 %s 的桌面项\n", app->id.c_str());
  } else {
    std::printf("✔ 已写入 %s\n", app->desktop_entry.c_str());
    std::printf("  Name=%s  Categories=%s  Terminal=%s\n",
                app->desktop.name.empty() ? app->name.c_str() : app->desktop.name.c_str(),
                app->desktop.categories.c_str(), app->desktop.terminal ? "true" : "false");
  }
  return 0;
}

int cmd_self(Manager& m, const Args& a) {
  const std::string sub = a.pos.size() > 1 ? a.pos[1] : "status";
  const SelfInfo info = SelfInfo::detect();

  if (sub == "status") {
    std::string desktop;
    const bool integrated = self_is_integrated(&desktop);
    const std::string installed = self_installed_version(m.paths());
    if (g_json) {
      json::Value j = json::Value::object();
      j.set("ok", json::Value::make_bool(true));
      j.set("version", json::Value::make_str(info.version));
      j.set("installed_version", json::Value::make_str(installed));
      j.set("appimage", json::Value::make_bool(info.appimage));
      j.set("path", json::Value::make_str(info.appimage_path));
      j.set("appdir", json::Value::make_str(info.appdir));
      j.set("repo", json::Value::make_str(self_default_repo()));
      j.set("integrated", json::Value::make_bool(integrated));
      j.set("desktop_entry", json::Value::make_str(desktop));
      print_json(j);
    } else {
      std::printf("运行版本:  %s\n", info.version.c_str());
      std::printf("磁盘版本:  %s\n", installed.c_str());
      std::printf("运行形态:  %s\n", info.appimage ? "AppImage" : "普通可执行文件");
      std::printf("路径:      %s\n", info.appimage_path.c_str());
      if (!info.appdir.empty()) std::printf("APPDIR:    %s\n", info.appdir.c_str());
      std::printf("更新源:    %s\n", self_default_repo().c_str());
      std::printf("启动器项:  %s\n", integrated ? desktop.c_str() : "未创建（aimgr self integrate）");
    }
    return 0;
  }

  if (sub == "update") {
    SelfUpdateOptions opts;
    opts.repo = a.get("repo");
    opts.asset_regex = a.get("asset-regex");
    opts.target = a.get("path");
    opts.force = a.has("force");
    opts.dry_run = a.has("dry-run");
    std::string lerr;
    if (!m.begin_mutation(&lerr)) {
      std::fprintf(stderr, "%s\n", lerr.c_str());
      return 1;
    }
    Http http;
    SelfUpdateResult r = self_update(opts, http, Logger{log_sink}, m.paths());
    m.end_mutation();
    if (g_json) {
      json::Value j = json::Value::object();
      j.set("ok", json::Value::make_bool(r.ok));
      j.set("updated", json::Value::make_bool(r.updated));
      j.set("from", json::Value::make_str(r.from));
      j.set("to", json::Value::make_str(r.to));
      j.set("asset", json::Value::make_str(r.asset));
      j.set("url", json::Value::make_str(r.url));
      j.set("backup", json::Value::make_str(r.backup));
      j.set("error", json::Value::make_str(r.error));
      print_json(j);
    } else if (!r.ok) {
      std::fprintf(stderr, "自更新失败: %s\n", r.error.c_str());
    } else if (r.updated) {
      std::printf("✔ aimgr %s -> %s（备份: %s）\n", r.from.c_str(), r.to.c_str(),
                  r.backup.empty() ? "-" : r.backup.c_str());
      std::printf("  重启后生效%s\n", a.has("restart") ? "" : "（或用 --restart 自动重启）");
    } else {
      std::printf("已是最新版本（%s）。\n", r.from.c_str());
    }
    if (r.ok && r.updated && a.has("restart")) {
      const std::string target = info.appimage_path;
      if (!target.empty()) {
        char* argv2[] = {const_cast<char*>(target.c_str()), nullptr};
        ::execv(target.c_str(), argv2);
        std::fprintf(stderr, "重启失败: %s\n", errno_str().c_str());
        return 1;
      }
    }
    return r.ok ? 0 : 1;
  }

  if (sub == "integrate" || sub == "install-desktop") {
    std::string err, desktop;
    if (!self_integrate(&err, &desktop)) {
      std::fprintf(stderr, "创建启动器项失败: %s\n", err.c_str());
      return 1;
    }
    if (g_json) {
      json::Value j = json::Value::object();
      j.set("ok", json::Value::make_bool(true));
      j.set("desktop_entry", json::Value::make_str(desktop));
      print_json(j);
    } else {
      std::printf("✔ 已创建启动器项 %s\n", desktop.c_str());
    }
    return 0;
  }

  if (sub == "remove" || sub == "deintegrate") {
    std::string err;
    if (!self_deintegrate(&err)) {
      std::fprintf(stderr, "移除启动器项失败: %s\n", err.c_str());
      return 1;
    }
    std::printf("✔ 已移除启动器项\n");
    return 0;
  }

  std::fprintf(stderr,
               "用法: aimgr self <status|update|integrate|remove>\n"
               "  update   [--repo owner/repo] [--asset-regex R] [--force] [--dry-run] [--restart]\n");
  return 2;
}

int cmd_doctor(Manager& m, const Args&) {
  const Paths& p = m.paths();
  std::printf("aimgr %s\n", AIMGR_VERSION);
  std::printf("配置:     %s%s\n", p.config.string().c_str(), path_exists(p.config) ? "" : " (尚未创建)");
  std::printf("状态目录: %s\n", p.state_dir.string().c_str());
  std::printf("缓存目录: %s\n", p.cache_dir.string().c_str());
  std::printf("备份目录: %s\n", expand_tilde(m.config().defaults.backup_dir).string().c_str());
  std::printf("下载目录: %s\n", expand_tilde(m.config().defaults.download_dir).string().c_str());
  std::printf("GitHub API: %s\n", Http::github_api_base().c_str());
  const char* token = ::getenv("AIMGR_GITHUB_TOKEN");
  if (!token) token = ::getenv("GITHUB_TOKEN");
  std::printf("GitHub token: %s\n", token ? "已设置" : "未设置（匿名请求有 60 次/小时的限制）");
  std::printf("登记应用: %zu 个\n", m.config().apps.size());
  const SelfInfo info = SelfInfo::detect();
  std::string desktop;
  std::printf("自身形态: %s  %s\n", info.appimage ? "AppImage" : "普通二进制",
              info.appimage_path.c_str());
  std::printf("自身更新源: %s\n", self_default_repo().c_str());
  std::printf("启动器项: %s\n",
              self_is_integrated(&desktop) ? desktop.c_str() : "未创建");
  return 0;
}

int cmd_gui(const Args& a) {
  const char* self = "/usr/bin/aimgr-gui";
  std::string exe;
  char buf[4096];
  ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n > 0) {
    buf[n] = '\0';
    exe = fs::path(buf).parent_path() / "aimgr-gui";
  } else {
    exe = self;
  }
  if (!path_exists(exe)) {
    std::fprintf(stderr, "找不到 %s，请先构建 GUI（cmake -DAIMGR_BUILD_GUI=ON）。\n", exe.c_str());
    return 1;
  }
  // execv (not system()): no shell, so argument contents can never be
  // interpreted as commands.
  std::vector<std::string> args;
  args.push_back(exe);
  if (a.has("config")) {
    args.push_back("--config");
    args.push_back(a.get("config"));
  }
  if (a.has("screenshot")) {
    args.push_back("--screenshot");
    args.push_back(a.get("screenshot"));
  }
  if (a.has("autoquit-ms")) {
    args.push_back("--autoquit-ms");
    args.push_back(a.get("autoquit-ms"));
  }
  for (size_t i = 1; i < a.pos.size(); ++i) args.push_back(a.pos[i]);
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (auto& x : args) argv.push_back(const_cast<char*>(x.c_str()));
  argv.push_back(nullptr);
  ::execv(exe.c_str(), argv.data());
  std::fprintf(stderr, "无法启动 %s: %s\n", exe.c_str(), errno_str().c_str());
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> raw(argv + 1, argv + argc);
  std::string perr;
  Args a = parse_args(raw, &perr);
  if (!perr.empty()) {
    std::fprintf(stderr, "%s\n\n%s", perr.c_str(), kUsage);
    return 2;
  }
  g_json = a.has("json");
  g_quiet = a.has("quiet");
  g_verbose = a.has("verbose");

  if (a.has("version")) {
    std::printf("aimgr %s\n", AIMGR_VERSION);
    return 0;
  }
  if (a.has("help") || a.pos.empty() || a.pos[0] == "help") {
    std::printf("%s", kUsage);
    return a.pos.empty() ? 2 : 0;
  }

  Paths paths = Paths::resolve();
  if (a.has("config")) paths.config = fs::path(expand_tilde(a.get("config")));
  Logger logger;
  logger.sink = log_sink;
  Manager m(paths, logger);
  std::string err;
  if (!m.load(&err)) {
    std::fprintf(stderr, "读取配置失败: %s\n", err.c_str());
    return 1;
  }
  m.on_progress = print_progress;

  const std::string cmd = a.pos[0];
  if (cmd == "gui") return cmd_gui(a);
  if (cmd == "doctor") return cmd_doctor(m, a);
  if (cmd == "self") return cmd_self(m, a);

  // Repair a previously interrupted update before doing anything else.
  if (cmd != "list" || g_verbose) m.recover();

  if (cmd == "list" || cmd == "ls") return cmd_list(m, a);
  if (cmd == "add") return cmd_add(m, a);
  if (cmd == "install" || cmd == "download") return cmd_install(m, a);
  if (cmd == "set-source") return cmd_set_source(m, a);
  if (cmd == "check") return cmd_check(m, a);
  if (cmd == "update" || cmd == "upgrade") return cmd_update(m, a);
  if (cmd == "backups") return cmd_backups(m, a);
  if (cmd == "rollback") return cmd_rollback(m, a);
  if (cmd == "prune") return cmd_prune(m, a);
  if (cmd == "uninstall" || cmd == "remove") return cmd_uninstall(m, a);
  if (cmd == "scan") return cmd_scan(m, a);
  if (cmd == "integrate" || cmd == "desktop" || cmd == "deintegrate")
    return cmd_integrate(m, a);
  if (cmd == "recover") {
    std::string msg = m.recover();
    if (!g_json) std::printf("%s\n", msg.empty() ? "没有需要修复的中断任务。" : msg.c_str());
    return 0;
  }

  std::fprintf(stderr, "未知命令: %s\n\n%s", cmd.c_str(), kUsage);
  return 2;
}
