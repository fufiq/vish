// The meaning of the interpreter.
//
// Value type - tag (§5.1, D4): numerical id in a closed world VM. Aggregates
// with a name - nominal, anonymous - in structure.
#pragma once

#include "ast.hpp"
#include "pages.hpp"

#include <deque>
#include <unordered_map>

namespace vish {

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
// own copy. Elements of the general array (rows, aggregates, arrays) - in the pages of VM,
// as the field of the aggregate (§12.2): growth - transfer by the designer of movement.
//
//
struct value_list {
  words raw;
  uint64_t n = 0; // word: machine code reads the size of the total layout array from memory

  value_list() = default;
  value_list(value_list const & o);
  value_list(value_list && o) noexcept : raw(std::move(o.raw)), n(o.n) { o.n = 0; }
  value_list & operator=(value_list const & o);
  value_list & operator=(value_list && o) noexcept;
  ~value_list();

  value * begin() { return reinterpret_cast<value *>(raw.data); }
  value * end();
  value const * begin() const { return reinterpret_cast<value const *>(raw.data); }
  value const * end() const;
  uint32_t size() const { return n; }
  bool empty() const { return n == 0; }
  value & operator[](uint32_t i);
  value const & operator[](uint32_t i) const;
  void push_back(value v);
  void clear();
  void grow(uint32_t want);
};

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
    dense = src.dense;
    shape_known = src.shape_known;
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

  int64_t const * ints() const { return reinterpret_cast<int64_t const *>(r().dense.data); }
  double const * floats() const { return reinterpret_cast<double const *>(r().dense.data); }
  int64_t * ints_mut() {
    own();
    return reinterpret_cast<int64_t *>(dense.data);
  }
  double * floats_mut() {
    own();
    return reinterpret_cast<double *>(dense.data);
  }
  value * generic_cell(uint32_t i) {
    if(kind() != layout::generic) return nullptr;
    own();
    shape_known = 0; // The place of the element - its shape can change
    return &items[i];
  }
  array_v const & r() const { return borrowed ? *borrowed : *this; }
  void own() {
    if(borrowed) *this = *borrowed;
  }
  void to_generic();

  layout repr = layout::generic;
  value_list items;
  // dense i64/f64 - words in VM pages (pages.hpp): the growing array does not move
  words dense;
  array_v const * borrowed = nullptr;
  bool pinned = false; // pin(xs): Data in its range of addresses, their growth does not move
  // form of elements of the general layout: 0 - not counted, 1 - one (array), 2 - different (tuple).
  // Array type - for each call (choice overload, instance): without it - bypassing all elements, and
  // the growth of the array through the function (push) - square (*my*)
  mutable uint8_t shape_known = 0;
  // address of the first element: moved - references to elements of this implementation are broken
  void const * base() const { return repr == layout::generic ? static_cast<void const *>(items.raw.data) : dense.data; }
  void pin() {
    own();
    pinned = true;
    (repr == layout::generic ? items.raw : dense).pin();
  }
};

// The aggregate fields are in VM pages (§12.2, author's decision 2026-10-01): the field number
// is static (order of type fields), the machine code reads and writes the offset field that
// informs the VM. Growth is the transfer of movement by the designer (values are not bytes).
struct field_list {
  words raw; // place: words of VM pages
  uint32_t n = 0;

  field_list() = default;
  field_list(std::initializer_list<std::pair<symbol, value>> xs);
  field_list(field_list const & o);
  field_list(field_list && o) noexcept : raw(std::move(o.raw)), n(o.n) { o.n = 0; }
  field_list & operator=(field_list const & o);
  field_list & operator=(field_list && o) noexcept;
  ~field_list();

  std::pair<symbol, value> * begin() { return reinterpret_cast<std::pair<symbol, value> *>(raw.data); }
  std::pair<symbol, value> * end();
  std::pair<symbol, value> const * begin() const { return reinterpret_cast<std::pair<symbol, value> const *>(raw.data); }
  std::pair<symbol, value> const * end() const;
  uint32_t size() const { return n; }
  bool empty() const { return n == 0; }
  std::pair<symbol, value> & operator[](uint32_t i);
  std::pair<symbol, value> const & operator[](uint32_t i) const;
  std::pair<symbol, value> & back();
  void push_back(std::pair<symbol, value> f);
  void clear();
  void grow(uint32_t want);
};

struct agg_v {
  type_id type;
  field_list fields;
  uint32_t rid = 0; // Resource (type with destroy): number - copies divided, destroy - once
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
  uint32_t frame; // function from which `return` within the expression
  scope * env = nullptr; // Calling area (lives longer than the call)
  uint64_t ctx = 0; // context in which the expression is written
  scope * rules = nullptr; // structure block: area of her body with rules (operator, syntax...)
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

// The step of the link path after the first (§12.1, tail of step 10): the array element is the
// index, the aggregate field is the name.
struct ref_step {
  int64_t index = 0;
  symbol field; // field; empty - element
  bool rt_index = false; // parsing: The index is known only in runtime
};

// Reference to place (§12.1): the root is a variable cell, the path is up to three steps: the element
// and the field. ;- reference is a base and an index, not an address: the place is searched again with
// each call, the VM memory transition is not visible to it ( ). &e = v[i] is one step-element; &e =
// p.x, &f = ps[i].y, &e = r[k] from the link r is a path. It lives only in the auto and parameter
// variable.
//
struct ref_v { // The order of the fields is tight: the value size does not grow (more - in array_v).
  value * cell = nullptr;
  int64_t index = 0; // first step: element
  uint32_t born = 0; // node where (+1) is taken
  symbol name; // The name of the array-support: hooks parsing get its place
  uint32_t memo = 0; // parsing: what the link remembered when taking (ref_taken), the number in ct_states +1
  symbol field; // The first step is a field (otherwise an element).
  bool rt_index = false; // The index is known only in runtime (parsing).
  uint8_t more = 0; // steps after the first
  ref_step rest[2];
  bool path() const { return more || field.valid(); }
};

struct value {
  std::variant<unit_v, bool, int64_t, double, str_v, type_v, array_v, agg_v, fn_v, thunk_v, rt_v, split_v, ref_v> v;
  // Proof of meaning (check mode): fact number +1 -
// , "r - range(xs.size)." A copy of the meaning carries a fact.
  // The new value (i + 1) is not.
  uint32_t proof = 0;
  // Compilation state (typestate, §5.6): the ct field of the aggregate is "mutable
  // constexpr". Erasure to the type does not affect it; different states - different
  // patterns (splitting continuation).
  bool pinned = false;
  uint32_t hidden = 0; // Name hidden (hide / own): seat node +1
  // ct-state of value (§5.6, §12.1):
// 
  // The language code reads and changes it (ct_state/set_ct_state); the number in
  // ct_states+1. A copy of the meaning carries it, the new meaning does not.
  uint32_t ct = 0;

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
    // the sample lives the entire program - the copy is in the default level (not in the
    // process lane and not in the state of the program that closes earlier)
    address_space * saved = std::exchange(current_space, nullptr);
    items.push_back(value(v));
    current_space = saved;
    index.emplace(std::move(key), uint32_t(items.size() - 1));
    return uint32_t(items.size() - 1);
  }

  // Canonical text of the sample: only types and structure, without meaning.
  static void key_of(value const & x, std::string & out) {
    if(x.hidden) out += "H" + std::to_string(x.hidden);
    if(x.ct) out += "C" + std::to_string(x.ct);
    if(x.pinned) { // condition
      out += 'P';
      if(x.is<bool>()) out += x.as<bool>() ? '1' : '0';
      else if(x.is<int64_t>()) out += std::to_string(x.as<int64_t>());
      else if(x.is<double>()) out += cat(x.as<double>());
      else if(x.is<str_v>()) out += x.as<str_v>();
    }
    std::visit([&](auto const & a) {
      using T = std::remove_cvref_t<decltype(a)>;
      if constexpr(std::is_same_v<T, unit_v>) out += 'u';
      else if constexpr(std::is_same_v<T, bool>) out += 'b';
      else if constexpr(std::is_same_v<T, int64_t>) out += 'i';
      else if constexpr(std::is_same_v<T, double>) out += 'f';
      else if constexpr(std::is_same_v<T, str_v>) out += 's';
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

// ct-states of values (value::ct): interned - equal states - one number. The key is given by
// the interpreter (type and text of the meaning).
struct ct_state_table {
  std::deque<value> items;
  std::unordered_map<std::string, uint32_t> index;

  uint32_t intern(value v, std::string key) {
    if(auto it = index.find(key); it != index.end()) return it->second;
    address_space * saved = std::exchange(current_space, nullptr); // live-in
    items.push_back(value(v));
    current_space = saved;
    index.emplace(std::move(key), uint32_t(items.size()));
    return uint32_t(items.size());
  }
  value const & at(uint32_t id) const { return items[id - 1]; }
};

ct_state_table ct_states;

value adopt_into(value v, address_space * lv);

// --- array_v: definitions after value ---

array_v array_v::of(std::vector<value> items) {
  array_v a;
  for(auto & x: items) a.push(std::move(x));
  return a;
}

uint32_t array_v::size() const {
  array_v const & a = r();
  switch(a.repr) {
    case layout::ints:
    case layout::floats: return a.dense.size();
    default: return uint32_t(a.items.size());
  }
}

value array_v::get(uint32_t i) const {
  array_v const & a = r();
  switch(a.repr) {
    case layout::ints: return value(int64_t(a.dense[i]));
    case layout::floats: return value(std::bit_cast<double>(a.dense[i]));
    default: return a.items[i];
  }
}

void array_v::to_generic() {
  own();
  if(repr == layout::generic) return;
  value_list all;
  for(uint32_t i = 0; i < size(); ++i) all.push_back(get(i));
  address_space * home = dense.home; // General layout - without dense words: capacity 0 (machine code)
  dense.release();                    // Writes in words only when n < cap
  dense.home = home;
  items = std::move(all);
  repr = layout::generic;
  shape_known = 0;
  if(pinned) items.raw.pin();
}

bool same_shape(value const & a, value const & b);

void array_v::push(value v) {
  own();
  if(size() == 0 && repr == layout::generic) { // first element selects the layout
    if(v.is<int64_t>()) repr = layout::ints;
    else if(v.is<double>()) repr = layout::floats;
    if(pinned) (repr == layout::generic ? items.raw : dense).pin();
  }
  if(repr == layout::ints && v.is<int64_t>()) {
    dense.push_back(uint64_t(v.as<int64_t>()));
    return;
  }
  if(repr == layout::floats && v.is<double>()) {
    dense.push_back(std::bit_cast<uint64_t>(v.as<double>()));
    return;
  }
  to_generic();
  if(items.size() == 0) shape_known = 1;
  else if(shape_known == 1 && !same_shape(items[0], v)) shape_known = 2;
  items.push_back(std::move(v));
}

void array_v::set(uint32_t i, value v) {
  own();
  if(repr == layout::ints && v.is<int64_t>()) {
    dense[i] = uint64_t(v.as<int64_t>());
    return;
  }
  if(repr == layout::floats && v.is<double>()) {
    dense[i] = std::bit_cast<uint64_t>(v.as<double>());
    return;
  }
  to_generic();
  shape_known = 0;
  items[i] = adopt_into(std::move(v), items.raw.home);
}

// Program status file directory (§12.2): by default, next to the program; test runner, its
// temporary.
std::string state_dir;

// The value is in the memory level of the container, if it is non-volatile (program state): the
// copy is made where it will live; otherwise - as is. In the state - only data: numbers, lines,
// arrays, aggregates.
bool persistable(value const & v) {
  if(v.is<unit_v>() || v.is<bool>() || v.is<int64_t>() || v.is<double>() || v.is<str_v>() || v.is<type_v>()) return true;
  if(v.is<array_v>()) {
    array_v const & a = v.as<array_v>().r();
    if(a.kind() != array_v::layout::generic) return true;
    for(auto const & x: a.items)
      if(!persistable(x)) return false;
    return true;
  }
  if(v.is<agg_v>()) {
    for(auto const & [k, x]: v.as<agg_v>().fields)
      if(!persistable(x)) return false;
    return true;
  }
  return false;
}
value adopt_into(value v, address_space * lv) {
  if(!lv || !lv->persistent) return v;
  if(!persistable(v)) throw diagnostic("в состояние программы — только данные (числа, строки, массивы, агрегаты)", {});
  address_space * saved = current_space;
  current_space = lv;
  value c = v; // copy - in level pages
  current_space = saved;
  return c;
}

// --- value_list: definitions after value ---

value * value_list::end() { return begin() + n; }
value const * value_list::end() const { return begin() + n; }
value & value_list::operator[](uint32_t i) { return begin()[i]; }
value const & value_list::operator[](uint32_t i) const { return begin()[i]; }
void value_list::grow(uint32_t want) {
  uint64_t w = (uint64_t(want) * sizeof(value) + 7) / 8;
  if(w <= raw.cap) return;
  words fresh;
  fresh.home = raw.home; // memory level - former
  fresh.reserve(std::max(w, raw.cap * 2));
  auto * to = reinterpret_cast<value *>(fresh.data);
  for(uint32_t i = 0; i < n; ++i) {
    new(to + i) value(std::move(begin()[i]));
    begin()[i].~value();
  }
  raw = std::move(fresh);
}
void value_list::push_back(value v) {
  grow(n + 1);
  new(begin() + n) value(adopt_into(std::move(v), raw.home));
  ++n;
}
void value_list::clear() {
  for(uint32_t i = 0; i < n; ++i) begin()[i].~value();
  n = 0;
}
value_list::value_list(value_list const & o) {
  raw.home = &space(); // copy at the level where it is made
  for(auto const & x: o) push_back(x);
}
value_list & value_list::operator=(value_list const & o) {
  if(this == &o) return *this;
  clear();
  if(!raw.data) raw.home = &space();
  for(auto const & x: o) push_back(x);
  return *this;
}
value_list & value_list::operator=(value_list && o) noexcept {
  if(this == &o) return *this;
  clear();
  raw = std::move(o.raw);
  n = o.n;
  o.n = 0;
  return *this;
}
value_list::~value_list() { clear(); }

// --- field_list: definitions after value ---

std::pair<symbol, value> * field_list::end() { return begin() + n; }
std::pair<symbol, value> const * field_list::end() const { return begin() + n; }
std::pair<symbol, value> & field_list::operator[](uint32_t i) { return begin()[i]; }
std::pair<symbol, value> const & field_list::operator[](uint32_t i) const { return begin()[i]; }
std::pair<symbol, value> & field_list::back() { return begin()[n - 1]; }

void field_list::grow(uint32_t want) {
  uint64_t w = (uint64_t(want) * sizeof(std::pair<symbol, value>) + 7) / 8;
  if(w <= raw.cap) return;
  words fresh;
  fresh.home = raw.home; // memory level - former
  fresh.reserve(std::max(w, raw.cap * 2));
  auto * to = reinterpret_cast<std::pair<symbol, value> *>(fresh.data);
  for(uint32_t i = 0; i < n; ++i) {
    new(to + i) std::pair<symbol, value>(std::move(begin()[i]));
    begin()[i].~pair();
  }
  raw = std::move(fresh);
}
void field_list::push_back(std::pair<symbol, value> f) {
  grow(n + 1);
  f.second = adopt_into(std::move(f.second), raw.home);
  new(begin() + n) std::pair<symbol, value>(std::move(f));
  ++n;
}
void field_list::clear() {
  for(uint32_t i = 0; i < n; ++i) begin()[i].~pair();
  n = 0;
}
field_list::field_list(std::initializer_list<std::pair<symbol, value>> xs) {
  for(auto const & x: xs) push_back(x);
}
field_list::field_list(field_list const & o) {
  raw.home = &space(); // copy at the level where it is made
  for(auto const & x: o) push_back(x);
}
field_list & field_list::operator=(field_list const & o) {
  if(this == &o) return *this;
  clear();
  if(!raw.data) raw.home = &space();
  for(auto const & x: o) push_back(x);
  return *this;
}
field_list & field_list::operator=(field_list && o) noexcept {
  if(this == &o) return *this;
  clear();
  raw = std::move(o.raw);
  n = o.n;
  o.n = 0;
  return *this;
}
field_list::~field_list() { clear(); }

// Dense array in the cell value: data offsets and size - machine code reads them from memory
// (§12.2, without copies); cell array - loan indicates the array in the cell.
struct array_offsets {
  uint64_t data, size, cap, items_n, in_value;
};
array_offsets const & array_layout() {
  static array_offsets o = [] {
    value v{array_v{}};
    auto const * b = reinterpret_cast<char const *>(&v);
    auto const & a = v.as<array_v>();
    return array_offsets{uint64_t(reinterpret_cast<char const *>(&a.dense.data) - b), uint64_t(reinterpret_cast<char const *>(&a.dense.n) - b),
        uint64_t(reinterpret_cast<char const *>(&a.dense.cap) - b),
        uint64_t(reinterpret_cast<char const *>(&a.items.n) - b), uint64_t(reinterpret_cast<char const *>(&a) - b)};
  }();
  return o;
}
// The aggregate in the value cell: displacement of data fields, field pitch, scalar in the field (§12.2)
struct agg_offsets {
  uint64_t data, stride, scalar;
};
agg_offsets const & agg_layout() {
  static agg_offsets o = [] {
    value v{agg_v{}};
    auto const * b = reinterpret_cast<char const *>(&v);
    std::pair<symbol, value> p{symbol{}, value(int64_t(0))};
    auto const * q = reinterpret_cast<char const *>(&p);
    std::pair<symbol, value> pf{symbol{}, value(0.5)};
    contract_assert(reinterpret_cast<char const *>(&pf.second.as<double>()) - reinterpret_cast<char const *>(&pf) == reinterpret_cast<char const *>(&p.second.as<int64_t>()) - q);
    return agg_offsets{uint64_t(reinterpret_cast<char const *>(&v.as<agg_v>().fields.raw.data) - b), sizeof(std::pair<symbol, value>),
        uint64_t(reinterpret_cast<char const *>(&p.second.as<int64_t>()) - q)};
  }();
  return o;
}

value * cell_of(array_v const * a) { return reinterpret_cast<value *>(const_cast<char *>(reinterpret_cast<char const *>(a)) - array_layout().in_value); }

// Whether the samples are the same types (structurally, without values).
bool same_shape(value const & a, value const & b);

// The array, whose elements are of different types, is tuple
// tuple with one type - it is an array.
bool is_tuple(array_v const & a) {
  array_v const & s = a.r();
  if(s.kind() != array_v::layout::generic) return false;
  if(s.shape_known) return s.shape_known == 2;
  bool mixed = false;
  for(uint32_t i = 1; i < s.size() && !mixed; ++i)
    if(!same_shape(s.items[0], s.items[i])) mixed = true;
  s.shape_known = mixed ? 2 : 1;
  return mixed;
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
  else if(x.is<str_v>()) shape = value(std::string("x"));
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
  value r(rt_v{shapes.intern(std::move(shape))});
  r.ct = x.ct; // ct-state erasure does not affect
  return r;
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
  // Machine representations (§5.8, O15): the kernel only knows them, the names of the tags
  // prelude (struct i64 { __int64 value; }):
// 
//;.
  static constexpr std::string_view machine_names[] = {"", "__bool", "__int64", "__float64", "__bytes"};
  static constexpr uint32_t m_bool = 1, m_int = 2, m_float = 3, m_bytes = 4;

  type_registry() {
    for(auto n: {"unit", "__bool", "__int64", "__float64", "__bytes", "type", "array", "fn", "expr", "tuple"}) intern_builtin(n);
  }

  type_id builtin(std::string_view name) const {
    for(uint32_t i = 0, n = names.size(); i < n; ++i)
      if(nm(i) == name) return type_id(i);
    for(uint32_t i = 1; i < std::size(machine_names); ++i) // presentation - and under his own name
      if(machine_names[i] == name) return type_id(i);
    return {};
  }
  // Presentation by name ( int64...); 0 is not representation.
  static uint32_t machine(std::string_view name) {
    for(uint32_t i = 1; i < std::size(machine_names); ++i)
      if(machine_names[i] == name) return i;
    return 0;
  }
  // The prelude tag gives the view a name; false is the name of another type.
  bool bind_tag(uint32_t m, std::string_view name) {
    lane_guard g(lock);
    if(nm(m) == name) return true;
    if(nm(m) != machine_names[m]) return false; // There is already a tag (the other type above it is not yet)
    if(type_id other = builtin(name); other && other.value != m) return false;
    nm(m) = std::string(name);
    tag_names[m] = std::string(name);
    return true;
  }
  // Presentation tag name for code without registry (JIT, machine): prelude one on VM.
  static inline std::string tag_names[5];
  static std::string_view tag(uint32_t m) { return tag_names[m].empty() ? machine_names[m] : std::string_view(tag_names[m]); }
  // Tag names (in C++ - representations only): bool, i64, f64, str prelude.
  std::string_view bool_name() const { return nm(m_bool); }
  std::string_view int_name() const { return nm(m_int); }
  std::string_view float_name() const { return nm(m_float); }
  std::string_view str_name() const { return nm(m_bytes); }

  type_id named(std::string_view name) { // nominal
    if(auto t = builtin(name)) return t;
    lane_guard g(lock); // Two stripes don't get the same name twice.
    if(auto t = builtin(name)) return t;
    return intern_builtin(name);
  }

  // The hierarchy of nominal types:
// 
  //;.
  void set_parent(type_id t, type_id p) {
    lane_guard g(lock);
    if(parents.capacity() == 0) parents.reserve(1u << 16); // Do not move: parents read without lock
    if(parents.size() <= t.value) parents.resize(t.value + 1);
    parents[t.value] = p;
  }
  type_id parent(type_id t) const { return t && t.value < parents.size() ? parents[t.value] : type_id{}; }
  // have - want or his heir
  bool is_a(std::string_view have, std::string_view want) const {
    for(type_id t = builtin(have); t; t = parent(t))
      if(nm(t.value) == want) return true;
    return false;
  }

  std::string_view name(type_id t) const { return t ? std::string_view(nm(t.value)) : "?"; }

  // Anonymous aggregate {x: 1.0} is a type by structure: the name is a record of {x: f64} fields
  // (names and types in order), one structure is one type.
// 
//.
  static bool anonymous(std::string_view n) { return n.starts_with('{'); }
  bool anonymous(type_id t) const { return t && anonymous(name(t)); }
  // Anonymous field type by its record: name and field type record (array<f64>, {a: i64}).
  static std::vector<std::pair<std::string_view, std::string_view>> record_parts(std::string_view s) {
    std::vector<std::pair<std::string_view, std::string_view>> out;
    if(!anonymous(s) || s.size() < 2) return out;
    s = s.substr(1, s.size() - 2);
    std::size_t from = 0;
    int depth = 0;
    for(std::size_t i = 0; i <= s.size(); ++i) {
      char c = i < s.size() ? s[i] : ',';
      if(c == '{' || c == '<') ++depth;
      else if(c == '}' || c == '>') --depth;
      else if(c == ',' && depth == 0) {
        std::string_view f = s.substr(from, i - from);
        if(f.starts_with(' ')) f.remove_prefix(1);
        std::size_t colon = f.find(": ");
        if(colon != std::string_view::npos) out.push_back({f.substr(0, colon), f.substr(colon + 2)});
        from = i + 1;
      }
    }
    return out;
  }

  type_id of(value const & x) const {
    if(x.is<rt_v>()) return x.as<rt_v>() ? of(*shape_of(x)) : type_id{};
    return std::visit([&](auto const & a) -> type_id {
      using T = std::remove_cvref_t<decltype(a)>;
      if constexpr(std::is_same_v<T, unit_v>) return type_id(0);
      else if constexpr(std::is_same_v<T, bool>) return type_id(1);
      else if constexpr(std::is_same_v<T, int64_t>) return type_id(2);
      else if constexpr(std::is_same_v<T, double>) return type_id(3);
      else if constexpr(std::is_same_v<T, str_v>) return type_id(4);
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
      // elements - at the link: get copies, and the motorcade of arrays is the entire array for each call
      // (call with the motorcade of arrays was square, 2026-10-21)
      if(is_tuple(a)) {
        std::string s;
        for(uint32_t i = 0; i < a.size(); ++i) s += (i ? ", " : "") + describe(a.r().items[i]);
        return cat("tuple<", s, ">");
      }
      if(a.empty()) return "array<?>";
      return cat("array<", a.kind() == array_v::layout::generic ? describe(a.r().items[0]) : describe(a.get(0)), ">");
    }
    return std::string(name(of(x)));
  }
  type_id intern_builtin(std::string_view n) {
    lane_guard g(lock);
    return type_id(names.add(std::string(n)).value);
  }
  // Names - stable arena: read unlocked while another band turns type (step 12.3b)
  struct name_tag;
  std::string const & nm(uint32_t i) const { return names[id<name_tag>(i)]; }
  std::string & nm(uint32_t i) { return names[id<name_tag>(i)]; }
  void assign_names(auto const & xs) { // Type numbers - as in a dump (machine)
    names.clear();
    for(auto const & x: xs) names.add(std::string(x));
  }
  arena<std::string, name_tag> names;
  std::vector<type_id> parents; // nominal parent (empty - root)
  std::recursive_mutex lock;
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
    } else if constexpr(std::is_same_v<T, str_v>) return a.str();
    else if constexpr(std::is_same_v<T, type_v>) return std::string(types.name(a.type));
    else if constexpr(std::is_same_v<T, array_v>) {
      std::string s = "[";
      for(uint32_t i = 0; i < a.size(); ++i) {
        if(i) s += ", ";
        s += show(a.get(i), types, strings);
      }
      return s + "]";
    } else if constexpr(std::is_same_v<T, agg_v>) {
      std::string s = cat(types.anonymous(a.type) ? "" : types.name(a.type), "{");
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

} // namespace vish
