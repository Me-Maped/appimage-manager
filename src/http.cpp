#include "aimgr/http.hpp"

#include <curl/curl.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>

#include "aimgr/util.hpp"

namespace aimgr {
namespace {

std::once_flag g_curl_init;

struct SlistGuard {
  curl_slist* list = nullptr;
  ~SlistGuard() { if (list) curl_slist_free_all(list); }
  void add(const std::string& h) { list = curl_slist_append(list, h.c_str()); }
};

struct BodySink {
  std::string* out = nullptr;
  size_t cap = 0;
  bool overflow = false;
};

size_t write_string(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* sink = static_cast<BodySink*>(userdata);
  if (nmemb != 0 && size > std::numeric_limits<size_t>::max() / nmemb) {
    sink->overflow = true;
    return 0;
  }
  const size_t n = size * nmemb;
  if (n > sink->cap || sink->out->size() > sink->cap - n) {
    sink->overflow = true;
    return 0;  // abort transfer
  }
  sink->out->append(ptr, n);
  return n;
}

size_t write_file(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* fp = static_cast<FILE*>(userdata);
  return std::fwrite(ptr, size, nmemb, fp);
}

struct ProgressCtx {
  const std::function<bool(uint64_t, uint64_t)>* cb = nullptr;
};

int xferinfo(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
  auto* ctx = static_cast<ProgressCtx*>(clientp);
  if (ctx && ctx->cb && *ctx->cb) {
    const bool keep_going = (*ctx->cb)(static_cast<uint64_t>(dlnow), static_cast<uint64_t>(dltotal));
    if (!keep_going) return 1;  // non-zero aborts
  }
  return 0;
}

bool insecure_env() {
  const char* v = ::getenv("AIMGR_ALLOW_INSECURE_HTTP");
  return v && (std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0);
}

/// Curl option holding the allowed protocol set; https-only by default so a
/// redirect cannot silently downgrade the transfer to plaintext.
const char* allowed_protocols() { return insecure_env() ? "http,https" : "https"; }

/// Rejects non-https URLs up front with an actionable message instead of
/// letting libcurl fail with "protocol not supported".
bool scheme_allowed(const std::string& url, std::string* err) {
  const auto pos = url.find("://");
  if (pos == std::string::npos || pos == 0) {
    if (err) *err = "URL has no scheme: " + url.substr(0, 120);
    return false;
  }
  std::string scheme = url.substr(0, pos);
  for (auto& c : scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (scheme == "https") return true;
  if (scheme == "http" && insecure_env()) return true;
  if (err) {
    *err = "refusing " + scheme + ":// (only https is allowed; set "
           "AIMGR_ALLOW_INSECURE_HTTP=1 for a trusted local test server)";
  }
  return false;
}

void common_opts(CURL* h, char* errbuf) {
  curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(h, CURLOPT_MAXREDIRS, 10L);
  curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 20L);
  curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1024L);
  curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 60L);
  curl_easy_setopt(h, CURLOPT_USERAGENT, "aimgr/" AIMGR_VERSION " (AppImage manager)");
  curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "gzip");
  curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
  // Explicit (they are also the defaults): never accept an unverified peer.
  curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
  curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, allowed_protocols());
  curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS_STR, allowed_protocols());
  // Never forward credentials to a different host on redirect.
  curl_easy_setopt(h, CURLOPT_UNRESTRICTED_AUTH, 0L);
}

std::string token_from_env(const char* a, const char* b) {
  for (const char* name : {a, b}) {
    const char* v = ::getenv(name);
    if (v && *v) return std::string(v);
  }
  return "";
}

}  // namespace

Http::Http() { std::call_once(g_curl_init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); }); }
Http::~Http() = default;

std::string Http::github_api_base() {
  const char* v = ::getenv("AIMGR_GITHUB_API");
  std::string base = (v && *v) ? std::string(v) : std::string("https://api.github.com");
  while (!base.empty() && base.back() == '/') base.pop_back();
  return base;
}

std::vector<std::string> Http::github_headers() {
  std::vector<std::string> h = {
      "Accept: application/vnd.github+json",
      "X-GitHub-Api-Version: 2022-11-28",
  };
  const std::string token = token_from_env("AIMGR_GITHUB_TOKEN", "GITHUB_TOKEN");
  if (!token.empty()) h.push_back("Authorization: Bearer " + token);
  return h;
}

bool Http::insecure_http_allowed() { return insecure_env(); }

uint64_t Http::max_download_bytes() {
  const char* v = ::getenv("AIMGR_MAX_DOWNLOAD_BYTES");
  if (v && *v) {
    char* end = nullptr;
    unsigned long long n = std::strtoull(v, &end, 10);
    if (end && *end == '\0' && n >= (1u << 20)) return n;
  }
  return 8ull << 30;  // 8 GiB
}

HttpResponse Http::get(const std::string& url, const std::vector<std::string>& headers,
                       size_t max_bytes) {
  HttpResponse res;
  if (!scheme_allowed(url, &res.error)) return res;
  CURL* h = curl_easy_init();
  if (!h) {
    res.error = "curl_easy_init failed";
    return res;
  }
  char errbuf[CURL_ERROR_SIZE] = {0};
  BodySink sink{&res.body, max_bytes, false};
  SlistGuard hdrs;
  for (const auto& x : headers) hdrs.add(x);

  common_opts(h, errbuf);
  curl_easy_setopt(h, CURLOPT_URL, url.c_str());
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_string);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &sink);
  curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs.list);
  curl_easy_setopt(h, CURLOPT_TIMEOUT, 120L);

  CURLcode rc = curl_easy_perform(h);
  curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &res.status);
  char* eff = nullptr;
  if (curl_easy_getinfo(h, CURLINFO_EFFECTIVE_URL, &eff) == CURLE_OK && eff)
    res.effective_url = eff;
  curl_easy_cleanup(h);

  if (rc != CURLE_OK) {
    res.error = sink.overflow ? "response exceeded size limit" : std::string(errbuf[0] ? errbuf : curl_easy_strerror(rc));
    return res;
  }
  res.ok = res.status >= 200 && res.status < 300;
  if (!res.ok) {
    std::string snippet = strip_control(res.body.substr(0, 400));
    res.error = "HTTP " + std::to_string(res.status) + (snippet.empty() ? "" : ": " + snippet);
  }
  return res;
}

DownloadResult Http::download(const DownloadRequest& req) {
  DownloadResult res;
  if (!scheme_allowed(req.url, &res.error)) return res;
  if (!ensure_dir(req.dest.parent_path(), &res.error)) return res;

  for (int attempt = 0; attempt < 2; ++attempt) {
    const bool want_resume = req.resume && attempt == 0;
    const uint64_t cap = max_download_bytes();
    uint64_t have = want_resume ? path_size(req.dest) : 0;
    if (have > cap) {
      std::error_code ec;
      std::filesystem::remove(req.dest, ec);
      have = 0;
    }
    // O_NOFOLLOW: a symlink planted in the cache directory must not turn the
    // download into an arbitrary-file append.
    const int flags = O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK |
                      (have > 0 ? O_APPEND : O_TRUNC);
    int fd = ::open(req.dest.c_str(), flags, 0600);
    if (fd < 0 && errno == ELOOP) {
      std::error_code ec;
      std::filesystem::remove(req.dest, ec);
      fd = ::open(req.dest.c_str(), flags, 0600);
    }
    struct stat fst {};
    if (fd < 0 || ::fstat(fd, &fst) != 0 || !S_ISREG(fst.st_mode)) {
      const int saved = errno;
      if (fd >= 0) ::close(fd);
      res.error = "cannot open " + req.dest.string() +
                  " as a regular file: " + std::strerror(saved);
      return res;
    }
    FILE* fp = ::fdopen(fd, have > 0 ? "ab" : "wb");
    if (!fp) {
      const int saved = errno;
      ::close(fd);
      res.error = "cannot open " + req.dest.string() + ": " + std::strerror(saved);
      return res;
    }
    CURL* h = curl_easy_init();
    if (!h) {
      std::fclose(fp);
      res.error = "curl_easy_init failed";
      return res;
    }
    char errbuf[CURL_ERROR_SIZE] = {0};
    SlistGuard hdrs;
    for (const auto& x : req.headers) hdrs.add(x);
    ProgressCtx pctx{&req.progress};

    common_opts(h, errbuf);
    curl_easy_setopt(h, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_file);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs.list);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, xferinfo);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, &pctx);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    if (have > 0) curl_easy_setopt(h, CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(have));
    curl_easy_setopt(h, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(cap));

    CURLcode rc = curl_easy_perform(h);
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &res.status);
    char* eff = nullptr;
    if (curl_easy_getinfo(h, CURLINFO_EFFECTIVE_URL, &eff) == CURLE_OK && eff)
      res.effective_url = eff;
    bool flush_ok = std::fflush(fp) == 0;
    int saved_errno = errno;
    struct stat st {};
    uint64_t downloaded = 0;
    if (flush_ok && ::fstat(::fileno(fp), &st) == 0 && S_ISREG(st.st_mode))
      downloaded = static_cast<uint64_t>(st.st_size);
    else if (flush_ok)
      flush_ok = false;
    if (flush_ok && ::fsync(::fileno(fp)) != 0) {
      saved_errno = errno;
      flush_ok = false;
    }
    if (std::fclose(fp) != 0 && flush_ok) {
      saved_errno = errno;
      flush_ok = false;
    }
    curl_easy_cleanup(h);

    if (!flush_ok) {
      res.error = "cannot flush download to disk: " + std::string(std::strerror(saved_errno));
      return res;
    }
    if (downloaded > cap) {
      std::error_code ec;
      std::filesystem::remove(req.dest, ec);
      res.error = "download exceeds the configured size limit (" + format_size(cap) + ")";
      return res;
    }

    if (rc != CURLE_OK) {
      res.error = errbuf[0] ? errbuf : curl_easy_strerror(rc);
      if (rc == CURLE_ABORTED_BY_CALLBACK) res.error = "cancelled";
      return res;
    }
    // The server ignored our Range header and sent the whole body: restart.
    if (have > 0 && res.status == 200) {
      std::error_code ec;
      std::filesystem::remove(req.dest, ec);
      continue;
    }
    if (res.status == 416) {  // already complete per server, or stale part file
      std::error_code ec;
      std::filesystem::remove(req.dest, ec);
      if (attempt == 0) continue;
      res.error = "server rejected resume (HTTP 416)";
      return res;
    }
    if (res.status < 200 || res.status >= 300) {
      res.error = "HTTP " + std::to_string(res.status);
      return res;
    }
    res.ok = true;
    res.resumed = have > 0;
    res.bytes = downloaded;
    return res;
  }
  res.error = "download failed after retry";
  return res;
}

}  // namespace aimgr
