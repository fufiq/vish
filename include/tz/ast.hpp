// The AST database is the source of program truth (§3.1, D7).
//
// node are data aggregates owned by ast_db. The links between node are indexes
// (node_id), not pointers or names in the text: Each change increases the revision
// - on this is reactive elaboration (§10.1).
//
#pragma once

#include "base.hpp"

#include <deque>
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

// blank[k] - empty lines before the element k (over canonical), the last - before the
// end: the layout of the text that a person put Enter (author's decision 2026-09-29).
// The performance doesn't see her.
struct n_module {
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
};
struct n_block {
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
struct n_member {
  node_id object;
  symbol name;
};
// v.[f] is a descriptor field known in compilation (§7.4)
struct n_field {
  node_id object;
  node_id key;
};
struct n_index {
  node_id object;
  node_id index;
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
// Repeated call clause: catch(A a) {…} catch(B b) {…} is one argument.
struct n_pack {
  node_list items;
};
// Named aggregate (§5.1): nominal type; fields - n_var (type, name, default value).
// Created positionally: Name {a, b}.
struct n_struct {
  symbol name;
  node_list fields;
  node_list attrs;
  symbol base; // struct e: base { … } - heir: base fields first, base substitution
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
};
// Code as value (§7.4): quote { объявления } and insert #x.
//
struct n_quote {
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
}; // array{1, 2, 3}
struct n_auto {}; // `auto` in type position
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
// Tag "text {выражение} text" - a template with the tag:
//  
// parts: текст (n_str), выражение, текст, …, текст.
struct n_template {
  node_id tag;
  node_list parts;
};

// native "cpp" { … } is a function body in C++ (§3.3):
// 
// Text as written; `#x` is the parameter x of the surrounding function with the
// value of VM. It is going to VM (tz native).
struct n_native {
  symbol lang;
  std::string code;
};

using node_kind = std::variant<n_module, n_fn, n_param, n_var, n_block, n_ident, n_int, n_float, n_str,
    n_call, n_op, n_member, n_field, n_index, n_if, n_loop, n_break, n_continue, n_return,
    n_ct, n_lambda, n_construct, n_auto, n_hole, n_throw, n_pack, n_chain, n_struct, n_extend, n_quote, n_splice, n_pat, n_unpack, n_pat_field, n_subject, n_spread, n_template, n_use, n_native>;

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
using journal_op = std::variant<j_intern, j_add, j_replace, j_link, j_import>;

struct journal_entry {
  uint64_t revision;
  journal_op op;
};

struct ast_db {
  node_id add(node_kind k, span where) {
    ++revision;
    where.source = uint32_t(sources.empty() ? 0 : sources.size() - 1);
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

  // require(c) is a normal kernel call, not node: condition c, otherwise empty
  node_id require_cond(node_id n) const {
    if(!is<n_call>(n)) return {};
    auto const & c = as<n_call>(n);
    if(c.args.size() != 1 || !c.words.empty() || !is<n_ident>(c.callee) || strings.text(as<n_ident>(c.callee).name) != "require") return {};
    return c.args[0];
  }

  // try_compile { … } - kernel call with tail unit:
// 
// block, otherwise empty
  node_id try_compile_body(node_id n) const {
    if(!is<n_call>(n)) return {};
    auto const & c = as<n_call>(n);
    if(c.args.size() != 1 || !c.tail || !c.words.empty() || !is<n_ident>(c.callee) || strings.text(as<n_ident>(c.callee).name) != "try_compile") return {};
    return c.args[0];
  }

  // Hide x is a kernel call over a name: a name, otherwise empty.
  symbol hide_name(node_id n) const {
    if(!is<n_call>(n)) return {};
    auto const & c = as<n_call>(n);
    if(c.args.size() != 1 || !c.words.empty() || !is<n_ident>(c.callee) || !is<n_ident>(c.args[0]) || strings.text(as<n_ident>(c.callee).name) != "hide") return {};
    return as<n_ident>(c.args[0]).name;
  }

  void replace(node_id n, node_kind k) {
    ++revision;
    nodes[n].kind = std::move(k);
    // Up to function: Cache granularity - functions, not files
    for(node_id x = n; x; x = nodes[x].parent) {
      node_rev[x.value] = revision;
      if(std::holds_alternative<n_fn>(nodes[x].kind)) break;
    }
    log({j_replace{n.value, nodes[n].kind}});
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
          if(add(op.kind, op.where).value != op.id) throw diagnostic("журнал повреждён: порядок узлов", {});
        } else if constexpr(std::is_same_v<T, j_replace>) {
          replace(node_id(op.id), op.kind);
        } else if constexpr(std::is_same_v<T, j_link>) {
          link_parents(node_id(op.root));
        } else {
          record_import(op.text, op.label);
        }
      },
          e.op);
    }
  }

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
};

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

// The name node is for debugging (through reflection, without manual tables).
std::string_view kind_name(node const & n) {
  return std::visit([](auto const & k) -> std::string_view {
    return std::meta::identifier_of(std::meta::remove_cvref(^^decltype(k)));
  },
      n.kind);
}

} // namespace tz
