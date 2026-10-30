#pragma once
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aimgr::json {

/// Small, dependency-free JSON value + parser.  Deliberately narrow: it exists
/// so the project builds with nothing but libcurl, and it is only ever fed
/// data we either wrote ourselves (config, journal) or that GitHub returned.
class Value {
 public:
  enum class T { Null, Bool, Num, Str, Arr, Obj };

  T type = T::Null;
  bool b = false;
  double num = 0;
  std::string str;
  std::vector<Value> arr;
  std::vector<std::pair<std::string, Value>> obj;

  Value() = default;
  static Value object();
  static Value array();
  static Value make_str(std::string s);
  static Value make_num(double d);
  static Value make_bool(bool v);
  static Value make_null();

  bool is_null() const { return type == T::Null; }
  bool is_obj() const { return type == T::Obj; }
  bool is_arr() const { return type == T::Arr; }
  bool is_str() const { return type == T::Str; }
  bool is_num() const { return type == T::Num; }

  size_t size() const;
  const Value& at(size_t i) const;

  const Value* find(std::string_view key) const;
  Value& set(std::string_view key, Value v);
  Value& push(Value v);

  std::string get_str(std::string_view key, std::string def = "") const;
  double get_num(std::string_view key, double def = 0) const;
  bool get_bool(std::string_view key, bool def = false) const;
};

bool parse(std::string_view text, Value& out, std::string& err);
std::string dump(const Value& v, int indent = 2);
std::string escape(const std::string& s);

}  // namespace aimgr::json
