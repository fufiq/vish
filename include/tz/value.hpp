// The meaning of the interpreter.
//
// Value type - tag (§5.1, D4): numerical id in a closed world VM. Aggregates
// with a name - nominal, anonymous - in structure.
#pragma once

#include "ast.hpp"

#include <deque>
#include <unordered_map>

namespace tz {

struct type_tag;
using type_id = id<type_tag>;

struct value;
struct scope;

struct unit_v {};
struct type_v {
  type_id type;
};
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
struct array_v {
  // clang-format off
  enum class layout : uint8_t { generic, ints, floats };
  // clang-format on

  array_v() = default;
  array_v(array_v const & o) { *this = o; }
  array_v(array_v && o) noexcept = default;
  array_v & operator=(array_v const & o) {
    if(this == &o) return *this;
    array_v const & src = o.borrowed ? *o.borrowed : o;
    repr = src.repr;
    items = src.items;
    int_items = src.int_items;
    float_items = src.float_items;
    gen = src.gen;
    borrowed = nullptr;
    return *this;
  }
  array_v & operator=(array_v && o) noexcept = default;

  static array_v of(std::vector<value> items);
  static array_v borrow(array_v const & cell) { // caller
    array_v a;
    a.borrowed = cell.borrowed ? cell.borrowed : &cell;
    return a;
  }

  uint32_t size() const;
  bool empty() const { return size() == 0; }
  value get(uint32_t i) const;
  void set(uint32_t i, value v);
  void push(value v);
  layout kind() const { return r().repr; }

  int64_t const * ints() const { return r().int_items.data(); }
  double const * floats() const { return r().float_items.data(); }
  int64_t * ints_mut() {
    own();
    return int_items.data();
  }
  double * floats_mut() {
    own();
    return float_items.data();
  }
  value * generic_cell(uint32_t i) {
    if(kind() != layout::generic) return nullptr;
    own();
    return &items[i];
  }
  array_v const & r() const { return borrowed ? *borrowed : *this; }
  void own() {
    if(borrowed) *this = *borrowed;
  }
  void to_generic();

  layout repr = layout::generic;
  std::vector<value> items;
  std::vector<int64_t> int_items;
  std::vector<double> float_items;
  array_v const * borrowed = nullptr;
  uint64_t gen = 0; // Buffer generation: changes with growth and replacement (element references, §12.1)
};

struct agg_v {
  type_id type;
  std::vector<std::pair<symbol, value>> fields;
};

// Function as value: a set of overloads by name or lambda with environment.
struct fn_v {
  symbol name; // overload
  node_id lambda; // lambda
  scope * env = nullptr; // Lambda encirclement (area in arena, captured)
  // Local functions of one name in one block - a set of overloads:
// 
  // 
  std::vector<node_id> set;
};

// Uncomputed expression (expr parameter, §6.1): Calculated in the context of the
// caller at each mention.
struct thunk_v {
  node_id expr;
  scope * env = nullptr; // Calling area (lives longer than the call)
  uint32_t frame; // function from which `return` within the expression
  uint64_t ctx = 0; // context in which the expression is written
};

// Verification mode (value approach):
//
//
// rt_v is an erased runtime value: only the type is known. The type is given by the
// sample (shape): the meaning of this type, the leaves of which are also erased. The
// samples are interned in the shapes table, rt_v stores the number: no_shape - type
// unknown (for example, recursion in static parsing).
//
constexpr uint32_t no_shape = ~uint32_t{0};
struct rt_v {
  uint32_t shape = no_shape;
  explicit operator bool() const { return shape != no_shape; }
};

// split_v is the value of split context: the branch branch has its own type (§5.5).
// It's not an "A" or a "B" type: a sequel is sorted out for each branch.
struct split_v {
  std::vector<value> alts;
};

// Reference to the array element (§12.1): the support is the array buffer in the
// cell. Lives only in the auto& variable and in the parameter; when reading, check
// the generation (execution) or cell version (parsing), then the element.
struct ref_v {
  value * cell = nullptr;
  int64_t index = 0;
  bool rt_index = false; // The index is known only in runtime (parsing).
  uint64_t gen = 0; // Buffer generation (execution) / cell version (parsing)
  uint32_t born = 0; // node where (+1) is taken
};

struct value {
  std::variant<unit_v, bool, int64_t, double, std::string, type_v, array_v, agg_v, fn_v, thunk_v, rt_v, split_v, ref_v> v;
  // Proof of meaning (check mode): fact number +1 -
// , "r - range(xs.size)." A copy of the meaning carries a fact.
  // The new value (i + 1) is not.
  uint32_t proof = 0;
  // Compilation state (typestate, §5.6): the ct field of the aggregate is "mutable
  // constexpr". Erasure to the type does not affect it; different states - different
  // patterns (splitting continuation).
  bool pinned = false;
  uint32_t hidden = 0; // Name hidden (hide / own): seat node +1

  value() : v(unit_v{}) {}
  value(auto x) : v(std::move(x)) {}

  template<typename T>
  bool is() const { return std::holds_alternative<T>(v); }
  template<typename T>
  T & as() { return std::get<T>(v); }
  template<typename T>
  T const & as() const { return std::get<T>(v); }

  bool known() const { return !is<rt_v>() && !is<split_v>(); } // compile-time: value known
};

// Table of samples of types (arena): the same sample - one number.
struct shape_table {
  std::deque<value> items;
  std::unordered_map<std::string, uint32_t> index;

  uint32_t intern(value v) {
    std::string key;
    key_of(v, key);
    if(auto it = index.find(key); it != index.end()) return it->second;
    items.push_back(std::move(v));
    index.emplace(std::move(key), uint32_t(items.size() - 1));
    return uint32_t(items.size() - 1);
  }

  // Canonical text of the sample: only types and structure, without meaning.
  static void key_of(value const & x, std::string & out) {
    if(x.hidden) out += "H" + std::to_string(x.hidden);
    if(x.pinned) { // condition
      out += 'P';
      if(x.is<bool>()) out += x.as<bool>() ? '1' : '0';
      else if(x.is<int64_t>()) out += std::to_string(x.as<int64_t>());
      else if(x.is<double>()) out += cat(x.as<double>());
      else if(x.is<std::string>()) out += x.as<std::string>();
    }
    std::visit([&](auto const & a) {
      using T = std::remove_cvref_t<decltype(a)>;
      if constexpr(std::is_same_v<T, unit_v>) out += 'u';
      else if constexpr(std::is_same_v<T, bool>) out += 'b';
      else if constexpr(std::is_same_v<T, int64_t>) out += 'i';
      else if constexpr(std::is_same_v<T, double>) out += 'f';
      else if constexpr(std::is_same_v<T, std::string>) out += 's';
      else if constexpr(std::is_same_v<T, type_v>) {
        out += 'T';
        out += std::to_string(a.type.value);
      } else if constexpr(std::is_same_v<T, rt_v>) {
        out += 'R';
        out += std::to_string(a.shape);
      } else if constexpr(std::is_same_v<T, array_v>) {
        out += '[';
        for(uint32_t i = 0; i < a.size(); ++i) {
          key_of(a.get(i), out);
          out += ',';
        }
        out += ']';
      } else if constexpr(std::is_same_v<T, agg_v>) {
        out += 'G';
        out += std::to_string(a.type.value);
        out += '{';
        for(auto const & [k, v]: a.fields) {
          out += std::to_string(k.value);
          out += ':';
          key_of(v, out);
          out += ',';
        }
        out += '}';
      } else if constexpr(std::is_same_v<T, split_v>) {
        out += '(';
        for(auto const & v: a.alts) {
          key_of(v, out);
          out += '|';
        }
        out += ')';
      } else if constexpr(std::is_same_v<T, fn_v>) {
        out += 'F';
        out += std::to_string(a.name.value);
        out += '/';
        out += std::to_string(a.lambda.value);
      } else {
        out += 'X';
        out += std::to_string(x.v.index());
      }
    },
        x.v);
  }
};

shape_table shapes; // one on VM: Sample numbers common to all parsing

// --- array_v: definitions after value ---

array_v array_v::of(std::vector<value> items) {
  array_v a;
  for(auto & x: items) a.push(std::move(x));
  return a;
}

uint32_t array_v::size() const {
  array_v const & a = r();
  switch(a.repr) {
    case layout::ints: return uint32_t(a.int_items.size());
    case layout::floats: return uint32_t(a.float_items.size());
    default: return uint32_t(a.items.size());
  }
}

value array_v::get(uint32_t i) const {
  array_v const & a = r();
  switch(a.repr) {
    case layout::ints: return value(a.int_items[i]);
    case layout::floats: return value(a.float_items[i]);
    default: return a.items[i];
  }
}

void array_v::to_generic() {
  own();
  if(repr == layout::generic) return;
  std::vector<value> all;
  for(uint32_t i = 0; i < size(); ++i) all.push_back(get(i));
  int_items.clear();
  float_items.clear();
  items = std::move(all);
  repr = layout::generic;
}

void array_v::push(value v) {
  own();
  if(size() == 0 && repr == layout::generic) { // first element selects the layout
    if(v.is<int64_t>()) repr = layout::ints;
    else if(v.is<double>()) repr = layout::floats;
  }
  if(repr == layout::ints && v.is<int64_t>()) {
    int_items.push_back(v.as<int64_t>());
    return;
  }
  if(repr == layout::floats && v.is<double>()) {
    float_items.push_back(v.as<double>());
    return;
  }
  to_generic();
  items.push_back(std::move(v));
}

void array_v::set(uint32_t i, value v) {
  own();
  if(repr == layout::ints && v.is<int64_t>()) {
    int_items[i] = v.as<int64_t>();
    return;
  }
  if(repr == layout::floats && v.is<double>()) {
    float_items[i] = v.as<double>();
    return;
  }
  to_generic();
  items[i] = std::move(v);
}

// Whether the samples are the same types (structurally, without values).
bool same_shape(value const & a, value const & b);

// The array, whose elements are of different types, is tuple
// tuple with one type - it is an array.
bool is_tuple(array_v const & a) {
  if(a.kind() != array_v::layout::generic) return false;
  for(uint32_t i = 1; i < a.size(); ++i)
    if(!same_shape(a.get(0), a.get(i))) return true;
  return false;
}

value erase(value const & x);

// Erase the value to type (run-time). Samples of numbers - 1, so that the calculation of the
// type of the result of the operation on the samples does not rest on the division by zero.
value erase(value const & x) {
  if(x.is<rt_v>() || x.is<split_v>() || x.pinned || x.hidden) return x; // condition during compilation is not erased
  value shape;
  if(x.is<int64_t>()) shape = value(int64_t(1));
  else if(x.is<double>()) shape = value(double(1));
  else if(x.is<bool>()) shape = value(true);
  else if(x.is<std::string>()) shape = value(std::string("x"));
  else if(x.is<array_v>()) { // Array - a sample of one element; tuple - samples of all
    auto const & src = x.as<array_v>();
    array_v a;
    if(is_tuple(src))
      for(uint32_t i = 0; i < src.size(); ++i) a.push(erase(src.get(i)));
    else if(!src.empty()) a.push(erase(src.get(0)));
    shape = value(std::move(a));
  } else if(x.is<agg_v>()) {
    agg_v g{x.as<agg_v>().type, {}};
    for(auto const & [k, v]: x.as<agg_v>().fields) g.fields.push_back({k, erase(v)});
    shape = value(std::move(g));
  } else {
    return x; // Types, functions - always known
  }
  return value(rt_v{shapes.intern(std::move(shape))});
}

// Sample type of erased value (for operations on run-time).
value const * shape_of(value const & x) {
  if(x.is<rt_v>()) return x.as<rt_v>() ? &shapes.items[x.as<rt_v>().shape] : nullptr;
  return &x;
}

bool same_shape(value const & a, value const & b) {
  value const * x = shape_of(a);
  value const * y = shape_of(b);
  if(!x || !y) return x == y;
  if(x->v.index() != y->v.index()) return false;
  if(x->is<array_v>()) {
    auto const & p = x->as<array_v>();
    auto const & q = y->as<array_v>();
    bool tp = is_tuple(p), tq = is_tuple(q);
    if(tp != tq) return false;
    if(tp) {
      if(p.size() != q.size()) return false;
      for(uint32_t i = 0; i < p.size(); ++i)
        if(!same_shape(p.get(i), q.get(i))) return false;
      return true;
    }
    if(p.empty() || q.empty()) return p.empty() == q.empty();
    return same_shape(p.get(0), q.get(0));
  }
  if(x->is<agg_v>()) {
    auto const & p = x->as<agg_v>();
    auto const & q = y->as<agg_v>();
    if(p.type != q.type || p.fields.size() != q.fields.size()) return false;
    for(uint32_t i = 0; i < p.fields.size(); ++i)
      if(!same_shape(p.fields[i].second, q.fields[i].second)) return false;
    return true;
  }
  return true;
}

// Register of types: closed system of types VM - id instead of strings (§5.1, §9.2).
struct type_registry {
  type_registry() {
    for(auto n: {"unit", "bool", "i64", "f64", "str", "type", "array", "fn", "expr", "tuple"}) intern_builtin(n);
  }

  type_id builtin(std::string_view name) const {
    for(uint32_t i = 0; i < names.size(); ++i)
      if(names[i] == name) return type_id(i);
    return {};
  }

  type_id named(std::string_view name) { // nominal
    if(auto t = builtin(name)) return t;
    return intern_builtin(name);
  }

  std::string_view name(type_id t) const { return t ? std::string_view(names[t.value]) : "?"; }

  type_id of(value const & x) const {
    if(x.is<rt_v>()) return x.as<rt_v>() ? of(*shape_of(x)) : type_id{};
    return std::visit([&](auto const & a) -> type_id {
      using T = std::remove_cvref_t<decltype(a)>;
      if constexpr(std::is_same_v<T, unit_v>) return type_id(0);
      else if constexpr(std::is_same_v<T, bool>) return type_id(1);
      else if constexpr(std::is_same_v<T, int64_t>) return type_id(2);
      else if constexpr(std::is_same_v<T, double>) return type_id(3);
      else if constexpr(std::is_same_v<T, std::string>) return type_id(4);
      else if constexpr(std::is_same_v<T, type_v>) return type_id(5);
      else if constexpr(std::is_same_v<T, array_v>) return type_id(is_tuple(a) ? 9 : 6);
      else if constexpr(std::is_same_v<T, fn_v>) return type_id(7);
      else if constexpr(std::is_same_v<T, thunk_v>) return type_id(8);
      else if constexpr(std::is_same_v<T, agg_v>) return a.type;
      else return type_id{};
    },
        x.v);
  }

  // The full name of the value type, including the array element type: array<f64>.
  std::string describe(value const & x) const {
    if(x.is<rt_v>()) return x.as<rt_v>() ? describe(*shape_of(x)) : "?";
    if(x.is<split_v>()) {
      std::string s;
      for(auto const & a: x.as<split_v>().alts) s += (s.empty() ? "" : " | ") + describe(a);
      return "{" + s + "}";
    }
    if(x.is<array_v>()) {
      auto const & a = x.as<array_v>();
      if(is_tuple(a)) {
        std::string s;
        for(uint32_t i = 0; i < a.size(); ++i) s += (i ? ", " : "") + describe(a.get(i));
        return cat("tuple<", s, ">");
      }
      return cat("array<", a.empty() ? std::string("?") : describe(a.get(0)), ">");
    }
    return std::string(name(of(x)));
  }
  type_id intern_builtin(std::string_view n) {
    names.emplace_back(n);
    return type_id(uint32_t(names.size() - 1));
  }

  std::vector<std::string> names;
};

std::string show(value const & x, type_registry const & types, interner const & strings) {
  return std::visit([&](auto const & a) -> std::string {
    using T = std::remove_cvref_t<decltype(a)>;
    if constexpr(std::is_same_v<T, unit_v>) return "()";
    else if constexpr(std::is_same_v<T, bool>) return a ? "true" : "false";
    else if constexpr(std::is_same_v<T, int64_t>) return std::to_string(a);
    else if constexpr(std::is_same_v<T, double>) {
      auto s = cat(a);
      if(!s.contains('.') && !s.contains('e') && !s.contains('n')) s += ".0";
      return s;
    } else if constexpr(std::is_same_v<T, std::string>) return a;
    else if constexpr(std::is_same_v<T, type_v>) return std::string(types.name(a.type));
    else if constexpr(std::is_same_v<T, array_v>) {
      std::string s = "[";
      for(uint32_t i = 0; i < a.size(); ++i) {
        if(i) s += ", ";
        s += show(a.get(i), types, strings);
      }
      return s + "]";
    } else if constexpr(std::is_same_v<T, agg_v>) {
      std::string s = cat(types.name(a.type), "{");
      for(uint32_t i = 0; i < a.fields.size(); ++i) {
        if(i) s += ", ";
        s += cat(strings.text(a.fields[i].first), ": ", show(a.fields[i].second, types, strings));
      }
      return s + "}";
    } else if constexpr(std::is_same_v<T, fn_v>) return "<fn>";
    else if constexpr(std::is_same_v<T, rt_v>) return cat("рт ", types.describe(x));
    else if constexpr(std::is_same_v<T, split_v>) return cat("расщеплено ", types.describe(x));
    else return "<expr>";
  },
      x.v);
}

} // namespace tz
