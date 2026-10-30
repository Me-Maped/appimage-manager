#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace aimgr {

struct HttpResponse {
  bool ok = false;
  long status = 0;
  std::string body;
  std::string error;
  std::string effective_url;
};

struct DownloadResult {
  bool ok = false;
  long status = 0;
  uint64_t bytes = 0;
  bool resumed = false;
  std::string error;
  std::string effective_url;
};

struct DownloadRequest {
  std::string url;
  std::filesystem::path dest;
  bool resume = true;
  std::vector<std::string> headers;
  /// Called periodically; return false to abort the transfer.
  std::function<bool(uint64_t done, uint64_t total)> progress;
};

/// Thin libcurl wrapper: string GETs and resumable file downloads.
class Http {
 public:
  Http();
  ~Http();
  Http(const Http&) = delete;
  Http& operator=(const Http&) = delete;

  HttpResponse get(const std::string& url, const std::vector<std::string>& headers = {},
                   size_t max_bytes = 8u << 20);

  DownloadResult download(const DownloadRequest& req);

  /// Headers GitHub likes; adds Authorization when a token is available in
  /// $AIMGR_GITHUB_TOKEN or $GITHUB_TOKEN.
  static std::vector<std::string> github_headers();
  /// API base, overridable with $AIMGR_GITHUB_API (used by the test suite).
  static std::string github_api_base();

  /// Only https is accepted unless $AIMGR_ALLOW_INSECURE_HTTP=1 (local tests,
  /// self-hosted plain-HTTP mirrors). Prevents TLS-stripping redirects and
  /// accidental plaintext downloads in production.
  static bool insecure_http_allowed();

  /// Hard ceiling for a single downloaded file ($AIMGR_MAX_DOWNLOAD_BYTES).
  static uint64_t max_download_bytes();
};

}  // namespace aimgr
