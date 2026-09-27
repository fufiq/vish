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
struct array_v { std::vector<value> items; };
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
};

struct value {
    std::variant<unit_v, bool, i64, f64, std::string, type_v, array_v, agg_v, fn_v, thunk_v> v;

    value() : v(unit_v{}) {}
    template <class T> value(T x) : v(std::move(x)) {}

    template <class T> bool     is() const { return std::holds_alternative<T>(v); }
    template <class T> T&       as()       { return std::get<T>(v); }
    template <class T> const T& as() const { return std::get<T>(v); }
};

// Register of types: closed system of types VM - id instead of strings (§5.1, §9.2).
class type_registry {
public:
    type_registry() {
        for (auto n : {"unit", "bool", "i64", "f64", "str", "type", "array", "fn", "expr"}) intern_builtin(n);
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
        return std::visit([&](const auto& a) -> type_id {
            using T = std::remove_cvref_t<decltype(a)>;
            if constexpr (std::is_same_v<T, unit_v>) return type_id(0);
            else if constexpr (std::is_same_v<T, bool>) return type_id(1);
            else if constexpr (std::is_same_v<T, i64>) return type_id(2);
            else if constexpr (std::is_same_v<T, f64>) return type_id(3);
            else if constexpr (std::is_same_v<T, std::string>) return type_id(4);
            else if constexpr (std::is_same_v<T, type_v>) return type_id(5);
            else if constexpr (std::is_same_v<T, array_v>) return type_id(6);
            else if constexpr (std::is_same_v<T, fn_v>) return type_id(7);
            else if constexpr (std::is_same_v<T, thunk_v>) return type_id(8);
            else return a.type;
        }, x.v);
    }

    // The full name of the value type, including the array element type: array<f64>.
    std::string describe(const value& x) const {
        if (x.is<array_v>()) {
            const auto& items = x.as<array_v>().items;
            return std::format("array<{}>", items.empty() ? std::string("?") : describe(items[0]));
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
            for (u32 i = 0; i < a.items.size(); ++i) {
                if (i) s += ", ";
                s += show(a.items[i], types, strings);
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
        else return "<expr>";
    }, x.v);
}

} // namespace tz
