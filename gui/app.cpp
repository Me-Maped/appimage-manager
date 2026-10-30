// aimgr-gui - ImGui (Me-Maped/imgui fork) + SDL2 + OpenGL3 front-end for aimgr.
//
// The GUI never touches the config or the network on the render thread: all
// work is queued on a single worker thread that owns the Manager, and the UI
// renders a snapshot guarded by a mutex. This keeps the interface responsive
// during multi-hundred-megabyte AppImage downloads.
//
// Theming comes from the fork's application-side theme module
// (misc/themes/imgui_themes.h): six built-in presets plus any .ini produced by
// the theme studio, persisted in ~/.config/aimgr/config.json under "ui".
#include <SDL.h>
#include <SDL_opengl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "aimgr/manager.hpp"
#include "aimgr/paths.hpp"
#include "aimgr/selfupdate.hpp"
#include "aimgr/source.hpp"
#include "aimgr/util.hpp"
#include "aimgr/version.hpp"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"
#ifdef AIMGR_HAS_IMGUI_THEMES
#include "imgui_themes.h"
#endif

using namespace aimgr;

namespace {

bool g_cjk = false;
const char* T(const char* zh, const char* en) { return g_cjk ? zh : en; }

struct Row {
  std::string id, name, path, installed, latest, status, error;
  // source details, so the source dialog can be prefilled without touching the
  // Manager from the render thread
  int source_type = 0;  // 0 none, 1 github, 2 url
  std::string repo, asset_regex, tag_prefix, url, version_url, version_regex;
  std::string sha256, sha256_url;
  bool prerelease = false;
  // per-app .desktop generation settings / state
  bool integrated = false;
  std::string desktop_path, desktop_name, desktop_comment, desktop_categories;
  std::string desktop_icon, desktop_args;
  bool desktop_terminal = false;
};

struct Shared {
  std::mutex mu;
  std::vector<Row> rows;
  std::vector<std::pair<LogLevel, std::string>> logs;
  std::vector<BackupEntry> backups;
  std::string backups_for;
  bool busy = false;
  std::string activity;
  uint64_t done = 0, total = 0;
  std::string restart_target;      // set after a successful self-update
  std::string prompt_source_for;   // set after a scan to open the source dialog
  std::string self_version, self_path, self_desktop;
  bool self_appimage = false, self_integrated = false;
};

void push_log(Shared& sh, LogLevel lvl, const std::string& msg) {
  std::lock_guard<std::mutex> lk(sh.mu);
  sh.logs.emplace_back(lvl, strip_control(msg));
  if (sh.logs.size() > 2000) sh.logs.erase(sh.logs.begin(), sh.logs.begin() + 500);
}

class Worker {
 public:
  Worker(Shared& sh) : sh_(sh) {}
  ~Worker() { stop(); }

  void start() { th_ = std::thread([this] { loop(); }); }

  void submit(const std::string& label, std::function<void()> fn) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      q_.push_back({label, std::move(fn)});
      std::lock_guard<std::mutex> lk2(sh_.mu);
      sh_.busy = true;
      sh_.activity = label;
      sh_.done = sh_.total = 0;
    }
    cv_.notify_one();
  }

  void stop() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
  }

 private:
  struct Job {
    std::string label;
    std::function<void()> fn;
  };
  void loop() {
    for (;;) {
      Job job;
      {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
        if (stop_ && q_.empty()) return;
        job = std::move(q_.front());
        q_.pop_front();
      }
      {
        std::lock_guard<std::mutex> lk(sh_.mu);
        sh_.activity = job.label;
        sh_.done = sh_.total = 0;
      }
      try {
        job.fn();
      } catch (const std::exception& e) {
        push_log(sh_, LogLevel::Error, std::string("internal error: ") + e.what());
      } catch (...) {
        push_log(sh_, LogLevel::Error, "internal error");
      }
      std::lock_guard<std::mutex> lk(mu_);
      if (q_.empty()) {
        std::lock_guard<std::mutex> lk2(sh_.mu);
        sh_.busy = false;
        sh_.activity.clear();
      }
    }
  }

  Shared& sh_;
  std::thread th_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Job> q_;
  bool stop_ = false;
};

int source_type_of(const App& a) {
  switch (a.source) {
    case SourceType::Github: return 1;
    case SourceType::Url: return 2;
    default: return 0;
  }
}

void refresh_rows(Manager& m, Shared& sh) {
  std::map<std::string, Row> prev;
  {
    std::lock_guard<std::mutex> lk(sh.mu);
    for (auto& r : sh.rows) prev[r.id] = r;
  }
  std::vector<Row> rows;
  for (auto* a : m.all()) {
    Row r;
    r.id = a->id;
    r.name = a->name;
    r.path = a->path;
    r.installed = m.installed_version_of(*a);
    r.source_type = source_type_of(*a);
    r.repo = a->github.repo;
    r.asset_regex = a->github.asset_regex;
    r.tag_prefix = a->github.tag_prefix;
    r.prerelease = a->github.allow_prerelease;
    r.url = a->url.url;
    r.version_url = a->url.version_url;
    r.version_regex = a->url.version_regex;
    r.sha256 = a->url.sha256;
    r.sha256_url = a->url.sha256_url;
    r.desktop_path = m.desktop_path_for(*a);
    r.integrated = path_is_regular(r.desktop_path);
    r.desktop_name = a->desktop.name.empty() ? a->name : a->desktop.name;
    r.desktop_comment = a->desktop.comment;
    r.desktop_categories = a->desktop.categories;
    r.desktop_icon = a->desktop.icon;
    r.desktop_args = a->desktop.args;
    r.desktop_terminal = a->desktop.terminal;
    auto it = prev.find(r.id);
    if (it != prev.end()) {
      r.latest = it->second.latest;
      r.error = it->second.error;
      r.status = it->second.status;
    } else {
      r.status = a->last_status;
    }
    rows.push_back(std::move(r));
  }
  std::lock_guard<std::mutex> lk(sh.mu);
  sh.rows = std::move(rows);
}

void apply_check_results(Manager& m, Shared& sh) {
  auto results = m.check_all({});
  std::lock_guard<std::mutex> lk(sh.mu);
  for (auto& c : results)
    for (auto& r : sh.rows)
      if (r.id == c.id) {
        r.status = c.status;
        r.latest = c.latest;
        r.error = c.error;
        r.installed = c.installed;
      }
}

void refresh_self(Shared& sh) {
  const SelfInfo info = SelfInfo::detect();
  std::string desktop;
  const bool integrated = self_is_integrated(&desktop);
  std::lock_guard<std::mutex> lk(sh.mu);
  sh.self_version = info.version;
  sh.self_path = info.appimage_path;
  sh.self_appimage = info.appimage;
  sh.self_integrated = integrated;
  sh.self_desktop = desktop;
}

ImU32 status_color(const std::string& s) {
  if (s == "up-to-date") return IM_COL32(90, 200, 120, 255);
  if (s == "update-available") return IM_COL32(240, 180, 60, 255);
  if (s == "error") return IM_COL32(240, 90, 90, 255);
  if (s == "pinned" || s == "no-source" || s == "unknown") return IM_COL32(150, 150, 150, 255);
  return IM_COL32(200, 200, 200, 255);
}
// ---------------------------------------------------------------- PNG writer
// Tiny dependency-free PNG encoder (zlib "stored" blocks). Used by
// --screenshot so the UI can be verified in headless CI without ImageMagick.
namespace {

uint32_t crc_table[256];
bool crc_ready = false;

void crc_init() {
  for (uint32_t n = 0; n < 256; ++n) {
    uint32_t c = n;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
    crc_table[n] = c;
  }
  crc_ready = true;
}

uint32_t crc32_of(const void* data, size_t len) {
  if (!crc_ready) crc_init();
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint32_t c = 0xffffffffu;
  for (size_t i = 0; i < len; ++i) c = crc_table[(c ^ p[i]) & 0xff] ^ (c >> 8);
  return c ^ 0xffffffffu;
}

void put_u32(std::string& out, uint32_t v) {
  out += char((v >> 24) & 0xff);
  out += char((v >> 16) & 0xff);
  out += char((v >> 8) & 0xff);
  out += char(v & 0xff);
}

void put_chunk(std::string& out, const char* type, const std::string& body) {
  put_u32(out, static_cast<uint32_t>(body.size()));
  const size_t start = out.size();
  out += type;
  out += body;
  const uint32_t crc = crc32_of(out.data() + start, out.size() - start);
  put_u32(out, crc);
}

std::string zlib_store(const std::string& raw) {
  std::string z;
  z += char(0x78);
  z += char(0x01);
  size_t off = 0;
  while (off < raw.size()) {
    const size_t n = std::min<size_t>(65535, raw.size() - off);
    z += char(off + n >= raw.size() ? 1 : 0);  // BFINAL, stored block
    z += char(n & 0xff);
    z += char((n >> 8) & 0xff);
    z += char(~n & 0xff);
    z += char((~n >> 8) & 0xff);
    z.append(raw, off, n);
    off += n;
  }
  uint32_t a = 1, b = 0;
  for (unsigned char c : raw) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  put_u32(z, (b << 16) | a);
  return z;
}

bool write_png_rgba(const char* path, int w, int h, const std::vector<unsigned char>& rgba) {
  std::string raw;
  raw.reserve(static_cast<size_t>(h) * (w * 4 + 1));
  for (int y = h - 1; y >= 0; --y) {  // OpenGL reads bottom-up
    raw += char(0);
    raw.append(reinterpret_cast<const char*>(rgba.data()) + static_cast<size_t>(y) * w * 4,
               static_cast<size_t>(w) * 4);
  }
  std::string png("\x89PNG\r\n\x1a\n", 8);
  std::string ihdr;
  put_u32(ihdr, static_cast<uint32_t>(w));
  put_u32(ihdr, static_cast<uint32_t>(h));
  ihdr += char(8);
  ihdr += char(6);
  ihdr += char(0);
  ihdr += char(0);
  ihdr += char(0);
  put_chunk(png, "IHDR", ihdr);
  put_chunk(png, "IDAT", zlib_store(raw));
  put_chunk(png, "IEND", std::string());
  std::FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  const bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
  std::fclose(f);
  return ok;
}

}  // namespace


// --------------------------------------------------------------- theming
struct ThemeState {
  std::string preset_slug = "bento";
  std::string custom_file;
  float scale = 1.0f;
  std::string label = "bento";
#ifdef AIMGR_HAS_IMGUI_THEMES
  ImGuiThemes::Id preset = ImGuiThemes::Id::Bento;
  ImGuiThemes::Theme theme = ImGuiThemes::Make(ImGuiThemes::Id::Bento);
#endif
};

#ifdef AIMGR_HAS_IMGUI_THEMES
ImGuiThemes::Id preset_from_slug(const std::string& slug, bool* ok) {
  for (int i = 0; i < ImGuiThemes::ThemeCount; ++i) {
    const auto id = static_cast<ImGuiThemes::Id>(i);
    if (slug == ImGuiThemes::Slug(id)) {
      if (ok) *ok = true;
      return id;
    }
  }
  if (ok) *ok = false;
  return ImGuiThemes::Id::Bento;
}

void theme_use_preset(ThemeState& st, ImGuiThemes::Id id) {
  st.preset = id;
  st.theme = ImGuiThemes::Make(id);
  st.preset_slug = ImGuiThemes::Slug(id);
  st.label = ImGuiThemes::Name(id);
  st.custom_file.clear();
}

bool theme_use_file(ThemeState& st, const std::string& path, std::string* err) {
  ImGuiThemes::Theme t;
  std::string e;
  if (!ImGuiThemes::Load(expand_tilde(path).c_str(), t, e)) {
    if (err) *err = e;
    return false;
  }
  st.theme = t;
  st.custom_file = path;
  st.preset = t.id;
  st.preset_slug = ImGuiThemes::Slug(t.id);
  st.label = fs::path(path).filename().string();
  return true;
}
#endif

/// Applies the theme to the ImGui style. Must run before ImGui::NewFrame().
void theme_apply(const ThemeState& st) {
#ifdef AIMGR_HAS_IMGUI_THEMES
  ImGuiThemes::Apply(st.theme, st.scale);
#endif
  ImGui::GetStyle().FontScaleMain = st.scale;
}

/// Semantic button: 0 primary, 1 secondary, 2 danger.
bool sbutton(const ThemeState& st, int role, const char* label, ImVec2 size = ImVec2(0, 0)) {
#ifdef AIMGR_HAS_IMGUI_THEMES
  return ImGuiThemes::Button(st.theme, static_cast<ImGuiThemes::ButtonRole>(role), label, size,
                             st.scale);
#else
  (void)st;
  (void)role;
  return ImGui::Button(label, size);
#endif
}

struct AddForm {
  int mode = 0;  // 0 single file, 1 scan folder
  char id[64] = {0};
  char name[64] = {0};
  char path[1024] = {0};
  char scan_dir[1024] = {0};
  bool recursive = true;
  int source = 0;  // 0 github, 1 url
  char repo[256] = {0};
  char asset_regex[256] = {0};
  char tag_prefix[32] = {0};
  bool prerelease = false;
  char url[1024] = {0};
  char version_url[1024] = {0};
  char version_regex[256] = {0};
  char sha256[80] = {0};
  char sha256_url[1024] = {0};
};

struct SourceForm {
  std::string id;
  int type = 0;  // 0 none, 1 github, 2 url
  char repo[256] = {0};
  char asset_regex[256] = {0};
  char tag_prefix[32] = {0};
  bool prerelease = false;
  char url[1024] = {0};
  char version_url[1024] = {0};
  char version_regex[256] = {0};
  char sha256[80] = {0};
  char sha256_url[1024] = {0};
};

struct DownloadForm {
  int source = 0;  // 0 github, 1 url
  char repo[256] = {0};
  char asset_regex[256] = {0};
  char tag_prefix[32] = {0};
  bool prerelease = false;
  char url[1024] = {0};
  char version_url[1024] = {0};
  char version_regex[256] = {0};
  char sha256[80] = {0};
  char sha256_url[1024] = {0};
  char directory[1024] = {0};
  char id[64] = {0};
  char name[64] = {0};
  char filename[256] = {0};
  bool replace = false;
};

struct DesktopForm {
  std::string id;
  char name[128] = {0};
  char comment[256] = {0};
  char categories[128] = {0};
  char icon[1024] = {0};
  char args[256] = {0};
  bool terminal = false;
  bool integrated = false;
  std::string path;
};

void copy_str(char* dst, size_t cap, const std::string& s) {
  std::snprintf(dst, cap, "%s", s.c_str());
}

bool load_fonts(ImGuiIO& io, float size_px) {
  const char* cjk_candidates[] = {
      "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
      "/usr/share/fonts/noto-cjk/NotoSansCJK-Light.ttc",
      "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
      "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
      "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
      "/usr/share/fonts/truetype/arphic/uming.ttc",
  };
  for (const char* p : cjk_candidates) {
    if (!path_is_regular(p)) continue;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    if (io.Fonts->AddFontFromFileTTF(p, size_px, &cfg, io.Fonts->GetGlyphRangesChineseFull())) {
      g_cjk = true;
      return true;
    }
  }
  io.Fonts->AddFontDefault();
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  fs::path config_override;
  int autoquit_ms = 0;
  std::string screenshot_path;
  std::string theme_arg;
  int screenshot_frame = 25;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--config" && i + 1 < argc) config_override = fs::path(argv[++i]);
    else if (a == "--autoquit-ms" && i + 1 < argc) autoquit_ms = std::atoi(argv[++i]);
    else if (a == "--screenshot" && i + 1 < argc) screenshot_path = argv[++i];
    else if (a == "--theme" && i + 1 < argc) theme_arg = argv[++i];
    else if (a == "--help" || a == "-h") {
      std::printf(
          "aimgr-gui [--config PATH] [--theme SLUG|file.ini] [--autoquit-ms N] "
          "[--screenshot out.png]\n"
          "  built-in themes: default, cyberpunk, minimal, wireframe, brutalist, bento\n");
      return 0;
    }
  }

  Paths paths = Paths::resolve();
  if (!config_override.empty()) paths.config = expand_tilde(config_override);

  Shared sh;
  Logger logger;
  logger.sink = [&sh](LogLevel l, const std::string& m) { push_log(sh, l, m); };
  Manager manager(paths, logger);
  manager.on_progress = [&sh](const std::string&, uint64_t done, uint64_t total) {
    std::lock_guard<std::mutex> lk(sh.mu);
    sh.done = done;
    sh.total = total;
  };

  std::string err;
  if (!manager.load(&err)) {
    std::fprintf(stderr, "cannot load config: %s\n", err.c_str());
    return 1;
  }
  manager.recover();

  ThemeState theme;
  theme.scale = manager.config().ui.font_scale;
  theme.custom_file = manager.config().ui.custom_theme_file;
#ifdef AIMGR_HAS_IMGUI_THEMES
  bool slug_ok = false;
  theme.preset = preset_from_slug(manager.config().ui.theme, &slug_ok);
  if (!slug_ok) theme.preset = ImGuiThemes::Id::Bento;
  theme.theme = ImGuiThemes::Make(theme.preset);
  theme.preset_slug = ImGuiThemes::Slug(theme.preset);
  theme.label = ImGuiThemes::Name(theme.preset);
  if (!theme.custom_file.empty()) {
    std::string terr;
    if (!theme_use_file(theme, theme.custom_file, &terr))
      std::fprintf(stderr, "custom theme ignored: %s\n", terr.c_str());
  }
  if (!theme_arg.empty()) {
    const std::string t = theme_arg;
    if (ends_with(to_lower(t), ".ini")) {
      std::string terr;
      if (!theme_use_file(theme, t, &terr)) std::fprintf(stderr, "theme: %s\n", terr.c_str());
    } else {
      theme_use_preset(theme, preset_from_slug(t, &slug_ok));
    }
  }
#endif
  std::string theme_error;

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

  SDL_Window* window = SDL_CreateWindow("aimgr — AppImage Manager", SDL_WINDOWPOS_CENTERED,
                                        SDL_WINDOWPOS_CENTERED, 1240, 760,
                                        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
  if (!window) {
    std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }
  SDL_GLContext gl = SDL_GL_CreateContext(window);
  if (!gl) {
    std::fprintf(stderr, "cannot create OpenGL 3 context: %s\n", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  SDL_GL_MakeCurrent(window, gl);
  SDL_GL_SetSwapInterval(1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.IniFilename = nullptr;  // no ~/.config/aimgr/imgui.ini side effects
  load_fonts(io, 17.0f);
  ImGui::StyleColorsDark();
  theme_apply(theme);

  ImGui_ImplSDL2_InitForOpenGL(window, gl);
  ImGui_ImplOpenGL3_Init("#version 130");

  Worker worker(sh);
  worker.start();

  const std::string config_dir =
      fs::path(paths.config).parent_path().empty() ? std::string(".")
                                                   : fs::path(paths.config).parent_path().string();
  const std::string theme_dir = config_dir + "/themes";

  AddForm form;
  copy_str(form.path, sizeof(form.path), default_install_dir().string());
  copy_str(form.scan_dir, sizeof(form.scan_dir), default_install_dir().string());
  char scan_dir[1024] = {0};
  copy_str(scan_dir, sizeof(scan_dir), default_install_dir().string());
  SourceForm src_form;
  DownloadForm dl_form;
  copy_str(dl_form.directory, sizeof(dl_form.directory), default_install_dir().string());
  DesktopForm desk_form;
  std::string force_update_id;
  char theme_io_path[1024] = {0};
  copy_str(theme_io_path, sizeof(theme_io_path), theme_dir + "/custom.ini");
  bool show_launcher_banner = false;
  {
    const SelfInfo si = SelfInfo::detect();
    std::string dpath;
    if (si.appimage && !self_is_integrated(&dpath) && !manager.config().ui.launcher_prompt_done)
      show_launcher_banner = true;
  }
  std::string pending_uninstall;
  bool show_add = false, show_scan = false, show_backups = false, show_about = false;
  bool show_uninstall_confirm = false, show_source = false, show_theme_io = false;
  bool show_restart = false, show_download = false, show_desktop = false;
  bool show_force_update = false;
  int theme_io_mode = 0;  // 0 load, 1 save
  bool auto_scroll = true;

  worker.submit(T("加载应用列表", "Loading apps"), [&manager, &sh] {
    refresh_rows(manager, sh);
    refresh_self(sh);
  });
  // Startup is deliberately passive: loading the list must not perform network
  // checks. Update checking is an explicit user action (toolbar/menu/row button).

  const Uint32 start_ticks = SDL_GetTicks();
  bool running = true;
  long frame_index = 0;

  while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      ImGui_ImplSDL2_ProcessEvent(&e);
      if (e.type == SDL_QUIT) running = false;
      if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE &&
          e.window.windowID == SDL_GetWindowID(window))
        running = false;
    }
    if (autoquit_ms > 0 && SDL_GetTicks() - start_ticks > static_cast<Uint32>(autoquit_ms))
      running = false;

    theme_apply(theme);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    {
      std::string prompt;
      {
        std::lock_guard<std::mutex> lk(sh.mu);
        prompt.swap(sh.prompt_source_for);
      }
      if (!prompt.empty()) {
        for (const auto& r : sh.rows) {
          if (r.id != prompt) continue;
          src_form.id = r.id;
          src_form.type = r.source_type;
          copy_str(src_form.repo, sizeof(src_form.repo), r.repo);
          copy_str(src_form.asset_regex, sizeof(src_form.asset_regex), r.asset_regex);
          copy_str(src_form.tag_prefix, sizeof(src_form.tag_prefix), r.tag_prefix);
          src_form.prerelease = r.prerelease;
          copy_str(src_form.url, sizeof(src_form.url), r.url);
          copy_str(src_form.version_url, sizeof(src_form.version_url), r.version_url);
          copy_str(src_form.version_regex, sizeof(src_form.version_regex), r.version_regex);
          copy_str(src_form.sha256, sizeof(src_form.sha256), r.sha256);
          copy_str(src_form.sha256_url, sizeof(src_form.sha256_url), r.sha256_url);
          show_source = true;
          break;
        }
      }
    }

    bool busy;
    uint64_t done, total;
    std::string activity, restart_target;
    {
      std::lock_guard<std::mutex> lk(sh.mu);
      busy = sh.busy;
      activity = sh.activity;
      done = sh.done;
      total = sh.total;
      restart_target = sh.restart_target;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_MenuBar;
    ImGui::Begin("main", nullptr, flags);

    if (ImGui::BeginMenuBar()) {
      if (ImGui::BeginMenu(T("文件", "File"))) {
        if (ImGui::MenuItem(T("添加应用…", "Add app…"), nullptr, false, !busy)) show_add = true;
        if (ImGui::MenuItem(T("扫描目录…", "Scan folder…"), nullptr, false, !busy)) show_scan = true;
        if (ImGui::MenuItem(T("下载 AppImage…", "Download AppImage…"), nullptr, false, !busy))
          show_download = true;
        ImGui::Separator();
        if (ImGui::MenuItem(T("重新加载", "Reload"), nullptr, false, !busy)) {
          worker.submit(T("加载应用列表", "Loading apps"), [&manager, &sh] {
            refresh_rows(manager, sh);
            refresh_self(sh);
          });
        }
        ImGui::Separator();
        if (ImGui::MenuItem(T("退出", "Quit"))) running = false;
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu(T("操作", "Actions"))) {
        if (ImGui::MenuItem(T("检查全部更新", "Check all"), nullptr, false, !busy))
          worker.submit(T("检查更新", "Checking for updates"),
                        [&manager, &sh] { apply_check_results(manager, sh); });
        if (ImGui::MenuItem(T("更新全部", "Update all"), nullptr, false, !busy)) {
          worker.submit(T("更新全部", "Updating all"), [&manager, &sh] {
            auto results = manager.update_all({});
            for (const auto& u : results)
              push_log(sh, u.ok ? LogLevel::Info : LogLevel::Error,
                       u.id + ": " +
                           (u.updated ? u.from + " -> " + u.to
                                      : (u.ok ? std::string("up to date") : u.error)));
            refresh_rows(manager, sh);
          });
        }
        ImGui::Separator();
        if (ImGui::MenuItem(T("强制下载更新…", "Force download update…"), nullptr, false,
                            !busy)) {
          force_update_id.clear();
          show_force_update = true;
        }
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu(T("主题", "Theme"))) {
#ifdef AIMGR_HAS_IMGUI_THEMES
        for (int i = 0; i < ImGuiThemes::ThemeCount; ++i) {
          const auto id = static_cast<ImGuiThemes::Id>(i);
          const bool selected = theme.custom_file.empty() && theme.preset == id;
          if (ImGui::MenuItem(ImGuiThemes::Name(id), nullptr, selected)) {
            theme_use_preset(theme, id);
            const std::string slug = theme.preset_slug, cfile = theme.custom_file;
            const float scale = theme.scale;
            worker.submit(T("保存主题", "Saving theme"), [&manager, &sh, slug, cfile, scale] {
              UiSettings ui = manager.config().ui;
              ui.theme = slug;
              ui.custom_theme_file = cfile;
              ui.font_scale = scale;
              std::string e;
              if (!manager.set_ui(ui, &e)) push_log(sh, LogLevel::Error, "save UI settings: " + e);
            });
          }
        }
        ImGui::Separator();
        if (ImGui::MenuItem(T("载入 .ini 主题…", "Load .ini theme…"))) {
          theme_io_mode = 0;
          show_theme_io = true;
        }
        if (ImGui::MenuItem(T("保存当前主题为 .ini…", "Save theme as .ini…"))) {
          theme_io_mode = 1;
          show_theme_io = true;
        }
        ImGui::Separator();
        ImGui::TextDisabled("%s: %s", T("当前", "Current"), theme.label.c_str());
        ImGui::SetNextItemWidth(180);
        if (ImGui::SliderFloat(T("界面缩放", "UI scale"), &theme.scale, 0.8f, 1.6f, "%.2f"))
          theme_apply(theme);
#else
        ImGui::TextDisabled("theme module unavailable");
#endif
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu(T("自身", "aimgr"))) {
        std::string ver, path, desktop;
        bool appimage = false, integrated = false;
        {
          std::lock_guard<std::mutex> lk(sh.mu);
          ver = sh.self_version;
          path = sh.self_path;
          appimage = sh.self_appimage;
          integrated = sh.self_integrated;
          desktop = sh.self_desktop;
        }
        ImGui::TextDisabled("aimgr %s (%s)", ver.c_str(), appimage ? "AppImage" : "binary");
        ImGui::Separator();
        if (ImGui::MenuItem(T("检查自身更新", "Check for self-update"), nullptr, false, !busy)) {
          worker.submit(T("检查自身更新", "Checking for self-update"), [&manager, &sh] {
            Http http;
            SelfUpdateOptions opts;
            opts.dry_run = true;
            SelfUpdateResult r = self_update(opts, http, Logger{[&sh](LogLevel l, const std::string& m) { push_log(sh, l, m); }}, manager.paths());
            push_log(sh, r.ok ? LogLevel::Info : LogLevel::Error,
                     r.ok ? ("aimgr " + r.from + " -> " + (r.to.empty() ? r.from : r.to)) : r.error);
          });
        }
        if (ImGui::MenuItem(T("立即自更新", "Update aimgr now"), nullptr, false, !busy)) {
          worker.submit(T("自更新", "Self-updating"), [&manager, &sh] {
            Http http;
            Logger l;
            l.sink = [&sh](LogLevel lv, const std::string& m) { push_log(sh, lv, m); };
            SelfUpdateResult r = self_update(SelfUpdateOptions{}, http, l, manager.paths());
            if (!r.ok) push_log(sh, LogLevel::Error, "self-update failed: " + r.error);
            if (r.ok && r.updated) {
              std::lock_guard<std::mutex> lk(sh.mu);
              sh.restart_target = SelfInfo::detect().appimage_path;
            }
            refresh_self(sh);
          });
        }
        if (ImGui::MenuItem(integrated ? T("移除启动器项", "Remove launcher entry")
                                       : T("添加到应用启动器", "Add to launcher"),
                           nullptr, false, !busy)) {
          worker.submit(T("更新启动器项", "Updating launcher entry"), [&sh, integrated] {
            std::string e;
            bool ok = integrated ? self_deintegrate(&e) : self_integrate(&e);
            push_log(sh, ok ? LogLevel::Info : LogLevel::Error,
                     ok ? (integrated ? "launcher entry removed" : "launcher entry created") : e);
            refresh_self(sh);
          });
        }
        ImGui::Separator();
        ImGui::TextDisabled("%s", path.c_str());
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu(T("帮助", "Help"))) {
        if (ImGui::MenuItem(T("关于", "About"))) show_about = true;
        ImGui::EndMenu();
      }
      ImGui::EndMenuBar();
    }

    // ------------------------------------------------------------- toolbar
    ImGui::BeginDisabled(busy);
    if (sbutton(theme, 0, T("检查全部更新", "Check all")))
      worker.submit(T("检查更新", "Checking for updates"),
                    [&manager, &sh] { apply_check_results(manager, sh); });
    ImGui::SameLine();
    if (sbutton(theme, 1, T("更新全部", "Update all"))) {
      worker.submit(T("更新全部", "Updating all"), [&manager, &sh] {
        auto results = manager.update_all({});
        for (const auto& u : results)
          push_log(sh, u.ok ? LogLevel::Info : LogLevel::Error,
                   u.id + ": " + (u.updated ? u.from + " -> " + u.to
                                            : (u.ok ? std::string("up to date") : u.error)));
        refresh_rows(manager, sh);
      });
    }
    ImGui::SameLine();
    if (sbutton(theme, 1, T("扫描目录…", "Scan folder…"))) show_scan = true;
    ImGui::SameLine();
    if (sbutton(theme, 1, T("添加应用…", "Add app…"))) show_add = true;
    ImGui::SameLine();
    if (sbutton(theme, 1, T("下载 AppImage…", "Download AppImage…"))) show_download = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    if (ImGui::BeginCombo("##theme", theme.label.c_str())) {
#ifdef AIMGR_HAS_IMGUI_THEMES
      for (int i = 0; i < ImGuiThemes::ThemeCount; ++i) {
        const auto id = static_cast<ImGuiThemes::Id>(i);
        const bool selected = theme.custom_file.empty() && theme.preset == id;
        if (ImGui::Selectable(ImGuiThemes::Name(id), selected)) {
          theme_use_preset(theme, id);
          const std::string slug = theme.preset_slug, cfile = theme.custom_file;
          const float scale = theme.scale;
          worker.submit(T("保存主题", "Saving theme"), [&manager, slug, cfile, scale] {
            UiSettings ui = manager.config().ui;
            ui.theme = slug;
            ui.custom_theme_file = cfile;
            ui.font_scale = scale;
            manager.set_ui(ui, nullptr);
          });
        }
      }
#endif
      ImGui::EndCombo();
    }
    ImGui::EndDisabled();

    if (busy) {
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "%s…", activity.c_str());
      if (total > 0) {
        ImGui::SameLine();
        ImGui::ProgressBar(static_cast<float>(done) / static_cast<float>(total), ImVec2(220, 0),
                           (format_size(done) + " / " + format_size(total)).c_str());
      } else {
        ImGui::SameLine();
        static float spinner = 0.0f;
        spinner += ImGui::GetIO().DeltaTime * 3.0f;
        ImGui::ProgressBar(-1.0f * static_cast<float>(fmod(spinner, 1.0f)), ImVec2(120, 0), "");
      }
    }
    if (!theme_error.empty()) {
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.5f, 1.0f), "%s", theme_error.c_str());
    }

    if (show_launcher_banner) {
      ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.22f, 0.30f, 1.0f));
      ImGui::BeginChild("launcher-banner", ImVec2(0, 56), ImGuiChildFlags_Borders);
      ImGui::TextUnformatted(
          T("检测到正在以 AppImage 方式运行。是否把 aimgr 添加到应用启动器（.desktop）？",
            "Running as an AppImage. Add aimgr to your application launcher?"));
      if (sbutton(theme, 0, T("添加到启动器", "Add to launcher"), ImVec2(150, 0))) {
        worker.submit(T("创建启动器项", "Creating launcher entry"), [&manager, &sh] {
          std::string e;
          if (self_integrate(&e))
            push_log(sh, LogLevel::Info, "launcher entry created");
          else
            push_log(sh, LogLevel::Error, e);
          UiSettings ui = manager.config().ui;
          ui.launcher_prompt_done = true;
          manager.set_ui(ui, nullptr);
          refresh_self(sh);
        });
        show_launcher_banner = false;
      }
      ImGui::SameLine();
      if (sbutton(theme, 1, T("以后再说", "Not now"), ImVec2(120, 0))) {
        worker.submit(T("保存偏好", "Saving preference"), [&manager] {
          UiSettings ui = manager.config().ui;
          ui.launcher_prompt_done = true;
          manager.set_ui(ui, nullptr);
        });
        show_launcher_banner = false;
      }
      ImGui::EndChild();
      ImGui::PopStyleColor();
    }

    ImGui::Separator();

    // --------------------------------------------------------------- table
    std::vector<Row> rows;
    {
      std::lock_guard<std::mutex> lk(sh.mu);
      rows = sh.rows;
    }
    const float logs_height = 180.0f;
    const float table_height = ImGui::GetContentRegionAvail().y - logs_height - 8.0f;
    // All row actions share one explicit size. Previously SmallButton() and
    // theme-drawn buttons were mixed, so heights and widths differed per row.
    const char* const action_labels[] = {
        T("检查", "Check"),       T("更新", "Update"),     T("强制更新", "Force"),
        T("源", "Source"),        T("备份", "Backups"),    T("桌面", "Desktop"),
        T("卸载", "Uninstall")};
    const ImGuiStyle& row_style = ImGui::GetStyle();
    float action_text_w = 0.0f;
    for (const char* label : action_labels)
      action_text_w = std::max(action_text_w, ImGui::CalcTextSize(label).x);
    const float action_btn_w = std::max(58.0f, action_text_w + row_style.FramePadding.x * 2 + 4.0f);
    const float action_btn_h = ImGui::GetFrameHeight();
    const float actions_width =
        action_btn_w * 6.0f + row_style.ItemSpacing.x * 5.0f + row_style.CellPadding.x * 2.0f + 4.0f;
    const ImGuiTableFlags table_flags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
        ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("apps", 7, table_flags,
                          ImVec2(0, table_height > 120 ? table_height : 120))) {
      // Keep every column at an explicit width and let the table overflow
      // horizontally instead of squeezing/cropping cells in a narrow window.
      // Drag a header separator to change a column width; TableSetupScrollFreeze
      // keeps the header (and the ID column) visible while scrolling.
      ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed |
                                         ImGuiTableColumnFlags_NoHide,
                              100.0f);
      ImGui::TableSetupColumn(T("名称", "Name"),
                              ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHide,
                              115.0f);
      ImGui::TableSetupColumn(T("已安装", "Installed"), ImGuiTableColumnFlags_WidthFixed,
                              90.0f);
      ImGui::TableSetupColumn(T("最新", "Latest"), ImGuiTableColumnFlags_WidthFixed, 90.0f);
      ImGui::TableSetupColumn(T("更新源", "Source"), ImGuiTableColumnFlags_WidthFixed,
                              160.0f);
      ImGui::TableSetupColumn(T("状态", "Status"), ImGuiTableColumnFlags_WidthFixed, 180.0f);
      ImGui::TableSetupColumn(T("操作", "Actions"),
                              ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHide,
                              actions_width);
      ImGui::TableSetupScrollFreeze(1, 1);
      ImGui::TableHeadersRow();

      for (const auto& r : rows) {
        ImGui::TableNextRow();
        ImGui::PushID(r.id.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(r.id.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(r.name.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.path.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(r.installed.empty() ? "?" : r.installed.c_str());
        ImGui::TableNextColumn();
        const bool newer = !r.latest.empty() && is_newer(r.latest, r.installed);
        ImGui::TextColored(newer ? ImVec4(1.0f, 0.75f, 0.25f, 1.0f)
                                 : ImVec4(0.75f, 0.75f, 0.75f, 1.0f),
                           "%s", r.latest.empty() ? "?" : r.latest.c_str());
        ImGui::TableNextColumn();
        if (r.source_type == 0)
          ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.3f, 1.0f), "%s", T("未配置", "not set"));
        else if (r.source_type == 1)
          ImGui::TextUnformatted(("gh:" + r.repo).c_str());
        else
          ImGui::TextUnformatted(T("URL", "URL"));
        ImGui::TableNextColumn();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(status_color(r.status)), "%s",
                           r.status.empty() ? "-" : r.status.c_str());
        if (!r.error.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.error.c_str());

        ImGui::TableNextColumn();
        ImGui::BeginDisabled(busy);
        const ImVec2 action_size(action_btn_w, action_btn_h);
        if (sbutton(theme, 1, T("检查", "Check"), action_size)) {
          const std::string id = r.id;
          worker.submit(T("检查 ", "Checking ") + id, [&manager, &sh, id] {
            App* a = manager.find(id);
            if (!a) return;
            CheckResult c = manager.check(*a);
            std::lock_guard<std::mutex> lk(sh.mu);
            for (auto& row : sh.rows)
              if (row.id == id) {
                row.status = c.status;
                row.latest = c.latest;
                row.error = c.error;
                row.installed = c.installed;
              }
          });
        }
        ImGui::SameLine();
        // A direct-URL source often has no comparable version. In that case
        // "update" means "download the current source and replace", which is
        // destructive enough to require an explicit confirmation.
        const bool force_needed =
            (r.source_type == 2 && (r.latest.empty() || r.status == "unknown"));
        if (sbutton(theme, 0,
                    force_needed ? T("强制更新", "Force") : T("更新", "Update"),
                    action_size)) {
          const std::string id = r.id;
          if (force_needed) {
            force_update_id = id;
            show_force_update = true;
          } else {
            worker.submit(T("更新 ", "Updating ") + id, [&manager, &sh, id] {
              App* a = manager.find(id);
              if (!a) return;
              UpdateResult u = manager.update(*a);
              push_log(sh, u.ok ? LogLevel::Info : LogLevel::Error,
                       id + ": " +
                           (u.updated ? u.from + " -> " + u.to
                                      : (u.ok ? std::string("already up to date") : u.error)));
              refresh_rows(manager, sh);
            });
          }
        }
        ImGui::SameLine();
        if (sbutton(theme, 1, T("源", "Source"), action_size)) {
          src_form.id = r.id;
          src_form.type = r.source_type;
          copy_str(src_form.repo, sizeof(src_form.repo), r.repo);
          copy_str(src_form.asset_regex, sizeof(src_form.asset_regex), r.asset_regex);
          copy_str(src_form.tag_prefix, sizeof(src_form.tag_prefix), r.tag_prefix);
          src_form.prerelease = r.prerelease;
          copy_str(src_form.url, sizeof(src_form.url), r.url);
          copy_str(src_form.version_url, sizeof(src_form.version_url), r.version_url);
          copy_str(src_form.version_regex, sizeof(src_form.version_regex), r.version_regex);
          copy_str(src_form.sha256, sizeof(src_form.sha256), r.sha256);
          copy_str(src_form.sha256_url, sizeof(src_form.sha256_url), r.sha256_url);
          show_source = true;
        }
        ImGui::SameLine();
        if (sbutton(theme, 1, T("备份", "Backups"), action_size)) {
          show_backups = true;
          const std::string id = r.id;
          worker.submit(T("读取备份 ", "Loading backups ") + id, [&manager, &sh, id] {
            App* a = manager.find(id);
            if (!a) return;
            auto list = manager.backups(*a);
            std::lock_guard<std::mutex> lk(sh.mu);
            sh.backups = std::move(list);
            sh.backups_for = id;
          });
        }
        ImGui::SameLine();
        if (sbutton(theme, 1, T("桌面", "Desktop"), action_size)) {
          desk_form.id = r.id;
          copy_str(desk_form.name, sizeof(desk_form.name), r.desktop_name);
          copy_str(desk_form.comment, sizeof(desk_form.comment), r.desktop_comment);
          copy_str(desk_form.categories, sizeof(desk_form.categories), r.desktop_categories);
          copy_str(desk_form.icon, sizeof(desk_form.icon), r.desktop_icon);
          copy_str(desk_form.args, sizeof(desk_form.args), r.desktop_args);
          desk_form.terminal = r.desktop_terminal;
          desk_form.integrated = r.integrated;
          desk_form.path = r.desktop_path;
          show_desktop = true;
        }
        ImGui::SameLine();
        if (sbutton(theme, 2, T("卸载", "Uninstall"), action_size)) {
          pending_uninstall = r.id;
          show_uninstall_confirm = true;
        }
        ImGui::EndDisabled();
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    if (rows.empty())
      ImGui::TextWrapped("%s",
                         T("还没有登记任何 AppImage。点“扫描目录…”自动发现，或点“添加应用…”手动登记。",
                           "No AppImages registered yet. Use \"Scan folder…\" or \"Add app…\"."));

    // ---------------------------------------------------------------- logs
    ImGui::Separator();
    ImGui::Checkbox(T("自动滚动", "Auto-scroll"), &auto_scroll);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", T("日志", "Log"));
    ImGui::BeginChild("logs", ImVec2(0, 0), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    {
      std::lock_guard<std::mutex> lk(sh.mu);
      for (const auto& [lvl, msg] : sh.logs) {
        ImVec4 c(0.8f, 0.8f, 0.8f, 1.0f);
        if (lvl == LogLevel::Warn) c = ImVec4(0.95f, 0.75f, 0.3f, 1.0f);
        else if (lvl == LogLevel::Error) c = ImVec4(0.95f, 0.4f, 0.4f, 1.0f);
        else if (lvl == LogLevel::Debug) c = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
        ImGui::TextColored(c, "%s", msg.c_str());
      }
      if (auto_scroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::End();

    // ------------------------------------------------------------- modals
    if (show_add) {
      ImGui::OpenPopup(T("添加应用", "Add app"));
      show_add = false;
    }
    ImGui::SetNextWindowSize(ImVec2(680, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(T("添加应用", "Add app"), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::RadioButton(T("登记单个 AppImage", "Register one AppImage"), &form.mode, 0);
      ImGui::SameLine();
      ImGui::RadioButton(T("扫描文件夹", "Scan a folder"), &form.mode, 1);
      ImGui::Separator();
      if (form.mode == 0) {
        ImGui::InputText(T("ID", "ID"), form.id, sizeof(form.id));
        ImGui::InputText(T("名称", "Name"), form.name, sizeof(form.name));
        ImGui::InputText(T("AppImage 路径", "AppImage path"), form.path, sizeof(form.path));
        ImGui::Combo(T("更新源", "Source"), &form.source,
                     T("GitHub Release\0下载 URL\0", "GitHub release\0Download URL\0"));
        if (form.source == 0) {
          ImGui::InputText("owner/repo", form.repo, sizeof(form.repo));
          ImGui::InputText(T("资产正则 (可空)", "Asset regex (optional)"), form.asset_regex,
                           sizeof(form.asset_regex));
          ImGui::InputText(T("标签前缀 (如 v)", "Tag prefix (e.g. v)"), form.tag_prefix,
                           sizeof(form.tag_prefix));
          ImGui::Checkbox(T("允许预发布版本", "Allow pre-releases"), &form.prerelease);
        } else {
          ImGui::InputText(T("下载 URL (可用 ${version})", "Download URL (may use ${version})"),
                           form.url, sizeof(form.url));
          ImGui::InputText(T("版本发布页 (可空)", "Version page (optional)"), form.version_url,
                           sizeof(form.version_url));
          ImGui::InputText(T("版本正则 (可空)", "Version regex (optional)"), form.version_regex,
                           sizeof(form.version_regex));
          ImGui::InputText(T("SHA-256 (可空)", "SHA-256 (optional)"), form.sha256,
                           sizeof(form.sha256));
          ImGui::InputText(T("SHA-256 校验地址 (可空)", "SHA-256 URL (optional)"),
                           form.sha256_url, sizeof(form.sha256_url));
        }
      } else {
        ImGui::InputText(T("扫描目录", "Folder"), form.scan_dir, sizeof(form.scan_dir));
        ImGui::Checkbox(T("包含子文件夹", "Include subfolders"), &form.recursive);
        ImGui::TextDisabled("%s",
                            T("会登记该目录（及子目录）里所有未登记的 *.AppImage，"
                              "扫描完成后可逐个配置更新源。",
                              "Every unregistered *.AppImage in this folder (and below) is "
                              "registered; configure update sources afterwards."));
      }
      ImGui::Separator();
      if (form.mode == 0) {
        if (sbutton(theme, 0, T("添加", "Add"), ImVec2(120, 0))) {
          App app;
          app.id = form.id;
          app.name = form.name;
          app.path = form.path;
          if (form.source == 0) {
            app.source = SourceType::Github;
            app.github.repo = form.repo;
            app.github.asset_regex = form.asset_regex;
            app.github.tag_prefix = form.tag_prefix;
            app.github.allow_prerelease = form.prerelease;
          } else {
            app.source = SourceType::Url;
            app.url.url = form.url;
            app.url.version_url = form.version_url;
            app.url.version_regex = form.version_regex;
            app.url.sha256 = form.sha256;
            app.url.sha256_url = form.sha256_url;
          }
          worker.submit(T("添加应用", "Adding app"), [&manager, &sh, app] {
            std::string e;
            if (manager.add_app(app, false, &e))
              push_log(sh, LogLevel::Info, "added " + app.id);
            else
              push_log(sh, LogLevel::Error, "add failed: " + e);
            refresh_rows(manager, sh);
          });
          ImGui::CloseCurrentPopup();
        }
      } else {
        if (sbutton(theme, 0, T("扫描并登记", "Scan & add"), ImVec2(140, 0))) {
          const std::string dir = form.scan_dir;
          const bool recursive = form.recursive;
          worker.submit(T("扫描文件夹", "Scanning folder"), [&manager, &sh, dir, recursive] {
            std::string e;
            auto added = manager.scan_folder(dir, recursive, &e);
            if (!e.empty()) push_log(sh, LogLevel::Error, e);
            push_log(sh, LogLevel::Info,
                     "scan: " + std::to_string(added.size()) + " new AppImage(s) registered");
            refresh_rows(manager, sh);
            if (!added.empty()) {
              std::lock_guard<std::mutex> lk(sh.mu);
              sh.prompt_source_for = added.front().id;
            }
          });
          ImGui::CloseCurrentPopup();
        }
      }
      ImGui::SameLine();
      if (sbutton(theme, 1, T("取消", "Cancel"), ImVec2(120, 0))) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }

    // ---- update source dialog (also used right after a scan) -------------
    if (show_source) {
      ImGui::OpenPopup(T("配置更新源", "Update source"));
      show_source = false;
    }
    ImGui::SetNextWindowSize(ImVec2(680, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(T("配置更新源", "Update source"), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::Text("%s: %s", T("应用", "App"), src_form.id.c_str());
      ImGui::Combo(T("类型", "Type"), &src_form.type,
                   T("不跟踪\0GitHub Release\0下载 URL\0", "Not tracked\0GitHub release\0Download URL\0"));
      if (src_form.type == 1) {
        ImGui::InputText("owner/repo", src_form.repo, sizeof(src_form.repo));
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("%s", T("支持粘贴完整链接：https://github.com/owner/repo/releases/latest",
                                    "A full URL also works: https://github.com/owner/repo/releases/latest"));
        ImGui::InputText(T("资产正则 (可空)", "Asset regex (optional)"), src_form.asset_regex,
                         sizeof(src_form.asset_regex));
        ImGui::InputText(T("标签前缀 (如 v)", "Tag prefix (e.g. v)"), src_form.tag_prefix,
                         sizeof(src_form.tag_prefix));
        ImGui::Checkbox(T("允许预发布版本", "Allow pre-releases"), &src_form.prerelease);
      } else if (src_form.type == 2) {
        ImGui::InputText(T("下载 URL (可用 ${version})", "Download URL (may use ${version})"),
                         src_form.url, sizeof(src_form.url));
        ImGui::InputText(T("版本发布页 (可空)", "Version page (optional)"), src_form.version_url,
                         sizeof(src_form.version_url));
        ImGui::InputText(T("版本正则 (可空)", "Version regex (optional)"), src_form.version_regex,
                         sizeof(src_form.version_regex));
        ImGui::InputText(T("SHA-256 (可空)", "SHA-256 (optional)"), src_form.sha256,
                         sizeof(src_form.sha256));
        ImGui::InputText(T("SHA-256 校验地址 (可空)", "SHA-256 URL (optional)"),
                         src_form.sha256_url, sizeof(src_form.sha256_url));
      }
      ImGui::Separator();
      if (sbutton(theme, 0, T("保存并检查", "Save & check"), ImVec2(150, 0))) {
        SourceForm f = src_form;
        worker.submit(T("保存更新源", "Saving source"), [&manager, &sh, f] {
          App* a = manager.find(f.id);
          if (!a) return;
          SourceGithub gh;
          SourceUrl su;
          SourceType type = SourceType::None;
          if (f.type == 1) {
            type = SourceType::Github;
            gh.repo = f.repo;
            gh.asset_regex = f.asset_regex;
            gh.tag_prefix = f.tag_prefix;
            gh.allow_prerelease = f.prerelease;
          } else if (f.type == 2) {
            type = SourceType::Url;
            su.url = f.url;
            su.version_url = f.version_url;
            su.version_regex = f.version_regex;
            su.sha256 = f.sha256;
            su.sha256_url = f.sha256_url;
          }
          std::string e;
          if (!manager.set_source(*a, type, gh, su, &e)) {
            push_log(sh, LogLevel::Error, f.id + ": " + e);
            return;
          }
          refresh_rows(manager, sh);
          if (type != SourceType::None) {
            CheckResult c = manager.check(*a);
            std::lock_guard<std::mutex> lk(sh.mu);
            for (auto& row : sh.rows)
              if (row.id == f.id) {
                row.status = c.status;
                row.latest = c.latest;
                row.error = c.error;
                row.source_type = source_type_of(*a);
              }
          }
        });
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (sbutton(theme, 1, T("取消", "Cancel"), ImVec2(120, 0))) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }

    if (show_scan) {
      ImGui::OpenPopup(T("扫描目录", "Scan folder"));
      show_scan = false;
    }
    ImGui::SetNextWindowSize(ImVec2(640, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(T("扫描目录", "Scan folder"), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::InputText(T("目录", "Directory"), scan_dir, sizeof(scan_dir));
      static bool scan_recursive = true;
      ImGui::Checkbox(T("包含子文件夹", "Include subfolders"), &scan_recursive);
      if (sbutton(theme, 0, T("开始扫描", "Scan"), ImVec2(120, 0))) {
        const std::string dir = scan_dir;
        const bool recursive = scan_recursive;
        worker.submit(T("扫描目录", "Scanning folder"), [&manager, &sh, dir, recursive] {
          std::string e;
          auto added = manager.scan_folder(dir, recursive, &e);
          if (!e.empty()) push_log(sh, LogLevel::Error, e);
          push_log(sh, LogLevel::Info,
                   "scan: " + std::to_string(added.size()) + " new AppImage(s) registered");
          refresh_rows(manager, sh);
          if (!added.empty()) {
            std::lock_guard<std::mutex> lk(sh.mu);
            sh.prompt_source_for = added.front().id;
          }
        });
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (sbutton(theme, 1, T("取消", "Cancel"), ImVec2(120, 0))) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }

    // ---- force update confirmation ---------------------------------------
    if (show_force_update) {
      ImGui::OpenPopup(T("强制下载更新", "Force download update"));
      show_force_update = false;
    }
    ImGui::SetNextWindowSize(ImVec2(620, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(T("强制下载更新", "Force download update"), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      const Row* selected = nullptr;
      for (const auto& r : rows) {
        if (force_update_id.empty() || r.id == force_update_id) {
          selected = &r;
          break;
        }
      }
      if (!rows.empty()) {
        ImGui::SetNextItemWidth(360);
        if (ImGui::BeginCombo(T("应用", "App"), selected ? selected->id.c_str() : "")) {
          for (const auto& r : rows) {
            const bool on = selected && selected->id == r.id;
            if (ImGui::Selectable((r.id + "  (" + r.path + ")").c_str(), on)) {
              force_update_id = r.id;
              selected = &r;
            }
            if (on) ImGui::SetItemDefaultFocus();
          }
          ImGui::EndCombo();
        }
      }
      if (!selected) {
        ImGui::TextDisabled("%s", T("没有可更新的应用。", "No apps to update."));
      } else {
        ImGui::Text("%s: %s", T("目标文件", "Target"), selected->path.c_str());
        ImGui::Text("%s: %s", T("更新源", "Source"),
                    selected->source_type == 1
                        ? ("github:" + selected->repo).c_str()
                        : selected->source_type == 2 ? selected->url.c_str()
                                                     : T("未配置", "not set"));
        if (selected->source_type == 2 && selected->latest.empty()) {
          ImGui::Spacing();
          ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f), "%s",
                             T("这个下载链接没有提供可用于比较的版本号。",
                               "This download URL does not publish a comparable version."));
          ImGui::TextWrapped("%s",
                             T("aimgr 将从源重新下载当前构建并原子替换原文件。"
                               "旧版本会先备份，可在“备份”里回滚。",
                               "aimgr will download the current build from the source and "
                               "atomically replace the file. The old build is backed up and "
                               "can be restored from Backups."));
        } else {
          ImGui::TextWrapped("%s",
                             T("强制更新会重新下载源上的构建并替换当前文件（版本相同也执行）。"
                               "旧版本会先备份。",
                               "A forced update re-downloads the source build and replaces the "
                               "current file even when versions match. The old build is backed up."));
        }
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s",
                           T("此操作需要确认：确认后才会开始下载。",
                             "Confirmation required: no download starts before you confirm."));
      }
      ImGui::Separator();
      ImGui::BeginDisabled(!selected || selected->source_type == 0 || busy);
      if (sbutton(theme, 0, T("确认下载并替换", "Download & replace"), ImVec2(180, 0))) {
        const std::string id = selected->id;
        worker.submit(T("强制更新 ", "Force-updating ") + id, [&manager, &sh, id] {
          App* a = manager.find(id);
          if (!a) return;
          UpdateOptions opts;
          opts.force = true;
          UpdateResult u = manager.update(*a, opts);
          push_log(sh, u.ok ? LogLevel::Info : LogLevel::Error,
                   id + ": " +
                       (u.updated ? u.from + " -> " +
                                        (u.to.empty() ? std::string("unknown") : u.to)
                                  : (u.ok ? std::string("no update performed") : u.error)));
          refresh_rows(manager, sh);
        });
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (sbutton(theme, 1, T("取消", "Cancel"), ImVec2(120, 0))) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }

    // ---- download from a source and register it ---------------------------
    if (show_download) {
      ImGui::OpenPopup(T("下载 AppImage", "Download AppImage"));
      show_download = false;
    }
    ImGui::SetNextWindowSize(ImVec2(700, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(T("下载 AppImage", "Download AppImage"), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextDisabled("%s", T("从 GitHub Release 或直接下载链接获取 AppImage，"
                                  "保存到指定目录并加入管理列表。",
                                  "Fetch an AppImage from a GitHub release or direct URL, save it "
                                  "to a folder and add it to the managed list."));
      ImGui::RadioButton("GitHub Release", &dl_form.source, 0);
      ImGui::SameLine();
      ImGui::RadioButton(T("下载 URL", "Download URL"), &dl_form.source, 1);
      if (dl_form.source == 0) {
        ImGui::InputText("owner/repo", dl_form.repo, sizeof(dl_form.repo));
        ImGui::InputText(T("资产正则 (可空)", "Asset regex (optional)"), dl_form.asset_regex,
                         sizeof(dl_form.asset_regex));
        ImGui::InputText(T("标签前缀 (如 v)", "Tag prefix (e.g. v)"), dl_form.tag_prefix,
                         sizeof(dl_form.tag_prefix));
        ImGui::Checkbox(T("允许预发布版本", "Allow pre-releases"), &dl_form.prerelease);
      } else {
        ImGui::InputText(T("下载 URL (可用 ${version})", "Download URL (may use ${version})"),
                         dl_form.url, sizeof(dl_form.url));
        ImGui::InputText(T("版本发布页 (可空)", "Version page (optional)"), dl_form.version_url,
                         sizeof(dl_form.version_url));
        ImGui::InputText(T("版本正则 (可空)", "Version regex (optional)"), dl_form.version_regex,
                         sizeof(dl_form.version_regex));
        ImGui::InputText(T("SHA-256 (可空)", "SHA-256 (optional)"), dl_form.sha256,
                         sizeof(dl_form.sha256));
        ImGui::InputText(T("SHA-256 校验地址 (可空)", "SHA-256 URL (optional)"),
                         dl_form.sha256_url, sizeof(dl_form.sha256_url));
      }
      ImGui::Separator();
      ImGui::InputText(T("保存目录", "Destination folder"), dl_form.directory,
                       sizeof(dl_form.directory));
      ImGui::InputText(T("ID (可空)", "ID (optional)"), dl_form.id, sizeof(dl_form.id));
      ImGui::InputText(T("名称 (可空)", "Name (optional)"), dl_form.name, sizeof(dl_form.name));
      ImGui::InputText(T("文件名 (可空)", "File name (optional)"), dl_form.filename,
                       sizeof(dl_form.filename));
      ImGui::Checkbox(T("覆盖同名文件 / 已有应用", "Overwrite an existing file or app"),
                      &dl_form.replace);
      ImGui::Separator();
      if (sbutton(theme, 0, T("下载并登记", "Download & add"), ImVec2(150, 0))) {
        InstallOptions opts;
        opts.directory = dl_form.directory;
        opts.id = dl_form.id;
        opts.name = dl_form.name;
        opts.filename = dl_form.filename;
        opts.replace = dl_form.replace;
        opts.allow_prerelease = dl_form.prerelease;
        if (dl_form.source == 0) {
          opts.source = SourceType::Github;
          opts.github.repo = dl_form.repo;
          opts.github.asset_regex = dl_form.asset_regex;
          opts.github.tag_prefix = dl_form.tag_prefix;
          opts.github.allow_prerelease = dl_form.prerelease;
        } else {
          opts.source = SourceType::Url;
          opts.url.url = dl_form.url;
          opts.url.version_url = dl_form.version_url;
          opts.url.version_regex = dl_form.version_regex;
          opts.url.sha256 = dl_form.sha256;
          opts.url.sha256_url = dl_form.sha256_url;
        }
        worker.submit(T("下载 AppImage", "Downloading AppImage"), [&manager, &sh, opts] {
          InstallResult r = manager.install_from_source(opts);
          if (r.ok) {
            push_log(sh, LogLevel::Info,
                     "downloaded " + r.id + " (" +
                         (r.version.empty() ? std::string("version unknown") : r.version) +
                         ") -> " + r.path);
          } else {
            push_log(sh, LogLevel::Error, "download failed: " + r.error);
          }
          refresh_rows(manager, sh);
        });
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (sbutton(theme, 1, T("取消", "Cancel"), ImVec2(120, 0))) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }

    // ---- per-app .desktop create / modify / remove ------------------------
    if (show_desktop) {
      ImGui::OpenPopup(T("桌面项", "Desktop entry"));
      show_desktop = false;
    }
    ImGui::SetNextWindowSize(ImVec2(680, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(T("桌面项", "Desktop entry"), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::Text("%s: %s", T("应用", "App"), desk_form.id.c_str());
      ImGui::TextDisabled("%s", desk_form.path.c_str());
      ImGui::InputText(T("名称", "Name"), desk_form.name, sizeof(desk_form.name));
      ImGui::InputText(T("说明", "Comment"), desk_form.comment, sizeof(desk_form.comment));
      ImGui::InputText(T("分类", "Categories"), desk_form.categories,
                       sizeof(desk_form.categories));
      ImGui::InputText(T("图标 (可空，名称或路径)", "Icon (optional name or path)"),
                       desk_form.icon, sizeof(desk_form.icon));
      ImGui::InputText(T("额外参数 (可空，默认 %U)", "Extra args (optional, default %U)"),
                       desk_form.args, sizeof(desk_form.args));
      ImGui::Checkbox(T("在终端中运行", "Run in a terminal"), &desk_form.terminal);
      ImGui::TextDisabled("%s", T("Exec 始终指向这个受管理的 AppImage，不会变成任意命令。",
                                  "Exec always points at this managed AppImage; it cannot become "
                                  "an arbitrary command."));
      ImGui::Separator();
      if (sbutton(theme, 0, T("创建 / 保存", "Create / save"), ImVec2(150, 0))) {
        DesktopEntry de;
        de.name = desk_form.name;
        de.comment = desk_form.comment;
        de.categories = desk_form.categories;
        de.icon = desk_form.icon;
        de.args = desk_form.args;
        de.terminal = desk_form.terminal;
        const std::string id = desk_form.id;
        worker.submit(T("写入桌面项 ", "Writing desktop entry ") + id,
                      [&manager, &sh, id, de] {
                        App* a = manager.find(id);
                        if (!a) return;
                        std::string e;
                        if (!manager.set_desktop(*a, de, true, &e))
                          push_log(sh, LogLevel::Error, e);
                        else
                          push_log(sh, LogLevel::Info, id + ": desktop entry saved");
                        refresh_rows(manager, sh);
                      });
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      ImGui::BeginDisabled(!desk_form.integrated || busy);
      if (sbutton(theme, 2, T("删除", "Delete"), ImVec2(120, 0))) {
        const std::string id = desk_form.id;
        worker.submit(T("删除桌面项 ", "Removing desktop entry ") + id,
                      [&manager, &sh, id] {
                        App* a = manager.find(id);
                        if (!a) return;
                        std::string e;
                        if (!manager.deintegrate(*a, &e))
                          push_log(sh, LogLevel::Error, e);
                        else
                          push_log(sh, LogLevel::Info, id + ": desktop entry removed");
                        refresh_rows(manager, sh);
                      });
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (sbutton(theme, 1, T("关闭", "Close"), ImVec2(120, 0))) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }

    if (show_backups) {
      ImGui::OpenPopup(T("备份与回滚", "Backups & rollback"));
      show_backups = false;
    }
    ImGui::SetNextWindowSize(ImVec2(780, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(T("备份与回滚", "Backups & rollback"), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      std::vector<BackupEntry> list;
      std::string for_id;
      {
        std::lock_guard<std::mutex> lk(sh.mu);
        list = sh.backups;
        for_id = sh.backups_for;
      }
      ImGui::Text("%s: %s", T("应用", "App"), for_id.c_str());
      if (ImGui::BeginTable("backups", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn(T("版本", "Version"));
        ImGui::TableSetupColumn(T("时间", "Created"));
        ImGui::TableSetupColumn(T("大小", "Size"));
        ImGui::TableSetupColumn(T("操作", "Action"));
        ImGui::TableHeadersRow();
        for (const auto& b : list) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(b.version.c_str());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(b.created_at.c_str());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(format_size(b.size).c_str());
          ImGui::TableNextColumn();
          ImGui::PushID(b.file.c_str());
          ImGui::BeginDisabled(busy);
          if (sbutton(theme, 0, T("还原此版本", "Restore"))) {
            const std::string id = for_id, file = b.file;
            worker.submit(T("回滚 ", "Rolling back ") + id, [&manager, &sh, id, file] {
              App* a = manager.find(id);
              if (!a) return;
              std::string e, restored;
              if (manager.rollback(*a, file, &e, &restored))
                push_log(sh, LogLevel::Info, id + ": rolled back to " + restored);
              else
                push_log(sh, LogLevel::Error, id + ": rollback failed: " + e);
              refresh_rows(manager, sh);
            });
          }
          ImGui::EndDisabled();
          ImGui::PopID();
        }
        ImGui::EndTable();
      }
      if (list.empty()) ImGui::TextDisabled("%s", T("没有备份。", "No backups."));
      if (sbutton(theme, 1, T("关闭", "Close"), ImVec2(120, 0))) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }

    if (show_uninstall_confirm) {
      ImGui::OpenPopup(T("卸载", "Uninstall"));
      show_uninstall_confirm = false;
    }
    ImGui::SetNextWindowSize(ImVec2(540, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(T("卸载", "Uninstall"), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextWrapped("%s",
                         T("AppImage 会被移动到备份目录（可再还原），并从列表中移除。",
                           "The AppImage will be moved into the backup folder (recoverable) and "
                           "removed from the list."));
      static bool purge = false;
      ImGui::Checkbox(T("同时删除所有备份", "Also delete all backups"), &purge);
      if (sbutton(theme, 2, T("确认卸载", "Uninstall"), ImVec2(140, 0))) {
        const std::string id = pending_uninstall;
        const bool p = purge;
        worker.submit(T("卸载 ", "Uninstalling ") + id, [&manager, &sh, id, p] {
          App* a = manager.find(id);
          if (!a) return;
          std::string e;
          if (manager.uninstall(*a, p, false, false, &e))
            push_log(sh, LogLevel::Info, id + ": uninstalled");
          else
            push_log(sh, LogLevel::Error, id + ": uninstall failed: " + e);
          refresh_rows(manager, sh);
        });
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (sbutton(theme, 1, T("取消", "Cancel"), ImVec2(120, 0))) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }

    if (show_theme_io) {
      ImGui::OpenPopup(T("主题文件", "Theme file"));
      show_theme_io = false;
    }
    ImGui::SetNextWindowSize(ImVec2(680, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(T("主题文件", "Theme file"), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextDisabled("%s", T("主题目录: ", "Theme directory: "));
      ImGui::SameLine();
      ImGui::TextUnformatted(theme_dir.c_str());
      ImGui::InputText(T("路径", "Path"), theme_io_path, sizeof(theme_io_path));
      if (theme_io_mode == 0) {
        ImGui::TextDisabled("%s", T("目录里的 .ini（点击载入）：", ".ini files in that folder:"));
        std::error_code ec;
        if (fs::is_directory(theme_dir, ec)) {
          for (const auto& e : fs::directory_iterator(theme_dir, ec)) {
            if (!e.is_regular_file() || e.path().extension() != ".ini") continue;
            ImGui::PushID(e.path().c_str());
            if (ImGui::SmallButton(e.path().filename().c_str())) {
              copy_str(theme_io_path, sizeof(theme_io_path), e.path().string());
            }
            ImGui::PopID();
            ImGui::SameLine();
          }
          ImGui::NewLine();
        }
      }
      if (sbutton(theme, 0, theme_io_mode == 0 ? T("载入", "Load") : T("保存", "Save"),
                  ImVec2(120, 0))) {
#ifdef AIMGR_HAS_IMGUI_THEMES
        const std::string path = theme_io_path;
        if (theme_io_mode == 0) {
          std::string e;
          if (theme_use_file(theme, path, &e)) {
            theme_error.clear();
            const std::string slug = theme.preset_slug, cfile = theme.custom_file;
            const float scale = theme.scale;
            worker.submit(T("保存主题", "Saving theme"), [&manager, &sh, slug, cfile, scale] {
              UiSettings ui = manager.config().ui;
              ui.theme = slug;
              ui.custom_theme_file = cfile;
              ui.font_scale = scale;
              std::string err;
              if (!manager.set_ui(ui, &err)) push_log(sh, LogLevel::Error, "save UI settings: " + err);
              push_log(sh, LogLevel::Info, "custom theme loaded");
            });
            ImGui::CloseCurrentPopup();
          } else {
            theme_error = e;
          }
        } else {
          std::string e;
          if (ImGuiThemes::Save(path.c_str(), theme.theme, e)) {
            theme_error.clear();
            copy_str(theme_io_path, sizeof(theme_io_path), path);
          } else {
            theme_error = e;
          }
        }
#else
        theme_error = "theme module unavailable";
#endif
      }
      ImGui::SameLine();
      if (sbutton(theme, 1, T("关闭", "Close"), ImVec2(120, 0))) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }

    if (show_restart) {
      ImGui::OpenPopup(T("需要重启", "Restart required"));
      show_restart = false;
    }
    if (ImGui::BeginPopupModal(T("需要重启", "Restart required"), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextWrapped("%s", T("aimgr 已更新，重启后生效。", "aimgr was updated. Restart to use it."));
      if (sbutton(theme, 0, T("立即重启", "Restart now"), ImVec2(140, 0))) {
        const std::string target = restart_target;
        if (!target.empty()) {
          char* argv2[] = {const_cast<char*>(target.c_str()), nullptr};
          ::execv(target.c_str(), argv2);
        }
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (sbutton(theme, 1, T("稍后", "Later"), ImVec2(120, 0))) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
    if (!restart_target.empty()) show_restart = true;

    if (show_about) {
      ImGui::OpenPopup("About");
      show_about = false;
    }
    if (ImGui::BeginPopupModal("About", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::Text("aimgr %s", AIMGR_VERSION);
      ImGui::TextUnformatted(T("AppImage 更新 / 备份 / 回滚管理器",
                               "AppImage update / backup / rollback manager"));
      ImGui::TextUnformatted("UI: Dear ImGui (Me-Maped/imgui fork) + SDL2 + OpenGL3");
#ifdef AIMGR_HAS_IMGUI_THEMES
      ImGui::Text("theme: %s (scale %.2f)", theme.label.c_str(), theme.scale);
#endif
      ImGui::Text("%s: %s", T("配置", "Config"), manager.paths().config.string().c_str());
      if (sbutton(theme, 1, "OK", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }

    ImGui::Render();
    int w, h;
    SDL_GetWindowSize(window, &w, &h);
    glViewport(0, 0, w, h);
    glClearColor(0.06f, 0.07f, 0.09f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    if (!screenshot_path.empty() && frame_index == screenshot_frame) {
      std::vector<unsigned char> buf(static_cast<size_t>(w) * h * 4);
      glPixelStorei(GL_PACK_ALIGNMENT, 1);
      glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
      if (write_png_rgba(screenshot_path.c_str(), w, h, buf))
        std::fprintf(stderr, "screenshot written to %s (%dx%d), CJK font: %s, theme: %s\n",
                     screenshot_path.c_str(), w, h, g_cjk ? "yes" : "no", theme.label.c_str());
      else
        std::fprintf(stderr, "failed to write screenshot %s\n", screenshot_path.c_str());
      screenshot_frame = -1;
    }
    ++frame_index;
    SDL_GL_SwapWindow(window);
  }

  worker.stop();
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
  SDL_GL_DeleteContext(gl);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
