// The meaning of the interpreter.
//
// Value type - tag (§5.1, D4): numerical id in a closed world VM. Aggregates
// with a name - nominal, anonymous - in structure.
#pragma once

#include "ast.hpp"

#include <memory>

namespace tz {

struct type_tag;
using type_id = id<type_tag>;

struct value;
struct scope;

struct unit_v {};
struct type_v { type_id type; };
// Array. Uniform numbers are stored tightly (i64/f64 is a real buffer, it works with
// both the interpreter and the native JIT code without copying); the rest is a general
// list of values, but the layout must be honest.
//
//
// No reference counters. "" There's no copying where it's thrown out by parsing: the
// argument-place is transmitted by the borrowed view of the caller's cell. The loan does
// not own and lives no longer than the call; a copy or record of the loan array makes its
// own copy.
//
class array_v {
public:
    enum class layout : u8 { generic, ints, floats };

    array_v() = default;
    array_v(const array_v& o) { *this = o; }
    array_v(array_v&& o) noexcept = default;
    array_v& operator=(const array_v& o) {
        if (this == &o) return *this;
        const array_v& src = o.borrowed_ ? *o.borrowed_ : o;
        layout_   = src.layout_;
        items_    = src.items_;
        ints_     = src.ints_;
        floats_   = src.floats_;
        borrowed_ = nullptr;
        return *this;
    }
    array_v& operator=(array_v&& o) noexcept = default;

    static array_v of(std::vector<value> items);
    static array_v borrow(const array_v& cell) { // caller
        array_v a;
        a.borrowed_ = cell.borrowed_ ? cell.borrowed_ : &cell;
        return a;
    }

    u32    size() const;
    bool   empty() const { return size() == 0; }
    value  get(u32 i) const;
    void   set(u32 i, value v);
    void   push(value v);
    layout kind() const { return r().layout_; }

    const i64* ints() const { return r().ints_.data(); }
    const f64* floats() const { return r().floats_.data(); }
    i64*       ints_mut() { own(); return ints_.data(); }
    f64*       floats_mut() { own(); return floats_.data(); }
    value*     generic_cell(u32 i) {
        if (kind() != layout::generic) return nullptr;
        own();
        return &items_[i];
    }

private:
    const array_v& r() const { return borrowed_ ? *borrowed_ : *this; }
    void           own() {
        if (borrowed_) *this = *borrowed_;
    }
    void to_generic();

    layout             layout_ = layout::generic;
    std::vector<value> items_;
    std::vector<i64>   ints_;
    std::vector<f64>   floats_;
    const array_v*     borrowed_ = nullptr;
};

struct agg_v { type_id type; std::vector<std::pair<symbol, value>> fields; };

// Function as value: a set of overloads by name or lambda with environment.
struct fn_v {
    symbol               name;       // overload
    node_id              lambda;     // lambda
    std::shared_ptr<scope> env;      // lambda
};

// Uncomputed expression (expr parameter, §6.1): Calculated in the context of the
// caller at each mention.
struct thunk_v {
    node_id                expr;
    std::shared_ptr<scope> env;
    u32                    frame; // function from which `return` within the expression
    u64                    ctx = 0; // context in which the expression is written
};

// Verification mode (value approach):
//
//
// rt_v is an erased runtime value: only the type is known. The type is given by the sample
// (shape): the meaning of this type, the leaves of which are also erased. shape == nullptr
// - type unknown (e.g. recursion in static parsing).
struct rt_v { std::shared_ptr<value> shape; };

// split_v is the value of split context: the branch branch has its own type (§5.5).
// It's not an "A" or a "B" type: a sequel is sorted out for each branch.
struct split_v { std::vector<value> alts; };

struct value {
    std::variant<unit_v, bool, i64, f64, std::string, type_v, array_v, agg_v, fn_v, thunk_v, rt_v, split_v> v;

    value() : v(unit_v{}) {}
    template <class T> value(T x) : v(std::move(x)) {}

    template <class T> bool     is() const { return std::holds_alternative<T>(v); }
    template <class T> T&       as()       { return std::get<T>(v); }
    template <class T> const T& as() const { return std::get<T>(v); }

    bool known() const { return !is<rt_v>() && !is<split_v>(); } // compile-time: value known
};

// --- array_v: definitions after value ---

inline array_v array_v::of(std::vector<value> items) {
    array_v a;
    for (auto& x : items) a.push(std::move(x));
    return a;
}

inline u32 array_v::size() const {
    const array_v& a = r();
    switch (a.layout_) {
    case layout::ints: return u32(a.ints_.size());
    case layout::floats: return u32(a.floats_.size());
    default: return u32(a.items_.size());
    }
}

inline value array_v::get(u32 i) const {
    const array_v& a = r();
    switch (a.layout_) {
    case layout::ints: return value(a.ints_[i]);
    case layout::floats: return value(a.floats_[i]);
    default: return a.items_[i];
    }
}

inline void array_v::to_generic() {
    own();
    if (layout_ == layout::generic) return;
    std::vector<value> items;
    for (u32 i = 0; i < size(); ++i) items.push_back(get(i));
    ints_.clear();
    floats_.clear();
    items_  = std::move(items);
    layout_ = layout::generic;
}

inline void array_v::push(value v) {
    own();
    if (size() == 0 && layout_ == layout::generic) { // first element selects the layout
        if (v.is<i64>()) layout_ = layout::ints;
        else if (v.is<f64>()) layout_ = layout::floats;
    }
    if (layout_ == layout::ints && v.is<i64>()) { ints_.push_back(v.as<i64>()); return; }
    if (layout_ == layout::floats && v.is<f64>()) { floats_.push_back(v.as<f64>()); return; }
    to_generic();
    items_.push_back(std::move(v));
}

inline void array_v::set(u32 i, value v) {
    own();
    if (layout_ == layout::ints && v.is<i64>()) { ints_[i] = v.as<i64>(); return; }
    if (layout_ == layout::floats && v.is<f64>()) { floats_[i] = v.as<f64>(); return; }
    to_generic();
    items_[i] = std::move(v);
}

// Whether the samples are the same types (structurally, without values).
inline bool same_shape(const value& a, const value& b);

// The array, whose elements are of different types, is tuple
// tuple with one type - it is an array.
inline bool is_tuple(const array_v& a) {
    if (a.kind() != array_v::layout::generic) return false;
    for (u32 i = 1; i < a.size(); ++i)
        if (!same_shape(a.get(0), a.get(i))) return true;
    return false;
}

inline value erase(const value& x);

// Erase the value to type (run-time). Samples of numbers - 1, so that the calculation of the
// type of the result of the operation on the samples does not rest on the division by zero.
inline value erase(const value& x) {
    if (x.is<rt_v>() || x.is<split_v>()) return x;
    value shape;
    if (x.is<i64>()) shape = value(i64(1));
    else if (x.is<f64>()) shape = value(f64(1));
    else if (x.is<bool>()) shape = value(true);
    else if (x.is<std::string>()) shape = value(std::string("x"));
    else if (x.is<array_v>()) { // Array - a sample of one element; tuple - samples of all
        const auto& src = x.as<array_v>();
        array_v     a;
        if (is_tuple(src)) for (u32 i = 0; i < src.size(); ++i) a.push(erase(src.get(i)));
        else if (!src.empty()) a.push(erase(src.get(0)));
        shape = value(std::move(a));
    } else if (x.is<agg_v>()) {
        agg_v g{x.as<agg_v>().type, {}};
        for (const auto& [k, v] : x.as<agg_v>().fields) g.fields.push_back({k, erase(v)});
        shape = value(std::move(g));
    } else {
        return x; // Types, functions - always known
    }
    return value(rt_v{std::make_shared<value>(std::move(shape))});
}

// Sample type of erased value (for operations on run-time).
inline const value* shape_of(const value& x) {
    if (x.is<rt_v>()) return x.as<rt_v>().shape.get();
    return &x;
}

inline bool same_shape(const value& a, const value& b) {
    const value* x = shape_of(a);
    const value* y = shape_of(b);
    if (!x || !y) return x == y;
    if (x->v.index() != y->v.index()) return false;
    if (x->is<array_v>()) {
        const auto& p = x->as<array_v>();
        const auto& q = y->as<array_v>();
        bool tp = is_tuple(p), tq = is_tuple(q);
        if (tp != tq) return false;
        if (tp) {
            if (p.size() != q.size()) return false;
            for (u32 i = 0; i < p.size(); ++i)
                if (!same_shape(p.get(i), q.get(i))) return false;
            return true;
        }
        if (p.empty() || q.empty()) return p.empty() == q.empty();
        return same_shape(p.get(0), q.get(0));
    }
    if (x->is<agg_v>()) {
        const auto& p = x->as<agg_v>();
        const auto& q = y->as<agg_v>();
        if (p.type != q.type || p.fields.size() != q.fields.size()) return false;
        for (u32 i = 0; i < p.fields.size(); ++i)
            if (!same_shape(p.fields[i].second, q.fields[i].second)) return false;
        return true;
    }
    return true;
}

// Register of types: closed system of types VM - id instead of strings (§5.1, §9.2).
class type_registry {
public:
    type_registry() {
        for (auto n : {"unit", "bool", "i64", "f64", "str", "type", "array", "fn", "expr", "tuple"}) intern_builtin(n);
    }

    type_id builtin(std::string_view name) const {
        for (u32 i = 0; i < names_.size(); ++i)
            if (names_[i] == name) return type_id(i);
        return {};
    }

    type_id named(std::string_view name) { // nominal
        if (auto t = builtin(name)) return t;
        return intern_builtin(name);
    }

    std::string_view name(type_id t) const { return t ? std::string_view(names_[t.value()]) : "?"; }

    type_id of(const value& x) const {
        if (x.is<rt_v>()) return x.as<rt_v>().shape ? of(*x.as<rt_v>().shape) : type_id{};
        return std::visit([&](const auto& a) -> type_id {
            using T = std::remove_cvref_t<decltype(a)>;
            if constexpr (std::is_same_v<T, unit_v>) return type_id(0);
            else if constexpr (std::is_same_v<T, bool>) return type_id(1);
            else if constexpr (std::is_same_v<T, i64>) return type_id(2);
            else if constexpr (std::is_same_v<T, f64>) return type_id(3);
            else if constexpr (std::is_same_v<T, std::string>) return type_id(4);
            else if constexpr (std::is_same_v<T, type_v>) return type_id(5);
            else if constexpr (std::is_same_v<T, array_v>) return type_id(is_tuple(a) ? 9 : 6);
            else if constexpr (std::is_same_v<T, fn_v>) return type_id(7);
            else if constexpr (std::is_same_v<T, thunk_v>) return type_id(8);
            else if constexpr (std::is_same_v<T, agg_v>) return a.type;
            else return type_id{};
        }, x.v);
    }

    // The full name of the value type, including the array element type: array<f64>.
    std::string describe(const value& x) const {
        if (x.is<rt_v>()) return x.as<rt_v>().shape ? describe(*x.as<rt_v>().shape) : "?";
        if (x.is<split_v>()) {
            std::string s;
            for (const auto& a : x.as<split_v>().alts) s += (s.empty() ? "" : " | ") + describe(a);
            return "{" + s + "}";
        }
        if (x.is<array_v>()) {
            const auto& a = x.as<array_v>();
            if (is_tuple(a)) {
                std::string s;
                for (u32 i = 0; i < a.size(); ++i) s += (i ? ", " : "") + describe(a.get(i));
                return std::format("tuple<{}>", s);
            }
            return std::format("array<{}>", a.empty() ? std::string("?") : describe(a.get(0)));
        }
        return std::string(name(of(x)));
    }

private:
    type_id intern_builtin(std::string_view n) {
        names_.emplace_back(n);
        return type_id(u32(names_.size() - 1));
    }

    std::vector<std::string> names_;
};

inline std::string show(const value& x, const type_registry& types, const interner& strings) {
    return std::visit([&](const auto& a) -> std::string {
        using T = std::remove_cvref_t<decltype(a)>;
        if constexpr (std::is_same_v<T, unit_v>) return "()";
        else if constexpr (std::is_same_v<T, bool>) return a ? "true" : "false";
        else if constexpr (std::is_same_v<T, i64>) return std::to_string(a);
        else if constexpr (std::is_same_v<T, f64>) {
            auto s = std::format("{}", a);
            if (!s.contains('.') && !s.contains('e') && !s.contains('n')) s += ".0";
            return s;
        } else if constexpr (std::is_same_v<T, std::string>) return a;
        else if constexpr (std::is_same_v<T, type_v>) return std::string(types.name(a.type));
        else if constexpr (std::is_same_v<T, array_v>) {
            std::string s = "[";
            for (u32 i = 0; i < a.size(); ++i) {
                if (i) s += ", ";
                s += show(a.get(i), types, strings);
            }
            return s + "]";
        } else if constexpr (std::is_same_v<T, agg_v>) {
            std::string s = std::format("{}{{", types.name(a.type));
            for (u32 i = 0; i < a.fields.size(); ++i) {
                if (i) s += ", ";
                s += std::format("{}: {}", strings.text(a.fields[i].first), show(a.fields[i].second, types, strings));
            }
            return s + "}";
        } else if constexpr (std::is_same_v<T, fn_v>) return "<fn>";
        else if constexpr (std::is_same_v<T, rt_v>) return std::format("рт {}", types.describe(x));
        else if constexpr (std::is_same_v<T, split_v>) return std::format("расщеплено {}", types.describe(x));
        else return "<expr>";
    }, x.v);
}

} // namespace tz
