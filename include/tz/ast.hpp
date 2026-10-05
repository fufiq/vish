// The AST database is the source of program truth (§3.1, D7).
//
// node are data aggregates owned by ast_db. The links between node are indexes
// (node_id), not pointers or names in the text: Each change increases the revision
// - on this is reactive elaboration (§10.1).
//
#pragma once

#include "base.hpp"
#include "lexer.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <unordered_map>
#include <variant>

namespace tz {

struct node_tag;
using node_id = id<node_tag>;
using node_list = std::vector<node_id>;

// Parameter mode (§6.2 steak)
// clang-format off
enum class param_mode : uint8_t { let, mut, own, out, expr, ct };
// clang-format on

// --- node ---

// Context is a node that enters the region (author’s decision 2026-10-02): the module, the
// `{}` block (and the body of the structure is its block) are the heirs of the context, and
// there is no node wrapper. Rules parsing (syntax, operator, construction) - from ads in
// context: operate from the announcement to the end of its context and in nested, search -
// from place up the context, prelude - the external context of all modules (§4.4).
// 
// 
//.
struct n_context {};

// blank[k] - empty lines before the element k (over canonical), the last - before the
// end: the layout of the text that a person put Enter (author's decision 2026-09-29).
// The performance doesn't see her.
struct n_module : n_context {
  symbol name;
  node_list items;
  std::vector<uint32_t> blank;
  // internal elements (node numbers from items):
// - in base and execution, but not in
  // presentation of the editor module; access - transition. The layout is like blank; the
  // numbers, not node_list, are not children (they are not seen twice by rounds in the fields).
  std::vector<uint32_t> inner;
  bool sealed = false; // sealed module k; - entity modules do not expand externally (§9.1)
};
struct n_fn {
  symbol name;
  node_list params;
  node_id result_type;
  node_id body;
  bool outline = false;
  bool expr_body = false;
  node_list attrs; // [[…]] - values calculated during compilation (§7.4)
  bool scoped = false; // Using Operator - Redefinition in Context (§8.2)
  // syntax: запись формы — слова и символы, "(" — параметр в скобках, "{" —
  // parameter-block (in order of parameters); the first word is the name entity (§4.4 )
  std::vector<symbol> form;
};
struct n_param {
  symbol name;
  node_id type;
  param_mode mode = param_mode::let;
  bool pack = false;
  symbol clause; // the word of the clause, which belongs to the parameter: try(…) catch(…) (empty - head)
  // explicit T x - "type equals" T x - "value is conceivable" (with transformation):
  // 
  // 
  bool strict = false;
  int64_t source = -1; // block parameter entered by the design: `auto x : xs` is the xs argument number in the call
};
struct n_var {
  symbol name;
  node_id type;
  node_id init;
  bool is_const = false;
  bool ct = false; // Compilation field state: struct file { ct bool open = true; }
  bool ref = false; // auto&e = v[i] - reference to place (§12.1)
  bool strict = false; // explicit T x = e - type exactly T (§5.8)
  node_list attrs; // [[state]] x = e is the state of the program (§12.2)
};
struct n_block : n_context {
  node_list items;
  bool has_value = false;
  std::vector<uint32_t> blank; // n_module
}; // has_value: Last expression without `;`
struct n_ident {
  symbol name;
};
struct n_int {
  int64_t value = 0;
  bool ch = false; // written with the symbol 'c' (byte)
  bool hex = false; // recorded 0x...
};
struct n_float {
  double value = 0;
};
struct n_str {
  std::string value;
};
struct n_call {
  node_id callee;
  node_list args;
  std::vector<symbol> words; // The Word of Clause in Every Argument (Empty - Everything in Your Head)
  bool tail = false; // The final argument is the tail unit / instruction
  uint32_t form = 0; // Syntax ad entry: ad node + 1
  // Tag "text {x} text" - call tag ("text", x, "text"):
//;
  // arguments - text (n_str), expression, ..., text; writing - template
  bool tagged = false;
};
struct n_op {
  symbol op;
  node_list args;
  bool postfix = false; // one argument: a++ (otherwise prefix -a)
}; // operator = operator entity call (§4.4)
// A chain of infix operators, which has several trees parsing: the tree is
// selected by brute force at instantiation - by type (§4.4).
struct n_chain {
  node_list atoms;
  std::vector<symbol> ops; // ops[k] - between atoms[k] and atoms[k+1]
};
struct n_if {
  node_id cond;
  node_id then_;
  node_id else_;
};
// kernel cycle (§3.2): for/while is a library. Break/continue is the second exit
// path that catches the loop (like throw-catch, just jmp).
struct n_loop {
  node_id body;
};
struct n_break {};
// Hide x - the name disappears from the area:
//
struct n_continue {};
struct n_return {
  node_id value;
  node_id target;
}; // return(f) x is the output from the external function f; return(caller) x is the output from the calling function (§6.1)
// Exceptions (§8.1): Two exit paths - throw - expression.
// try/catch is a library over intercept (§8.1).
//
struct n_throw {
  node_id value;
};
// Extension of entity:
//  
// Fields (n_var), functions (n_fn), and attributes added to the Name.
struct n_extend {
  symbol name;
  node_list items;
  node_list attrs;
  // Extension of a specific function by reference to its node (number, not child):
  // 
// 
  // Items is its overload; name is the current name of the target.
  uint32_t target = 0;
  // Struct The name { поля } is the first extension to declare the name a struct instance:
// 
//.
  // Named aggregate (§5.1): nominal type; items - n_var fields (type, name, default
  // value); created positionally: Name {a, b}.
  symbol of;
  symbol base; // struct e: base { … } - heir: base fields first, base substitution
};
// Code as value (§7.4): quote { объявления } and insert #x.
//
struct n_quote : n_context { // context: declared in the workpiece does not change parsing from the outside (author's decision 2026-10-02)
  node_list items;
};
struct n_splice {
  symbol name;
};
struct n_ct {
  node_id body;
}; // ct e / ct { }
struct n_lambda {
  node_list params;
  node_id body;
  bool block = false; // construction block: for(auto x : xs) {…} - semantics inline, not closing
};
struct n_construct {
  node_id type;
  node_list items;
  // {x: 1.0, y: 2.0} is an anonymous structural literal: type no, names[k] is the name of items[k].
  // 
//  
  std::vector<symbol> names;
}; // array{1, 2, 3}
struct n_hole {
  std::string text;
  std::string reason;
}; // undisassembled fragment (§4.1)

// Unpacking (P12):
// 
// The sample is written as a constructor (author's decision 2026-09-30): Name {…} /
// array {…}; elements - n_param (name, T name, , ... residue), nested n_pat or
// expression-value (in the text - in parentheses).
//
struct n_pat {
  node_id type;
  node_list items;
};
// sample = value: instruction (failure - exclusion of unpack_failed) or predicate in the
// expression - require(…), if(…), && (success - truth, names - with success):
//  
struct n_unpack {
  node_id pattern;
  node_id value;
  bool test = false; // predicate, not instruction
};
// Sample element - reference to the interface of the unpackable value:
// 
// access is an expression from n_subject (.field, .size, .f(),
// [k]); sub is a sub-sample of the result.
struct n_pat_field {
  node_id access;
  node_id sub;
};
struct n_subject {}; // Unpackable value (implicit root of circulation)
// ...t in the call arguments - the array/cortege is revealed in the arguments:
// 
//  
struct n_spread {
  node_id value;
};
// use geo.vec; / use geo.vec. {norm, dot} ; - Module as semantic inclusion:
// 
// 
// Names - Empty: All module
// announcements.
struct n_use {
  symbol key;
  std::vector<symbol> names;
};
// native "cpp" { … } is a function body in C++ (§3.3):
// 
// Text as written; `#x` is the parameter x of the surrounding function with the
// value of VM. It is going to VM (tz native).
struct n_native {
  symbol lang;
  std::string code;
};
// The link to the ad (function, struct, ct-variable) is its node number, not its name:
// 
// ; Generates ct code (ref(f)): the name in the insert location does not intercept it.
// Print is the current name of the target.
//
struct n_ref {
  uint32_t target = 0;
};
// Raw block (§4.4, "parsing code"): A block of construction whose body declares the parsing
// rule `syntax {r} => f(r);` is stored as a result of phase 1 - brackets, atoms (names,
// numbers, strings, symbols) and sub-blocks with positions; node from it makes a function on
// the language lazily, when analyzed (like the instantiation template).
// 
// 
// 
// The text is how it is written (the print does not change it).
struct n_raw {
  std::string open; // bracket: {[[]
  node_list items; // atoms (n_ident, n_int, n_float, n_str, n_sym, n_hole) and subblocks of n_raw
  std::string text; // Block text with brackets - as written
  uint32_t rule = 0; // announcement of the structure, the body of which is disassembled by the block (+1); for sub-blocks - 0
  bool glued = false; // close to the previous one: f(x) vs. f(x)
};
// The operator symbol of the raw unit is one: “the operator is always
// single-character” gluing (==, ->) is visible by glued.
struct n_sym {
  std::string text;
  bool glued = false;
};

using node_kind = std::variant<n_module, n_fn, n_param, n_var, n_block, n_ident, n_int, n_float, n_str,
    n_call, n_op, n_if, n_loop, n_break, n_continue, n_return,
    n_ct, n_lambda, n_construct, n_hole, n_throw, n_chain, n_extend, n_quote, n_splice, n_pat, n_unpack, n_pat_field, n_subject, n_spread, n_use, n_native, n_ref, n_raw, n_sym>;

struct node {
  node_kind kind;
  span where;
  node_id parent;
};

// journal of base operations (§3.1):
// 
// The base state is the reproduction of journal;
// journal is the history of change.
struct j_intern {
  uint32_t id;
  std::string text;
};
struct j_add {
  uint32_t id;
  node_kind kind;
  span where;
};
struct j_replace {
  uint32_t id;
  node_kind kind;
};
struct j_link {
  uint32_t root;
}; // Put the parent in the subtree (deterministically)
struct j_import {
  std::string text;
  std::string label;
}; // projection: span node - position in it
// Mutation node ct code (§7.4): Replace the type node with the source node ct code that made it.
// .
//
struct j_mutate {
  uint32_t id;
  node_kind kind;
  uint32_t source;
};
// Return of mutations of the source (0 - all) and all later:
//
struct j_rollback {
  uint32_t source;
};
using journal_op = std::variant<j_intern, j_add, j_replace, j_link, j_import, j_mutate, j_rollback>;

struct journal_entry {
  uint64_t revision;
  journal_op op;
};

// a[i]: operator node object and index []
struct index_view {
  node_id object;
  node_id index;
};
// a.x: object and name node postfix operator .x
struct member_view {
  node_id object;
  symbol name;
};
// v.[f]: object and field descriptor node operator .[] (§7.4 ]
struct field_view {
  node_id object;
  node_id key;
};

struct ast_db {
  node_id add(node_kind k, span where) {
    ++revision;
    where.source = pinned_source != ~uint32_t{0} ? pinned_source : uint32_t(sources.empty() ? 0 : sources.size() - 1);
    node_id id = nodes.add(node{std::move(k), where, {}});
    node_rev.push_back(revision);
    log({j_add{id.value, nodes[id].kind, where}});
    return id;
  }

  // node revision - when the last time its subtree within a function changed (reactive
  // disability: editing a block also changes the function revision).
  uint64_t node_revision(node_id n) const { return node_rev[n.value]; }

  node_id add(auto k, span where) {
    return add(node_kind(std::move(k)), where);
  }

  node & operator[](node_id n) { return nodes[n]; }
  node const & operator[](node_id n) const { return nodes[n]; }

  template<typename kind>
  bool is(node_id n) const { return n && std::holds_alternative<kind>(nodes[n].kind); }
  template<typename kind>
  kind & as(node_id n) { return std::get<kind>(nodes[n].kind); }
  template<typename kind>
  kind const & as(node_id n) const { return std::get<kind>(nodes[n].kind); }

  // The character class for node checks at each execution step: the text of the character
  // is unchanged - the answer is remembered by number (without comparing lines).
  enum sym_class : uint8_t { sc_known = 1, sc_index = 2, sc_field = 4, sc_member = 8, sc_require = 16, sc_hide = 32, sc_try = 64, sc_assign = 128 };
  uint8_t sym_kind(symbol s) const {
    if(!s) return sc_known;
    if(s.value >= sym_kinds.size()) sym_kinds.resize(std::max<size_t>(strings.size(), s.value + 1), 0);
    uint8_t & k = sym_kinds[s.value];
    if(!k) {
      auto t = strings.text(s);
      k = sc_known | (t == "[]" ? sc_index : 0) | (t == ".[]" ? sc_field : 0) | (t.size() > 1 && t[0] == '.' && lexer::is_ident_start(t[1]) ? sc_member : 0) |
          (t == "require" ? sc_require : 0) | (t == "hide" ? sc_hide : 0) | (t == "try_compile" ? sc_try : 0) |
          (t == "=" || t == "+=" || t == "-=" || t == "*=" || t == "/=" ? sc_assign : 0);
    }
    return k;
  }
  mutable std::vector<uint8_t> sym_kinds;
  // Call by name with character class (require, hide, try_compile)
  bool call_named_kind(n_call const & c, uint8_t k) const { return c.words.empty() && is<n_ident>(c.callee) && (sym_kind(as<n_ident>(c.callee).name) & k); }

  // require(c)/require(c, "сообщение", "пометка", место, …) is a normal kernel call, not a form
  // of node : condition c, otherwise empty
  node_id require_cond(node_id n) const {
    if(!is<n_call>(n)) return {};
    auto const & c = as<n_call>(n);
    if(c.args.empty() || !call_named_kind(c, sc_require)) return {};
    return c.args[0];
  }

  // a[i] is a paired operator [] over an object and an index:
// node - n_op "[]" {a, i}
  bool is_index(node_id n) const {
    if(!is<n_op>(n)) return false;
    auto const & o = as<n_op>(n);
    return o.args.size() == 2 && !o.postfix && (sym_kind(o.op) & sc_index);
  }
  // node in the code prelude : modules are included, prelude does not
  // include a program module - overload [] programs are not visible in it
  bool in_prelude(node_id n) const {
    while(n && !is<n_module>(n)) n = nodes[n].parent;
    return n && n == prelude;
  }
  index_view index_of(node_id n) const { return {as<n_op>(n).args[0], as<n_op>(n).args[1]}; }

  // a.x is a post-fix operator .x:
// name in the operator symbol
  // (node doesn't have a baby name: rounds won't take a field for a variable)
  bool is_member(node_id n) const {
    if(!is<n_op>(n)) return false;
    auto const & o = as<n_op>(n);
    return o.args.size() == 1 && o.postfix && (sym_kind(o.op) & sc_member);
  }
  member_view member_of(node_id n) const { return {as<n_op>(n).args[0], member_name(as<n_op>(n).op)}; }
  // field name by operator symbol .x (parsing internees both)
  symbol member_name(symbol op) const {
    if(op.value >= member_names.size()) member_names.resize(strings.size());
    symbol & s = member_names[op.value];
    if(!s) s = strings.index.at(std::string(strings.text(op).substr(1)));
    return s;
  }
  mutable std::vector<symbol> member_names;
  // v.[f] is the operator .[] over the value and descriptor of the field
  bool is_field(node_id n) const {
    if(!is<n_op>(n)) return false;
    auto const & o = as<n_op>(n);
    return o.args.size() == 2 && !o.postfix && (sym_kind(o.op) & sc_field);
  }
  field_view field_of(node_id n) const { return {as<n_op>(n).args[0], as<n_op>(n).args[1]}; }

  // Struct Name { … } - Extension Declaring Struct Instance
  bool is_struct(node_id n) const { return is<n_extend>(n) && as<n_extend>(n).of && strings.text(as<n_extend>(n).of) == "struct"; }

  // try_compile { … } - kernel call with tail unit:
// 
// block, otherwise empty
  node_id try_compile_body(node_id n) const {
    if(!is<n_call>(n)) return {};
    auto const & c = as<n_call>(n);
    if(c.args.size() != 1 || !c.tail || !call_named_kind(c, sc_try)) return {};
    return c.args[0];
  }

  // Repeated catch(A a) {…} catch(B b) {…} Call Clause - One Argument-Cortege of Array {…}
  // Blocks
// Pak is a motorcade, all the elements of which are blocks: elements, otherwise empty.
  node_list const * clause_pack(node_id n) const {
    if(!is<n_construct>(n)) return nullptr;
    auto const & c = as<n_construct>(n);
    if(c.items.empty() || !is<n_ident>(c.type) || strings.text(as<n_ident>(c.type).name) != "array") return nullptr;
    for(node_id x: c.items)
      if(!is<n_block>(x) && !(is<n_lambda>(x) && as<n_lambda>(x).block)) return nullptr;
    return &c.items;
  }
  // Processor blocks: the pack - its elements, otherwise - one
  node_list handlers_of(node_id n) const {
    if(auto p = clause_pack(n)) return *p;
    return {n};
  }

  // Hide x is a kernel call over a name: a name, otherwise empty.
  symbol hide_name(node_id n) const {
    if(!is<n_call>(n)) return {};
    auto const & c = as<n_call>(n);
    if(c.args.size() != 1 || !call_named_kind(c, sc_hide) || !is<n_ident>(c.args[0])) return {};
    return as<n_ident>(c.args[0]).name;
  }

  void replace(node_id n, node_kind k) {
    // Quoting the source of the mutations (or node over it - deletion) rolls them back.
    if(!mutations.empty())
      for(auto const & m: mutations)
        if(within(node_id(m.source - 1), n) || within(n, node_id(m.source - 1))) {
          rollback(m.source);
          break;
        }
    ++revision;
    ++edits;
    nodes[n].kind = std::move(k);
    touch(n);
    if(edited_at.size() <= n.value) edited_at.resize(n.value + 1);
    edited_at[n.value] = revision;
    log({j_replace{n.value, nodes[n].kind}});
  }

  // Up to function: Cache granularity - functions, not files
  void touch(node_id n) {
    for(node_id x = n; x; x = nodes[x].parent) {
      if(touch_logging) touch_undo.push_back({x.value, node_rev[x.value]});
      node_rev[x.value] = revision;
      if(std::holds_alternative<n_fn>(nodes[x].kind)) break;
    }
  }
  bool within(node_id x, node_id top) const {
    for(; x; x = nodes[x].parent)
      if(x == top) return true;
    return false;
  }

  // -- mutations of the ct code (§7.4) The node changes the ct code (`e.поле = v` during
  // elaboration: the former form is in the mutation record (rollback), the source is a ct-code
  // node. The view of the node is transferred, not overwritten: executable rounds hold links to
  // its lists - old buffers live in records and in retired ones.
  //
//  
// The mutation changes the base node.
  // source - node ct-code number + 1; 0 - without writing: the node created by the
  // same elaboration is completed, and does not mutate the program (no rollback)
  struct mutation {
    uint32_t target;
    uint32_t source;
    uint64_t at; // Mutation revision: the right node does not roll back after it
    node_kind old;
  };
  void mutate(node_id n, node_kind k, uint32_t source) {
    ++revision;
    if(source) mutations.push_back({n.value, source, revision, std::move(nodes[n].kind)});
    else retired.push_back(std::move(nodes[n].kind));
    nodes[n].kind = std::move(k);
    touch(n);
    changed.push_back(n.value);
    log({j_mutate{n.value, nodes[n].kind, source}});
  }
  // The rollback of mutations of the source (0 - all) and all after the first of them - later
  // ones could read its result (canonical order, §7.3 p.5):
// The node that is right after the mutation remains.
  // The righteousness of the scriptures.
  void rollback(uint32_t source) {
    std::size_t from = 0;
    while(from < mutations.size() && source && mutations[from].source != source) ++from;
    if(from == mutations.size()) return;
    ++revision;
    for(std::size_t i = mutations.size(); i-- > from;) {
      mutation & m = mutations[i];
      node_id n(m.target);
      if(m.target < edited_at.size() && edited_at[m.target] > m.at) continue;
      retired.push_back(std::move(nodes[n].kind));
      nodes[n].kind = std::move(m.old);
      touch(n);
      changed.push_back(m.target);
    }
    for(std::size_t i = from; i < mutations.size(); ++i) retired.push_back(std::move(mutations[i].old));
    mutations.resize(from);
    log({j_rollback{source}});
  }

  // Re-elaboration (ct module over written): rollback of all mutations, ct code makes them again.
  // *My:* if the result is the same (the subtrees of the modified node are the same in
  // structure), node revisions return to the same, and rollback and repetition are not in
  // journal: dependent analysis (instance, JIT) invalidates only the present change, and nested
  // elaborations (parsing instance, compiler on the language) do not reset it.
  struct elab_mark {
    std::vector<std::pair<uint32_t, node_kind>> was; // modified node before rollback
    std::size_t log_from, changed_from;
    bool nested; // inside another elaboration (ct code caused parsing, JIT): node - as is
  };
  elab_mark begin_elaboration() {
    if(elab_depth++) return {{}, 0, 0, true};
    elab_mark m{{}, oplog.size(), changed.size(), false};
    for(auto const & x: mutations)
      if(std::ranges::none_of(m.was, [&](auto const & w) { return w.first == x.target; })) m.was.push_back({x.target, nodes[node_id(x.target)].kind});
    touch_undo.clear();
    touch_logging = true;
    rollback(0);
    return m;
  }
  void end_elaboration(elab_mark const & m) {
    --elab_depth;
    if(m.nested) return;
    touch_logging = false;
    bool same = true;
    for(auto const & x: mutations) // Every change is changed before and before.
      same = same && std::ranges::any_of(m.was, [&](auto const & w) { return w.first == x.target; });
    for(auto const & [t, k]: m.was)
      same = same && std::ranges::any_of(mutations, [&](auto const & x) { return x.target == t; }) && same_kind(k, nodes[node_id(t)].kind);
    if(same) {
      for(std::size_t i = touch_undo.size(); i-- > 0;) node_rev[touch_undo[i].first] = touch_undo[i].second;
      changed.resize(m.changed_from);
      auto tail = std::ranges::remove_if(oplog.begin() + std::ptrdiff_t(m.log_from), oplog.end(), [](auto const & e) {
        return std::holds_alternative<j_rollback>(e.second.op) || (std::holds_alternative<j_mutate>(e.second.op) && std::get<j_mutate>(e.second.op).source);
      });
      oplog.erase(tail.begin(), tail.end());
    }
    touch_undo.clear();
  }
  // Subtrees of the same structure (node numbers may vary)
  bool same_tree(node_id a, node_id b) const {
    if(a == b) return true;
    if(!a || !b) return false;
    return same_kind(nodes[a].kind, nodes[b].kind);
  }
  bool same_kind(node_kind const & a, node_kind const & b) const {
    if(a.index() != b.index()) return false;
    bool same = true;
    std::visit([&](auto const & x) {
      auto const & y = std::get<std::remove_cvref_t<decltype(x)>>(b);
      template for(constexpr auto m: fields_of<std::remove_cvref_t<decltype(x)>>()) {
        using field_t = [:std::meta::type_of(m):];
        if constexpr(std::is_same_v<field_t, node_id>) same = same && same_tree(x.[:m:], y.[:m:]);
        else if constexpr(std::is_same_v<field_t, node_list>) {
          same = same && x.[:m:].size() == y.[:m:].size();
          for(std::size_t i = 0; same && i < x.[:m:].size(); ++i) same = same_tree(x.[:m:][i], y.[:m:][i]);
        } else same = same && x.[:m:] == y.[:m:];
      }
    },
        a);
    return same;
  }

  // Assign a parent to all descendants - bypass through reflection of node fields.
  void link_parents(node_id root) {
    ++revision;
    if(is<n_module>(root) && as<n_module>(root).name && strings.text(as<n_module>(root).name) == "prelude") prelude = root;
    link_rec(root);
    log({j_link{root.value}});
  }

  // Text input - operation on the base (§3.1): the text is stored in journal, the
  // positions node (span) indicate it.
  uint32_t record_import(std::string text, std::string label = {}) {
    sources.push_back(text);
    labels.push_back(label);
    log({j_import{std::move(text), std::move(label)}});
    return uint32_t(sources.size() - 1);
  }

  // Import tag for diagnostics (empty - main file).
  std::string const & source_label(uint32_t i) const {
    static std::string const empty;
    return i < labels.size() ? labels[i] : empty;
  }

  std::string const & source(uint32_t i = 0) const {
    static std::string const empty;
    return i < sources.size() ? sources[i] : empty;
  }

  // journal with new strings before each operation.
  std::vector<journal_entry> journal() const {
    std::vector<journal_entry> out;
    uint32_t emitted = 0;
    for(auto const & [strings_count, e]: oplog) {
      for(; emitted < strings_count; ++emitted)
        out.push_back({e.revision, j_intern{emitted, std::string(strings.text(symbol(emitted)))}});
      out.push_back(e);
    }
    for(; emitted < strings.size(); ++emitted)
      out.push_back({revision, j_intern{emitted, std::string(strings.text(symbol(emitted)))}});
    return out;
  }

  // Play journal to an empty database.
  void replay(std::vector<journal_entry> const & entries) {
    for(auto const & e: entries) {
      std::visit([&](auto const & op) {
        using T = std::remove_cvref_t<decltype(op)>;
        if constexpr(std::is_same_v<T, j_intern>) {
          if(strings.intern(op.text).value != op.id) throw diagnostic("журнал повреждён: порядок строк", {});
        } else if constexpr(std::is_same_v<T, j_add>) {
          pinned_source = op.where.source; // as written (node parsing raw block - in its text)
          node_id got = add(op.kind, op.where);
          pinned_source = ~uint32_t{0};
          if(got.value != op.id) throw diagnostic("журнал повреждён: порядок узлов", {});
        } else if constexpr(std::is_same_v<T, j_replace>) {
          replace(node_id(op.id), op.kind);
        } else if constexpr(std::is_same_v<T, j_link>) {
          link_parents(node_id(op.root));
        } else if constexpr(std::is_same_v<T, j_mutate>) {
          mutate(node_id(op.id), op.kind, op.source);
        } else if constexpr(std::is_same_v<T, j_rollback>) {
          rollback(op.source);
        } else {
          record_import(op.text, op.label);
        }
      },
          e.op);
    }
  }

  // Node - context (module, block): heir to n_context
  bool is_context(node_id n) const {
    return n && std::visit([](auto const & k) { return std::is_base_of_v<n_context, std::remove_cvref_t<decltype(k)>>; }, nodes[n].kind);
  }
  // contextual elements
  node_list const & context_items(node_id n) const { return is<n_block>(n) ? as<n_block>(n).items : is<n_quote>(n) ? as<n_quote>(n).items : as<n_module>(n).items; }

  // All daughter node_id node: One realization for all species through reflection.
  void for_each_child(node_id n, auto && f) const {
    std::visit([&](auto const & k) { for_each_child_of(k, f); }, nodes[n].kind);
  }

  uint32_t size() const { return nodes.size(); }

  interner strings;
  void log(journal_op op) { oplog.push_back({strings.size(), journal_entry{revision, std::move(op)}}); }

  void link_rec(node_id root) {
    for_each_child(root, [&](node_id child) {
      nodes[child].parent = root;
      link_rec(child);
    });
  }

  // A copy of a subtree; subst(n) can return a node replacement (#x insert).
  node_id clone(node_id n, auto && subst) {
    if(!n) return n;
    if(auto r = subst(n)) return *r;
    node_kind k = nodes[n].kind; // Copy: Arena could move out
    span w = nodes[n].where;
    std::visit([&](auto & x) {
      template for(constexpr auto m: fields_of<std::remove_cvref_t<decltype(x)>>()) {
        using field_t = [:std::meta::type_of(m):];
        if constexpr(std::is_same_v<field_t, node_id>) x.[:m:] = clone(x.[:m:], subst);
        else if constexpr(std::is_same_v<field_t, node_list>)
          for(node_id & c: x.[:m:]) c = clone(c, subst);
      }
    },
        k);
    return add(std::move(k), w);
  }

  static void for_each_child_of(auto const & k, auto & f) {
    template for(constexpr auto m: fields_of<std::remove_cvref_t<decltype(k)>>()) {
      using field_t = [:std::meta::type_of(m):];
      if constexpr(std::is_same_v<field_t, node_id>) {
        if(k.[:m:]) f(k.[:m:]);
      } else if constexpr(std::is_same_v<field_t, node_list>) {
        for(node_id c: k.[:m:])
          if(c) f(c);
      }
    }
  }

  arena<node, node_tag> nodes;
  node_id prelude; // prelude module: kernel designs written in language
  bool loading_prelude = false;
  uint64_t revision = 0;
  std::vector<std::pair<uint32_t, journal_entry>> oplog; // (line interned at time, operation)
  std::deque<std::string> sources; // Deque: Text references are stable
  std::vector<std::string> labels;
  std::vector<uint64_t> node_rev;
  std::vector<uint64_t> edited_at; // revision of the last node edit (not mutations)
  std::vector<mutation> mutations; // active ct-code mutations in order
  std::deque<node_kind> retired; // Species filmed by mutation and rollback: bypasses can keep their lists
  std::vector<uint32_t> changed; // node modified by mutation/rollback (editor updates them)
  std::vector<std::pair<uint32_t, uint64_t>> touch_undo; // previous revisions (elaboration with the same result)
  bool touch_logging = false;
  uint32_t elab_depth = 0; // elaborative
  uint32_t pinned_source = ~uint32_t{0}; // import of new node (parsing raw block - its text); ~0 - last

  // parsing raw block (§4.4) - derivative, like instance template: not human editing and not
  // ct-code mutation (journal source no, there is nothing to roll back). Cache by node; the key
  // is a revision of the block, the structure and its rules and the base correction counter (the
  // parsing library edit also changes the result). *My:* node total is in journal (j_add), the
  // cache itself is not: the .tzdb block is disassembled again.
  struct derived_rec {
    std::array<uint64_t, 4> key;
    uint32_t node;
  };
  std::unordered_map<uint32_t, derived_rec> derived;
  uint64_t edits = 0; // replacement
};

// The parsing rule of the decl blocks is: `syntax {r} => f(r);` in her body. The ad repeats the
// recording, like operator and syntax: "" {r} "" is the entire block.
bool raw_rule_form(ast_db const & db, n_fn const & f) {
  return f.form.size() == 1 && db.strings.text(f.form[0]) == "{" && f.params.size() == 1;
}
node_id raw_rule_of(ast_db const & db, node_id decl) {
  if(!db.is<n_fn>(decl) || !db.is<n_block>(db.as<n_fn>(decl).body)) return {};
  for(node_id x: db.as<n_block>(db.as<n_fn>(decl).body).items)
    if(db.is<n_fn>(x) && raw_rule_form(db, db.as<n_fn>(x))) return x;
  return {};
}

// Module with key in the database (last import - valid).
node_id find_module(ast_db const & db, symbol key) {
  node_id found;
  for(uint32_t i = 0; i < db.size(); ++i)
    if(db.is<n_module>(node_id(i)) && db.as<n_module>(node_id(i)).name == key) found = node_id(i);
  return found;
}

// Diagnostics on the basis: the text is taken from the import to which the node belongs.
std::string render(diagnostic const & d, std::string_view file, ast_db const & db) {
  auto name = [&](span w) -> std::string_view {
    auto const & l = db.source_label(w.source);
    return l.empty() ? file : std::string_view(l);
  };
  auto lc = locate(db.source(d.where.source), d.where.begin);
  auto out = cat(name(d.where), ":", lc.line, ":", lc.col, ": ошибка: ", d.what(), "\n");
  for(auto const & f: d.trace) {
    auto l = locate(db.source(f.where.source), f.where.begin);
    out += cat("  ", name(f.where), ":", l.line, ":", l.col, ": ", f.what, "\n");
  }
  return out;
}

// The name of the type node is the name of its structure (n_op, n_if...): a tag of the
// form node for code as a value (§7.4). Is there any alternative to node_kind?
bool is_node_kind(std::string_view name) {
  bool found = false;
  template for(constexpr auto t: std::define_static_array(std::meta::template_arguments_of(std::meta::dealias(^^node_kind))))
    found = found || std::meta::identifier_of(t) == name;
  return found;
}

// The node type number in node_kind is named for the structure (-1 is not).
int node_kind_index(std::string_view name) {
  int out = -1, i = 0;
  template for(constexpr auto t: std::define_static_array(std::meta::template_arguments_of(std::meta::dealias(^^node_kind)))) {
    if(std::meta::identifier_of(t) == name) out = i;
    ++i;
  }
  return out;
}

// Fields in the form node; -1 - not all fields node (such a node does not build a record).
int node_code_fields(int kind) {
  int out = -1, i = 0;
  template for(constexpr auto t: std::define_static_array(std::meta::template_arguments_of(std::meta::dealias(^^node_kind)))) {
    if(i == kind) {
      int n = 0;
      bool code = true;
      template for(constexpr auto m: fields_of<typename[:t:]>()) {
        code = code && std::is_same_v<typename[:std::meta::type_of(m):], node_id>;
        ++n;
      }
      out = code ? n : -1;
    }
    ++i;
  }
  return out;
}

// Kind node with fields in order (empty node_id - the field is empty).
node_kind make_node(int kind, std::vector<node_id> const & places) {
  node_kind out;
  int i = 0;
  template for(constexpr auto t: std::define_static_array(std::meta::template_arguments_of(std::meta::dealias(^^node_kind)))) {
    if(i == kind) {
      typename[:t:] x{};
      uint32_t k = 0;
      template for(constexpr auto m: fields_of<typename[:t:]>()) {
        if constexpr(std::is_same_v<typename[:std::meta::type_of(m):], node_id>)
          if(k < places.size()) x.[:m:] = places[k];
        ++k;
      }
      out = std::move(x);
    }
    ++i;
  }
  return out;
}

// Place node (if the field is a node)
node_id node_place(node_kind const & k, uint32_t place) {
  node_id out;
  std::visit([&](auto const & x) {
    uint32_t i = 0;
    template for(constexpr auto m: fields_of<std::remove_cvref_t<decltype(x)>>()) {
      if constexpr(std::is_same_v<typename[:std::meta::type_of(m):], node_id>)
        if(i == place) out = x.[:m:];
      ++i;
    }
  },
      k);
  return out;
}

// Write (§4.4): Syntax-ad, whose body is one node above the parameters: `syntax loop
// {body} => n_loop{body};` is the node record of the kernel; `syntax hide {x} =>
// hide(x);` is the record of the kernel call under the head name.
// 
// . parsing builds the node itself (not a function call), node prints on the record; the
// record is not a function (not registered, not called). `_` in the body is an empty field.
//
struct record_info {
  int kind = -1; // node (number in node_kind); -1 - kernel call with head name
  std::vector<uint32_t> slot; // parameter k → place in the body (field node / call argument)
  uint32_t places = 0; // body-place
};

std::optional<record_info> record_of(ast_db const & db, node_id fn) {
  if(!db.is<n_fn>(fn)) return std::nullopt;
  auto const & f = db.as<n_fn>(fn);
  if(f.form.empty() || !f.body) return std::nullopt;
  record_info r;
  node_list items;
  if(db.is<n_construct>(f.body) && db.is<n_ident>(db.as<n_construct>(f.body).type)) {
    r.kind = node_kind_index(db.strings.text(db.as<n_ident>(db.as<n_construct>(f.body).type).name));
    if(r.kind < 0) return std::nullopt;
    int n = node_code_fields(r.kind);
    items = db.as<n_construct>(f.body).items;
    if(n < 0 || items.size() > uint32_t(n)) return std::nullopt;
    r.places = uint32_t(n);
  } else if(db.is<n_call>(f.body)) {
    auto const & c = db.as<n_call>(f.body);
    if(!db.is<n_ident>(c.callee) || db.as<n_ident>(c.callee).name != f.name || !c.words.empty() || c.tagged || c.form) return std::nullopt;
    items = c.args;
    r.places = uint32_t(items.size());
  } else return std::nullopt;
  r.slot.assign(f.params.size(), ~uint32_t{0});
  for(uint32_t i = 0; i < items.size(); ++i) {
    if(!db.is<n_ident>(items[i])) return std::nullopt;
    symbol s = db.as<n_ident>(items[i]).name;
    if(r.kind >= 0 && db.strings.text(s) == "_") continue;
    uint32_t k = 0;
    while(k < f.params.size() && db.as<n_param>(f.params[k]).name != s) ++k;
    if(k == f.params.size() || r.slot[k] != ~uint32_t{0}) return std::nullopt;
    r.slot[k] = i;
  }
  for(uint32_t s: r.slot)
    if(s == ~uint32_t{0}) return std::nullopt;
  return r;
}

// node field type in the program: node - code, list node and names - array, name and text - str.
template<typename F>
consteval char const * node_field_type() {
  if constexpr(std::is_same_v<F, node_id>) return "code";
  else if constexpr(std::is_same_v<F, node_list> || std::is_same_v<F, std::vector<symbol>> || std::is_same_v<F, std::vector<uint32_t>>) return "array";
  else if constexpr(std::is_same_v<F, symbol> || std::is_same_v<F, std::string>) return "str";
  else if constexpr(std::is_same_v<F, bool>) return "bool";
  else if constexpr(std::is_floating_point_v<F>) return "f64";
  else return "i64";
}

// node AST - struct programs:
// 
// Ads are generated from node C++ structures (one source) and parsed together with
// prelude. The node base (n_block: n_context) is the program's struct base: is_a, a
// sample base.
std::string ast_structs_text() {
  std::string out = "// узлы АСТ (порождено из структур узлов VM)\n";
  out += cat("struct ", std::meta::identifier_of(^^n_context), " { }\n");
  template for(constexpr auto t: std::define_static_array(std::meta::template_arguments_of(std::meta::dealias(^^node_kind)))) {
    out += cat("struct ", std::meta::identifier_of(t));
    template for(constexpr auto b: std::define_static_array(std::meta::bases_of(t, std::meta::access_context::unchecked())))
      out += cat(" : ", std::meta::identifier_of(std::meta::type_of(b)));
    out += " {";
    template for(constexpr auto m: fields_of<typename[:t:]>())
      out += cat(" ", node_field_type<typename[:std::meta::type_of(m):]>(), " ", std::meta::identifier_of(m), ";");
    out += " }\n";
  }
  return out;
}

// The name node is for debugging (through reflection, without manual tables).
std::string_view kind_name(node const & n) {
  return std::visit([](auto const & k) -> std::string_view {
    return std::meta::identifier_of(std::meta::remove_cvref(^^decltype(k)));
  },
      n.kind);
}

} // namespace tz
