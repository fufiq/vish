// VM base layer: identifiers, arenas, line internment, errors.
//
// Principle: There are no pointers in the data. Everything is addressed by
// indices (u32) - So dumping, journal, and recovery is all about copying
// arenas.
#pragma once

#include <cstdint>
#include <cstring>
#include <format>
#include <meta>
#include <print>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tz {

// Typed index: id< node_tag> and id< symbol_tag> are not mixed.
template <class tag>
class id {
public:
    static constexpr uint32_t none_value = ~uint32_t{0};

    constexpr id() = default;
    constexpr explicit id(uint32_t v) : value_(v) {}

    constexpr uint32_t  value() const { return value_; }
    constexpr bool valid() const { return value_ != none_value; }
    constexpr explicit operator bool() const { return valid(); }
    constexpr auto operator<=>(const id&) const = default;

private:
    uint32_t value_ = none_value;
};

// Arena: a dense array of objects of the same kind addressed by the index.
template <class T, class tag = T>
class arena {
public:
    using handle = id<tag>;

    handle add(T value) {
        items_.push_back(std::move(value));
        return handle(uint32_t(items_.size() - 1));
    }

    T&       operator[](handle h)       pre(h.value() < items_.size()) { return items_[h.value()]; }
    const T& operator[](handle h) const pre(h.value() < items_.size()) { return items_[h.value()]; }

    uint32_t  size() const { return uint32_t(items_.size()); }
    auto begin() const { return items_.begin(); }
    auto end() const { return items_.end(); }
    void clear() { items_.clear(); }

private:
    std::vector<T> items_;
};

// Internated line: Comparison - Comparison of numbers.
struct symbol_tag;
using symbol = id<symbol_tag>;

class interner {
public:
    symbol intern(std::string_view s) {
        if (auto it = index_.find(std::string(s)); it != index_.end()) return it->second;
        symbol sym(uint32_t(strings_.size()));
        strings_.emplace_back(s);
        index_.emplace(strings_.back(), sym);
        return sym;
    }

    uint32_t size() const { return uint32_t(strings_.size()); }

    std::string_view text(symbol s) const {
        return s.valid() ? std::string_view(strings_[s.value()]) : std::string_view("<none>");
    }

private:
    std::vector<std::string> strings_;
    std::unordered_map<std::string, symbol> index_;
};

// Position in the text projection (for diagnostics and feedback with the editor).
struct span {
    uint32_t begin  = 0;
    uint32_t end    = 0;
    uint32_t source = 0; // text import number in the database (adjustments - new imports)
};

// Type of error: instantiation (name, overload, type - "not compiled") or runtime
// (boundary, division by zero). SFINAE-block try_compile discards only the first:
//
//
enum class diag_kind : uint8_t { instantiation, runtime };

// Diagnosis is a track, not a single line.
class diagnostic : public std::runtime_error {
public:
    struct frame {
        std::string what;
        span        where;
    };

    diagnostic(std::string message, span where, diag_kind kind = diag_kind::instantiation)
        : std::runtime_error(message), where_(where), kind_(kind) {}

    diag_kind kind() const { return kind_; }

    diagnostic& note(std::string what, span where = {}) {
        trace_.push_back({std::move(what), where});
        return *this;
    }

    span                      where() const { return where_; }
    const std::vector<frame>& trace() const { return trace_; }

private:
    span               where_;
    diag_kind          kind_;
    std::vector<frame> trace_;
};

// The line / column for span - only when output to a person.
struct line_col {
    uint32_t line = 1;
    uint32_t col  = 1;
};

inline line_col locate(std::string_view text, uint32_t offset) {
    line_col lc;
    for (uint32_t i = 0; i < offset && i < text.size(); ++i) {
        if (text[i] == '\n') { ++lc.line; lc.col = 1; }
        else ++lc.col;
    }
    return lc;
}

inline std::string render(const diagnostic& d, std::string_view file, std::string_view text) {
    auto lc  = locate(text, d.where().begin);
    auto out = std::format("{}:{}:{}: ошибка: {}\n", file, lc.line, lc.col, d.what());
    for (const auto& f : d.trace()) {
        auto l = locate(text, f.where.begin);
        out += std::format("  {}:{}:{}: {}\n", file, l.line, l.col, f.what);
    }
    return out;
}

// --- Reflection: one implementation for all aggregates ---

template <class T>
consteval auto fields_of() {
    return std::define_static_array(
        std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked()));
}

} // namespace tz
