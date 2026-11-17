// The AST database is the source of program truth (§3.1, D7).
//
// node are data aggregates owned by ast_db. The links between node are indexes
// (node_id), not pointers or names in the text: Each change increases the revision
// - on this is reactive elaboration (§10.1).
//
#pragma once

#include "base.hpp"
#include "lexer.hpp"
#include "os.hpp"
#include "pool.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <deque>
#include <optional>
#include <unordered_map>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <variant>

namespace vish {

struct node_tag;
using node_id = id<node_tag>;
using node_list = std::vector<node_id>; // time lists (parsing, bypasses); node has list<node_id> in the pool
using node_span = std::span<node_id const>; // Read the list: both vector and list node

// Parameter mode (§6.2 steak)
// clang-format off
enum class param_mode : uint8_t { let, mut, own, out, expr, ct, implicit };
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
  list<node_id> items;
  list<uint32_t> blank;
  // internal elements (node numbers from items):
// - in base and execution, but not in
  // presentation of the editor module; access - transition. The layout is like blank; numbers,
  // not list< node_id> are not children (they are not seen twice by roundabouts in the fields).
  list<uint32_t> inner;
  // [[extension(проверка)]] module k; - control of extensions entity module (§9.1):
  // 
  // 
  list<node_id> attrs;
};
struct n_fn {
  symbol name;
  list<node_id> params;
  node_id result_type;
  node_id body;
  bool outline = false;
  bool expr_body = false;
  list<node_id> attrs; // [[…]] - values calculated during compilation (§7.4)
  bool scoped = false; // Using Operator - Redefinition in Context (§8.2)
  // syntax: запись формы — слова и символы, "(" — параметр в скобках, "{" —
  // parameter-block (in order of parameters); the first word is the name entity (§4.4 )
  list<symbol> form;
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
  list<node_id> attrs; // [[state]] x = e is the state of the program (§12.2)
};
struct n_block : n_context {
  list<node_id> items;
  bool has_value = false;
  list<uint32_t> blank; // n_module
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
  node_text value;
};
struct n_call {
  node_id callee;
  list<node_id> args;
  list<symbol> words; // The Word of Clause in Every Argument (Empty - Everything in Your Head)
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
  list<node_id> args;
  bool postfix = false; // one argument: a++ (otherwise prefix -a)
}; // operator = operator entity call (§4.4)
// A chain of infix operators, which has several trees parsing: the tree is
// selected by brute force at instantiation - by type (§4.4).
struct n_chain {
  list<node_id> atoms;
  list<symbol> ops; // ops[k] - between atoms[k] and atoms[k+1]
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
  list<node_id> items;
  list<node_id> attrs;
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
  list<node_id> items;
};
struct n_splice {
  symbol name;
};
struct n_ct {
  node_id body;
}; // ct e / ct { }
struct n_lambda {
  list<node_id> params;
  node_id body;
  bool block = false; // construction block: for(auto x : xs) {…} - semantics inline, not closing
};
struct n_construct {
  node_id type;
  list<node_id> items;
  // {x: 1.0, y: 2.0} is an anonymous structural literal: type no, names[k] is the name of items[k].
  // 
//  
  list<symbol> names;
}; // array{1, 2, 3}
struct n_hole {
  node_text text;
  node_text reason;
}; // undisassembled fragment (§4.1)

// Unpacking (P12):
// 
// The sample is written as a constructor (author's decision 2026-09-30): Name {…} /
// array {…}; elements - n_param (name, T name, , ... residue), nested n_pat or
// expression-value (in the text - in parentheses).
//
struct n_pat {
  node_id type;
  list<node_id> items;
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
  list<symbol> names;
};
// native "cpp" { … } is a function body in C++ (§3.3):
// 
// Text as written; `#x` is the parameter x of the surrounding function with the value
// of VM. It is part of VM (vish native).
struct n_native {
  symbol lang;
  node_text code;
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
  node_text open; // bracket: {[[]
  list<node_id> items; // atoms (n_ident, n_int, n_float, n_str, n_sym, n_hole) and subblocks of n_raw
  node_text text; // Block text with brackets - as written
  uint32_t rule = 0; // announcement of the structure, the body of which is disassembled by the block (+1); for sub-blocks - 0
  bool glued = false; // close to the previous one: f(x) vs. f(x)
};
// The operator symbol of the raw unit is one: “the operator is always
// single-character” gluing (==, ->) is visible by glued.
struct n_sym {
  node_text text;
  bool glued = false;
};

// Node of the type declared by the program: struct ast.twice { code x; }
// 
// The meaning of the form is the disclosure in the known node ([[transform]]
// f(ast.twice t), author's decision 2026-10-17: ). *My:* field is node (code) in the
// order of the announcement; in node_kind - the last (view numbers in journal .vishdb
// are not shifted).
struct n_node {
  node_text kind; // ast.twice
  list<node_id> items;
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
// Sh5.0: node - only numbers and descriptors of the pool, without pointers and ownership (memcpy, display in file)
static_assert(std::is_trivially_copyable_v<node>);

// journal of base operations (§3.1):
// 
// The base state is the reproduction of journal;
// journal is the history of change.
struct j_intern {
  uint32_t id;
  std::string text;
};
// home - a pool in which lists and text are kind (S5.0): the recording is self-sufficient only with it;
// playback to another database transfers them to its pool; when writing to a disk is not written, when reading
// - the pool in which they read.
struct j_add {
  uint32_t id;
  node_kind kind;
  span where;
  node_pool const * home = nullptr;
};
struct j_replace {
  uint32_t id;
  node_kind kind;
  node_pool const * home = nullptr;
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
  node_pool const * home = nullptr;
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
// Node id - derivative (derive): playback journal puts it in the pool, and elaboration after loading
// .vishdb takes the former derivatives, rather than producing new ones (base growth, test 217).
struct j_derived {
  uint32_t id;
};
using journal_op = std::variant<j_intern, j_add, j_replace, j_link, j_import, j_mutate, j_rollback, j_place, j_note, j_gap, j_derived>;

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

// Base area in storage (S5.1, plan S5): the node array and the list/text pool are in the storage file
// (os::storage_area: name - key, not path; flock - one execution; display 64 GB addresses per file).
// 
// ; Layout (*my*): header (db header) | empty to nodes at | array of nodes (node cap × × ). sizeof(node)| empty to
// pool at | pool(s). Pieces are separated by file holes (sparse), only the pool grows. Everything inside is indexes
// and offsets, not addresses: the area opens anywhere. Fixation (commit) - data on the disk, then the header with the
// sign “closed”; the first change after it resets the feature on the disk. Opening checks magic, format version, node
// layout print (another build with other node fields is failure), file size, feature and length: understandable
// reason, not a fall.
// -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// They enter the area when they are fixed (in pool lists: records without pointers, types of nodes share pieces with
// live ones) and are reconstructed when opened. Live in the area - nodes, pool, intern and texts of imports. Why a
// snapshot, not a live translation: the journal is erased and rewritten (squash log, rollback of elaboration),
// mutations and derivative nodes rule by dozens of elaboration sites; translation of each is a separate step (S5.3),
// and fixation already writes pages to the disk.
//
//
struct pnote {
  list<node_text> lead;
  node_text tail;
  list<node_text> end;
};
struct note_rec {
  uint32_t id;
  uint32_t pad;
  pnote note;
};
struct s_add {
  uint32_t id;
  node_kind kind;
  span where;
};
struct s_replace {
  uint32_t id;
  node_kind kind;
};
struct s_link {
  uint32_t root;
};
struct s_import {
  node_text text; // in a row pool, the same piece as the sources
  node_text label;
};
struct s_mutate {
  uint32_t id;
  node_kind kind;
  uint32_t source;
};
struct s_rollback {
  uint32_t source;
};
struct s_place {
  uint32_t id;
  span where;
};
struct s_note {
  uint32_t id;
  pnote note;
};
struct s_gap {
  uint32_t nodes;
  uint32_t imports;
};
struct s_derived {
  uint32_t id;
};
using stored_op = std::variant<s_add, s_replace, s_link, s_import, s_mutate, s_rollback, s_place, s_note, s_gap, s_derived>;
struct stored_entry {
  uint64_t revision;
  uint32_t strings; // How many lines were interned at the time of recording
  stored_op op;
};
struct born_snap {
  uint32_t id;
  uint32_t pad;
  uint64_t key;
  node_kind kind;
};
struct reuse_snap {
  uint64_t key;
  uint64_t cursor;
  list<uint32_t> ids;
};

// The database roots (S5.2): the descriptors of the pool lists and the counters that are not derived from nodes. They live in the area (right
// after the header, edited in place) or in the process heap (ordinary memory).
struct parent_undo {
  node_id child, old, next;
};
struct mutation_rec {
  uint32_t target;
  uint32_t source;
  uint64_t at; // Mutation revision: the right node does not roll back after it
  node_kind old;
  list<parent_undo> parents;
};
struct db_roots {
  interner_roots strings;
  list<node_text> sources; // Import texts (pool lines) - live
  list<node_text> labels;
  // a snapshot of the operating state at fixation; valid = 1 - the shot is coordinated with node and the intern
  uint64_t valid;
  uint64_t revision, births, edits;
  uint32_t prelude;
  uint32_t program_root; // Program root +1 (0 is not written): saving the product puts it at save_area
  list<stored_entry> oplog;
  list<note_rec> notes; // live (S5.3): comments node; erased - id = ~0
  list<uint32_t> note_ix; // node → place in notes + 1 (0 - no comments)
  list<mutation_rec> mutations;
  list<born_snap> born; // live (S5.3): derivatives of node in order of birth
  list<uint32_t> born_ix; // node → place in born + 1 (0 - non-derivative)
  list<reuse_snap> reuse; // live: free derivatives by key type and place
  list<uint32_t> reuse_tab; // hash table of keys (open addressing, degree of two): place in reuse + 1
  list<uint64_t> node_rev;
  list<uint64_t> edited_at;
};

struct db_header {
  char magic[8];
  uint32_t version;
  uint32_t header_bytes;
  uint64_t layout; // node_kind layout print (names and sizes of fields)
  uint32_t node_bytes;
  uint32_t node_cap;
  uint64_t nodes_at; // byte
  uint64_t strings_at; // Pool of lines (intern texts: grows, does not shrink or move)
  uint32_t strings_cap; // word
  uint32_t strings_used;
  uint64_t roots_at; // db_roots
  uint64_t pool_at;
  uint32_t pool_cap; // word
  uint32_t node_count; // fixation
  uint32_t pool_used;
  uint32_t clean; // 1 - fixed and unchanged
  uint64_t generation;
  uint64_t checksum; // higher up
};

struct db_area {
  static constexpr uint32_t format_version = 4; // 4 - the parents of mutations; 3 - S5.3: born and reusable live in the roots; 2 - S5.2: the database roots and the string pool (1 - S5.1: only nodes and the pool)
  static constexpr uint32_t default_node_cap = 1u << 21;
  static constexpr uint32_t default_strings_cap = 1u << 24; // words (64 MiB addresses)
  static constexpr uint64_t header_gap = uint64_t(1) << 16; // subtitle
  static constexpr uint64_t step = uint64_t(1) << 20; // file grows in pieces

  os::storage_area file;
  db_header * h = nullptr;
  bool fresh = false;

  static uint64_t layout_hash() {
    uint64_t x = 1469598103934665603ull;
    auto mix = [&](std::string_view t) {
      for(char c: t) x = (x ^ uint8_t(c)) * 1099511628211ull;
      x = (x ^ 0xff) * 1099511628211ull;
    };
    mix(cat(sizeof(node), ":", sizeof(node_kind), ":", std::variant_size_v<node_kind>));
    template for(constexpr auto t: std::define_static_array(std::meta::template_arguments_of(std::meta::dealias(^^node_kind)))) {
      mix(std::meta::identifier_of(t));
      template for(constexpr auto m: fields_of<typename[:t:]>()) {
        mix(std::meta::identifier_of(m));
        mix(cat(sizeof(typename[:std::meta::type_of(m):])));
      }
    }
    return x;
  }
  static uint64_t sum(db_header const & x) {
    uint64_t v = 1469598103934665603ull;
    auto const * p = reinterpret_cast<unsigned char const *>(&x);
    for(size_t i = 0; i < offsetof(db_header, checksum); ++i) v = (v ^ p[i]) * 1099511628211ull;
    return v;
  }
  // Reserve near the area for live editing: node and the row pool do not move and do not grow beyond the capacity - do not open less stock live
  static constexpr uint32_t node_headroom = 1u << 16, strings_headroom = 1u << 18;
  bool has_headroom() const { return h && uint64_t(h->node_count) + node_headroom <= h->node_cap && uint64_t(h->strings_used) + strings_headroom <= h->strings_cap; }
  uint64_t nodes_off() const { return h->nodes_at; }
  node * nodes() const { return reinterpret_cast<node *>(file.at + h->nodes_at); }
  uint32_t * pool() const { return reinterpret_cast<uint32_t *>(file.at + h->pool_at); }
  uint32_t * strings() const { return reinterpret_cast<uint32_t *>(file.at + h->strings_at); }
  db_roots * roots() const { return reinterpret_cast<db_roots *>(file.at + h->roots_at); }

  // * open (fresh - new, empty); otherwise, the reason, the area is closed node_cap, strings_cap - capacity of the new area (0 -
  // by default): the file is sparse, but the apparent size - their sum
  std::string open(std::string const & dir, std::string_view name, bool create = true, bool shared = true, bool lock = true, uint32_t node_cap = 0, uint32_t strings_cap = 0) {
    if(std::string why = file.open(dir, name, shared, lock); !why.empty()) return why;
    auto fail = [&](std::string why) {
      file.close();
      h = nullptr;
      return cat("область базы ", name, ": ", why);
    };
    if(file.size == 0) {
      if(!create) return fail("нет такой области");
      if(!node_cap) node_cap = default_node_cap;
      if(!strings_cap) strings_cap = default_strings_cap;
      uint64_t nodes_at = header_gap, strings_at = (nodes_at + uint64_t(node_cap) * sizeof(node) + step - 1) / step * step;
      uint64_t pool_at = strings_at + uint64_t(strings_cap) * 4;
      if(std::string why = file.grow(pool_at + step); !why.empty()) return fail(why);
      h = reinterpret_cast<db_header *>(file.at);
      std::memset(h, 0, sizeof *h);
      std::memcpy(h->magic, "VISHAREA", 8);
      h->version = format_version;
      h->header_bytes = sizeof(db_header);
      h->layout = layout_hash();
      h->node_bytes = sizeof(node);
      h->node_cap = node_cap;
      h->nodes_at = nodes_at;
      h->strings_at = strings_at;
      h->strings_cap = strings_cap;
      h->strings_used = 1;
      h->roots_at = (sizeof(db_header) + 63) / 64 * 64;
      h->pool_at = pool_at;
      h->pool_cap = uint32_t(node_pool::words_max - 1);
      h->pool_used = 1;
      h->clean = 1;
      h->checksum = sum(*h);
      fresh = true;
      return "";
    }
    if(file.size < sizeof(db_header)) return fail(cat("файл короче заголовка (", file.size, " байт): область обрезана"));
    h = reinterpret_cast<db_header *>(file.at);
    if(std::memcmp(h->magic, "VISHAREA", 8) != 0) return fail("не область базы vish");
    if(h->version != format_version) return fail(cat("версия формата ", h->version, ", эта сборка читает ", format_version));
    if(h->checksum != sum(*h)) return fail("заголовок повреждён (контрольная сумма)");
    if(h->header_bytes != sizeof(db_header) || h->node_bytes != sizeof(node) || h->layout != layout_hash())
      return fail("создана сборкой с другим устройством узлов (раскладка не совпадает)");
    if(h->nodes_at < h->roots_at + sizeof(db_roots) || h->roots_at < sizeof(db_header) || h->roots_at + sizeof(db_roots) > h->nodes_at || h->nodes_at + uint64_t(h->node_cap) * sizeof(node) > h->strings_at || h->strings_at + uint64_t(h->strings_cap) * 4 > h->pool_at || h->node_count > h->node_cap || h->strings_used < 1 || h->strings_used > h->strings_cap || h->pool_used < 1 || h->pool_used > h->pool_cap)
      return fail("размеры в заголовке несовместимы");
    if(!h->clean) return fail("не была зафиксирована (сорвана посреди работы): данные могут быть несогласованы");
    if(file.size < h->pool_at + uint64_t(h->pool_used) * 4) return fail(cat("файл короче данных (", file.size, " байт, нужно ", h->pool_at + uint64_t(h->pool_used) * 4, "): область обрезана"));
    fresh = false;
    return "";
  }
  // The file covers the pool before the word
  void grow_pool(uint64_t words) {
    uint64_t need = h->pool_at + words * 4;
    if(need <= file.size) return;
    if(std::string why = file.grow((need - file.size + step - 1) / step * step); !why.empty()) throw diagnostic(cat("область базы: ", why), {});
  }
  uint64_t pool_ready() const { return (file.size - h->pool_at) / 4; }
  uint64_t nodes_ready() const { return h->node_cap; } // File holes up to pool_at - displayed in their entirety
  // First change after fixation: a sign to the disk before the data floats
  void dirtied() {
    if(!h->clean) return;
    h->clean = 0;
    h->checksum = sum(*h);
    ::msync(file.at, 4096, MS_SYNC);
  }
  // Data on the disk, then a header with a feature (two steps: the header “closed” does not outpace the data)
  std::string commit(uint32_t node_count, uint32_t pool_used, uint32_t strings_used) {
    if(std::string why = file.commit(); !why.empty()) return why;
    h->node_count = node_count;
    h->pool_used = pool_used;
    h->strings_used = strings_used;
    ++h->generation;
    h->clean = 1;
    h->checksum = sum(*h);
    if(::msync(file.at, 4096, MS_SYNC) != 0) return std::strerror(errno);
    int rc;
    do rc = ::fsync(file.fd); while(rc < 0 && errno == EINTR);
    return rc < 0 ? std::string(std::strerror(errno)) : std::string();
  }
};

// Area without a name to check: file in the temporary directory, immediately unlink (lives while open). VISH_DB_SCRATCH =
// directory is turned on - each process base lives in a region, as in a repository (the whole set of tests - verification).
inline std::unique_ptr<db_area> scratch_area() {
  static std::atomic<uint32_t> serial{0};
  char const * dir = std::getenv("VISH_DB_SCRATCH");
  if(!dir || !*dir) return nullptr;
  std::string where = cat(dir, "/vish-area-", ::getpid());
  auto a = std::make_unique<db_area>();
  std::string name = cat("a", serial.fetch_add(1));
  // VISH_DB_SCRATCH_SHARED = 1 - General display, as in the storage (fork-child replays it private); otherwise the
  // private immediately
  if(!a->open(where, name, true, bool(std::getenv("VISH_DB_SCRATCH_SHARED"))).empty()) return nullptr;
  ::unlink(cat(where, "/", name).c_str());
  return a;
}

struct ast_db {
  // The listing and text pool node (S5.0): each ast_db has its own base, the current thread_local. Any reference
  // to node exposes it; the code that holds a list of one base and touches another calls use_pool.
  std::unique_ptr<db_area> own_area; // domain created by the base itself (VISH_DB_SCRATCH check)
  db_area * area = nullptr; // storage area (S5.1); empty - normal memory
  ~ast_db() {
    if(pool.used > 20000 && std::getenv("VISH_POOL_STATS")) { // Pool growth measurement: before and after compaction
      auto st = pool_stats();
      std::fprintf(stderr, "пул: занято %u слов, живых %u, к возврату по оценке %llu, узлов %u\n", st.used, st.live, (unsigned long long)reclaimable(), nodes.size());
      uint32_t freed = compact_pool();
      std::fprintf(stderr, "пул после компактации: занято %u слов (вернула %u)\n", pool.used, freed);
    }
  }
  node_pool pool;
  // Pool of lines of the interner: text symbols - a separate pool (grows, does not shrink, does not move)
  node_pool spool;
  std::unique_ptr<db_roots> own_roots; // roots in the process heap (ordinary memory); in the area, in its header
  // Roots: Lists inside are read through the THIS database pool - access only through R() (exhibits the current pool); raw pointer - for the constructor
  db_roots * unsafe_roots;
  db_roots & R() const {
    use_pool();
    return *unsafe_roots;
  }
  interner strings;
  explicit ast_db(db_area * a = nullptr)
      : own_area(a ? nullptr : scratch_area()), area(a ? a : own_area.get()), pool(make_pool(area)), spool(make_spool(area)), own_roots(area ? nullptr : std::make_unique<db_roots>()),
        unsafe_roots(area ? area->roots() : own_roots.get()), strings(&spool, &pool, &unsafe_roots->strings), nodes(make_nodes(area)) {
    nodes.owner = &pool;
    use_pool();
    if(area && !area->fresh && R().valid) restore_state();
  }
  static node_pool make_pool(db_area * a) {
    if(!a) return node_pool();
    return node_pool(a->pool(), node_pool::words_max, a->h->pool_used, a->pool_ready(), [a](uint64_t w) { a->grow_pool(w); }, [a] { a->dirtied(); });
  }
  static node_pool make_spool(db_area * a) {
    if(!a) return node_pool(uint64_t(db_area::default_strings_cap));
    return node_pool(a->strings(), a->h->strings_cap, a->h->strings_used, a->h->strings_cap, [](uint64_t) {}, [a] { a->dirtied(); });
  }
  static node_array<node, node_tag> make_nodes(db_area * a) {
    constexpr uint64_t anonymous_cap = uint64_t(1) << 24;
    if(!a) return node_array<node, node_tag>(anonymous_cap);
    return node_array<node, node_tag>(a->nodes(), a->h->node_cap, a->h->node_count, a->nodes_ready(), [a](uint64_t) {}, [a] { a->dirtied(); });
  }
  // Pool: achievable pieces, statistics, compaction (S5.1) ------------------------------------ bypass all pool descriptors that hold the base:
  // f (displacement, meta, words per element; 0 - text). Holders: nodes, species in journal entries (j add/j replace/j mutate), mutated
  // species, born/reborn, retired. No one else, so compaction is only allowed where there are no rounds (between server messages and session
  // commands).
  void for_each_handle(auto && f) {
    auto kind = [&](node_kind & k) {
      std::visit([&](auto & x) {
        template for(constexpr auto m: fields_of<std::remove_cvref_t<decltype(x)>>()) {
          using F = [:std::meta::type_of(m):];
          if constexpr(is_pool_list<F>::value) f(x.[:m:].off, x.[:m:].meta, F::ew);
          else if constexpr(std::is_same_v<F, node_text>) f(x.[:m:].off, x.[:m:].meta, 0u);
        }
      },
          k);
    };
    // The container communicates before its elements - only through own(): in the second pass (after the shift), its descriptor
    // must already point to a new place, otherwise the elements are read from unmoved (S5.3 trap, now structurally)
    auto own = [&](auto & l, auto && elements) {
      f(l.off, l.meta, std::remove_cvref_t<decltype(l)>::ew);
      elements();
    };
    auto note = [&](pnote & p) {
      own(p.lead, [&] { for(node_text & t: p.lead) f(t.off, t.meta, 0u); });
      f(p.tail.off, p.tail.meta, 0u);
      own(p.end, [&] { for(node_text & t: p.end) f(t.off, t.meta, 0u); });
    };
    for(uint32_t i = 0, n = nodes.size(); i < n; ++i) kind(nodes[node_id(i)].kind);
    db_roots & r = R();
    own(r.oplog, [&] {
      for(stored_entry & e: r.oplog) {
        if(auto * x = std::get_if<s_add>(&e.op)) kind(x->kind);
        else if(auto * y = std::get_if<s_replace>(&e.op)) kind(y->kind);
        else if(auto * z = std::get_if<s_mutate>(&e.op)) kind(z->kind);
        else if(auto * w = std::get_if<s_note>(&e.op)) note(w->note);
      }
    });
    own(r.mutations, [&] { for(mutation_rec & m: r.mutations) { kind(m.old); own(m.parents, [] {}); } });
    own(r.node_rev, [] {});
    own(r.edited_at, [] {});
    own(r.born, [&] { for(born_snap & b: r.born) kind(b.kind); });
    own(r.born_ix, [] {});
    own(r.reuse, [&] { for(reuse_snap & u: r.reuse) own(u.ids, [] {}); });
    own(r.reuse_tab, [] {});
    own(r.notes, [&] {
      for(note_rec & n: r.notes)
        if(n.id != ~uint32_t{0}) note(n.note);
    });
    own(r.note_ix, [] {});
    for(auto & r: reborn) kind(r.was);
    for(auto & k: retired) kind(k);
    // roots live: arrays of interns and sources - in the main pool (the texts themselves - in the pool of lines, he does not move)
    own(r.strings.strs, [] {});
    own(r.strings.table, [] {});
    own(r.sources, [] {});
    own(r.labels, [] {});
  }
  // A state shot (journal, comments...) divides the pieces with node and the compaction shift is not updated:
  // before the shift it is reset (the pieces go away as abandoned), the next fixation collects it again.
  void drop_snapshot() {
    db_roots & r = R();
    auto zero = [](auto & l) { l.off = 0; l.meta = 0; };
    r.valid = 0;
  }
  struct pool_stat {
    uint32_t used, live; // The words are occupied / need to live (with a container in length)
  };
  // words per live piece: list - ew · 2^k in length, text - 2^k in length in words
  static uint32_t handle_need(uint32_t meta, uint32_t ew) {
    uint32_t len = meta & ((uint32_t(1) << node_pool::len_bits) - 1);
    return ew ? len : (len + 3) / 4;
  }
  static uint64_t handle_words(uint32_t need, uint32_t ew) { return need ? (uint64_t(ew ? ew : 1) << pool_class_of(need)) : 0; }
  pool_stat pool_stats() {
    use_pool();
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> need; // Displacement (requires elements, words per element)
    for_each_handle([&](uint32_t & off, uint32_t & meta, uint32_t ew) {
      if(!off) return;
      auto & n = need[off];
      n.first = std::max(n.first, handle_need(meta, ew));
      n.second = ew;
    });
    uint64_t live = 1;
    for(auto const & [off, n]: need) live += handle_words(n.first, n.second);
    return {pool.used, uint32_t(std::min<uint64_t>(live, pool.used))};
  }
  // Move the live pieces to the beginning of the pool, the container - along the length; thrown pieces and free lists
  // disappear. Returns freed words. It is impossible when the second lane is working and where the lists (bypasses) are kept.
  uint32_t compact_pool() {
    use_pool();
    if(lanes_on.load(std::memory_order_acquire)) return 0;
    sweep_retired();
    lane_guard g(pool.lock);
    struct run {
      uint32_t need = 0; // elements (text - words)
      uint32_t ew = 0;
      uint32_t to = 0;
    };
    std::map<uint32_t, run> runs;
    for_each_handle([&](uint32_t & off, uint32_t & meta, uint32_t ew) {
      if(!off) return;
      auto & r = runs[off];
      r.need = std::max(r.need, handle_need(meta, ew));
      r.ew = ew;
    });
    uint32_t before = pool.used, cursor = 1;
    pool.touch(); // The area is marked "changed" before the first root editing
    drop_snapshot();
    for(auto & [off, r]: runs) { // The new place is not higher than the old and does not touch the not yet shifted
      if(!r.need) continue;
      cursor = (cursor + 1) & ~uint32_t{1}; // even word: alignment 8 (see node_pool::take)
      r.to = cursor;
      if(r.to != off) std::memmove(pool.base + r.to, pool.base + off, size_t(r.ew ? r.need * r.ew : r.need) * 4);
      cursor += uint32_t(handle_words(r.need, r.ew));
    }
    for_each_handle([&](uint32_t & off, uint32_t & meta, uint32_t ew) {
      if(!off) return;
      uint32_t len = meta & ((uint32_t(1) << node_pool::len_bits) - 1);
      run const & r = runs.at(off);
      if(!r.need) {
        off = 0;
        meta = 0;
        return;
      }
      off = r.to;
      meta = len | (pool_class_of(r.need) << node_pool::len_bits);
    });
    pool.used = cursor;
    pool.garbage = 0;
    pool.free_runs.clear();
    return before - cursor;
  }
  // Words that the compaction will return by evaluating: thrown by rewriting + free chunks by class
  uint64_t reclaimable() const {
    return pool.garbage + pool.free_words();
  }
  // Automatic: not less than 64 K words and not less than a quarter of the pool
  void maybe_compact() {
    if(reclaimable() >= (uint64_t(1) << 16) && reclaimable() * 4 >= pool.used && !std::getenv("VISH_NO_COMPACT")) compact_pool(); // VISH_NO_COMPACT - Measurement without compaction
  }
  // --- picture of working condition and recovery (S5.2) ---
  static pnote pool_note(node_note const & n) {
    pnote p;
    for(auto const & l: n.lead) p.lead.push_back(node_text(l));
    p.tail = node_text(n.tail);
    for(auto const & l: n.end) p.end.push_back(node_text(l));
    return p;
  }
  static node_note plain_note(pnote const & p) {
    node_note n;
    for(node_text const & l: p.lead) n.lead.push_back(l.str());
    n.tail = p.tail.str();
    for(node_text const & l: p.end) n.end.push_back(l.str());
    return n;
  }
  static void release_note(pnote & p) {
    for(node_text & t: p.lead) t.release();
    p.lead.release();
    p.tail.release();
    for(node_text & t: p.end) t.release();
    p.end.release();
  }
  // text from a pool of lines (its descriptor is not from the main pool, the current base is not decreed to it)
  std::string spool_str(node_text const & t) const { return std::string(reinterpret_cast<char const *>(spool.base + t.off), t.size()); }
  node_text source_text(uint32_t i) const { return reinterpret_cast<node_text const *>(pool.base + R().sources.off)[i]; }
  node_text source_label_text(uint32_t i) const { return reinterpret_cast<node_text const *>(pool.base + R().labels.off)[i]; }
  // Working state (journal, comments, mutations, derivatives of node, tables on node) - in the roots of the field
  void snapshot_save() {
    use_pool();
    pool_scope in_pool(&pool);
    lane_guard g(pool.lock);
    db_roots & r = R();
    r.revision = revision;
    r.births = births;
    r.edits = edits;
    r.prelude = prelude.value;
    r.valid = 1;
  }
  // Base over the open area: working structures - from the snapshot of roots (without journal); caches - again
  void restore_state() {
    use_pool();
    db_roots & r = R();
    revision = r.revision;
    births = r.births;
    edits = r.edits;
    prelude = node_id(r.prelude);
    note_cache.clear();
    uint32_t n = nodes.size();
    node_rev.resize(n, revision);
    birth.assign(n, 0);
    rebuild_refs();
    for(uint32_t k = 0; k < n; ++k) note_fn(node_id(k));
  }
  std::string commit_area() {
    if(!area) return "база не в области хранилища";
    snapshot_save();
    return area->commit(nodes.size(), pool.used, spool.used);
  }
  void use_pool() const {
    if(current_pool != &pool) current_pool = const_cast<node_pool *>(&pool);
  }

  // Mine (RA-1): sorted numbers by symbol, without allocating memory per node.
  // Detached nodes stay in the database and in the index; belonging is computed from parent.
  // A node's keys depend ONLY on its kind and fields (nothing is interned or looked up in const methods):
  // otherwise a later interning of a name would leave the live index shorter than the recount (I1).
  //   refs_by_name[symbol]  - n_ident.name, n_op.op (including a postfix `.x` - under the symbol `.x`),
  //                           n_chain.ops, n_use.key, n_splice.name, n_extend.name/of/base;
  //   refs_by_target[node]  - n_ref.target, n_extend.target (a reference by number, not by name: renaming the target does not go stale).
  // callee, parameter/variable types - child n_ident: the use node itself is indexed.
  std::vector<std::vector<uint32_t>> refs_by_name, refs_by_target;
  void ref_keys(node_kind const & k, auto emit) const { // emit(table: 0 name / 1 target, key)
    auto name = [&](symbol s) { if(s) emit(0u, s.value); };
    auto target = [&](uint32_t t) { if(t) emit(1u, t); };
    if(auto p = std::get_if<n_ident>(&k)) name(p->name);
    else if(auto p = std::get_if<n_op>(&k)) {
#ifdef VISH_RA1_MUTANT_NO_MEMBER // an oracle check (rdepscheck must fail): `.field` is not indexed
      if(!(p->postfix && strings.text(p->op).starts_with('.')))
#endif
        name(p->op);
    }
    else if(auto p = std::get_if<n_chain>(&k)) { for(auto s: p->ops) name(s); }
    else if(auto p = std::get_if<n_use>(&k)) { name(p->key); for(auto s: p->names) name(s); }
    else if(auto p = std::get_if<n_splice>(&k)) name(p->name);
    else if(auto p = std::get_if<n_extend>(&k)) { name(p->name); name(p->of); name(p->base); target(p->target); }
#ifndef VISH_RA1_MUTANT_NO_REF // an oracle check: n_ref is not indexed
    else if(auto p = std::get_if<n_ref>(&k)) target(p->target);
#endif
  }
  void index_refs(node_id n, bool insert) {
    ref_keys(nodes[n].kind, [&](uint32_t table, uint32_t key) {
      auto & by = table ? refs_by_target : refs_by_name;
      if(by.size() <= key) {
        if(!insert) return;
        by.resize(key + 1);
      }
      auto & xs = by[key];
      auto i = std::ranges::lower_bound(xs, n.value);
      if(insert) { if(i == xs.end() || *i != n.value) xs.insert(i, n.value); }
      else if(i != xs.end() && *i == n.value) xs.erase(i);
    });
  }
  void rebuild_refs() {
    use_pool();
    refs_by_name.clear();
    refs_by_target.clear();
    for(uint32_t i = 0; i < size(); ++i) index_refs(node_id(i), true);
  }
  node_id enclosing_fn(node_id n) const {
    for(; n; n = nodes[n].parent) if(is<n_fn>(n)) return n;
    return {};
  }
  symbol dependency_name(node_id n) const {
    if(is<n_fn>(n)) return as<n_fn>(n).name;
    if(is<n_module>(n)) return as<n_module>(n).name;
    if(is<n_extend>(n)) return as<n_extend>(n).name;
    if(is<n_ident>(n)) return as<n_ident>(n).name;
    if(is<n_var>(n)) return as<n_var>(n).name;
    return {};
  }
  // A symbol, if it is already interned (creates nothing).
  symbol known_symbol(std::string_view text) const { return strings.lookup(text); }
  static void unique_nodes(node_list & xs) {
    std::ranges::sort(xs, {}, &node_id::value);
    xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
  }
  void collect_users(symbol s, node_list & out) const {
    if(s && s.value < refs_by_name.size())
      for(auto i: refs_by_name[s.value]) out.push_back(node_id(i));
  }
  // Uses of a name: the nodes where it is named and the postfix `.name` (a field/method, indexed under the symbol `.name`).
  node_list users_of(symbol s) const {
    node_list out;
    if(!s) return out;
    collect_users(s, out);
    auto text = strings.text(s);
    if(!text.empty() && text[0] != '.') {
      std::size_t before = out.size();
      collect_users(known_symbol("." + std::string(text)), out);
      if(out.size() != before) unique_nodes(out);
    }
    return out;
  }
  // Uses of a declaration: by its name; by a reference by number (ref(f), an extension of f by reference); for an operator function
  // (infix+ / prefix- / postfix? ...) - by the operator symbol, conservatively for any role and arity (a superset).
  node_list users_of(node_id n) const {
    node_list out = users_of(dependency_name(n));
    if(n.value < refs_by_target.size())
      for(auto i: refs_by_target[n.value]) out.push_back(node_id(i));
    if(is<n_fn>(n)) {
      auto text = strings.text(as<n_fn>(n).name);
      for(std::string_view role: {"infix", "prefix", "postfix"})
        if(text.size() > role.size() && text.starts_with(role)) {
          collect_users(known_symbol(text.substr(role.size())), out);
          break;
        }
    }
    unique_nodes(out);
    return out;
  }
  // Mine (RA-1, B5): the FULL table of implicit calls - the program's functions that the VM core (interp.hpp, machine.hpp) calls
  // not by name in the text, but by the value's type, by a construct or by an attribute. A dependent of such a function is a function where the
  // construct may arise; this is not computable by name, so it is ALL the program's functions (not the prelude's), a superset.
  // One source for the index, the reference model (rdeps_oracle.hpp), the unit that checks against the core sources, and docs/spec.md (the RA-1 paragraph).
  // The explicit core forms (intercept, alloc_in, append, push, pin_storage, set_ct_state, fiber_start, pressure_handler, process, yield,
  // is_index) and operators (infix+ ..., the infix entity op_associative/declared_ops) are resolved by name/symbol from the node's text -
  // they are already in the index (n_ident / n_op), and are not in the table.
  struct implicit_hook {
    std::string_view name; // the function name (empty - a mark by attribute, see is_implicit_hook)
    std::string_view when; // the implicit call condition (for documentation and check messages)
  };
  static constexpr implicit_hook implicit_hooks[] = {
      {"destroy", "конец жизни значения struct (res_any/member_fn_of: interp.hpp ~2399/2431/2514, machine.hpp ~1442/1468)"},
      {"copy", "копирование значения struct при присваивании/передаче (member_fn_of: interp.hpp ~2598, machine.hpp ~1504)"},
      {"implicit", "неявное преобразование got → want при передаче и присваивании (implicit_fn: interp.hpp ~3071, machine.hpp ~2264)"},
      {"text", "print значения именованного типа без своего вывода (print_text: interp.hpp ~9377, machine.hpp ~692/1545)"},
      {"ref_taken", "взятие ссылки `&x = …` на элемент/путь (hook_ref_taken: interp.hpp ~5955)"},
      {"ref_used", "использование ссылки (hook_ref_used: interp.hpp ~5980)"},
      {"eval", "член вида узла программы ast.X { eval(…) } (eval_node: interp.hpp ~5117, kind_members), узлы вида порождают разбор, ct и transform"},
      {"fit_rule", "правило на языке (lang_rule, interp.hpp ~3017): отношение типов got → want при каждой передаче, присваивании, возврате"},
      {"join_rule", "правило на языке (lang_rule, interp.hpp ~2189): слияние состояний/значений на путях"},
      {"analysis_limit", "правило на языке (lang_rule, interp.hpp ~3090): пределы разбора циклов и рекурсии"},
      {"chain_resolve", "правило на языке (interp.hpp ~5354): разбор цепочки операторов a + b * c"},
      {"overload_pick", "правило на языке (interp.hpp ~8323): выбор перегрузки при каждом вызове"},
      {"", "функция с атрибутом [[transform]] (transforms: interp.hpp ~939): зовётся при элаборации на каждом подходящем узле всех модулей программы"},
  };
  bool is_implicit_hook(node_id f) const {
    if(!is<n_fn>(f)) return false;
    auto const & fn = as<n_fn>(f);
    auto text = strings.text(fn.name);
    for(auto const & h: implicit_hooks) {
      if(h.name.empty() || h.name != text) continue;
      if(text == "eval") { // "a kind member" - only inside a declaration of kind ast.X; an unknown parent is conservatively a hook
        auto p = nodes[f].parent;
        if(p && !(is<n_extend>(p) && strings.text(as<n_extend>(p).name).starts_with("ast."))) continue;
      }
      return true;
    }
    for(auto a: fn.attrs)
      if(a && is<n_ident>(a) && strings.text(as<n_ident>(a).name) == "transform") return true;
    return false;
  }
  // The nearest declaration of an extension up the tree (struct/Name { ... }), if the node is not inside a function; otherwise empty.
  node_id enclosing_extend(node_id n) const {
    for(; n; n = nodes[n].parent) {
      if(is<n_fn>(n)) return {};
      if(is<n_extend>(n)) return n;
    }
    return {};
  }
  // The module in which the node stands outside functions and extensions (a global variable, a ct block, module attributes, [[extension(g)]]).
  node_id enclosing_module_level(node_id n) const {
    for(; n; n = nodes[n].parent) {
      if(is<n_fn>(n) || is<n_extend>(n)) return {};
      if(is<n_module>(n)) return n;
    }
    return {};
  }
  node_list dependents_of_fn(node_id n) const {
    node_list out;
    std::vector<uint8_t> included(size(), 0), scopes(size(), 0);
    auto put = [&](node_id f) { if(!included[f.value]) { included[f.value] = 1; out.push_back(f); } };
    auto scope = [&](node_id top) {
      if(scopes[top.value]) return;
      scopes[top.value] = 1;
      for(auto i: fn_nodes) if(is<n_fn>(node_id(i)) && within(node_id(i), top)) put(node_id(i));
    };
    for(auto u: users_of(n)) {
      if(auto f = enclosing_fn(u)) put(f);
      // Extensions and use at module level can change any of its functions.
      if(is<n_extend>(u) || is<n_use>(u)) {
        node_id top = u;
        if(is<n_use>(u)) while(nodes[top].parent && !is<n_module>(top)) top = nodes[top].parent;
        scope(top);
      }
      // A use outside functions: inside an extension (a struct field type, a default value) - changes the extension's members;
      // outside an extension (a global variable, a ct block, [[extension(g)]]) - runs before the module's functions are parsed and may
      // change any of them (conservatively; the prelude is not edited by the user).
      if(!enclosing_fn(u) && !is<n_extend>(u) && !is<n_use>(u)) {
        node_id top = enclosing_extend(u);
        if(!top) top = enclosing_module_level(u);
        if(top && !in_prelude(top))
          scope(top);
      }
    }
#ifndef VISH_RA1_MUTANT_NO_HOOKS // an oracle check (rdepscheck must fail): the core's implicit calls are not accounted for
    if(is_implicit_hook(n))
      for(auto i: fn_nodes) if(is<n_fn>(node_id(i)) && !in_prelude(node_id(i))) put(node_id(i));
#endif
    unique_nodes(out);
    return out;
  }
  // RA-1b, mine: syntax/operator - n_fn; an ordinary function also closes
  // the record (parser::learn/shadow_records). A ct variable changes the visible state.
  bool context_rule(node_id n) const {
    return is<n_fn>(n) || is<n_use>(n) || is<n_extend>(n) || is<n_ct>(n)
      || (is<n_var>(n) && as<n_var>(n).ct);
  }
  // A rule acts after its own record in the nearest context. Nested
  // elements are also check inputs; an extension has its own member list.
  node_list rule_suffix(node_id n) const {
    node_list out;
    if(!context_rule(n)) return out;
    auto p = nodes[n].parent;
    if(!p) return out;
    node_span items;
    if(is_context(p)) items = context_items(p);
    else if(is<n_extend>(p)) items = as<n_extend>(p).items;
    bool after = false;
    for(auto item: items) {
      if(after) out.push_back(item);
      if(item == n) after = true;
    }
    return out;
  }
  // The transitive closure. A descendant (struct D : B) is a dependent of B itself: the composition of D (the base's fields first) changes
  // together with B, so the walk continues from the descendant node - through its name to the consumers of D and to its descendants.
  node_list impact_of(node_span changed_nodes) const {
    node_list todo, out;
    std::vector<uint8_t> queued(size(), 0);
    auto put = [&](node_id n) {
      if(n && n.value < size() && !queued[n.value]) { queued[n.value] = 1; todo.push_back(n); }
    };
    for(auto n: changed_nodes) put(n);
    // Identical name queries have identical answers within this call.
    // The target number, the operator form and the implicit hook are kept separately in the key.
    struct impact_query { node_list users, dependents; bool expanded = false; };
    std::map<std::array<uint32_t, 4>, impact_query> queries;
    auto query = [&](node_id n) -> impact_query & {
      std::array<uint32_t, 4> key{dependency_name(n).value,
        n.value < refs_by_target.size() && !refs_by_target[n.value].empty() ? n.value : 0,
        uint32_t(is<n_fn>(n)), uint32_t(is_implicit_hook(n))};
      auto [it, fresh] = queries.try_emplace(key);
      if(fresh) it->second = {users_of(n), dependents_of_fn(n), false};
      return it->second;
    };
    // Instances depend on the caller's inputs too: we close the graph both ways.
    // Detaching a call requires a union of the answers before and after the edit.
    std::vector<node_list> callees(size());
    for(auto f: fn_nodes) if(is<n_fn>(node_id(f)))
      for(auto caller: query(node_id(f)).dependents) callees[caller.value].push_back(node_id(f));
    // ct reflection may read an arbitrary AST: until there is a read-set, the whole database.
    bool reflection = false;
    for(uint32_t j = 1; j < size(); ++j)
      if(is<n_ct>(node_id(j)) && !in_prelude(node_id(j))) { reflection = true; break; }
#ifndef VISH_RA1_MUTANT_NO_CT_COVER
    if(reflection && !changed_nodes.empty()) for(auto f: fn_nodes) put(node_id(f));
#endif
    // Mine: per query the suffixes of one context form nested intervals.
    // The positions are built once; an already included tail is not walked again.
    std::vector<uint32_t> position(size(), ~uint32_t{0}), frontier(size());
    std::vector<uint8_t> indexed(size(), 0);
    auto suffix = [&](node_id n) {
      node_list out;
      if(!context_rule(n)) return out;
      auto p = nodes[n].parent;
      if(!p) return out;
      node_span items;
      if(is_context(p)) items = context_items(p);
      else if(is<n_extend>(p)) items = as<n_extend>(p).items;
      if(!indexed[p.value]) {
        indexed[p.value] = 1;
        frontier[p.value] = items.size();
        for(uint32_t j = 0; j < items.size(); ++j) {
          auto item = items[j];
          if(item && nodes[item].parent == p && position[item.value] == ~uint32_t{0}) position[item.value] = j;
        }
      }
      if(position[n.value] == ~uint32_t{0}) return out;
      uint32_t start = position[n.value] + 1;
      for(uint32_t j = start; j < frontier[p.value]; ++j) out.push_back(items[j]);
      frontier[p.value] = std::min(frontier[p.value], start);
      return out;
    };
    std::vector<uint8_t> seen(size(), 0), expanded(size(), 0);
    for(size_t i = 0; i < todo.size(); ++i) {
      auto n = todo[i];
      if(!n || n.value >= size() || seen[n.value]) continue;
      seen[n.value] = 1;
      if(is<n_fn>(n) || is<n_module>(n)) out.push_back(n);
      if(auto p = nodes[n].parent) put(p);
#ifndef VISH_RA1B_MUTANT_NO_RULES
      for(auto item: suffix(n)) {
        out.push_back(item);
        // Including functions inside the following blocks/extensions, without a dependency by name.
        auto descend = [&](auto && self, node_id child) -> void {
          if(expanded[child.value]) return;
          expanded[child.value] = 1;
          put(child);
          for_each_child(child, [&](node_id next) { self(self, next); });
        };
        descend(descend, item);
      }
#endif
      auto & q = query(n);
      // All the outgoing name links of this key are already added to the same queue.
      if(!q.expanded) {
        q.expanded = true;
        for(auto d: q.dependents) put(d);
        if(auto name = dependency_name(n))
          for(auto u: q.users)
            if(is<n_extend>(u) && as<n_extend>(u).base == name) put(u);
        for(auto u: q.users)
          if(auto e = enclosing_extend(u); e && e != u) put(e);
      }
#ifndef VISH_RA1_MUTANT_NO_DOWN
      for(auto f: callees[n.value]) put(f);
#endif
    }
    unique_nodes(out);
    return out;
  }
  // limit - only nodes with a number below limit (comparison with the journal replay: nodes born by a check/elaboration are not in the program's journal)
  std::string rdeps_dump(bool rebuild = false, uint32_t limit = ~uint32_t{0}) const {
    use_pool();
    std::vector<std::array<uint32_t, 3>> entries;
    if(rebuild) {
      for(uint32_t i = 0; i < size() && i < limit; ++i)
        ref_keys(nodes[node_id(i)].kind, [&](uint32_t table, uint32_t key) { entries.push_back({table, key, i}); });
    } else {
      for(uint32_t t = 0; t < 2; ++t) {
        auto const & by = t ? refs_by_target : refs_by_name;
        for(uint32_t s = 0; s < by.size(); ++s)
          for(auto i: by[s]) if(i < limit) entries.push_back({t, s, i});
      }
    }
    std::ranges::sort(entries);
    entries.erase(std::unique(entries.begin(), entries.end()), entries.end());
    std::string out;
    for(auto [t, s, i]: entries) out += cat(t ? "t " : "r ", s, " ", i, "\n");
    for(uint32_t i = 0; i < size() && i < limit; ++i) {
      auto f = enclosing_fn(node_id(i));
      if(f) out += cat("f ", i, " ", f.value, "\n");
    }
    return out;
  }

  node_id add(node_kind k, span where) {
    use_pool();
    lane_guard g(lock); // Stripes (step 12.3b): the base grows from the background
    ++revision;
    where.source = pinned_source != ~uint32_t{0} ? pinned_source : uint32_t(source_count() == 0 ? 0 : source_count() - 1);
    node_id id = nodes.add(node{std::move(k), where, {}});
    note_fn(id);
    index_refs(id, true);
    node_rev.push_back(revision);
    birth.push_back(++births);
    log({j_add{id.value, kind_clone(nodes[id].kind), where, &pool}});
    return id;
  }

  // node, at least once former functions, increasing (superset: the view could change) - bypassing “all
  // functions of the base” without bypassing the entire database: it has a prelude and editor module,
  // and it grows with each edit (print: closing records, construction with records in the body). Mine. *
  std::vector<uint32_t> fn_nodes;
  std::vector<uint8_t> fn_noted;
  void note_fn(node_id n) {
    if(!std::holds_alternative<n_fn>(nodes[n].kind)) return;
    if(fn_noted.size() <= n.value) fn_noted.resize(std::max<std::size_t>(n.value + 1, fn_noted.size() * 2), 0);
    if(fn_noted[n.value]) return;
    fn_noted[n.value] = 1;
    if(fn_nodes.empty() || fn_nodes.back() < n.value) fn_nodes.push_back(n.value);
    else fn_nodes.insert(std::ranges::lower_bound(fn_nodes, n.value), n.value);
  }

  // node revision - when the last time its subtree within a function changed (reactive
  // disability: editing a block also changes the function revision).
  uint64_t node_revision(node_id n) const {
    use_pool();
    return node_rev[n.value];
  }

  node_id add(auto k, span where) {
    return add(node_kind(std::move(k)), where);
  }

  // reading node (nodes[]) sets the base itself - a separate use_pool here is superfluous (hot way)
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
    if(s.value >= sym_kinds.size()) grow_cache(sym_kinds, s.value + 1);
    std::atomic_ref<uint8_t> k(sym_kinds[s.value]); // Cache reads both bands: the value is one
    uint8_t v = k.load(std::memory_order_relaxed);
    if(!v) {
      auto t = strings.text(s);
      v = sc_known | (t == "[]" ? sc_index : 0) | (t == ".[]" ? sc_field : 0) | (t.size() > 1 && t[0] == '.' && lexer::is_ident_start(t[1]) ? sc_member : 0) |
          (t == "require" || t == "assume" ? sc_require : 0) | (t == "hide" ? sc_hide : 0) | (t == "try_compile" ? sc_try : 0) |
          (t == "=" || t == "+=" || t == "-=" || t == "*=" || t == "/=" ? sc_assign : 0);
      k.store(v, std::memory_order_relaxed);
    }
    return v;
  }
  // The cache on the symbol number grows under the lock; the capacity sets prepare_lanes (does not
  // move while another band reads it)
  static constexpr std::size_t cache_capacity = std::size_t(1) << 22;
  void grow_cache(auto & v, std::size_t n) const {
    lane_guard g(lock);
    if(v.size() < n) v.resize(std::max<std::size_t>(strings.size(), n));
  }
  void prepare_lanes() const {
    lane_guard g(lock);
    sym_kinds.reserve(cache_capacity);
    member_names.reserve(cache_capacity);
  }
  mutable std::recursive_mutex lock;
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
    if(op.value >= member_names.size()) grow_cache(member_names, op.value + 1);
    std::atomic_ref<uint32_t> s(member_names[op.value].value);
    uint32_t v = s.load(std::memory_order_relaxed);
    if(v == symbol::none_value) {
      v = const_cast<interner &>(strings).intern(strings.text(op).substr(1)).value; // parsing interns both
      s.store(v, std::memory_order_relaxed);
    }
    return symbol(v);
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
  std::optional<node_span> clause_pack(node_id n) const {
    if(!is<n_construct>(n)) return std::nullopt;
    auto const & c = as<n_construct>(n);
    if(c.items.empty() || !is<n_ident>(c.type) || strings.text(as<n_ident>(c.type).name) != "array") return std::nullopt;
    for(node_id x: c.items)
      if(!is<n_block>(x) && !(is<n_lambda>(x) && as<n_lambda>(x).block)) return std::nullopt;
    return node_span(c.items);
  }
  // Processor blocks: the pack - its elements, otherwise - one
  node_list handlers_of(node_id n) const {
    if(auto p = clause_pack(n)) return node_list(p->begin(), p->end());
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
    use_pool();
    // Quoting the source of the mutations (or node over it - deletion) rolls them back.
    if(!mutations.empty())
      for(auto const & m: mutations)
        if(within(node_id(m.source - 1), n) || within(n, node_id(m.source - 1))) {
          rollback(m.source);
          break;
        }
    // Filmed children no longer belong to this node; playback replace does the same,
    // so backlinks match once downloaded.
    std::size_t child_count = 0;
    auto count = [&](node_id) { ++child_count; };
    std::visit([&](auto const & next) { for_each_child_of(next, count); }, k);
    std::vector<uint32_t> kept;
    if(child_count > 8) {
      kept.reserve(child_count);
      auto keep = [&](node_id x) { kept.push_back(x.value); };
      std::visit([&](auto const & next) { for_each_child_of(next, keep); }, k);
      std::ranges::sort(kept); // A large module does not require a quadratic search for children.
    }
    std::visit([&](auto const & old) {
      auto detach = [&](node_id child) {
        if(nodes[child].parent != n) return;
        bool retained = false;
        if(child_count > 8) retained = std::ranges::binary_search(kept, child.value);
        else {
          auto find = [&](node_id x) { retained = retained || x == child; };
          std::visit([&](auto const & next) { for_each_child_of(next, find); }, k);
        }
        if(!retained) nodes[child].parent = {};
      };
      for_each_child_of(old, detach);
    }, nodes[n].kind);
    ++revision;
    ++edits;
    index_refs(n, false);
    nodes[n].kind = std::move(k);
    index_refs(n, true);
    note_fn(n);
    touch(n);
    use_pool();
    if(edited_at.size() <= n.value) edited_at.resize(n.value + 1);
    edited_at[n.value] = revision;
    log({j_replace{n.value, kind_clone(nodes[n].kind), &pool}});
  }
  // The node stood in a new place (raw block, redisassembled on site).
  void place(node_id n, span where) {
    nodes[n].where = where;
    log({j_place{n.value, where}});
  }

  // Up to function: Cache granularity - functions, not files
  void touch(node_id n) {
    use_pool();
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
  using mutation = mutation_rec;
  void mutate(node_id n, node_kind k, uint32_t source) {
    use_pool();
    use_pool();
    ++revision;
    // new module elements - generated by ct code instance (and when playing journal)
    if(source && is<n_module>(n) && std::holds_alternative<n_module>(k))
      for(node_id x: std::get<n_module>(k).items)
        if(!std::ranges::contains(as<n_module>(n).items, x)) {
          inst_emits.insert(x.value);
          emit_owner[x.value] = n.value;
        }
    list<parent_undo> parents;
    if(source) {
      auto remember = [&](auto && self, node_id owner, node_kind const & kind) -> void {
        std::visit([&](auto const & x) {
          auto child_parent = [&](node_id child) {
            if(nodes[child].parent != owner) parents.push_back({child, nodes[child].parent, owner});
            self(self, child, nodes[child].kind);
          };
          for_each_child_of(x, child_parent);
        }, kind);
      };
      remember(remember, n, k);
    }
    index_refs(n, false);
    if(source) mutations.push_back({n.value, source, revision, std::move(nodes[n].kind), std::move(parents)});
    else retired.push_back(std::move(nodes[n].kind));
    nodes[n].kind = std::move(k);
    index_refs(n, true);
    note_fn(n);
    if(!source && reopened(n)) return; // Derivative taken again: in journal - when reconciled
    touch(n);
    changed.push_back(n.value);
    log({j_mutate{n.value, kind_clone(nodes[n].kind), source, &pool}});
  }
  // The rollback of mutations of the source (0 - all) and all after the first of them - later
  // ones could read its result (canonical order, §7.3 p.5):
// The node that is right after the mutation remains.
  // The righteousness of the scriptures.
  void rollback(uint32_t source) {
    use_pool();
    std::size_t from = 0;
    while(from < mutations.size() && source && mutations[from].source != source) ++from;
    if(from == mutations.size()) return;
    ++revision;
    for(std::size_t i = mutations.size(); i-- > from;) {
      mutation & m = mutations[i];
      node_id n(m.target);
      if(m.target < edited_at.size() && edited_at[m.target] > m.at) continue;
      for(std::size_t j = m.parents.size(); j-- > 0;) {
        auto const & p = m.parents[j];
        if(nodes[p.child].parent == p.next) nodes[p.child].parent = p.old;
      }
      index_refs(n, false);
      retired.push_back(std::move(nodes[n].kind));
      nodes[n].kind = std::move(m.old);
      index_refs(n, true);
      note_fn(n);
      touch(n);
      changed.push_back(m.target);
    }
    for(std::size_t i = from; i < mutations.size(); ++i) retired.push_back(std::move(mutations[i].old));
    mutations.resize(from);
    log({j_rollback{source}});
  }

  // Retired: Name between server messages and session commands when there are no rounds (no one to keep their
  // lists; human editing - replace - and so destroys the former look at once). Otherwise, a long session of
  // the editor accumulates them with each elaboration. With stripes, the background can bypass the base - do
  // not touch.
  void sweep_retired() {
    if(lanes_on.load(std::memory_order_acquire)) return;
    retired.clear();
    retired.shrink_to_fit();
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
  // elaboration, which does not touch the program (editor code in the language): nested - does not roll
  // back the mutations of the ct program and does not declare its result to be valid
  elab_mark nested_elaboration() {
    ++elab_depth;
    return {{}, 0, 0, true};
  }
  std::unordered_map<uint32_t, node_id> elab_parents; // the baseline before a rollback for comparing nodes taken anew
  elab_mark begin_elaboration() {
    use_pool();
    if(elab_depth++ || elab_current()) return {{}, 0, 0, true};
    elab_edits = ~uint64_t{0}; // Goes again: not finished (error) - does not work
    elab_ct.clear();
    elab_pass_error.reset();
    elab_changes.clear();
    elab_mark m{{}, R().oplog.size(), changed.size(), false, reborn.size()};
    for(auto const & x: mutations)
      if(std::ranges::none_of(m.was, [&](auto const & w) { return w.first == x.target; })) m.was.push_back({x.target, nodes[node_id(x.target)].kind});
    elab_parents.clear();
    for(auto const & mutation: mutations)
      for(auto const & p: mutation.parents) elab_parents.try_emplace(p.child.value, nodes[p.child].parent);
    touch_undo.clear();
    touch_logging = true;
    rollback(0);
    return m;
  }
  void end_elaboration(elab_mark const & m) {
    use_pool();
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
      auto & lg = R().oplog;
      std::size_t w = m.log_from;
      for(std::size_t i = m.log_from; i < lg.size(); ++i) {
        stored_entry const & e = lg[i];
        bool drop = std::holds_alternative<s_rollback>(e.op) || (std::holds_alternative<s_mutate>(e.op) && std::get<s_mutate>(e.op).source);
        if(!drop) lg[w++] = e;
      }
      lg.resize(w);
    }
    touch_undo.clear();
    elab_parents.clear();
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
        else if constexpr(std::is_same_v<field_t, list<node_id>>) {
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
  struct reborn_rec {
    uint32_t id;
    node_kind was; // pre-capture
    node_id parent;
    void const * holder;
  };

  static void hash_mix(uint64_t & h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); }
  static uint64_t field_hash(auto const & f) {
    using F = std::remove_cvref_t<decltype(f)>;
    if constexpr(std::is_same_v<F, node_id> || std::is_same_v<F, symbol>) return f.value;
    else if constexpr(std::is_same_v<F, std::string>) return std::hash<std::string>{}(f);
    else if constexpr(std::is_same_v<F, node_text>) return std::hash<std::string_view>{}(f.view());
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
  // View with its own lists and text in the current database: a journal image does not share memory with a live
  // node (descriptor is a copy of the link: editing the node list in place should not change the recorded)
  static node_kind kind_clone(node_kind const & k) {
    node_kind out = k;
    std::visit([](auto & x) {
      template for(constexpr auto m: fields_of<std::remove_cvref_t<decltype(x)>>()) {
        using F = [:std::meta::type_of(m):];
        if constexpr(is_pool_list<F>::value || std::is_same_v<F, node_text>) x.[:m:] = x.[:m:].clone();
      }
    },
        out);
    return out;
  }
  // Copy of node for editing elements in place: lists - their own (otherwise editing the copy changes the living node)
  template<typename K>
  K copy_of(node_id n) {
    use_pool();
    K x = std::get<K>(nodes[n].kind);
    template for(constexpr auto m: fields_of<K>()) {
      using F = [:std::meta::type_of(m):];
      if constexpr(is_pool_list<F>::value || std::is_same_v<F, node_text>) x.[:m:] = x.[:m:].clone();
    }
    return x;
  }
  node_kind copy_kind(node_id n) {
    use_pool();
    return kind_clone(nodes[n].kind);
  }
  // View from pool to pool to (play journal to another database, download from disk)
  static node_kind kind_translate(node_kind const & k, node_pool const * from, node_pool * to) {
    node_kind out = k;
    std::visit([&](auto & x) {
      template for(constexpr auto m: fields_of<std::remove_cvref_t<decltype(x)>>()) {
        using F = [:std::meta::type_of(m):];
        if constexpr(is_pool_list<F>::value) {
          std::vector<typename F::value_type> tmp;
          {
            pool_scope a(const_cast<node_pool *>(from));
            tmp = x.[:m:].vec();
          }
          pool_scope b(to);
          x.[:m:] = F(tmp);
        } else if constexpr(std::is_same_v<F, node_text>) {
          std::string tmp;
          {
            pool_scope a(const_cast<node_pool *>(from));
            tmp = x.[:m:].str();
          }
          pool_scope b(to);
          x.[:m:] = node_text(tmp);
        }
      }
    },
        out);
    return out;
  }
  // Type of journal recording in the pool of this database
  node_kind own_kind(node_kind const & k, node_pool const * home) {
    use_pool();
    return home && home != &pool ? kind_translate(k, home, &pool) : k;
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
    use_pool();
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

  // --- derivatives of node live (S5.3): born - a list in the roots, born_ix - by node ; reuse - by key, hash table ---
  born_snap const * born_find(uint32_t id) const {
    use_pool();
    auto const & ix = R().born_ix;
    return id < ix.size() && ix[id] ? &R().born[ix[id] - 1] : nullptr;
  }
  bool born_has(uint32_t id) const { return born_find(id) != nullptr; }
  void born_add(uint32_t id, node_kind const & k, uint64_t key) {
    use_pool();
    auto & ix = R().born_ix;
    if(ix.size() <= id) ix.resize(std::max<std::size_t>(id + 1, ix.size() * 2), 0);
    R().born.push_back(born_snap{id, 0, key, k});
    ix[id] = R().born.size();
  }
  static uint32_t reuse_hash(uint64_t key) { return uint32_t((key * 0x9e3779b97f4a7c15ull) >> 40); }
  // Place the key in reuse; create - open empty; ~0 - no
  uint32_t reuse_at(uint64_t key, bool create) {
    use_pool();
    auto & tab = R().reuse_tab;
    if(tab.size() == 0) {
      if(!create) return ~uint32_t{0};
      tab.resize(64, 0);
    }
    uint32_t mask = tab.size() - 1;
    uint32_t i = reuse_hash(key) & mask;
    for(; tab[i]; i = (i + 1) & mask)
      if(R().reuse[tab[i] - 1].key == key) return tab[i] - 1;
    if(!create) return ~uint32_t{0};
    R().reuse.push_back(reuse_snap{key, 0, {}});
    tab[i] = R().reuse.size();
    if(uint64_t(R().reuse.size()) * 2 > tab.size()) { // table half: twice as much, keys moved
      list<uint32_t> bigger;
      bigger.resize(std::size_t(tab.size()) * 2, 0);
      uint32_t m2 = bigger.size() - 1;
      for(uint32_t k = 0; k < R().reuse.size(); ++k) {
        uint32_t j = reuse_hash(R().reuse[k].key) & m2;
        while(bigger[j]) j = (j + 1) & m2;
        bigger[j] = k + 1;
      }
      tab.release();
      tab = bigger;
    }
    return R().reuse.size() - 1;
  }

  // Derivative node: former free with the same view at birth and place, otherwise new. Holder is a
  // list of node that the interpreter holds (releases release).
  node_id derive(node_kind k, span where, std::vector<uint32_t> & holder) {
    use_pool();
    lane_guard g(lock);
    uint64_t key = kind_hash(k, where); // place - as at the source (not the last import: editing it does not change)
    uint32_t slot = reuse_at(key, true);
    for(std::size_t i = R().reuse[slot].cursor, steps = 0; i < R().reuse[slot].ids.size() && steps < 64; ++i, ++steps) {
      uint32_t x = R().reuse[slot].ids[i];
      node_id n(x);
      if(held_now(x) || reopened(n)) continue;
      auto const & b = *born_find(x);
      if(b.key != key || !kind_equal(b.kind, k) || nodes[n].where.begin != where.begin || nodes[n].where.end != where.end || nodes[n].where.source != where.source || installed(n)) continue;
      R().reuse[slot].cursor = i + 1;
      if(reopen.size() <= x) reopen.resize(x + 1, 0);
      reopen[x] = 1;
      index_refs(n, false);
      node_kind was = std::move(nodes[n].kind);
      nodes[n].kind = std::move(k);
      index_refs(n, true); // the kind at birth: the completion will be repeated
      note_fn(n);
      auto prior = elab_parents.find(x);
      reborn.push_back({x, std::move(was), prior == elab_parents.end() ? nodes[n].parent : prior->second, &holder});
      nodes[n].parent = {}; // the place belongs to the new owner (link_parents)
      birth[x] = ++births;
      hold(x, holder);
      return n;
    }
    uint32_t saved = std::exchange(pinned_source, where.source);
    node_id n = add(k, where);
    pinned_source = saved;
    born_add(n.value, k, key);
    R().reuse[slot].ids.push_back(n.value);
    R().reuse[slot].cursor = R().reuse[slot].ids.size();
    log({j_derived{n.value}});
    hold(n.value, holder);
    return n;
  }
  void hold(uint32_t x, std::vector<uint32_t> & holder) {
    lane_guard g(lock);
    if(held.size() <= x) held.resize(x + 1, 0);
    ++held[x];
    holder.push_back(x);
  }
  // The interpreter is over: his node is free (reconciliation of the ones he took anew - before that)
  void release(std::vector<uint32_t> & holder) {
    lane_guard g(lock);
    flush_reborn(0, &holder);
    for(uint32_t x: holder) {
      --held[x];
      if(auto b = born_find(x)) R().reuse[reuse_at(b->key, true)].cursor = 0;
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
          log({j_mutate{r.id, kind_clone(nodes[n].kind), 0, &pool}});
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
    use_pool();
    node_text t, l;
    {
      pool_scope in_strings(&spool); // Import texts - in a row pool: do not move like the previous deque
      t = node_text(text);
      l = node_text(label);
    }
    pool_scope in_pool(&pool);
    R().sources.push_back(t);
    R().labels.push_back(l);
    log({j_import{std::move(text), std::move(label)}});
    return uint32_t(R().sources.size() - 1);
  }
  uint32_t source_count() const { return R().sources.size(); }

  // Import tag for diagnostics (empty - main file).
  std::string_view source_label(uint32_t i) const {
    if(i >= R().labels.size()) return {};
    node_text const t = reinterpret_cast<node_text const *>(pool.base + R().labels.off)[i];
    return {reinterpret_cast<char const *>(spool.base + t.off), t.size()};
  }

  std::string_view source(uint32_t i = 0) const {
    if(i >= R().sources.size()) return {};
    node_text const t = reinterpret_cast<node_text const *>(pool.base + R().sources.off)[i];
    return {reinterpret_cast<char const *>(spool.base + t.off), t.size()};
  }

  // journal with new strings before each operation.
  std::vector<journal_entry> journal() const {
    use_pool(); // journal is the pool listings of this database, and the current one may be alien (save_area: domain base created later)
    std::vector<journal_entry> out;
    uint32_t emitted = 0;
    for(stored_entry const & e: R().oplog) {
      for(; emitted < e.strings; ++emitted)
        out.push_back({e.revision, j_intern{emitted, std::string(strings.text(symbol(emitted)))}});
      out.push_back(journal_entry{e.revision, const_cast<ast_db *>(this)->from_stored(e.op)});
    }
    for(; emitted < strings.size(); ++emitted)
      out.push_back({revision, j_intern{emitted, std::string(strings.text(symbol(emitted)))}});
    for(auto const & r: reborn) // Taken again, not yet verified: state - as is
      if(!kind_equal(nodes[node_id(r.id)].kind, r.was) || nodes[node_id(r.id)].parent != r.parent) {
        out.push_back({revision, j_mutate{r.id, nodes[node_id(r.id)].kind, 0, &pool}});
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
    use_pool();
    auto oplog = log_entries();
    std::size_t from = 0;
    while(from < oplog.size() && oplog[from].second.revision < since) ++from;
    if(from == oplog.size()) return;
    std::size_t imports = 0;
    for(std::size_t i = from; i < oplog.size(); ++i) imports += std::holds_alternative<j_import>(oplog[i].second.op);
    uint32_t source = uint32_t(source_count() - imports);
    std::set<uint32_t> used; // draft imports referred to by node wood
    for(uint32_t x = 0; x < nodes.size(); ++x)
      if(std::ranges::contains(drafts, nodes[node_id(x)].where.source) && !used.contains(nodes[node_id(x)].where.source) && installed(node_id(x))) used.insert(nodes[node_id(x)].where.source);
    auto draft_source = [&](uint32_t x) { return x != keep && std::ranges::contains(drafts, x) && !used.contains(x); };
    auto garbage = [&](uint32_t id) {
      node_id n(id);
      return id < nodes.size() && std::ranges::contains(drafts, nodes[n].where.source) && !born_has(id) && !installed(n);
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
    // Back to the roots: import numbers in order (j_import is one, j_gap is as many as imports)
    R().oplog.clear();
    uint32_t src = 0;
    for(auto const & [count, e]: out) R().oplog.push_back(stored_entry{e.revision, count, to_stored(e.op, src)});
  }

  // Play journal to an empty database.
  void replay(std::vector<journal_entry> const & entries) {
    use_pool();
    for(auto const & e: entries) {
      std::visit([&](auto const & op) {
        using T = std::remove_cvref_t<decltype(op)>;
        if constexpr(std::is_same_v<T, j_intern>) {
          if(strings.intern(op.text).value != op.id) throw diagnostic("журнал повреждён: порядок строк", {});
        } else if constexpr(std::is_same_v<T, j_add>) {
          pinned_source = op.where.source; // as written (node parsing raw block - in its text)
          node_id got = add(own_kind(op.kind, op.home), op.where);
          pinned_source = ~uint32_t{0};
          if(got.value != op.id) throw diagnostic("журнал повреждён: порядок узлов", {});
        } else if constexpr(std::is_same_v<T, j_replace>) {
          replace(node_id(op.id), own_kind(op.kind, op.home));
        } else if constexpr(std::is_same_v<T, j_link>) {
          link_parents(node_id(op.root));
        } else if constexpr(std::is_same_v<T, j_mutate>) {
          mutate(node_id(op.id), own_kind(op.kind, op.home), op.source);
        } else if constexpr(std::is_same_v<T, j_rollback>) {
          rollback(op.source);
        } else if constexpr(std::is_same_v<T, j_place>) {
          place(node_id(op.id), op.where);
        } else if constexpr(std::is_same_v<T, j_note>) {
          set_note(node_id(op.id), op.note);
        } else if constexpr(std::is_same_v<T, j_derived>) {
          node_id n(op.id);
          uint64_t key = kind_hash(nodes[n].kind, nodes[n].where);
          born_add(op.id, nodes[n].kind, key);
          R().reuse[reuse_at(key, true)].ids.push_back(op.id); // free: cursor from the beginning
          log({j_derived{op.id}});
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
    use_pool();
    auto const & ix = R().note_ix;
    if(n.value >= ix.size() || !ix[n.value]) return nullptr;
    auto it = note_cache.find(n.value);
    if(it == note_cache.end()) it = note_cache.emplace(n.value, plain_note(R().notes[ix[n.value] - 1].note)).first;
    return &it->second;
  }
  void set_note(node_id n, node_note x) {
    use_pool();
    bool empty = x.lead.empty() && x.tail.empty() && x.end.empty();
    auto & ix = R().note_ix;
    bool has = n.value < ix.size() && ix[n.value];
    if(empty && !has) return;
    if(has) {
      note_rec & old = R().notes[ix[n.value] - 1];
      release_note(old.note);
      if(empty) {
        old.id = ~uint32_t{0};
        old.note = {};
        ix[n.value] = 0;
        note_cache.erase(n.value);
      } else {
        R().notes[ix[n.value] - 1].note = pool_note(x);
        note_cache[n.value] = x;
      }
    } else {
      if(ix.size() <= n.value) ix.resize(std::max<std::size_t>(n.value + 1, ix.size() * 2), 0);
      R().notes.push_back(note_rec{n.value, 0, pool_note(x)});
      ix[n.value] = R().notes.size();
      note_cache[n.value] = x;
    }
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
  node_span context_items(node_id n) const { return is<n_block>(n) ? as<n_block>(n).items : is<n_quote>(n) ? as<n_quote>(n).items : as<n_module>(n).items; }

  // All daughter node_id node: One realization for all species through reflection.
  void for_each_child(node_id n, auto && f) const {
    std::visit([&](auto const & k) { for_each_child_of(k, f); }, nodes[n].kind);
  }

  uint32_t size() const { return nodes.size(); }

  mutable std::unordered_map<std::string, uint32_t> program_kinds; // ast.X → node announcement (program_kind)
  // Write journal live at roots (S5.3): stored_entry - without pointers, species share pieces of pool with the src
  // record - the import number that will occupy j_import (grows by j_import and imports by j_gap)
  stored_op to_stored(journal_op const & op, uint32_t & src) const {
    return std::visit([&](auto const & x) -> stored_op {
      using T = std::remove_cvref_t<decltype(x)>;
      if constexpr(std::is_same_v<T, j_add>) return s_add{x.id, x.kind, x.where};
      else if constexpr(std::is_same_v<T, j_replace>) return s_replace{x.id, x.kind};
      else if constexpr(std::is_same_v<T, j_link>) return s_link{x.root};
      else if constexpr(std::is_same_v<T, j_import>) {
        s_import i{source_text(src), source_label_text(src)};
        ++src;
        return i;
      } else if constexpr(std::is_same_v<T, j_mutate>) return s_mutate{x.id, x.kind, x.source};
      else if constexpr(std::is_same_v<T, j_rollback>) return s_rollback{x.source};
      else if constexpr(std::is_same_v<T, j_place>) return s_place{x.id, x.where};
      else if constexpr(std::is_same_v<T, j_note>) return s_note{x.id, pool_note(x.note)};
      else if constexpr(std::is_same_v<T, j_gap>) {
        src += x.imports;
        return s_gap{x.nodes, x.imports};
      } else if constexpr(std::is_same_v<T, j_derived>) return s_derived{x.id};
      else throw diagnostic("журнал: запись j_intern не хранится в журнале базы", {});
    },
        op);
  }
  journal_op from_stored(stored_op const & op) {
    return std::visit([&](auto const & x) -> journal_op {
      using T = std::remove_cvref_t<decltype(x)>;
      if constexpr(std::is_same_v<T, s_add>) return j_add{x.id, x.kind, x.where, &pool};
      else if constexpr(std::is_same_v<T, s_replace>) return j_replace{x.id, x.kind, &pool};
      else if constexpr(std::is_same_v<T, s_link>) return j_link{x.root};
      else if constexpr(std::is_same_v<T, s_import>) return j_import{spool_str(x.text), spool_str(x.label)};
      else if constexpr(std::is_same_v<T, s_mutate>) return j_mutate{x.id, x.kind, x.source, &pool};
      else if constexpr(std::is_same_v<T, s_rollback>) return j_rollback{x.source};
      else if constexpr(std::is_same_v<T, s_place>) return j_place{x.id, x.where};
      else if constexpr(std::is_same_v<T, s_note>) return j_note{x.id, plain_note(x.note)};
      else if constexpr(std::is_same_v<T, s_gap>) return j_gap{x.nodes, x.imports};
      else return j_derived{x.id};
    },
        op);
  }
  // journal as a whole (line to time, record) - for squash_log and export
  std::vector<std::pair<uint32_t, journal_entry>> log_entries() {
    use_pool();
    std::vector<std::pair<uint32_t, journal_entry>> out;
    out.reserve(R().oplog.size());
    for(stored_entry const & e: R().oplog) out.push_back({e.strings, journal_entry{e.revision, from_stored(e.op)}});
    return out;
  }
  void log(journal_op op) {
    use_pool();
    lane_guard g(lock);
    uint32_t src = std::holds_alternative<j_import>(op) ? uint32_t(R().sources.size() - 1) : 0;
    stored_op st = to_stored(op, src);
    R().oplog.push_back(stored_entry{revision, strings.size(), st});
  }

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
    node_kind k = kind_clone(nodes[n].kind); // copy: arena can move; lists are corrected on the spot
    span w = nodes[n].where;
    std::visit([&](auto & x) {
      template for(constexpr auto m: fields_of<std::remove_cvref_t<decltype(x)>>()) {
        using field_t = [:std::meta::type_of(m):];
        if constexpr(std::is_same_v<field_t, node_id>) x.[:m:] = clone(x.[:m:], subst, holder);
        else if constexpr(std::is_same_v<field_t, list<node_id>>)
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
      } else if constexpr(std::is_same_v<field_t, list<node_id>>) {
        for(node_id c: k.[:m:])
          if(c) f(c);
      }
    }
  }

  node_array<node, node_tag> nodes;
  node_id prelude; // prelude module: kernel designs written in language
  bool loading_prelude = false;
  uint64_t revision = 0;
  // Live in the roots of the area (S5.3): pool lists, not std::vector - fixing them does not copy
  list<uint64_t> & node_rev = R().node_rev;
  list<uint64_t> & edited_at = R().edited_at; // revision of the last node edit (not mutations)
  list<mutation_rec> & mutations = R().mutations; // active ct-code mutations in order
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
  // cache itself is not: from .vishdb the block is disassembled again; the recalculation with the
  // same result takes the previous node (derive).
  struct derived_rec {
    std::array<uint64_t, 4> key;
    uint32_t node;
  };
  std::unordered_map<uint32_t, derived_rec> derived;
  uint64_t edits = 0; // replacement
  // derivatives of node (derive)
  std::vector<uint32_t> held; // interpreters holding the knot
  std::vector<uint8_t> reopen; // It's taken over, it hasn't been reconciled yet.
  std::vector<reborn_rec> reborn;
  std::vector<uint64_t> birth; // The tact of birth or taking again: “created by this elaboration”
  uint64_t births = 0;
  // Choices of printing by code in the language (§4.4, variant C): the key is the selection function,
  // its revision, base edits and arguments → the result (0/1, -1 - error: conservative choice). A pure
  // function is one sum to the same arguments; printing does not change the base, so mutable.
  mutable std::unordered_map<std::string, int8_t> print_choices;
  // Cache for note_of (pointer to plain view): truth - R() .notes; filled on demand, set_note leads it
  mutable std::unordered_map<uint32_t, node_note> note_cache;
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
  for(std::size_t i = 0; i < d.trace.size();) { // the same frames in a row (deep recursion) - one line
    auto const & f = d.trace[i];
    std::size_t n = 1;
    while(i + n < d.trace.size() && d.trace[i + n].what == f.what && d.trace[i + n].where.begin == f.where.begin && d.trace[i + n].where.source == f.where.source) ++n;
    auto l = locate(db.source(f.where.source), f.where.begin);
    out += cat("  ", name(f.where), ":", l.line, ":", l.col, ": ", f.what, n > 1 ? cat(" ×", n) : std::string(), "\n");
    i += n;
  }
  for(auto const & a: d.also) out += render(a, file, db);
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
  lane_guard g(db.lock); // cache
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
  int kind = -1; // node (number in node_kind); -1 - kernel call with head name; record_template - workpiece
  std::vector<uint32_t> slot; // parameter k → place in the body (field node / call argument)
  uint32_t places = 0; // body-place
  std::string program; // View declared by the program (kind - n_node): ast.twice
  node_id tmpl; // workpiece: code in quote { … }, places - #x inserts and #x block parameters
  std::vector<uint8_t> part; // at the workpiece: type of place of each part (part_node ...)
};
// Types of workpiece locations: node (insert #x, place-block), block parameter name (#x
// in for(#x : …)), ad name (#name in function), list of parameters (#params).
constexpr uint8_t part_node = 0, part_binder = 1, part_name = 2, part_params = 3, part_items = 4; // items - struct fields #name { #fields }
// Write-workpiece (§4.5, step 11.2): the body is `quote { код }` of the base language, parts of
// the record are inserted into it: `syntax each (x) in (xs) {body} => quote { for(#x : #xs) #body
// };`. parsing builds the workpiece code with parts in place, the print recognizes the node by the
// workpiece - the second language writes the same node as the base language (for, try...),
// including blocks that enter names (Q`#x` is the name of the block parameter). ;
//
// 
// *My:* workpiece - quote, `#x` in the name place, block from one insert `{
// #body }` - place-block.
constexpr int record_template = -3;

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
bool form_place(std::string_view p) { return form_paren(p) || form_next(p) || p == "{" || p == "{=" || p == "#"; }
// Part `#имя` is the naked word name (define #name..., §4.5).
//
bool form_word(std::string_view p) { return p == "#"; }
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

// Place of workpiece t record f: #x insert or #x block parameter is the number of the write
// parameter (-1 is not the place).
int template_place(ast_db const & db, n_fn const & f, node_id t) {
  std::string_view name;
  if(db.is<n_splice>(t)) name = db.strings.text(db.as<n_splice>(t).name);
  else if(db.is<n_param>(t) && db.strings.text(db.as<n_param>(t).name).starts_with('#')) name = db.strings.text(db.as<n_param>(t).name).substr(1);
  else return -1;
  for(uint32_t k = 0; k < f.params.size(); ++k)
    if(db.strings.text(db.as<n_param>(f.params[k]).name) == name) return int(k);
  return -1;
}
// Name-place in the node character field of the workpiece (not the parameter):
// `#name(#params) { … }` is the part number of the record or -1.
int template_name(ast_db const & db, n_fn const & f, symbol s) {
  std::string_view name = db.strings.text(s);
  if(!s.valid() || !name.starts_with('#')) return -1;
  for(uint32_t k = 0; k < f.params.size(); ++k)
    if(db.strings.text(db.as<n_param>(f.params[k]).name) == name.substr(1)) return int(k);
  return -1;
}
// The blank block from one insert is the place-block `{ #body }`: part of the record is the
// node block (try { … } catch(e) { … } - blocks, not instructions).
int template_block_place(ast_db const & db, n_fn const & f, node_id t) {
  if(!db.is<n_block>(t) || db.as<n_block>(t).items.size() != 1 || !db.is<n_splice>(db.as<n_block>(t).items[0])) return -1;
  return template_place(db, f, db.as<n_block>(t).items[0]);
}

std::optional<record_info> record_of(ast_db const & db, node_id fn) {
  if(!db.is<n_fn>(fn)) return std::nullopt;
  auto const & f = db.as<n_fn>(fn);
  if(f.form.empty() || !f.body) return std::nullopt;
  record_info r;
  node_list items;
  if(db.is<n_quote>(f.body) && db.as<n_quote>(f.body).items.size() == 1) { // Preparation: Each part is exactly one place
    r.kind = record_template;
    r.tmpl = db.as<n_quote>(f.body).items[0];
    r.places = uint32_t(f.params.size());
    std::vector<std::string_view> pieces; // part of the recording of each parameter (in order of places)
    for(symbol w: f.form)
      if(auto p = db.strings.text(w); !form_mark(p) && form_place(p)) pieces.push_back(p);
    if(pieces.size() != f.params.size()) return std::nullopt;
    r.part.assign(f.params.size(), part_node);
    std::vector<uint32_t> uses(f.params.size());
    bool ok = true;
    auto count = [&](this auto const & self, node_id t) -> void {
      if(int k = template_place(db, f, t); k >= 0) {
        ++uses[k];
        if(db.is<n_param>(t)) r.part[k] = form_list(pieces[k]) ? part_params : part_binder;
      }
      std::visit([&](auto const & x) {
        template for(constexpr auto m: fields_of<std::remove_cvref_t<decltype(x)>>()) {
          using field_t = [:std::meta::type_of(m):];
          if constexpr(std::is_same_v<field_t, symbol>) {
            if(int k = db.is<n_param>(t) ? -1 : template_name(db, f, x.[:m:]); k >= 0) {
              ++uses[k];
              r.part[k] = part_name;
            }
          } else if constexpr(std::is_same_v<field_t, list<node_id>>) {
            for(node_id c: x.[:m:]) // The list of parameters is the only one in its list.
              if(int k = template_place(db, f, c); k >= 0 && db.is<n_param>(c) && form_list(pieces[k]) && x.[:m:].size() != 1) ok = false;
            if(db.is<n_extend>(t) && x.[:m:].size() == 1 && db.is<n_splice>(x.[:m:][0])) // struct #name { #fields } - part-block fields
              if(int k = template_place(db, f, x.[:m:][0]); k >= 0 && pieces[k] == "{") r.part[k] = part_items;
          }
        }
      },
          db[t].kind);
      db.for_each_child(t, [&](node_id c) { self(c); });
    };
    count(r.tmpl);
    if(!ok || std::ranges::any_of(uses, [](uint32_t u) { return u != 1; })) return std::nullopt;
    for(uint32_t k = 0; k < f.params.size(); ++k) r.slot.push_back(k);
    return r;
  }
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
    items = db.as<n_construct>(f.body).items.vec();
    if(n < 0 || items.size() > uint32_t(n)) return std::nullopt;
    r.places = uint32_t(n);
  } else if(db.is<n_call>(f.body)) {
    auto const & c = db.as<n_call>(f.body);
    if(!db.is<n_ident>(c.callee) || db.as<n_ident>(c.callee).name != f.name || !c.words.empty() || c.tagged || c.form) return std::nullopt;
    items = c.args.vec();
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
  else if constexpr(std::is_same_v<F, list<node_id>> || std::is_same_v<F, std::vector<symbol>> || std::is_same_v<F, std::vector<uint32_t>> || std::is_same_v<F, list<symbol>> || std::is_same_v<F, list<uint32_t>>) return "array";
  // Numbers and text - machine representations (§5.8, O15): tag names - prelude
  else if constexpr(std::is_same_v<F, symbol> || std::is_same_v<F, std::string> || std::is_same_v<F, node_text>) return "__bytes";
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

} // namespace vish
