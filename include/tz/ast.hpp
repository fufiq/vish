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
#include <bit>
#include <deque>
#include <optional>
#include <unordered_map>
#include <map>
#include <set>
#include <tuple>
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
  // [[extension(проверка)]] module k; - control of extensions entity module (§9.1):
  // 
  // 
  node_list attrs;
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
  // 10s - s(10) call : number and close name (castomic literature,
// without `_` : ); writing in
// literature
  bool literal_suffix = false;
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

// Node of the type declared by the program: struct ast.twice { code x; }
// 
// The meaning of the form is the disclosure in the known node ([[transform]]
// f(ast.twice t), author's decision 2026-10-17: ). *My:* field is node (code) in the
// order of the announcement; in node_kind - the last (type numbers in journal .tzdb do
// not shift).
struct n_node {
  std::string kind; // ast.twice
  node_list items;
};

using node_kind = std::variant<n_module, n_fn, n_param, n_var, n_block, n_ident, n_int, n_float, n_str,
    n_call, n_op, n_if, n_loop, n_break, n_continue, n_return,
    n_ct, n_lambda, n_construct, n_hole, n_throw, n_chain, n_extend, n_quote, n_splice, n_pat, n_unpack, n_pat_field, n_subject, n_spread, n_use, n_native, n_ref, n_raw, n_sym, n_node>;
constexpr std::size_t program_kind_index = std::variant_size_v<node_kind> - 1; // n_node

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
// New place node: the raw block, redisassembled in place (editor's unit, §13.1), retains the
// id, and its atoms - in the editing text; the block place - in the same place (it takes the
// text of the atom as written).
struct j_place {
  uint32_t id;
  span where;
};
// Comments (§4.0, author's decision 2026-10-05: ): the text of a person with an element of
// context is a layout in the database, as empty lines (blank); the execution does not see it.
// lead - the lines before the element (comments as written, "" - an empty line between them),
// tail - the comment at the end of the line of the element, end - at the context (block, module):
// the lines before its end. There is no logic in the comments:
//
struct node_note {
  std::vector<std::string> lead;
  std::string tail;
  std::vector<std::string> end;
};
struct j_note {
  uint32_t id;
  node_note note;
};
// Pass: nodes empty node and imports empty imports - the numbers dropped parsing draft
// editor (squash_log): the numbers of the others are the same.
struct j_gap {
  uint32_t nodes;
  uint32_t imports;
};
using journal_op = std::variant<j_intern, j_add, j_replace, j_link, j_import, j_mutate, j_rollback, j_place, j_note, j_gap>;

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
    birth.push_back(++births);
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
          (t == "require" || t == "assume" ? sc_require : 0) | (t == "hide" ? sc_hide : 0) | (t == "try_compile" ? sc_try : 0) |
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

  // assume(c) is a test as required, but not a condition for choosing overload and after that
  // condition is the fact of parsing (§10.2).
  bool is_assume(node_id n) const {
    return is<n_call>(n) && is<n_ident>(as<n_call>(n).callee) && strings.text(as<n_ident>(as<n_call>(n).callee).name) == "assume";
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
  // The node stood in a new place (raw block, redisassembled on site).
  void place(node_id n, span where) {
    nodes[n].where = where;
    log({j_place{n.value, where}});
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
    // new module elements - generated by ct code instance (and when playing journal)
    if(source && is<n_module>(n) && std::holds_alternative<n_module>(k))
      for(node_id x: std::get<n_module>(k).items)
        if(!std::ranges::contains(as<n_module>(n).items, x)) {
          inst_emits.insert(x.value);
          emit_owner[x.value] = n.value;
        }
    if(source) mutations.push_back({n.value, source, revision, std::move(nodes[n].kind)});
    else retired.push_back(std::move(nodes[n].kind));
    nodes[n].kind = std::move(k);
    if(!source && reopened(n)) return; // Derivative taken again: in journal - when reconciled
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
    std::size_t reborn_from = 0; // Taken again derivatives of this elaboration - reconciliation at the end
  };
  // The result of elaboration is valid as long as there are no human edits (*my*): the new
  // interpreter (JIT, parsing, run in session) does not repeat the rollback and parsing of the
  // program - node as is, as in nested elaboration. Otherwise, each parsing instance for JIT
  // would re-execute the ct code of all instance programs.
  bool elab_current() const { return elab_edits == edits; }
  elab_mark begin_elaboration() {
    if(elab_depth++ || elab_current()) return {{}, 0, 0, true};
    elab_edits = ~uint64_t{0}; // Goes again: not finished (error) - does not work
    elab_ct.clear();
    elab_pass_error.reset();
    elab_changes.clear();
    elab_mark m{{}, oplog.size(), changed.size(), false, reborn.size()};
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
    // Derivatives taken again: the other result is not the same (*my*)
    bool same = !flush_reborn(std::min(m.reborn_from, reborn.size()));
    touch_logging = false;
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

  // Derivative nodes: the same result - the same nodes - the same nodes - the same nodes - the
  // same nodes - the nodes that create the execution (n X{...} during elaboration and launch,
  // insert #x, the result of the analysis of the raw block, chain trees, sample nodes), - a
  // derivative, as a template instance.
// 
  // A node with the same appearance at birth and the same place - the same, if it is free - does
  // not stand in the program tree and is not held by a living interpreter. So the elaboration
  // again, the recalculation of the parsing block and the new interpreter (check, run, JIT, editor)
  // do not grow the base and journal; the derivative numbers are stable (JIT and audit caches,
  // editor). The new node is completed without journal; at the end (reconciliation - flush_reborn)
  // in journal there is only a difference from the previous result.
  struct born_rec {
    node_kind kind; // type at birth (before completion)
    uint64_t key;
  };
  struct reborn_rec {
    uint32_t id;
    node_kind was; // pre-capture
    node_id parent;
    void const * holder;
  };
  struct reuse_list {
    std::vector<uint32_t> ids;
    std::size_t cursor = 0; // What to look for free (liberation returns to the beginning)
  };

  static void hash_mix(uint64_t & h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); }
  static uint64_t field_hash(auto const & f) {
    using F = std::remove_cvref_t<decltype(f)>;
    if constexpr(std::is_same_v<F, node_id> || std::is_same_v<F, symbol>) return f.value;
    else if constexpr(std::is_same_v<F, std::string>) return std::hash<std::string>{}(f);
    else if constexpr(std::is_floating_point_v<F>) return std::bit_cast<uint64_t>(f);
    else if constexpr(std::is_enum_v<F> || std::is_integral_v<F>) return uint64_t(f);
    else {
      uint64_t h = f.size();
      for(auto const & x: f) hash_mix(h, field_hash(x));
      return h;
    }
  }
  static bool field_equal(auto const & a, auto const & b) {
    using F = std::remove_cvref_t<decltype(a)>;
    if constexpr(std::is_floating_point_v<F>) return std::bit_cast<uint64_t>(a) == std::bit_cast<uint64_t>(b);
    else return a == b;
  }
  static uint64_t kind_hash(node_kind const & k, span where) {
    uint64_t h = k.index();
    hash_mix(h, where.begin);
    hash_mix(h, where.end);
    hash_mix(h, where.source);
    std::visit([&](auto const & x) {
      template for(constexpr auto m: fields_of<std::remove_cvref_t<decltype(x)>>()) hash_mix(h, field_hash(x.[:m:]));
    },
        k);
    return h;
  }
  // Species match the field in the field (children are the same numbers)
  static bool kind_equal(node_kind const & a, node_kind const & b) {
    if(a.index() != b.index()) return false;
    bool same = true;
    std::visit([&](auto const & x) {
      auto const & y = std::get<std::remove_cvref_t<decltype(x)>>(b);
      template for(constexpr auto m: fields_of<std::remove_cvref_t<decltype(x)>>()) same = same && field_equal(x.[:m:], y.[:m:]);
    },
        a);
    return same;
  }
  bool contains(node_id p, node_id c) const {
    bool found = false;
    for_each_child(p, [&](node_id x) { found = found || x == c; });
    return found;
  }
  // Stands in the tree: each parent holds the node up (the result parsing block - the block in
  // derived; removed by the mutation - in its former form: rollback will return), the root -
  // the module. An outdated parent (node removed by rollback, completion) is not worth it.
  bool held_by_mutation(node_id p, node_id c) const {
    for(auto const & m: mutations)
      if(m.target == p.value) {
        bool found = false;
        auto see = [&](node_id x) { found = found || x == c; };
        std::visit([&](auto const & k) { for_each_child_of(k, see); }, m.old);
        if(found) return true;
      }
    return false;
  }
  bool installed(node_id c) const {
    for(node_id p = nodes[c].parent; p; c = p, p = nodes[p].parent)
      if(!contains(p, c) && !held_by_mutation(p, c)) {
        auto it = derived.find(p.value);
        if(!is<n_raw>(p) || it == derived.end() || it->second.node != c.value) return false;
      }
    if(auto it = emit_owner.find(c.value); it != emit_owner.end()) return contains(node_id(it->second), c); // instance is a module element
    return is<n_module>(c);
  }
  bool reopened(node_id n) const { return n.value < reopen.size() && reopen[n.value]; }
  bool held_now(uint32_t x) const { return x < held.size() && held[x]; }
  uint64_t birth_of(node_id n) const { return birth[n.value]; }

  // Derivative node: former free with the same view at birth and place, otherwise new. Holder is a
  // list of node that the interpreter holds (releases release).
  node_id derive(node_kind k, span where, std::vector<uint32_t> & holder) {
    uint64_t key = kind_hash(k, where); // place - as at the source (not the last import: editing it does not change)
    auto & list = reusable[key];
    for(std::size_t i = list.cursor, steps = 0; i < list.ids.size() && steps < 64; ++i, ++steps) {
      uint32_t x = list.ids[i];
      node_id n(x);
      if(held_now(x) || reopened(n)) continue;
      auto const & b = born.at(x);
      if(b.key != key || !kind_equal(b.kind, k) || nodes[n].where.begin != where.begin || nodes[n].where.end != where.end || nodes[n].where.source != where.source || installed(n)) continue;
      list.cursor = i + 1;
      if(reopen.size() <= x) reopen.resize(x + 1, 0);
      reopen[x] = 1;
      node_kind was = std::move(nodes[n].kind);
      nodes[n].kind = std::move(k); // Type at Birth: Continue reading
      reborn.push_back({x, std::move(was), nodes[n].parent, &holder});
      nodes[n].parent = {}; // The new owner (link_parents)
      birth[x] = ++births;
      hold(x, holder);
      return n;
    }
    uint32_t saved = std::exchange(pinned_source, where.source);
    node_id n = add(k, where);
    pinned_source = saved;
    born.emplace(n.value, born_rec{std::move(k), key});
    list.ids.push_back(n.value);
    list.cursor = list.ids.size();
    hold(n.value, holder);
    return n;
  }
  void hold(uint32_t x, std::vector<uint32_t> & holder) {
    if(held.size() <= x) held.resize(x + 1, 0);
    ++held[x];
    holder.push_back(x);
  }
  // The interpreter is over: his node is free (reconciliation of the ones he took anew - before that)
  void release(std::vector<uint32_t> & holder) {
    flush_reborn(0, &holder);
    for(uint32_t x: holder) {
      --held[x];
      if(auto b = born.find(x); b != born.end()) reusable[b->second.key].cursor = 0;
    }
    holder.clear();
  }
  // Reconciliation taken again (from the number from; holder - only his): the difference from the previous
  // result - in journal (complete), the changed node - a new revision. True, something's changed.
  bool flush_reborn(std::size_t from, void const * holder = nullptr) {
    bool any = false;
    std::size_t keep = from;
    for(std::size_t i = from; i < reborn.size(); ++i) {
      reborn_rec & r = reborn[i];
      if(holder && r.holder != holder) {
        if(keep != i) reborn[keep] = std::move(r);
        ++keep;
        continue;
      }
      node_id n(r.id);
      reopen[r.id] = 0;
      bool kind_same = kind_equal(nodes[n].kind, r.was);
      if(!kind_same || nodes[n].parent != r.parent) {
        any = any || !kind_same;
        ++revision;
        if(!kind_same) {
          touch(n);
          changed.push_back(r.id);
          log({j_mutate{r.id, nodes[n].kind, 0}});
        }
        log({j_link{r.id}});
        if(nodes[n].parent) log({j_link{nodes[n].parent.value}});
      }
      retired.push_back(std::move(r.was));
    }
    reborn.resize(keep);
    return any;
  }

  // Assign a parent to all descendants - bypass through reflection of node fields.
  void link_parents(node_id root) {
    ++revision;
    if(is<n_module>(root) && as<n_module>(root).name && strings.text(as<n_module>(root).name) == "prelude") prelude = root;
    link_rec(root);
    if(!reopened(root)) log({j_link{root.value}});
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
    for(auto const & r: reborn) // Taken again, not yet verified: state - as is
      if(!kind_equal(nodes[node_id(r.id)].kind, r.was) || nodes[node_id(r.id)].parent != r.parent) {
        out.push_back({revision, j_mutate{r.id, nodes[node_id(r.id)].kind, 0}});
        out.push_back({revision, j_link{r.id}});
        if(nodes[node_id(r.id)].parent) out.push_back({revision, j_link{nodes[node_id(r.id)].parent.value}});
      }
    return out;
  }

  // The draft editor is closed (author's decision 2026-10-12: set - as is, parsing - each key): his
  // keys in journal - one edit (*mine*). Records with revision after since: imports of typed text,
  // except for the last (keep), are empty (import numbers are the same: node places refer to them);
  // node of these imports, which fell out of wood, are empty holes (node numbers are the same), their
  // edits are out; editing one node in a row (without ct code mutations and kickbacks between them)
  // is the last. since - revision at the opening of the draft (its first import is recorded with it).
  // The state of the program tree after playback is the same, journal grows by the total, not by
  // every key.
  void squash_log(uint64_t since, std::vector<uint32_t> const & drafts, uint32_t keep) {
    std::size_t from = 0;
    while(from < oplog.size() && oplog[from].second.revision < since) ++from;
    if(from == oplog.size()) return;
    std::size_t imports = 0;
    for(std::size_t i = from; i < oplog.size(); ++i) imports += std::holds_alternative<j_import>(oplog[i].second.op);
    uint32_t source = uint32_t(sources.size() - imports);
    std::set<uint32_t> used; // draft imports referred to by node wood
    for(uint32_t x = 0; x < nodes.size(); ++x)
      if(std::ranges::contains(drafts, nodes[node_id(x)].where.source) && !used.contains(nodes[node_id(x)].where.source) && installed(node_id(x))) used.insert(nodes[node_id(x)].where.source);
    auto draft_source = [&](uint32_t x) { return x != keep && std::ranges::contains(drafts, x) && !used.contains(x); };
    auto garbage = [&](uint32_t id) {
      node_id n(id);
      return id < nodes.size() && std::ranges::contains(drafts, nodes[n].where.source) && !born.contains(id) && !installed(n);
    };
    std::vector<std::pair<uint32_t, journal_entry>> out(oplog.begin(), oplog.begin() + std::ptrdiff_t(from));
    // last edit of node in the segment without mutations: (option type, id) → record number
    std::map<std::pair<std::size_t, uint32_t>, std::size_t> last;
    std::vector<bool> drop(oplog.size(), false);
    std::vector<uint8_t> gap(oplog.size(), 0); // 1 is the node of the lost parsing, 2 is its import
    for(std::size_t i = from; i < oplog.size(); ++i) {
      auto & op = oplog[i].second.op;
      if(std::holds_alternative<j_mutate>(op) || std::holds_alternative<j_rollback>(op)) {
        last.clear();
        continue;
      }
      auto one = [&](uint32_t id) {
        if(garbage(id)) return void(drop[i] = true);
        auto key = std::pair{op.index(), id};
        if(auto it = last.find(key); it != last.end()) drop[it->second] = true;
        last[key] = i;
      };
      if(auto * x = std::get_if<j_replace>(&op)) one(x->id);
      else if(auto * x = std::get_if<j_place>(&op)) one(x->id);
      else if(auto * x = std::get_if<j_note>(&op)) one(x->id);
      else if(auto * x = std::get_if<j_link>(&op)) {
        if(garbage(x->root)) drop[i] = true;
        else one(x->root);
      } else if(auto * x = std::get_if<j_add>(&op); x && garbage(x->id)) gap[i] = 1;
      else if(std::holds_alternative<j_import>(op)) {
        if(draft_source(source)) gap[i] = 2;
        ++source;
      }
    }
    // They have only one pass between them.
    for(std::size_t i = from; i < oplog.size(); ++i) {
      if(drop[i]) continue;
      if(!gap[i]) {
        out.push_back(std::move(oplog[i]));
        continue;
      }
      if(out.size() <= from || !std::holds_alternative<j_gap>(out.back().second.op) || out.back().first != oplog[i].first)
        out.push_back({oplog[i].first, journal_entry{oplog[i].second.revision, j_gap{0, 0}}});
      auto & g = std::get<j_gap>(out.back().second.op);
      (gap[i] == 1 ? g.nodes : g.imports) += 1;
    }
    oplog = std::move(out);
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
        } else if constexpr(std::is_same_v<T, j_place>) {
          place(node_id(op.id), op.where);
        } else if constexpr(std::is_same_v<T, j_note>) {
          set_note(node_id(op.id), op.note);
        } else if constexpr(std::is_same_v<T, j_gap>) {
          for(uint32_t i = 0; i < op.nodes; ++i) {
            pinned_source = 0;
            add(n_hole{}, {});
            pinned_source = ~uint32_t{0};
          }
          for(uint32_t i = 0; i < op.imports; ++i) record_import({}, {});
        } else {
          record_import(op.text, op.label);
        }
      },
          e.op);
    }
  }

  // node (node_note) comments: Empty, no. Write in journal (as blank).
  node_note const * note_of(node_id n) const {
    if(notes.empty()) return nullptr;
    auto it = notes.find(n.value);
    return it == notes.end() ? nullptr : &it->second;
  }
  void set_note(node_id n, node_note x) {
    bool empty = x.lead.empty() && x.tail.empty() && x.end.empty();
    if(empty && !notes.contains(n.value)) return;
    if(empty) notes.erase(n.value);
    else notes[n.value] = x;
    log({j_note{n.value, std::move(x)}});
  }
  // Node to saved id, and disassembled anew as from: the text of the unit - without lines in
  // front of it and the tail (they are in the gap of context), so found new parsing inside it is
  // added to them; the end of the context - all in its text - is replaced.
  //
  void adopt_notes(node_id from, node_id to) {
    node_note const * f = note_of(from);
    if(!f || from == to) return;
    node_note x = note_of(to) ? *note_of(to) : node_note{};
    x.lead.insert(x.lead.end(), f->lead.begin(), f->lead.end());
    if(!f->tail.empty()) x.tail = x.tail.empty() ? f->tail : cat(f->tail, " ", x.tail);
    x.end = f->end;
    set_note(to, std::move(x));
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
  mutable std::unordered_map<std::string, uint32_t> program_kinds; // ast.X → node announcement (program_kind)
  void log(journal_op op) { oplog.push_back({strings.size(), journal_entry{revision, std::move(op)}}); }

  void link_rec(node_id root) {
    for_each_child(root, [&](node_id child) {
      nodes[child].parent = root;
      link_rec(child);
    });
  }

  // A copy of a subtree; subst(n) can return a replacement for node (Insert #x). holder - copy
  // derivative: the same copy of the same - the previous node.
  node_id clone(node_id n, auto && subst, std::vector<uint32_t> * holder = nullptr) {
    if(!n) return n;
    if(auto r = subst(n)) return *r;
    node_kind k = nodes[n].kind; // Copy: Arena could move out
    span w = nodes[n].where;
    std::visit([&](auto & x) {
      template for(constexpr auto m: fields_of<std::remove_cvref_t<decltype(x)>>()) {
        using field_t = [:std::meta::type_of(m):];
        if constexpr(std::is_same_v<field_t, node_id>) x.[:m:] = clone(x.[:m:], subst, holder);
        else if constexpr(std::is_same_v<field_t, node_list>)
          for(node_id & c: x.[:m:]) c = clone(c, subst, holder);
      }
    },
        k);
    if(holder) return derive(std::move(k), w, *holder);
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
  uint64_t elab_edits = ~uint64_t{0}; // edits to the end of the last completed elaboration (~0 - was not)
  std::set<std::pair<uint32_t, uint64_t>> elab_ct; // (ct node, instance) executed by parsing elaboration
  std::optional<diagnostic> elab_pass_error; // parsing elaboration did not pass (code did not change)
  std::vector<std::tuple<span, std::string, std::string>> elab_changes; // Inspector: place ct, instance, which is modified
  std::unordered_map<uint32_t, uint32_t> emit_owner; // Generated instance → Module (the element has no parent)
  std::set<uint32_t> inst_emits; // Advertisements generated by instance ct code (module elements after parsing)
  uint32_t pinned_source = ~uint32_t{0}; // import of new node (parsing raw block - its text); ~0 - last

  // parsing raw block (§4.4) - derivative, like instance template: not human editing and not
  // ct-code mutation (journal source no, nothing to roll back). Cache by node; the key is a
  // revision of the block, the design and its rules and the base correction counter (editing the
  // parsing library also changes the result). *My:* node of the total is in journal (j_add), the
  // cache itself is not: from .tzdb the block is disassembled again; the recalculation with the
  // same result takes the previous node (derive).
  struct derived_rec {
    std::array<uint64_t, 4> key;
    uint32_t node;
  };
  std::unordered_map<uint32_t, derived_rec> derived;
  uint64_t edits = 0; // replacement
  // derivatives of node (derive)
  std::unordered_map<uint32_t, born_rec> born;
  std::unordered_map<uint64_t, reuse_list> reusable; // Key to birth and location node
  std::vector<uint32_t> held; // interpreters holding the knot
  std::vector<uint8_t> reopen; // It's taken over, it hasn't been reconciled yet.
  std::vector<reborn_rec> reborn;
  std::vector<uint64_t> birth; // The tact of birth or taking again: “created by this elaboration”
  uint64_t births = 0;
  // Choices of printing by code in the language (§4.4, variant C): the key is the selection function,
  // its revision, base edits and arguments → the result (0/1, -1 - error: conservative choice). A pure
  // function is one sum to the same arguments; printing does not change the base, so mutable.
  mutable std::unordered_map<std::string, int8_t> print_choices;
  std::unordered_map<uint32_t, node_note> notes; // Node → comments (layout)
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
// The key of the node chain of names: geo.vec is a member of the vec name geo; not the chain of names is empty.
std::string module_path(ast_db const & db, node_id x) {
  std::string key;
  while(db.is_member(x)) {
    key = "." + std::string(db.strings.text(db.member_of(x).name)) + key;
    x = db.member_of(x).object;
  }
  if(!db.is<n_ident>(x)) return {};
  return std::string(db.strings.text(db.as<n_ident>(x).name)) + key;
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

// Names node in language (author's decision 2026-10-17): module `ast` -
//  
// The form node is `ast.<слово>` in the full word: the structure of C++ n_fn - `ast.function`,
// n_ident - `ast.name`; the field - without tail `_` (then → then: in C++ then / else - not
// field names). My words.
std::string_view lang_word(std::string_view cpp) {
  static constexpr std::pair<std::string_view, std::string_view> renamed[] = {
      {"n_fn", "function"}, {"n_param", "parameter"}, {"n_var", "variable"}, {"n_ident", "name"}, {"n_str", "string"},
      {"n_op", "operation"}, {"n_extend", "extension"}, {"n_pat", "pattern"}, {"n_pat_field", "pattern_field"},
      {"n_ref", "reference"}, {"n_sym", "symbol"}};
  for(auto const & [a, b]: renamed)
    if(a == cpp) return b;
  return cpp.starts_with("n_") ? cpp.substr(2) : cpp;
}
std::string lang_kind_name(std::string_view cpp) { return cat("ast.", lang_word(cpp)); }
std::string_view lang_field(std::string_view cpp) { return cpp.ends_with('_') ? cpp.substr(0, cpp.size() - 1) : cpp; }

// The name of the type node in the language by number in node_kind (table - once, by reflection).
std::string_view lang_kind(std::size_t index) {
  static std::vector<std::string> const names = [] {
    std::vector<std::string> v;
    template for(constexpr auto t: std::define_static_array(std::meta::template_arguments_of(std::meta::dealias(^^node_kind)))) v.push_back(lang_kind_name(std::meta::identifier_of(t)));
    return v;
  }();
  return index < names.size() ? std::string_view(names[index]) : std::string_view("ast.?");
}
std::string_view lang_context() {
  static std::string const n = lang_kind_name(std::meta::identifier_of(^^n_context));
  return n;
}

// node by name in language (ast.operation, ast.if...): a node tag for code as a value
// (§7.4). Is there any alternative to node_kind?
bool is_node_kind(std::string_view name) {
  if(!name.starts_with("ast.")) return false;
  for(std::size_t i = 0; i < program_kind_index; ++i)
    if(lang_kind(i) == name) return true;
  return false;
}

// Type node declared by the program (n_node): struct ast.X { code a; code b; } is field names in
// order; nullopt is no such announcement or field is no node. An ad is searched on the database
// only for a name that VM does not have; what is found is remembered and verified with each
// request (ad editing - other fields, renaming - search again).
std::optional<std::vector<symbol>> program_kind(ast_db const & db, std::string_view name) {
  if(!name.starts_with("ast.") || is_node_kind(name)) return std::nullopt;
  auto decl = [&](uint32_t i) {
    node_id d(i);
    if(!db.is<n_extend>(d)) return false;
    auto const & e = db.as<n_extend>(d);
    return e.of && db.strings.text(e.of) == "struct" && e.name && db.strings.text(e.name) == name;
  };
  uint32_t at = 0;
  if(auto it = db.program_kinds.find(std::string(name)); it != db.program_kinds.end() && decl(it->second)) at = it->second;
  else {
    for(uint32_t i = 1; i < db.size(); ++i)
      if(decl(i)) at = i; // last
    if(!at) return std::nullopt;
    db.program_kinds[std::string(name)] = at;
  }
  std::vector<symbol> fields;
  for(node_id x: db.as<n_extend>(node_id(at)).items) {
    if(!db.is<n_var>(x)) continue;
    auto const & v = db.as<n_var>(x);
    if(!v.type || !db.is<n_ident>(v.type) || db.strings.text(db.as<n_ident>(v.type).name) != "code") return std::nullopt;
    fields.push_back(v.name);
  }
  return fields;
}

// The node type number in node_kind is named for the structure (-1 is not).
int node_kind_index(std::string_view name) {
  for(std::size_t i = 0; i < program_kind_index; ++i)
    if(lang_kind(i) == name) return int(i);
  return -1;
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

// Kind node with fields in order (empty node_id is empty); program is the name of the program
// type (n_node).
node_kind make_node(int kind, std::vector<node_id> const & places, std::string_view program = {}) {
  if(!program.empty()) return n_node{std::string(program), places};
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
  if(auto const * u = std::get_if<n_node>(&k)) return place < u->items.size() ? u->items[place] : node_id{}; // programme
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
  std::string program; // View declared by the program (kind - n_node): ast.twice
};

// Marking layout in the syntax record (§4.4, projection): `[[line]] else` - part from the
// new line, `[[indent]] {x}` - body-non-block from the new line deeper, `[[space]]` -
// through the space, `[[glued]]` - close. Print only: parsing does not see gaps. In the form
// - before its part, the text "[name]".
bool form_mark(std::string_view p) { return p.size() > 2 && p[0] == '['; }
bool layout_mark_name(std::string_view w) { return w == "line" || w == "indent" || w == "space" || w == "glued"; }
// Part-list `(xs, ...)`: elements by comma, space by array {…} (§4.4).
bool form_list(std::string_view p) { return p.ends_with("..."); }
// Part in brackets: (x), close (x), list (xs, ...); y (a, b) - in place for part: "(+) - open
// and place, ", +" - comma and place, ",)" - comma, place, close.
bool form_paren(std::string_view p) { return !p.empty() && p[0] == '('; }
bool form_open(std::string_view p) { return p == "(+" || p == "()+"; }
bool form_next(std::string_view p) { return p == ",+" || p == ",)"; }
bool form_place(std::string_view p) { return form_paren(p) || form_next(p) || p == "{" || p == "{="; }
// k-th part of the record without layout marks (empty - no such).
std::string_view form_piece(ast_db const & db, n_fn const & f, std::size_t k) {
  for(symbol s: f.form) {
    auto p = db.strings.text(s);
    if(form_mark(p)) continue;
    if(k == 0) return p;
    --k;
  }
  return {};
}

std::optional<record_info> record_of(ast_db const & db, node_id fn) {
  if(!db.is<n_fn>(fn)) return std::nullopt;
  auto const & f = db.as<n_fn>(fn);
  if(f.form.empty() || !f.body) return std::nullopt;
  record_info r;
  node_list items;
  if(db.is<n_construct>(f.body) && db.is<n_ident>(db.as<n_construct>(f.body).type)) {
    std::string_view kind = db.strings.text(db.as<n_ident>(db.as<n_construct>(f.body).type).name);
    r.kind = node_kind_index(kind);
    int n = r.kind >= 0 ? node_code_fields(r.kind) : -1;
    if(r.kind < 0) { // Syntax twice (x) => ast.twice {x}
      auto pk = program_kind(db, kind);
      if(!pk) return std::nullopt;
      r.kind = int(program_kind_index);
      r.program = kind;
      n = int(pk->size());
    }
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
  // Numbers and text - machine representations (§5.8, O15): tag names - prelude
  else if constexpr(std::is_same_v<F, symbol> || std::is_same_v<F, std::string>) return "__bytes";
  else if constexpr(std::is_same_v<F, bool>) return "__bool";
  else if constexpr(std::is_floating_point_v<F>) return "__float64";
  else return "__int64";
}

// node AST - struct programs:
// 
// Ads are generated from node C++ structures (one source) and parsed together with
// prelude. The node base (n_block: n_context) is the program's struct base: is_a, a
// sample base.
std::string ast_structs_text() {
  std::string out = "// узлы АСТ (порождено из структур узлов VM)\n";
  out += cat("struct ", lang_context(), " { }\n");
  template for(constexpr auto t: std::define_static_array(std::meta::template_arguments_of(std::meta::dealias(^^node_kind)))) {
    if constexpr(!std::is_same_v<typename[:t:], n_node>) { // Programming program announces program
      out += cat("struct ", lang_kind_name(std::meta::identifier_of(t)));
      template for(constexpr auto b: std::define_static_array(std::meta::bases_of(t, std::meta::access_context::unchecked())))
        out += cat(" : ", lang_kind_name(std::meta::identifier_of(std::meta::type_of(b))));
      out += " {";
      template for(constexpr auto m: fields_of<typename[:t:]>())
        out += cat(" ", node_field_type<typename[:std::meta::type_of(m):]>(), " ", lang_field(std::meta::identifier_of(m)), ";");
      out += " }\n";
    }
  }
  return out;
}

// The name of node is ast.if, ast.call.
std::string_view lang_kind_of(node const & n) {
  if(auto const * u = std::get_if<n_node>(&n.kind)) return u->kind; // programme
  return lang_kind(n.kind.index());
}

// The name of the type node is the C++ structure (editor protocol, debugging output).
std::string_view kind_name(node const & n) {
  return std::visit([](auto const & k) -> std::string_view {
    return std::meta::identifier_of(std::meta::remove_cvref(^^decltype(k)));
  },
      n.kind);
}

} // namespace tz
