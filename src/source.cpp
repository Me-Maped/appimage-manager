#include "aimgr/source.hpp"

#include <sys/utsname.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <regex>

#include "aimgr/json.hpp"
#include "aimgr/version.hpp"

namespace aimgr {
namespace {

constexpr const char* kDefaultAssetRegex = ".*\\.AppImage$";
constexpr const char* kDefaultVersionRegex = "([0-9]+(?:\\.[0-9]+)+)";

bool match_icase(const std::string& text, const std::string& pattern, std::string* err) {
  try {
    std::regex re(pattern, std::regex::ECMAScript | std::regex::icase);
    return std::regex_search(text, re);
  } catch (const std::regex_error& e) {
    if (err) *err = "invalid regex '" + pattern + "': " + e.what();
    return false;
  }
}


// ------------------------------------------------------- asset selection
// A release usually ships several architectures plus auxiliary files. Picking
// the first regex match is how you end up downloading an aarch64 build on an
// x86_64 machine, so candidates are ranked: matching host architecture wins,
// foreign architectures and side-car files (.zsync/.sha256/...) lose.

bool has_token(const std::string& hay, const std::string& tok) {
  size_t pos = 0;
  while ((pos = hay.find(tok, pos)) != std::string::npos) {
    const bool left_ok = pos == 0 || !std::isalnum(static_cast<unsigned char>(hay[pos - 1]));
    const size_t end = pos + tok.size();
    const bool right_ok =
        end >= hay.size() || !std::isalnum(static_cast<unsigned char>(hay[end]));
    if (left_ok && right_ok) return true;
    pos = end;
  }
  return false;
}

std::string host_arch() {
  static std::string cached;
  if (!cached.empty()) return cached;
  struct utsname u {};
  std::string m = "x86_64";
  if (::uname(&u) == 0) m = to_lower(u.machine);
  if (m == "x86_64" || m == "amd64") cached = "x86_64";
  else if (m == "aarch64" || m == "arm64") cached = "aarch64";
  else if (m.rfind("arm", 0) == 0) cached = "armv7";
  else if (m == "i686" || m == "i386" || m == "x86") cached = "x86";
  else if (m == "riscv64") cached = "riscv64";
  else cached = m;
  return cached;
}

std::vector<std::string> arch_tokens(const std::string& group) {
  if (group == "x86_64") return {"x86_64", "x86-64", "amd64", "x64"};
  if (group == "aarch64") return {"aarch64", "arm64"};
  if (group == "armv7") return {"armv7", "armhf", "armv6", "arm"};
  if (group == "x86") return {"i686", "i386", "386"};
  if (group == "riscv64") return {"riscv64"};
  return {group};
}

/// >0: likely the host architecture, <0: likely a different one, 0: unclear.
int arch_score(const std::string& filename) {
  const std::string lower = to_lower(filename);
  const std::string host = host_arch();
  bool host_hit = false, foreign_hit = false;
  for (const char* g : {"x86_64", "aarch64", "armv7", "x86", "riscv64"}) {
    if (host == g) continue;
    for (const auto& t : arch_tokens(g))
      if (has_token(lower, t)) {
        foreign_hit = true;
        break;
      }
  }
  for (const auto& t : arch_tokens(host))
    if (has_token(lower, t)) {
      host_hit = true;
      break;
    }
  if (host_hit) return 3;
  if (foreign_hit) return -10;
  return 0;
}

/// Side-car files that are never the AppImage itself.
int asset_penalty(const std::string& filename) {
  const std::string lower = to_lower(filename);
  for (const char* suffix : {".zsync", ".sha256", ".sha512", ".sig", ".asc", ".txt", ".json",
                             ".blockmap", ".deb", ".rpm", ".tar.gz", ".zip"})
    if (ends_with(lower, suffix)) return -20;
  return 0;
}

int asset_rank(const std::string& filename) { return arch_score(filename) + asset_penalty(filename); }

std::string basename_of_url(const std::string& url) {
  std::string path = url;
  auto q = path.find('?');
  if (q != std::string::npos) path = path.substr(0, q);
  auto slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string find_sha256_token(const std::string& text) {
  static const std::regex hex64("[0-9a-fA-F]{64}");
  std::smatch m;
  if (std::regex_search(text, m, hex64)) return to_lower(m.str());
  return "";
}

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
  if (from.empty()) return s;
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
  return s;
}

// ---------------------------------------------------------------- web fallback
// The unauthenticated GitHub REST API allows only 60 requests/hour per IP, which
// is easy to exhaust on a shared connection. When that happens we fall back to
// the public release pages: /releases/latest redirects to the tag, and
// /releases/expanded_assets/<tag> lists the asset download links. No token,
// no API quota; the checksum is unavailable on this path, so the download is
// verified by ELF/AppImage structure and size only.

std::string html_unescape(std::string s) {
  const std::pair<const char*, const char*> ents[] = {
      {"&amp;", "&"}, {"&#38;", "&"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&lt;", "<"}, {"&gt;", ">"}};
  for (const auto& [from, to] : ents) s = replace_all(s, from, to);
  return s;
}

std::string github_web_base() {
  const char* v = ::getenv("AIMGR_GITHUB_WEB");
  std::string base = (v && *v) ? std::string(v) : std::string("https://github.com");
  while (!base.empty() && base.back() == '/') base.pop_back();
  return base;
}

/// Pulls "/owner/repo/releases/download/<tag>/<file>" links out of an HTML page.
/// Only same-origin GitHub pages for the expected repository are accepted, so
/// a crafted href in a release page cannot redirect a download to another host.
std::vector<std::string> html_asset_paths(const std::string& html, const std::string& web,
                                          const std::string& repo) {
  static const std::regex href_re(R"re(href="([^"]+?)")re");
  const std::string prefix = "/" + repo + "/releases/download/";
  std::vector<std::string> out;
  for (std::sregex_iterator it(html.begin(), html.end(), href_re), end; it != end; ++it) {
    std::string href = html_unescape((*it)[1].str());
    auto q = href.find_first_of("?#");
    if (q != std::string::npos) href = href.substr(0, q);
    if (href.empty()) continue;
    std::string path;
    if (href[0] == '/' && !starts_with(href, "//")) {
      path = href;
    } else if (starts_with(href, web + "/")) {
      path = href.substr(web.size());
    } else {
      continue;
    }
    if (!starts_with(path, prefix)) continue;
    out.push_back(web + path);
  }
  return out;
}

std::string tag_from_release_url(const std::string& url) {
  const std::string marker = "/releases/tag/";
  auto p = url.find(marker);
  if (p == std::string::npos) return "";
  std::string tag = url.substr(p + marker.size());
  auto stop = tag.find_first_of("?#");
  if (stop != std::string::npos) tag = tag.substr(0, stop);
  return tag;
}

RemoteRelease resolve_github_via_web(const SourceGithub& src, Http& http, const Logger& log,
                                     const std::string& repo, const std::string& asset_regex) {
  RemoteRelease out;
  const std::string web = github_web_base();
  const std::string base_url = web + "/" + repo;

  std::string tag;
  if (src.allow_prerelease) {
    // The newest entry on the releases index may be a pre-release.
    HttpResponse r = http.get(base_url + "/releases", {}, 4u << 20);
    if (!r.ok) {
      out.error = "cannot read " + base_url + "/releases: " + r.error;
      return out;
    }
    static const std::regex tag_re(R"re(href="/[^"]*/releases/tag/([^"]+)")re");
    std::smatch m;
    if (std::regex_search(r.body, m, tag_re)) tag = m[1].str();
  } else {
    HttpResponse r = http.get(base_url + "/releases/latest", {}, 4u << 20);
    tag = tag_from_release_url(r.effective_url);
    if (tag.empty()) tag = tag_from_release_url(r.body.substr(0, 400));
    if (tag.empty() && !r.ok) {
      out.error = "cannot resolve latest release of " + repo + ": " + r.error;
      return out;
    }
  }
  if (tag.empty()) {
    out.error = "cannot determine the latest tag of " + repo + " from the web pages";
    return out;
  }

  HttpResponse assets = http.get(base_url + "/releases/expanded_assets/" + tag, {}, 4u << 20);
  if (!assets.ok) {
    out.error = "cannot list assets of " + repo + " " + tag + ": " + assets.error;
    return out;
  }
  std::string regex_err;
  std::string best_url, best_name;
  int best_rank = INT_MIN;
  for (const auto& url : html_asset_paths(assets.body, web, repo)) {
    const std::string name = basename_of_url(url);
    if (!match_icase(name, asset_regex, &regex_err)) continue;
    const int rank = asset_rank(name);
    if (rank > best_rank) {
      best_rank = rank;
      best_url = url;
      best_name = name;
    }
  }
  if (!best_name.empty()) {
    out.ok = true;
    out.version = strip_tag_prefix(tag, src.tag_prefix);
    out.raw_tag = tag;
    out.url = best_url;
    out.asset = best_name;
    out.notes_url = base_url + "/releases/tag/" + tag;
    return out;
  }
  if (!regex_err.empty()) {
    out.error = regex_err;
  } else {
    out.error = "no asset of " + repo + " " + tag + " matches /" + asset_regex + "/";
  }
  return out;
}

}  // namespace


std::string parse_github_repo(const std::string& input) {
  std::string s = trim(input);
  if (s.empty()) return "";
  if (starts_with(s, "git@github.com:")) {
    s = s.substr(std::string("git@github.com:").size());
  } else if (starts_with(s, "https://") || starts_with(s, "http://")) {
    auto scheme = s.find("://");
    s = s.substr(scheme + 3);
    auto slash = s.find('/');
    if (slash == std::string::npos) return "";
    std::string host = to_lower(s.substr(0, slash));
    if (host != "github.com" && host != "www.github.com") return "";
    s = s.substr(slash + 1);
  } else if (s.find("github.com/") != std::string::npos) {
    s = s.substr(s.find("github.com/") + std::string("github.com/").size());
  }
  auto parts = split(s, '/');
  if (parts.size() < 2) return "";
  std::string owner = trim(parts[0]);
  std::string repo = trim(parts[1]);
  if (ends_with(repo, ".git")) repo = repo.substr(0, repo.size() - 4);
  auto valid = [](const std::string& x, bool allow_dot) {
    if (x.empty() || x.size() > 100 || x == "." || x == "..") return false;
    if (x.front() == '.' || x.front() == '-' || x.back() == '.' || x.back() == '-')
      return false;
    return std::all_of(x.begin(), x.end(), [allow_dot](unsigned char c) {
      const bool ascii_alnum = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                               (c >= 'a' && c <= 'z');
      return ascii_alnum || c == '-' || c == '_' || (allow_dot && c == '.');
    });
  };
  if (!valid(owner, false) || !valid(repo, true)) return "";
  return owner + "/" + repo;
}

RemoteRelease resolve_github(const SourceGithub& src, Http& http, const Logger& log) {
  RemoteRelease out;
  const std::string repo = parse_github_repo(src.repo);
  if (repo.empty()) {
    out.error = "invalid GitHub repository '" + src.repo + "' (expected owner/repo)";
    return out;
  }
  const std::string asset_regex =
      src.asset_regex.empty() ? std::string(kDefaultAssetRegex) : src.asset_regex;
  const std::string api = Http::github_api_base() + "/repos/" + repo + "/releases?per_page=40";
  log.debug("GET " + api);
  HttpResponse r = http.get(api, Http::github_headers());
  if (!r.ok) {
    const bool quota = r.status == 403 || r.status == 429 || r.status == 0;
    if (quota) {
      log.warn("GitHub API unavailable (" + std::to_string(r.status) +
               (r.error.empty() ? "" : " " + r.error) +
               "); retrying through the public release pages (no token needed)");
      RemoteRelease web = resolve_github_via_web(src, http, log, repo, asset_regex);
      if (web.ok) {
        log.info("web fallback resolved " + repo + " " + web.version + " (" + web.asset + ")");
        return web;
      }
      out.error = "GitHub API request failed (" + std::to_string(r.status) + "): " + r.error +
                  "\n  web fallback also failed: " + web.error +
                  "\n  hint: export AIMGR_GITHUB_TOKEN with a personal access token to avoid the "
                  "60 requests/hour anonymous limit.";
      return out;
    }
    out.error = "GitHub API request failed (" + std::to_string(r.status) + "): " + r.error;
    if (r.status == 404) out.error += "\n  hint: repository not found, renamed or private.";
    return out;
  }
  json::Value root;
  std::string jerr;
  if (!json::parse(r.body, root, jerr)) {
    out.error = "cannot parse GitHub response: " + jerr;
    return out;
  }
  if (!root.is_arr()) {
    out.error = "unexpected GitHub response (not a release list)";
    return out;
  }

  std::string regex_err;
  bool best = false;
  std::string seen_assets;
  for (size_t i = 0; i < root.size(); ++i) {
    const json::Value& rel = root.at(i);
    if (!rel.is_obj()) continue;  // a message object (rate limit etc.)
    if (rel.get_bool("draft", false)) continue;
    if (rel.get_bool("prerelease", false) && !src.allow_prerelease) continue;
    const std::string tag = rel.get_str("tag_name");
    if (tag.empty()) continue;

    const json::Value* assets = rel.find("assets");
    if (!assets || !assets->is_arr()) continue;
    const json::Value* picked = nullptr;
    int picked_rank = INT_MIN;
    for (size_t k = 0; k < assets->size(); ++k) {
      const json::Value& a = assets->at(k);
      const std::string name = a.get_str("name");
      if (seen_assets.size() < 600) seen_assets += (seen_assets.empty() ? "" : ", ") + name;
      if (!match_icase(name, asset_regex, &regex_err)) continue;
      const int rank = asset_rank(name);
      if (rank > picked_rank) {
        picked_rank = rank;
        picked = &a;
      }
    }
    if (!picked) continue;

    const std::string version = strip_tag_prefix(tag, src.tag_prefix);
    if (!best || compare_versions(version, out.version) > 0 ||
        (compare_versions(version, out.version) == 0 && rel.get_str("published_at") > out.published)) {
      best = true;
      out.ok = true;
      out.version = version;
      out.raw_tag = tag;
      out.url = picked->get_str("browser_download_url");
      out.asset = picked->get_str("name");
      out.size = static_cast<uint64_t>(picked->get_num("size", 0));
      out.published = rel.get_str("published_at");
      out.notes_url = rel.get_str("html_url");
      const std::string digest = picked->get_str("digest");
      if (starts_with(digest, "sha256:")) out.sha256 = to_lower(digest.substr(7));
      if (out.url.empty()) {
        out.ok = false;
        out.error = "release asset '" + out.asset + "' has no download URL";
      }
    }
  }

  if (!best) {
    if (!regex_err.empty()) {
      out.error = regex_err;
    } else {
      out.error = "no release of " + repo + " has an asset matching /" + asset_regex + "/";
      if (!seen_assets.empty()) out.error += "\n  assets seen: " + seen_assets;
      if (!src.allow_prerelease) out.error += "\n  hint: add --prerelease to consider pre-releases.";
    }
  }
  return out;
}

RemoteRelease resolve_url(const SourceUrl& src, const std::string& installed_version, Http& http,
                          const Logger& log) {
  RemoteRelease out;
  if (src.url.empty()) {
    out.error = "url source has no 'url'";
    return out;
  }
  std::string version;
  if (!src.version_url.empty()) {
    log.debug("GET " + src.version_url);
    // No auth headers here: version_url is an arbitrary user-supplied host.
    HttpResponse r = http.get(src.version_url, {}, 1u << 20);
    if (!r.ok) {
      out.error = "cannot fetch version from " + src.version_url + ": " + r.error;
      return out;
    }
    bool ok = false;
    version = extract_version(r.body, src.version_regex, &ok);
    if (!ok) {
      out.error = "no version matching /" +
                  (src.version_regex.empty() ? std::string(kDefaultVersionRegex)
                                             : src.version_regex) +
                  "/ in " + src.version_url;
      return out;
    }
  } else {
    // Only the last path segment may carry the version: extracting from the
    // whole URL would happily return "127.0.0.1" from the host.
    bool ok = false;
    version = extract_version(basename_of_url(src.url), src.version_regex, &ok);
    if (!ok) version.clear();
  }
  if (version.empty()) {
    if (src.url.find("${version}") != std::string::npos) {
      out.error = "download URL contains ${version}, but no version could be determined";
      return out;
    }
    // No way to name the build: we can still download it, but cannot compare.
    out.ok = true;
    out.version.clear();
    out.raw_tag.clear();
  } else {
    out.ok = true;
    out.version = normalize_version(version);
    out.raw_tag = version;
  }
  out.url = replace_all(src.url, "${version}", out.version);
  out.asset = basename_of_url(out.url);
  out.sha256 = to_lower(trim(src.sha256));
  if (out.sha256.empty() && !src.sha256_url.empty()) {
    HttpResponse r = http.get(src.sha256_url, {}, 1u << 20);
    if (r.ok)
      out.sha256 = find_sha256_token(r.body);
    else
      log.warn("cannot fetch checksum from " + src.sha256_url + ": " + r.error);
  }
  (void)installed_version;
  return out;
}

RemoteRelease resolve_source(const App& app, Http& http, const Logger& log) {
  RemoteRelease rr;
  switch (app.source) {
    case SourceType::Github:
      return resolve_github(app.github, http, log);
    case SourceType::Url:
      return resolve_url(app.url, app.installed_version, http, log);
    default:
      rr.error = "no update source configured for '" + app.id +
                 "' (use: aimgr set-source " + app.id + " --github owner/repo)";
      return rr;
  }
}

}  // namespace aimgr
