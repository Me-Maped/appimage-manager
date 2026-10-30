#pragma once
#include <cstdint>
#include <string>

#include "aimgr/config.hpp"
#include "aimgr/http.hpp"
#include "aimgr/util.hpp"

namespace aimgr {

/// One concrete, downloadable candidate produced by a source.
struct RemoteRelease {
  bool ok = false;
  std::string error;

  std::string version;     // display version, tag prefix stripped
  std::string raw_tag;     // untouched tag / url-derived label
  std::string url;         // direct download URL
  std::string asset;       // asset file name (GitHub) or last path segment
  uint64_t size = 0;       // 0 when unknown
  std::string sha256;      // empty when unknown
  std::string published;   // release date, when known
  std::string notes_url;   // html_url, when known
};

/// Parses "owner/repo", "https://github.com/owner/repo",
/// "https://github.com/owner/repo/releases/latest" and
/// ".../releases/tag/v1.2.3" into "owner/repo". Returns "" when it cannot.
std::string parse_github_repo(const std::string& input);

RemoteRelease resolve_github(const SourceGithub& src, Http& http, const Logger& log);
RemoteRelease resolve_url(const SourceUrl& src, const std::string& installed_version, Http& http,
                          const Logger& log);

RemoteRelease resolve_source(const App& app, Http& http, const Logger& log);

}  // namespace aimgr
