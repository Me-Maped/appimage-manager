#include "aimgr/util.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <random>
#include <sstream>

namespace aimgr {

namespace {
std::string home_dir() {
  const char* h = ::getenv("HOME");
  return h ? std::string(h) : std::string("/tmp");
}
/// Creates a sibling temp file with O_EXCL (no predictable name). The
/// descriptor is returned open, so callers never have to reopen by path and
/// cannot be tricked into following a symlink planted during a TOCTOU window.
int make_temp_sibling(const fs::path& target, fs::path& out) {
  std::string tmpl = target.string() + ".tmp-XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  int fd = ::mkstemp(buf.data());
  if (fd < 0) return -1;
  const int flags = ::fcntl(fd, F_GETFD);
  if (flags >= 0) ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
  out = fs::path(std::string(buf.data()));
  return fd;
}
bool fsync_dir_of(const fs::path& p) {
  int dfd = ::open(p.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
  if (dfd < 0) return false;
  ::fsync(dfd);
  ::close(dfd);
  return true;
}
}  // namespace

std::string format_size(uint64_t bytes) {
  const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  double v = static_cast<double>(bytes);
  int u = 0;
  while (v >= 1024.0 && u < 4) {
    v /= 1024.0;
    ++u;
  }
  char buf[64];
  if (u == 0)
    std::snprintf(buf, sizeof(buf), "%llu %s", static_cast<unsigned long long>(bytes), units[u]);
  else
    std::snprintf(buf, sizeof(buf), "%.2f %s", v, units[u]);
  return buf;
}

std::string now_iso() {
  auto now = std::chrono::system_clock::now();
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  ::gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

std::string utc_stamp() {
  auto now = std::chrono::system_clock::now();
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  ::gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm);
  return buf;
}

fs::path expand_tilde(const fs::path& path) {
  const std::string s = path.string();
  if (s.empty()) return path;
  if (s == "~") return fs::path(home_dir());
  if (s.rfind("~/", 0) == 0) return fs::path(home_dir() + s.substr(1));
  return path;
}

bool ensure_dir(const fs::path& dir, std::string* err) {
  std::error_code ec;
  if (dir.empty()) {
    if (err) *err = "empty directory path";
    return false;
  }
  if (fs::exists(dir, ec)) {
    if (fs::is_directory(dir, ec)) return true;
    if (err) *err = dir.string() + " exists but is not a directory";
    return false;
  }
  if (!fs::create_directories(dir, ec)) {
    if (err) *err = "cannot create " + dir.string() + ": " + ec.message();
    return false;
  }
  return true;
}

bool read_file(const fs::path& path, std::string& out, std::string* err) {
  // O_NONBLOCK keeps an attacker-controlled FIFO from blocking the caller;
  // fstat then rejects everything that is not a regular file. A bounded read
  // also prevents a huge local file from exhausting memory.
  constexpr size_t kMaxReadBytes = 64u << 20;
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) {
    if (err) *err = "cannot read " + path.string() + ": " + errno_str();
    return false;
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    const std::string why = errno_str();
    ::close(fd);
    if (err) *err = "cannot stat " + path.string() + ": " + why;
    return false;
  }
  if (!S_ISREG(st.st_mode)) {
    ::close(fd);
    if (err) *err = path.string() + " is not a regular file";
    return false;
  }
  if (st.st_size > 0 && static_cast<uint64_t>(st.st_size) > kMaxReadBytes) {
    ::close(fd);
    if (err) *err = path.string() + " is too large to read";
    return false;
  }
  out.clear();
  if (st.st_size > 0) out.reserve(static_cast<size_t>(st.st_size));
  char buf[64 * 1024];
  for (;;) {
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n == 0) break;
    if (n < 0) {
      if (errno == EINTR) continue;
      const std::string why = errno_str();
      ::close(fd);
      if (err) *err = "read failed for " + path.string() + ": " + why;
      return false;
    }
    if (out.size() + static_cast<size_t>(n) > kMaxReadBytes) {
      ::close(fd);
      if (err) *err = path.string() + " is too large to read";
      return false;
    }
    out.append(buf, static_cast<size_t>(n));
  }
  ::close(fd);
  return true;
}

bool write_file_atomic(const fs::path& path, const std::string& data, std::string* err, int mode) {
  if (!ensure_dir(path.parent_path(), err)) return false;
  fs::path tmp;
  int fd = make_temp_sibling(path, tmp);
  if (fd < 0) {
    if (err) *err = "cannot create a temporary file next to " + path.string() + ": " + errno_str();
    return false;
  }
  auto fail = [&](const std::string& why) {
    ::close(fd);
    ::unlink(tmp.c_str());
    if (err) *err = why;
    return false;
  };
  size_t off = 0;
  while (off < data.size()) {
    const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return fail("write failed: " + errno_str());
    }
    if (n == 0) return fail("write returned zero bytes");
    off += static_cast<size_t>(n);
  }
  if (::fsync(fd) != 0) return fail("fsync failed: " + errno_str());
  if (::fchmod(fd, static_cast<mode_t>(mode)) != 0)
    return fail("fchmod failed: " + errno_str());
  if (::close(fd) != 0) {
    fd = -1;
    const std::string why = "close failed: " + errno_str();
    ::unlink(tmp.c_str());
    if (err) *err = why;
    return false;
  }
  fd = -1;
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    const std::string why = "rename failed: " + errno_str();
    ::unlink(tmp.c_str());
    if (err) *err = why;
    return false;
  }
  fsync_dir_of(path);
  return true;
}

bool copy_file_atomic(const fs::path& src, const fs::path& dst, std::string* err, int mode) {
  if (!ensure_dir(dst.parent_path(), err)) return false;
  // O_NOFOLLOW also rejects a symlink swapped in after validation. Sources
  // are regular AppImages/backups; callers that support symlinked apps resolve
  // the link before reaching this helper.
  int sfd = ::open(src.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (sfd < 0) {
    if (err) *err = "cannot open source " + src.string() + ": " + errno_str();
    return false;
  }
  struct stat sst {};
  if (::fstat(sfd, &sst) != 0 || !S_ISREG(sst.st_mode)) {
    const std::string why = errno_str();
    ::close(sfd);
    if (err) *err = "source is not a regular file: " + src.string() +
                    (why.empty() ? "" : (": " + why));
    return false;
  }
  fs::path tmp;
  int dfd = make_temp_sibling(dst, tmp);
  if (dfd < 0) {
    ::close(sfd);
    if (err) *err = "cannot create a temporary file next to " + dst.string() + ": " + errno_str();
    return false;
  }
  auto fail = [&](const std::string& why) {
    ::close(sfd);
    ::close(dfd);
    ::unlink(tmp.c_str());
    if (err) *err = why;
    return false;
  };
  char buf[256 * 1024];
  for (;;) {
    const ssize_t n = ::read(sfd, buf, sizeof(buf));
    if (n == 0) break;
    if (n < 0) {
      if (errno == EINTR) continue;
      return fail("copy read failed: " + errno_str());
    }
    size_t off = 0;
    while (off < static_cast<size_t>(n)) {
      const ssize_t w = ::write(dfd, buf + off, static_cast<size_t>(n) - off);
      if (w < 0) {
        if (errno == EINTR) continue;
        return fail("copy write failed: " + errno_str());
      }
      if (w == 0) return fail("copy write returned zero bytes");
      off += static_cast<size_t>(w);
    }
  }
  if (::fsync(dfd) != 0) return fail("copy fsync failed: " + errno_str());
  if (::fchmod(dfd, static_cast<mode_t>(mode)) != 0)
    return fail("copy fchmod failed: " + errno_str());
  ::close(sfd);
  sfd = -1;
  if (::close(dfd) != 0) {
    dfd = -1;
    const std::string why = "copy close failed: " + errno_str();
    ::unlink(tmp.c_str());
    if (err) *err = why;
    return false;
  }
  dfd = -1;
  if (::rename(tmp.c_str(), dst.c_str()) != 0) {
    const std::string why = "rename failed: " + errno_str();
    ::unlink(tmp.c_str());
    if (err) *err = why;
    return false;
  }
  fsync_dir_of(dst);
  return true;
}

bool path_exists(const fs::path& p) {
  std::error_code ec;
  return fs::exists(p, ec);
}
bool path_is_regular(const fs::path& p) {
  std::error_code ec;
  return fs::is_regular_file(p, ec);
}
bool path_is_symlink(const fs::path& p) {
  std::error_code ec;
  return fs::is_symlink(fs::symlink_status(p, ec));
}
uint64_t path_size(const fs::path& p) {
  std::error_code ec;
  auto s = fs::file_size(p, ec);
  return ec ? 0 : static_cast<uint64_t>(s);
}
uint64_t path_inode(const fs::path& p) {
  struct stat st {};
  if (::stat(p.c_str(), &st) != 0) return 0;
  return static_cast<uint64_t>(st.st_ino);
}

std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string trim(std::string s) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

std::string slugify(const std::string& s) {
  std::string out;
  bool dash = false;
  for (unsigned char c : s) {
    if (std::isalnum(c)) {
      out += static_cast<char>(std::tolower(c));
      dash = false;
    } else if (!dash && !out.empty()) {
      out += '-';
      dash = true;
    }
  }
  while (!out.empty() && out.back() == '-') out.pop_back();
  return out;
}

bool starts_with(const std::string& s, const std::string& p) {
  return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}
bool ends_with(const std::string& s, const std::string& p) {
  return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == sep) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  out.push_back(cur);
  return out;
}

std::string join(const std::vector<std::string>& v, const std::string& sep) {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) out += sep;
    out += v[i];
  }
  return out;
}

std::string errno_str() { return std::strerror(errno); }

bool FileLock::acquire(const fs::path& path, std::string* err) {
  release();
  if (!ensure_dir(path.parent_path(), err)) return false;
  fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) {
    if (err) *err = "cannot open lock " + path.string() + ": " + errno_str();
    return false;
  }
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    if (err) *err = "another aimgr instance holds the lock (" + path.string() +
                    "). Wait for it or remove a stale lock file.";
    release();
    return false;
  }
  return true;
}

void FileLock::release() {
  if (fd >= 0) {
    ::flock(fd, LOCK_UN);
    ::close(fd);
    fd = -1;
  }
}

fs::path default_install_dir() {
  const std::string home = home_dir();
  for (const char* cand : {"Applications", "Apps", "AppImages"}) {
    fs::path p = fs::path(home) / cand;
    std::error_code ec;
    if (fs::is_directory(p, ec)) return p;
  }
  return fs::path(home) / "Applications";
}

bool looks_like_appimage(const fs::path& path, std::string* why) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    if (why) *why = "cannot open file: " + errno_str();
    return false;
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    ::close(fd);
    if (why) *why = "not a regular file";
    return false;
  }
  unsigned char hdr[16] = {0};
  const ssize_t got = ::read(fd, hdr, sizeof(hdr));
  ::close(fd);
  if (got < 0) {
    if (why) *why = "cannot read file: " + errno_str();
    return false;
  }
  if (static_cast<size_t>(got) < 11) {
    if (why) *why = "file shorter than an ELF header";
    return false;
  }
  if (!(hdr[0] == 0x7f && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F')) {
    if (why) *why = "not an ELF file (maybe an HTML error page or a partial download)";
    return false;
  }
  if (!(hdr[8] == 'A' && hdr[9] == 'I' && (hdr[10] == 1 || hdr[10] == 2))) {
    if (why) *why = "ELF file without the AppImage type marker at offset 8";
    return false;
  }
  return true;
}

std::string head_bytes_hex(const fs::path& path, size_t n) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return "";
  std::vector<unsigned char> buf(n, 0);
  in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(n));
  const auto got = static_cast<size_t>(in.gcount());
  static const char* hexd = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < got; ++i) {
    out += hexd[buf[i] >> 4];
    out += hexd[buf[i] & 0xf];
    if (i + 1 < got) out += ' ';
  }
  return out;
}

}  // namespace aimgr

namespace aimgr {

bool is_valid_id(const std::string& id) {
  if (id.empty() || id.size() > 64) return false;
  if (id == "." || id == "..") return false;
  if (id.front() == '-' || id.front() == '.') return false;
  return std::all_of(id.begin(), id.end(), [](unsigned char c) {
    const bool ascii_alnum = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                             (c >= 'a' && c <= 'z');
    return ascii_alnum || c == '.' || c == '_' || c == '-';
  });
}

std::string sanitize_component(const std::string& s, size_t max_len) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    const bool ascii_alnum = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                             (c >= 'a' && c <= 'z');
    if (ascii_alnum || c == '.' || c == '_' || c == '-')
      out += static_cast<char>(c);
    else
      out += '_';
  }
  // Never allow a leading dot or dash: hidden files and option-like names.
  size_t start = 0;
  while (start < out.size() && (out[start] == '.' || out[start] == '-')) ++start;
  out = out.substr(start);
  if (out.size() > max_len) out.resize(max_len);
  if (out.empty()) out = "unnamed";
  return out;
}

std::string strip_control(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    if (c == 0x1b) continue;              // ESC (ANSI escape sequences)
    if (c < 0x20 && c != '\t') continue;  // other control chars
    if (c == 0x7f) continue;
    out += static_cast<char>(c);
  }
  return out;
}

std::string escape_desktop_exec(const std::string& path) {
  std::string cleaned = strip_control(path);
  std::string out = "\"";
  for (char c : cleaned) {
    if (c == '"' || c == '\\' || c == '$' || c == '`') out += '\\';
    // A literal percent must not be interpreted as a desktop field code.
    if (c == '%') out += '%';
    out += c;
  }
  out += '"';
  return out;
}

std::string sanitize_desktop_value(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  bool last_space = false;
  for (unsigned char c : s) {
    if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    if (c < 0x20 || c == 0x7f) continue;
    if (c == ' ') {
      if (last_space) continue;
      last_space = true;
    } else {
      last_space = false;
    }
    out += static_cast<char>(c);
  }
  return trim(out);
}

bool path_is_within(const fs::path& child, const fs::path& parent) {
  std::error_code ec;
  auto norm = [](const fs::path& p) {
    std::error_code e;
    fs::path c = fs::weakly_canonical(p, e);
    return e ? p.lexically_normal() : c;
  };
  const fs::path c = norm(child);
  const fs::path p = norm(parent);
  auto ci = c.begin();
  for (auto pi = p.begin(); pi != p.end(); ++pi, ++ci) {
    if (pi->empty()) continue;
    if (ci == c.end() || *ci != *pi) return false;
  }
  return true;
}

std::string url_scheme(const std::string& url) {
  const auto pos = url.find("://");
  if (pos == std::string::npos || pos == 0 || pos > 16) return "";
  return to_lower(url.substr(0, pos));
}

}  // namespace aimgr
