// VM base layer: identifiers, arenas, line internment, errors.
//
// Principle: There are no pointers in the data. Everything is addressed by
// indices (u32) - So dumping, journal, and recovery is all about copying
// arenas.
#pragma once

#include <cstdint>
#include <cstring>
#include <deque>
#include <charconv>
#include <concepts>
#include <cstdio>
#include <meta>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tz {

// --- lineage ---
//
// A string is assembled from parts: numbers are compiled through to_chars, the rest is as text.
// 
// Conclusion - fwrite/fprintf.

void append(std::string & out, std::string_view s) { out.append(s); }
void append(std::string & out, char const * s) { out.append(s); } // Otherwise, literature will go to bool.
void append(std::string & out, char c) { out.push_back(c); }
void append(std::string & out, bool b) { out.append(b ? "true" : "false"); }
void append(std::string & out, std::integral auto x) {
  char buf[24];
  out.append(buf, std::to_chars(buf, buf + sizeof buf, x).ptr);
}
void append(std::string & out, double x) { // the shortest record that is read back
  char buf[32];
  out.append(buf, std::to_chars(buf, buf + sizeof buf, x).ptr);
}

std::string cat(auto const &... xs) {
  std::string out;
  (append(out, xs), ...);
  return out;
}

// Line in quotation marks with screening (for test messages).
std::string in_quotes(std::string_view s) {
  std::string out = "\"";
  for(char c: s) {
    if(c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if(c == '\n') out += "\\n";
    else if(c == '\t') out += "\\t";
    else out += c;
  }
  return out + '"';
}

// Add spaces to the right to the width n (in bytes).
std::string pad(std::string s, std::size_t n) {
  if(s.size() < n) s.append(n - s.size(), ' ');
  return s;
}

void put(FILE * f, std::string_view s) { fwrite(s.data(), 1, s.size(), f); }

// Typed index: id< node_tag> and id< symbol_tag> are not mixed.
template<typename tag>
struct id {
  static constexpr uint32_t none_value = ~uint32_t{0};

  constexpr id() = default;
  constexpr explicit id(uint32_t v) : value(v) {}

  constexpr bool valid() const { return value != none_value; }
  constexpr explicit operator bool() const { return valid(); }
  constexpr auto operator<=>(id const &) const = default;
  uint32_t value = none_value;
};

// Arena: a dense array of objects of the same kind addressed by the index.
template<typename T, typename tag = T>
struct arena {
  using handle = id<tag>;

  handle add(T value) {
    items.push_back(std::move(value));
    return handle(uint32_t(items.size() - 1));
  }

  T & operator[](handle h) pre(h.value < items.size()) { return items[h.value]; }
  T const & operator[](handle h) const pre(h.value < items.size()) { return items[h.value]; }

  uint32_t size() const { return uint32_t(items.size()); }
  auto begin() const { return items.begin(); }
  auto end() const { return items.end(); }
  void clear() { items.clear(); }
  // deque: ссылки на элементы стабильны при росте — база растёт и во время
  // performances (quote/emit generate node) as long as the
  std::deque<T> items;
};

// Internated line: Comparison - Comparison of numbers.
struct symbol_tag;
using symbol = id<symbol_tag>;

struct interner {
  symbol intern(std::string_view s) {
    if(auto it = index.find(std::string(s)); it != index.end()) return it->second;
    symbol sym(uint32_t(strings.size()));
    strings.emplace_back(s);
    index.emplace(strings.back(), sym);
    return sym;
  }

  uint32_t size() const { return uint32_t(strings.size()); }

  std::string_view text(symbol s) const {
    return s.valid() ? std::string_view(strings[s.value]) : std::string_view("<none>");
  }
  std::deque<std::string> strings; // Line addresses are stable: string_view from text() does not hang when growing
  std::unordered_map<std::string, symbol> index;
};

// Position in the text projection (for diagnostics and feedback with the editor).
struct span {
  uint32_t begin = 0;
  uint32_t end = 0;
  uint32_t source = 0; // text import number in the database (adjustments - new imports)
};

// Type of error: instantiation (name, overload, type - "not compiled") or runtime
// (boundary, division by zero). SFINAE-block try_compile discards only the former:
// .clang-format off
//
//
enum class diag_kind : uint8_t { instantiation, runtime };
// clang-format on

// Diagnosis is a track, not a single line.
struct diagnostic : std::runtime_error {
  struct frame {
    std::string what;
    span where;
  };

  diagnostic(std::string message, span where, diag_kind kind = diag_kind::instantiation)
      : std::runtime_error(message), where(where), kind(kind) {}

  diagnostic & note(std::string what, span where = {}) {
    trace.push_back({std::move(what), where});
    return *this;
  }

  span where;
  diag_kind kind;
  std::vector<frame> trace;
  std::string type; // runtime - VM error as language exception: out_of_bounds, div_by_zero
  bool fixed = false; // parsing: Violation known at compilation (require) - error, no exception
  bool hook = false; // from hook parsing [[g(…)]]: visible only parsing - stops launch
};

// The line / column for span - only when output to a person.
struct line_col {
  uint32_t line = 1;
  uint32_t col = 1;
};

line_col locate(std::string_view text, uint32_t offset) {
  line_col lc;
  for(uint32_t i = 0; i < offset && i < text.size(); ++i) {
    if(text[i] == '\n') {
      ++lc.line;
      lc.col = 1;
    } else ++lc.col;
  }
  return lc;
}

std::string render(diagnostic const & d, std::string_view file, std::string_view text) {
  auto lc = locate(text, d.where.begin);
  auto out = cat(file, ":", lc.line, ":", lc.col, ": ошибка: ", d.what(), "\n");
  for(auto const & f: d.trace) {
    auto l = locate(text, f.where.begin);
    out += cat("  ", file, ":", l.line, ":", l.col, ": ", f.what, "\n");
  }
  return out;
}

// --- Reflection: one implementation for all aggregates ---

template<typename T>
consteval auto fields_of() {
  return std::define_static_array(
      std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked()));
}

} // namespace tz
