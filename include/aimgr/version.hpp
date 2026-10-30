#pragma once
#include <string>

namespace aimgr {

/// Strips a leading "v"/"V" and surrounding whitespace.
std::string normalize_version(std::string v);

/// Semver-ish comparison that also copes with date versions (2025.09.21),
/// build suffixes (1.2.3-rc1, 1.2.3+build7) and non-numeric garbage.
/// Returns -1, 0 or 1.
int compare_versions(const std::string& a, const std::string& b);

/// True when `latest` should be installed over `installed`. An empty
/// `installed` always counts as "newer".
bool is_newer(const std::string& latest, const std::string& installed);

/// Extracts the first match of `regex` from `text` (capture group 1 when the
/// pattern has one). `ok` reports whether a match happened.
std::string extract_version(const std::string& text, const std::string& regex, bool* ok = nullptr);

/// Removes a configured tag prefix ("v", "release-") from a git tag.
std::string strip_tag_prefix(const std::string& tag, const std::string& prefix);

}  // namespace aimgr
