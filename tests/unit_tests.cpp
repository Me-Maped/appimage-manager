// Unit tests for the pure-logic pieces: version comparison, JSON, SHA-256 and
// GitHub repo parsing. No network, no filesystem writes.
#include <cstdio>
#include <string>
#include <vector>

#include "aimgr/json.hpp"
#include "aimgr/sha256.hpp"
#include "aimgr/source.hpp"
#include "aimgr/version.hpp"

static int g_fail = 0;
static int g_pass = 0;

#define EXPECT(cond)                                                              \
  do {                                                                            \
    if (cond) {                                                                   \
      ++g_pass;                                                                   \
    } else {                                                                      \
      ++g_fail;                                                                   \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);               \
    }                                                                             \
  } while (0)

#define EXPECT_EQ(a, b)                                                           \
  do {                                                                            \
    auto _a = (a);                                                                \
    auto _b = (b);                                                                \
    if (_a == _b) {                                                               \
      ++g_pass;                                                                   \
    } else {                                                                      \
      ++g_fail;                                                                   \
      std::printf("  FAIL %s:%d  %s == %s\n", __FILE__, __LINE__, #a, #b);        \
    }                                                                             \
  } while (0)

static void test_versions() {
  using aimgr::compare_versions;
  using aimgr::is_newer;

  EXPECT_EQ(compare_versions("1.0.0", "1.0.0"), 0);
  EXPECT_EQ(compare_versions("1.0.0", "1.0.1"), -1);
  EXPECT_EQ(compare_versions("1.0.1", "1.0.0"), 1);
  EXPECT_EQ(compare_versions("1.2", "1.2.0"), 0);          // padding zeros
  EXPECT_EQ(compare_versions("1.0.0", "1.0.0.0"), 0);
  EXPECT_EQ(compare_versions("v1.2.3", "1.2.3"), 0);        // tag prefix
  EXPECT_EQ(compare_versions("V2.0", "1.9.9"), 1);
  EXPECT_EQ(compare_versions("1.0.0-rc1", "1.0.0"), -1);    // pre-release is older
  EXPECT_EQ(compare_versions("1.0.0", "1.0.0-rc1"), 1);
  EXPECT_EQ(compare_versions("1.0.0-alpha", "1.0.0-beta"), -1);
  EXPECT_EQ(compare_versions("1.10.0", "1.9.0"), 1);        // numeric, not lexical
  EXPECT_EQ(compare_versions("2025.09.21", "2025.10.01"), -1);
  EXPECT_EQ(compare_versions("1.0.0+build7", "1.0.0"), 0);  // build metadata ignored
  EXPECT_EQ(compare_versions("2.0.0", "10.0.0"), -1);

  EXPECT(!is_newer("", "1.0.0"));
  EXPECT(is_newer("1.0.0", ""));
  EXPECT(is_newer("1.2.0", "1.1.9"));
  EXPECT(!is_newer("1.1.0", "1.1.0"));
  EXPECT(!is_newer("1.0.0", "1.0.1"));

  bool ok = false;
  EXPECT_EQ(aimgr::extract_version("Motrix-1.8.19.AppImage", "", &ok), "1.8.19");
  EXPECT(ok);
  EXPECT_EQ(aimgr::extract_version("App-2.0.0-beta-x86_64.AppImage",
                                   "App-([0-9.]+)-", &ok), "2.0.0");
  EXPECT(ok);
  aimgr::extract_version("no-version-here", "", &ok);
  EXPECT(!ok);
  // invalid regex must not throw
  aimgr::extract_version("1.2.3", "([0-9", &ok);
  EXPECT(!ok);

  EXPECT_EQ(aimgr::strip_tag_prefix("v1.2.3", "v"), "1.2.3");
  EXPECT_EQ(aimgr::strip_tag_prefix("release-4.1", "release-"), "4.1");
  EXPECT_EQ(aimgr::strip_tag_prefix("2.5.0", "v"), "2.5.0");
  EXPECT_EQ(aimgr::normalize_version("  v3.1.4  "), "3.1.4");
}

static void test_repo_parsing() {
  EXPECT_EQ(aimgr::parse_github_repo("acme/demo"), "acme/demo");
  EXPECT_EQ(aimgr::parse_github_repo("https://github.com/acme/demo"), "acme/demo");
  EXPECT_EQ(aimgr::parse_github_repo("https://github.com/acme/demo/"), "acme/demo");
  EXPECT_EQ(aimgr::parse_github_repo("https://github.com/acme/demo/releases/latest"), "acme/demo");
  EXPECT_EQ(aimgr::parse_github_repo("https://github.com/acme/demo/releases/tag/v1.2.3"), "acme/demo");
  EXPECT_EQ(aimgr::parse_github_repo("http://www.github.com/acme/demo.git"), "acme/demo");
  EXPECT_EQ(aimgr::parse_github_repo("git@github.com:acme/demo.git"), "acme/demo");
  EXPECT_EQ(aimgr::parse_github_repo("https://gitlab.com/acme/demo"), "");
  EXPECT_EQ(aimgr::parse_github_repo("acme"), "");
  EXPECT_EQ(aimgr::parse_github_repo(""), "");
  EXPECT_EQ(aimgr::parse_github_repo("acme/../etc"), "");
  EXPECT_EQ(aimgr::parse_github_repo("../acme/demo"), "");
  EXPECT_EQ(aimgr::parse_github_repo("acme/.hidden"), "");
  EXPECT_EQ(aimgr::parse_github_repo("acme/demo;rm"), "");
}

static void test_json() {
  const std::string src =
      R"({"a":1,"b":[true,false,null],"c":{"d":"x\ny","e":"\u4f60\u597d"},"f":-1.5})";
  aimgr::json::Value v;
  std::string err;
  EXPECT(aimgr::json::parse(src, v, err));
  EXPECT(v.is_obj());
  EXPECT_EQ(v.get_num("a", 0), 1.0);
  const aimgr::json::Value* b = v.find("b");
  EXPECT(b && b->is_arr() && b->size() == 3);
  EXPECT_EQ(v.get_str("c"), "");  // wrong type -> default
  const aimgr::json::Value* c = v.find("c");
  EXPECT(c && c->get_str("d") == "x\ny");
  EXPECT(c && c->get_str("e") == "你好");

  // round-trip
  const std::string dumped = aimgr::json::dump(v, -1);
  aimgr::json::Value again;
  EXPECT(aimgr::json::parse(dumped, again, err));
  EXPECT_EQ(again.get_num("f", 0), -1.5);

  // malformed input is rejected, not fatal
  aimgr::json::Value bad;
  EXPECT(!aimgr::json::parse("{\"a\": }", bad, err));
  EXPECT(!err.empty());
  EXPECT(!aimgr::json::parse("[1,2", bad, err));
  EXPECT(!aimgr::json::parse("", bad, err));

  // writing
  aimgr::json::Value out = aimgr::json::Value::object();
  out.set("k", aimgr::json::Value::make_str("va\"lue"));
  out.set("n", aimgr::json::Value::make_num(42));
  const std::string text = aimgr::json::dump(out, 0);
  EXPECT(text.find("va\\\"lue") != std::string::npos);
}

static void test_sha256() {
  EXPECT_EQ(aimgr::Sha256::hex_of("abc", 3),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(aimgr::Sha256::hex_of("", 0),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  // one million 'a' (streaming through many blocks)
  std::string big(1000000, 'a');
  EXPECT_EQ(aimgr::Sha256::hex_of(big.data(), big.size()),
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

static void test_hardening_helpers() {
  using namespace aimgr;
  // ---- ids become directory names, so traversal must be impossible
  EXPECT(is_valid_id("motrix"));
  EXPECT(is_valid_id("a.b_c-1"));
  EXPECT(!is_valid_id(""));
  EXPECT(!is_valid_id("."));
  EXPECT(!is_valid_id(".."));
  EXPECT(!is_valid_id("../etc"));
  EXPECT(!is_valid_id("a/b"));
  EXPECT(!is_valid_id("-rf"));
  EXPECT(!is_valid_id(".hidden"));
  EXPECT(!is_valid_id(std::string(65, 'a')));
  EXPECT(is_valid_id("release-2025.01_1"));

  // ---- arbitrary strings -> single path component
  const std::string evil = sanitize_component("../../etc/passwd");
  EXPECT(evil.find('/') == std::string::npos);
  EXPECT(evil.empty() || evil[0] != '.');
  EXPECT_EQ(sanitize_component("Demo 2.0.AppImage"), "Demo_2.0.AppImage");
  EXPECT_EQ(sanitize_component(""), "unnamed");
  EXPECT_EQ(sanitize_component("-leading"), "leading");

  // ---- terminal escape / control characters from remote data
  EXPECT_EQ(strip_control("a\x1b[31mb\nc"), "a[31mbc");
  EXPECT_EQ(strip_control("ok\ttext"), "ok\ttext");

  // ---- .desktop Exec escaping
  EXPECT_EQ(escape_desktop_exec("/tmp/a b/\"q\".AppImage"),
            "\"/tmp/a b/\\\"q\\\".AppImage\"");
  EXPECT_EQ(escape_desktop_exec("/tmp/x"), "\"/tmp/x\"");
  EXPECT_EQ(escape_desktop_exec("/tmp/a%Ub.AppImage"), "\"/tmp/a%%Ub.AppImage\"");
  EXPECT(escape_desktop_exec("/tmp/$(rm -rf ~)").find('$') != std::string::npos);

  // ---- .desktop value fields can never introduce a second line
  EXPECT_EQ(sanitize_desktop_value("Grab\nExec=/bin/sh"), "Grab Exec=/bin/sh");
  EXPECT_EQ(sanitize_desktop_value("  a\t\t b  "), "a b");
  EXPECT_EQ(sanitize_desktop_value(std::string("a\x1b[31mb\rc")), "a[31mb c");

  // ---- path containment used by rollback/recovery guards
  EXPECT(path_is_within("/a/b/c", "/a/b"));
  EXPECT(path_is_within("/a/b/c/d", "/a/b"));
  EXPECT(!path_is_within("/a/bc", "/a/b"));
  EXPECT(!path_is_within("/tmp/evil", "/home/u/.local/share/aimgr/backups"));

  // ---- URL schemes
  EXPECT_EQ(url_scheme("https://github.com/x"), "https");
  EXPECT_EQ(url_scheme("HTTP://example.com"), "http");
  EXPECT_EQ(url_scheme("file:///etc/passwd"), "file");
  EXPECT_EQ(url_scheme("no-scheme-here"), "");
}

int main() {
  std::printf("security helpers\n");
  test_hardening_helpers();
  std::printf("version comparison\n");
  test_versions();
  std::printf("github repo parsing\n");
  test_repo_parsing();
  std::printf("json\n");
  test_json();
  std::printf("sha256\n");
  test_sha256();
  std::printf("\npassed: %d   failed: %d\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
