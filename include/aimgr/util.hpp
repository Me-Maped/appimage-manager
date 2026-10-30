#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace aimgr {
namespace fs = std::filesystem;

enum class LogLevel { Debug, Info, Warn, Error };

/// Minimal logger: the CLI prints to stderr/stdout, the GUI pushes lines into a
/// ring buffer. Manager never writes to stdio directly so both front-ends can
/// share the exact same code path.
struct Logger {
  std::function<void(LogLevel, const std::string&)> sink;
  void log(LogLevel l, const std::string& m) const { if (sink) sink(l, m); }
  void debug(const std::string& m) const { log(LogLevel::Debug, m); }
  void info(const std::string& m) const { log(LogLevel::Info, m); }
  void warn(const std::string& m) const { log(LogLevel::Warn, m); }
  void error(const std::string& m) const { log(LogLevel::Error, m); }
};

/// Byte-size prettifier: 1234567 -> "1.18 MiB".
std::string format_size(uint64_t bytes);

/// ISO-8601 UTC timestamp, second precision: 2025-10-30T09:59:59Z
std::string now_iso();
/// Compact UTC stamp usable inside file names: 20251030T095959Z
std::string utc_stamp();

/// Expands a leading "~". Environment variables are deliberately not expanded
/// (tilde is what people actually type in config files).
fs::path expand_tilde(const fs::path& path);

bool ensure_dir(const fs::path& dir, std::string* err = nullptr);
bool read_file(const fs::path& path, std::string& out, std::string* err = nullptr);
/// Write `data` to `path` through a sibling temp file + fsync + rename so a
/// crash never leaves a half-written config/journal behind.
bool write_file_atomic(const fs::path& path, const std::string& data,
                       std::string* err = nullptr, int mode = 0644);
/// Copy `src` to `dst` atomically (temp + rename). Cross-device safe.
bool copy_file_atomic(const fs::path& src, const fs::path& dst,
                      std::string* err = nullptr, int mode = 0755);
bool path_exists(const fs::path& p);
bool path_is_regular(const fs::path& p);
bool path_is_symlink(const fs::path& p);
uint64_t path_size(const fs::path& p);
uint64_t path_inode(const fs::path& p);

std::string to_lower(std::string s);
std::string trim(std::string s);
std::string slugify(const std::string& s);
bool starts_with(const std::string& s, const std::string& p);
bool ends_with(const std::string& s, const std::string& p);
std::vector<std::string> split(const std::string& s, char sep);
std::string join(const std::vector<std::string>& v, const std::string& sep);
std::string errno_str();

/// Exclusive advisory lock guarding one config/journal mutation at a time.
struct FileLock {
  int fd = -1;
  bool acquire(const fs::path& path, std::string* err = nullptr);
  void release();
  ~FileLock() { release(); }
  FileLock() = default;
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
};

/// First existing directory out of $HOME/Applications, ~/Apps, ~/.local/bin.
fs::path default_install_dir();

/// True when the file looks like an AppImage: ELF magic plus the "AI\x01/02"
/// AppImage type marker at offset 8.  Fills `why` with the reason on failure.
bool looks_like_appimage(const fs::path& path, std::string* why = nullptr);

/// Reads the first bytes of `path` as hex (for logs/diagnostics).
std::string head_bytes_hex(const fs::path& path, size_t n = 12);

// ---------------------------------------------------------------- hardening
// Everything below exists so that attacker-influenced strings (asset names,
// release tags, ids from config files) can never escape the directory they are
// meant to live in, inject terminal escapes into logs, or break out of a
// .desktop Exec= field.

/// True for a handle that is safe to use as a path component and directory
/// name: 1..64 chars of [A-Za-z0-9._-], not "." / "..", must not start with '-'.
bool is_valid_id(const std::string& id);

/// Coerces an arbitrary string into a single safe path component.
std::string sanitize_component(const std::string& s, size_t max_len = 128);

/// Removes control characters (including ANSI escapes) so remote data cannot
/// drive the user's terminal.
std::string strip_control(const std::string& s);

/// Quotes a path for a .desktop `Exec=` line (and rejects control characters).
std::string escape_desktop_exec(const std::string& path);

/// Collapses an arbitrary string into one safe .desktop value line: CR/LF and
/// tabs become spaces and other control characters are dropped. Used for
/// Name=, Comment=, Icon=, Categories= and extra Exec arguments.
std::string sanitize_desktop_value(const std::string& s);

/// True when `child` resolves to a location inside `parent` (both may be
/// partially non-existent; lexical normalisation is used for the tail).
bool path_is_within(const fs::path& child, const fs::path& parent);

/// "https", "http", "file", ... or "" when the string has no scheme.
std::string url_scheme(const std::string& url);
}  // namespace aimgr
