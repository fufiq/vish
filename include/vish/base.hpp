// VM base layer: identifiers, arenas, line internment, errors.
//
// Principle: There are no pointers in the data. Everything is addressed by
// indices (u32) - So dumping, journal, and recovery is all about copying
// arenas.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
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

namespace vish {

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

// VM bands (§9.5, step 12.3b): monopolistic process and background - in different host threads; they
// have a common code base, lines, types, level memory, threads between processes. Protection is
// included with the first background band: without processes, the path is the same, there are no locks.
std::atomic<bool> lanes_on{false};
struct lane_guard {
  std::recursive_mutex & m;
  bool on;
  explicit lane_guard(std::recursive_mutex & m) : m(m), on(lanes_on.load(std::memory_order_acquire)) {
    if(on) m.lock();
  }
  ~lane_guard() {
    if(on) m.unlock();
  }
  lane_guard(lane_guard const &) = delete;
};

// Arena: a dense array of objects of the same kind addressed by the index. Pieces of 4096 - the catalog
// is set in advance and does not move, elements too: you can read without a lock, while another band
// adds (the base grows and during execution: quote / emit), the calling links do not hang.
template<typename T, typename tag = T>
struct arena {
  using handle = id<tag>;
  static constexpr uint32_t chunk_bits = 12, chunk_size = 1u << chunk_bits, max_chunks = 1u << 16;

  arena() = default;
  arena(arena const &) = delete;
  ~arena() { clear(); }

  handle add(T value) {
    lane_guard g(grow);
    uint32_t n = count.load(std::memory_order_relaxed);
    if((n & (chunk_size - 1)) == 0) {
      if(chunks.capacity() == 0) chunks.reserve(max_chunks); // catalogue doesn't move
      chunks.push_back(static_cast<T *>(::operator new(sizeof(T) * chunk_size)));
    }
    new(chunks[n >> chunk_bits] + (n & (chunk_size - 1))) T(std::move(value));
    count.store(n + 1, std::memory_order_release);
    return handle(n);
  }

  T & operator[](handle h) pre(h.value < size()) { return chunks[h.value >> chunk_bits][h.value & (chunk_size - 1)]; }
  T const & operator[](handle h) const pre(h.value < size()) { return chunks[h.value >> chunk_bits][h.value & (chunk_size - 1)]; }

  uint32_t size() const { return count.load(std::memory_order_acquire); }
  void clear() {
    for(uint32_t i = 0, n = size(); i < n; ++i) (*this)[handle(i)].~T();
    for(T * c: chunks) ::operator delete(c);
    chunks.clear();
    count.store(0, std::memory_order_release);
  }
  std::vector<T *> chunks;
  std::atomic<uint32_t> count{0};
  std::recursive_mutex grow;
};

// Internated line: Comparison - Comparison of numbers.
struct symbol_tag;
using symbol = id<symbol_tag>;

// Hash strings without a copy of the key: search string_view does not allocate memory (hot path sym()).
struct text_hash {
  using is_transparent = void;
  size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
};

struct interner {
  symbol intern(std::string_view s) {
    lane_guard g(lock); // search and paste - under the lock of stripes (reading text - without it)
    if(auto it = index.find(s); it != index.end()) return it->second;
    symbol sym(strings.size());
    auto h = strings.add(std::string(s));
    index.emplace(strings[h], sym);
    return sym;
  }

  uint32_t size() const { return strings.size(); }

  std::string_view text(symbol s) const {
    return s.valid() ? std::string_view(strings[id<string_tag>(s.value)]) : std::string_view("<none>");
  }
  struct string_tag;
  arena<std::string, string_tag> strings; // Line addresses are stable: string_view from text() does not hang when growing
  std::unordered_map<std::string, symbol, text_hash, std::equal_to<>> index;
  std::recursive_mutex lock;
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
  for(std::size_t i = 0; i < d.trace.size();) { // same frames in a row - one line
    auto const & f = d.trace[i];
    std::size_t n = 1;
    while(i + n < d.trace.size() && d.trace[i + n].what == f.what && d.trace[i + n].where.begin == f.where.begin) ++n;
    auto l = locate(text, f.where.begin);
    out += cat("  ", file, ":", l.line, ":", l.col, ": ", f.what, n > 1 ? cat(" ×", n) : std::string(), "\n");
    i += n;
  }
  return out;
}

// --- Reflection: one implementation for all aggregates ---

template<typename T>
consteval auto fields_of() {
  return std::define_static_array(
      std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked()));
}

} // namespace vish
