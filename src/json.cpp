#include "aimgr/json.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace aimgr::json {

Value Value::object() {
  Value v;
  v.type = T::Obj;
  return v;
}
Value Value::array() {
  Value v;
  v.type = T::Arr;
  return v;
}
Value Value::make_str(std::string s) {
  Value v;
  v.type = T::Str;
  v.str = std::move(s);
  return v;
}
Value Value::make_num(double d) {
  Value v;
  v.type = T::Num;
  v.num = d;
  return v;
}
Value Value::make_bool(bool b) {
  Value v;
  v.type = T::Bool;
  v.b = b;
  return v;
}
Value Value::make_null() { return Value(); }

size_t Value::size() const {
  if (type == T::Arr) return arr.size();
  if (type == T::Obj) return obj.size();
  return 0;
}

const Value& Value::at(size_t i) const {
  static const Value empty;
  if (type == T::Arr && i < arr.size()) return arr[i];
  return empty;
}

const Value* Value::find(std::string_view key) const {
  if (type != T::Obj) return nullptr;
  for (const auto& kv : obj)
    if (kv.first == key) return &kv.second;
  return nullptr;
}

Value& Value::set(std::string_view key, Value v) {
  if (type != T::Obj) {
    type = T::Obj;
    obj.clear();
  }
  for (auto& kv : obj) {
    if (kv.first == key) {
      kv.second = std::move(v);
      return kv.second;
    }
  }
  obj.emplace_back(std::string(key), std::move(v));
  return obj.back().second;
}

Value& Value::push(Value v) {
  if (type != T::Arr) {
    type = T::Arr;
    arr.clear();
  }
  arr.push_back(std::move(v));
  return arr.back();
}

std::string Value::get_str(std::string_view key, std::string def) const {
  const Value* v = find(key);
  return (v && v->is_str()) ? v->str : def;
}
double Value::get_num(std::string_view key, double def) const {
  const Value* v = find(key);
  if (!v) return def;
  if (v->is_num()) return v->num;
  return def;
}
bool Value::get_bool(std::string_view key, bool def) const {
  const Value* v = find(key);
  return (v && v->type == T::Bool) ? v->b : def;
}

namespace {
struct Parser {
  std::string_view s;
  size_t i = 0;
  std::string err = {};
  int depth = 0;

  bool eof() const { return i >= s.size(); }
  char peek() const { return i < s.size() ? s[i] : '\0'; }

  void ws() {
    while (i < s.size()) {
      char c = s[i];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
        ++i;
      else
        break;
    }
  }
  bool fail(const std::string& msg) {
    if (err.empty()) err = msg + " at offset " + std::to_string(i);
    return false;
  }
  static void put_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
      out += char(cp);
    } else if (cp < 0x800) {
      out += char(0xC0 | (cp >> 6));
      out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += char(0xE0 | (cp >> 12));
      out += char(0x80 | ((cp >> 6) & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    } else {
      out += char(0xF0 | (cp >> 18));
      out += char(0x80 | ((cp >> 12) & 0x3F));
      out += char(0x80 | ((cp >> 6) & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    }
  }
  bool parse_string(std::string& out) {
    if (peek() != '"') return fail("expected string");
    ++i;
    out.clear();
    while (i < s.size()) {
      char c = s[i++];
      if (c == '"') return true;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (eof()) return fail("unterminated escape");
      char e = s[i++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          if (i + 4 > s.size()) return fail("truncated \\u escape");
          uint32_t cp = 0;
          for (int k = 0; k < 4; ++k) {
            char h = s[i++];
            cp <<= 4;
            if (h >= '0' && h <= '9') cp |= uint32_t(h - '0');
            else if (h >= 'a' && h <= 'f') cp |= uint32_t(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') cp |= uint32_t(h - 'A' + 10);
            else return fail("bad \\u escape");
          }
          // surrogate pair
          if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
            size_t save = i;
            i += 2;
            uint32_t lo = 0;
            bool ok = true;
            for (int k = 0; k < 4; ++k) {
              char h = s[i++];
              lo <<= 4;
              if (h >= '0' && h <= '9') lo |= uint32_t(h - '0');
              else if (h >= 'a' && h <= 'f') lo |= uint32_t(h - 'a' + 10);
              else if (h >= 'A' && h <= 'F') lo |= uint32_t(h - 'A' + 10);
              else { ok = false; break; }
            }
            if (ok && lo >= 0xDC00 && lo <= 0xDFFF)
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            else
              i = save;
          }
          put_utf8(out, cp);
          break;
        }
        default: return fail("unknown escape");
      }
    }
    return fail("unterminated string");
  }
  bool parse_number(Value& v) {
    size_t start = i;
    if (peek() == '-') ++i;
    while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.' ||
                            s[i] == 'e' || s[i] == 'E' || s[i] == '+' || s[i] == '-'))
      ++i;
    if (start == i) return fail("bad number");
    std::string lit(s.substr(start, i - start));
    char* end = nullptr;
    double d = std::strtod(lit.c_str(), &end);
    if (end == lit.c_str()) return fail("bad number literal '" + lit + "'");
    v.type = Value::T::Num;
    v.num = d;
    return true;
  }
  bool parse_value(Value& v) {
    if (++depth > 64) return fail("nesting too deep");
    ws();
    if (eof()) return fail("unexpected end of input");
    char c = peek();
    bool ok = true;
    if (c == '{') {
      ++i;
      v.type = Value::T::Obj;
      ws();
      if (peek() == '}') {
        ++i;
        --depth;
        return true;
      }
      for (;;) {
        ws();
        std::string key;
        if (!parse_string(key)) { ok = false; break; }
        ws();
        if (peek() != ':') { ok = fail("expected ':'"); break; }
        ++i;
        Value child;
        if (!parse_value(child)) { ok = false; break; }
        v.obj.emplace_back(std::move(key), std::move(child));
        ws();
        if (peek() == ',') { ++i; continue; }
        if (peek() == '}') { ++i; break; }
        ok = fail("expected ',' or '}'");
        break;
      }
    } else if (c == '[') {
      ++i;
      v.type = Value::T::Arr;
      ws();
      if (peek() == ']') {
        ++i;
        --depth;
        return true;
      }
      for (;;) {
        Value child;
        if (!parse_value(child)) { ok = false; break; }
        v.arr.push_back(std::move(child));
        ws();
        if (peek() == ',') { ++i; continue; }
        if (peek() == ']') { ++i; break; }
        ok = fail("expected ',' or ']'");
        break;
      }
    } else if (c == '"') {
      v.type = Value::T::Str;
      ok = parse_string(v.str);
    } else if (c == 't' && s.substr(i, 4) == "true") {
      i += 4;
      v.type = Value::T::Bool;
      v.b = true;
    } else if (c == 'f' && s.substr(i, 5) == "false") {
      i += 5;
      v.type = Value::T::Bool;
      v.b = false;
    } else if (c == 'n' && s.substr(i, 4) == "null") {
      i += 4;
      v.type = Value::T::Null;
    } else {
      ok = parse_number(v);
    }
    --depth;
    return ok;
  }
};
}  // namespace

bool parse(std::string_view text, Value& out, std::string& err) {
  Parser p{text};
  if (!p.parse_value(out)) {
    err = p.err;
    return false;
  }
  p.ws();
  if (!p.eof()) {
    err = "trailing characters at offset " + std::to_string(p.i);
    return false;
  }
  return true;
}

std::string escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

namespace {
void dump_into(const Value& v, int indent, int level, std::string& out) {
  const bool pretty = indent >= 0;
  auto pad = [&](int lvl) {
    if (!pretty) return;
    out += '\n';
    if (lvl > 0 && indent > 0)
      out.append(static_cast<size_t>(lvl) * static_cast<size_t>(indent), ' ');
  };
  switch (v.type) {
    case Value::T::Null: out += "null"; break;
    case Value::T::Bool: out += v.b ? "true" : "false"; break;
    case Value::T::Num: {
      double d = v.num;
      if (d == std::floor(d) && std::fabs(d) < 1e15) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(d));
        out += buf;
      } else {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.17g", d);
        out += buf;
      }
      break;
    }
    case Value::T::Str: out += '"'; out += escape(v.str); out += '"'; break;
    case Value::T::Arr:
      if (v.arr.empty()) { out += "[]"; break; }
      out += '[';
      for (size_t i = 0; i < v.arr.size(); ++i) {
        if (i) out += ',';
        pad(level + 1);
        dump_into(v.arr[i], indent, level + 1, out);
      }
      pad(level);
      out += ']';
      break;
    case Value::T::Obj:
      if (v.obj.empty()) { out += "{}"; break; }
      out += '{';
      for (size_t i = 0; i < v.obj.size(); ++i) {
        if (i) out += ',';
        pad(level + 1);
        out += '"';
        out += escape(v.obj[i].first);
        out += "\": ";
        dump_into(v.obj[i].second, indent, level + 1, out);
      }
      pad(level);
      out += '}';
      break;
  }
}
}  // namespace

std::string dump(const Value& v, int indent) {
  std::string out;
  dump_into(v, indent, 0, out);
  if (indent >= 0) out += '\n';
  return out;
}

}  // namespace aimgr::json
