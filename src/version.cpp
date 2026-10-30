#include "aimgr/version.hpp"

#include <strings.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <regex>
#include <vector>

namespace aimgr {
namespace {

bool all_digits(const std::string& s) {
  if (s.empty()) return false;
  return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}

/// Splits "v1.2.3-rc1+b7" into {"1","2","3","rc1"} - separators are dropped and
/// trailing build metadata after '+' is ignored.
std::vector<std::string> tokenize(const std::string& raw) {
  std::string v = raw;
  auto plus = v.find('+');
  if (plus != std::string::npos) v = v.substr(0, plus);
  std::vector<std::string> out;
  std::string cur;
  for (unsigned char c : v) {
    if (std::isalnum(c)) {
      cur += static_cast<char>(c);
    } else if (!cur.empty()) {
      out.push_back(cur);
      cur.clear();
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

int cmp_numeric(const std::string& a, const std::string& b) {
  size_t ai = a.find_first_not_of('0');
  size_t bi = b.find_first_not_of('0');
  std::string av = (ai == std::string::npos) ? "0" : a.substr(ai);
  std::string bv = (bi == std::string::npos) ? "0" : b.substr(bi);
  if (av.size() != bv.size()) return av.size() < bv.size() ? -1 : 1;
  int c = av.compare(bv);
  if (c == 0) return 0;
  return c < 0 ? -1 : 1;
}

}  // namespace

std::string normalize_version(std::string v) {
  v.erase(std::remove_if(v.begin(), v.end(), [](unsigned char c) { return std::isspace(c); }),
          v.end());
  if (!v.empty() && (v[0] == 'v' || v[0] == 'V') && v.size() > 1 &&
      std::isdigit(static_cast<unsigned char>(v[1])))
    v.erase(v.begin());
  return v;
}

int compare_versions(const std::string& a, const std::string& b) {
  const auto A = tokenize(normalize_version(a));
  const auto B = tokenize(normalize_version(b));
  const size_t n = std::max(A.size(), B.size());
  for (size_t i = 0; i < n; ++i) {
    const bool has_a = i < A.size();
    const bool has_b = i < B.size();
    if (!has_a && !has_b) return 0;
    if (!has_a || !has_b) {
      // One side ran out. A trailing alphabetic token on the longer side means
      // it is a pre-release ("1.0.0-rc1" < "1.0.0"); trailing numeric zeros are
      // padding ("1.0" == "1.0.0").
      const bool rest_is_a = has_a;
      const std::string& t = rest_is_a ? A[i] : B[i];
      if (!all_digits(t)) return rest_is_a ? -1 : 1;
      long long val = std::strtoll(t.c_str(), nullptr, 10);
      if (val == 0) continue;
      return rest_is_a ? 1 : -1;
    }
    const std::string& x = A[i];
    const std::string& y = B[i];
    const bool nx = all_digits(x);
    const bool ny = all_digits(y);
    if (nx && ny) {
      int c = cmp_numeric(x, y);
      if (c) return c;
    } else if (nx != ny) {
      // numeric identifiers rank higher than alphanumeric ones (semver §11)
      return nx ? 1 : -1;
    } else {
      int c = ::strcasecmp(x.c_str(), y.c_str());
      if (c) return c < 0 ? -1 : 1;
    }
  }
  return 0;
}

bool is_newer(const std::string& latest, const std::string& installed) {
  if (normalize_version(latest).empty()) return false;
  if (normalize_version(installed).empty()) return true;
  return compare_versions(latest, installed) > 0;
}

std::string extract_version(const std::string& text, const std::string& regex, bool* ok) {
  if (ok) *ok = false;
  const std::string pattern = regex.empty() ? std::string("([0-9]+(?:\\.[0-9]+)+)") : regex;
  try {
    std::regex re(pattern, std::regex::ECMAScript);
    std::smatch m;
    if (std::regex_search(text, m, re)) {
      std::string out = (m.size() > 1 && m[1].matched) ? m[1].str() : m[0].str();
      if (ok) *ok = true;
      return out;
    }
  } catch (const std::regex_error&) {
    if (ok) *ok = false;
  }
  return "";
}

std::string strip_tag_prefix(const std::string& tag, const std::string& prefix) {
  if (!prefix.empty() && tag.rfind(prefix, 0) == 0) return tag.substr(prefix.size());
  return normalize_version(tag);
}

}  // namespace aimgr
