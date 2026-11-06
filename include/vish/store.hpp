// Storing the AST database on a disk: binary journal operations (.vishdb).
//
// Format: Magic "VISHDB0001", root module number, then journal recordings, each
// serialized generically through reflection (serial.hpp).
#pragma once

#include "ast.hpp"
#include "os.hpp"
#include "serial.hpp"

namespace vish {

constexpr std::string_view vishdb_magic = "VISHDB0035"; // 0035: Derivative nodes - in the pool when playing (j derived); 0034: skipping the node numbers and imports of dropped draft parsings (j gap); 0033: comments - layout in the database (j note); 0032: control of extensions - n module.attrs instead of sealed; 0031: the place of the node after re-examination on the spot - raw block as a unit of editing (j place); 0030: anonymous structural literature (n construct.s); 00: 00: 00: roll from the source of sym back (28 m) [[state]] variable module (n var.attrs); 0026: struct - instance struct, extension (n extend.of, without n struct); 0025: point - operators .x and .[] (without n member, n field); 0024: a[i] - pair operator [] (n op, without n index); 0023: auto - absence of type (without n pack - {blocks} (without n pack); 0021:ntrict (without 00: 00: 00: 00: n n n

void save_db(ast_db const & db, node_id root, std::string const & path) {
  writer w;
  for(char c: vishdb_magic) w.put(c);
  w.put(root.value);
  for(auto const & e: db.journal()) w.put(e);
  os::write_file_atomic(path, w.bytes);
}

// journal live prefix state; all numbers belong to the same database. The caller ensures no
// competitive operations and selects only its modules.
void restore_nodes(ast_db & live, ast_db const & target, std::vector<node_id> const & module_roots, std::vector<node_id> const & selected = {}) {
  auto reject = [] { throw diagnostic("несовместимая история базы или дерево модулей", {}); };
  auto a = target.journal();
  auto b = live.journal();
  if(a.size() > b.size() || target.size() > live.size() || target.strings.size() > live.strings.size() || target.source_count() > live.source_count()) reject();
  for(std::size_t i = 0; i < a.size(); ++i) {
    writer x, y;
    x.put(a[i].op); // replay recounts service audits; kinship is set by operations
    y.put(b[i].op);
    if(x.bytes != y.bytes) reject();
  }
  struct saved { node_id id; node_kind kind; span where; node_note note; };
  std::vector<saved> copies;
  std::vector<uint8_t> seen(target.size(), 0);
  std::vector<node_id> todo;
  for(auto root: module_roots) {
    if(!root || root.value >= target.size() || !target.is<n_module>(root)) reject();
    if(selected.empty()) todo.push_back(root);
  }
  for(auto n: selected) {
    if(!n || n.value >= target.size()) reject();
    node_id owner = n;
    while(target[owner].parent) owner = target[owner].parent;
    if(!std::ranges::contains(module_roots, owner)) reject();
    todo.push_back(n);
  }
  while(!todo.empty()) {
    node_id n = todo.back();
    todo.pop_back();
    if(!n || n.value >= target.size() || n.value >= live.size() || seen[n.value]) reject();
    seen[n.value] = 1;
    auto const & node = target[n];
    if(node.where.source >= target.source_count() && target.source_count()) reject();
    if(auto ref = std::get_if<n_ref>(&node.kind); ref && ref->target >= target.size()) reject();
    std::visit([&](auto const & kind) {
      template for(constexpr auto m: fields_of<std::remove_cvref_t<decltype(kind)>>()) {
        using F = [:std::meta::type_of(m):];
        if constexpr(std::is_same_v<F, symbol>)
          if(kind.[:m:] && kind.[:m:].value >= target.strings.size()) reject();
      }
    }, node.kind);
    if(live.installed(n)) {
      node_id owner = n;
      while(live[owner].parent) owner = live[owner].parent;
      if(!std::ranges::contains(module_roots, owner) && live.is<n_module>(owner)) {
        auto name = live.as<n_module>(owner).name;
        auto key = name ? live.strings.text(name) : std::string_view();
        if(key == "prelude" || key == "editor" || key == "tools" || key == "versions" || key == "cli" || key == "tests") reject();
      }
    }
    copies.push_back({n, node.kind, node.where, target.note_of(n) ? *target.note_of(n) : node_note{}});
    target.for_each_child(n, [&](node_id child) { todo.push_back(child); });
  }
  live.rollback(0);
  for(auto & x: copies) {
    live.replace(x.id, std::move(x.kind));
    live.place(x.id, x.where);
    live.set_note(x.id, std::move(x.note));
  }
  for(auto root: module_roots) live.link_parents(root);
}

// staging - a pool in which the lists and text of the read records lie: it must survive their reproduction
std::vector<journal_entry> read_journal(std::string const & path, node_id & root, node_pool & staging) {
  pool_scope in(&staging);
  os::mapped_file file(path);
  reader r(file.text());
  // Before the renaming of the language (2026-10-22), the base began with TZDB0035 - the same format.
  bool legacy = file.text().starts_with("TZDB0035");
  for(char c: legacy ? std::string_view("TZDB0035") : vishdb_magic)
    if(r.get<char>() != c) throw diagnostic(cat(path, ": не файл базы vish"), {});
  root = node_id(r.get<uint32_t>());
  std::vector<journal_entry> entries;
  while(!r.at_end()) entries.push_back(r.get<journal_entry>());
  return entries;
}

node_id load_db(ast_db & db, std::string const & path) {
  node_id root;
  node_pool staging;
  db.replay(read_journal(path, root, staging));
  return root;
}

// journal in text.
std::string describe_journal(std::vector<journal_entry> const & entries) {
  std::string out;
  for(auto const & e: entries) {
    out += cat("r", pad(cat(e.revision), 5), " ");
    std::visit([&](auto const & op) {
      using T = std::remove_cvref_t<decltype(op)>;
      if constexpr(std::is_same_v<T, j_intern>) out += cat("строка  #", op.id, " ", in_quotes(op.text));
      else if constexpr(std::is_same_v<T, j_add>) {
        out += cat("узел    #", op.id, " ");
        out += std::visit([](auto const & k) -> std::string_view {
          return std::meta::identifier_of(std::meta::remove_cvref(^^decltype(k)));
        },
            op.kind);
        out += cat(" [", op.where.begin, "..", op.where.end, "]");
      } else if constexpr(std::is_same_v<T, j_replace>) out += cat("замена  #", op.id);
      else if constexpr(std::is_same_v<T, j_link>) out += cat("связи   от #", op.root);
      else if constexpr(std::is_same_v<T, j_mutate>) out += op.source ? cat("мутация #", op.id, " ← ct #", op.source - 1) : cat("достройка #", op.id);
      else if constexpr(std::is_same_v<T, j_note>) out += cat("комментарии #", op.id);
      else if constexpr(std::is_same_v<T, j_rollback>) out += op.source ? cat("откат   ct #", op.source - 1, " и позднейших") : std::string("откат   всех мутаций ct");
      else if constexpr(std::is_same_v<T, j_place>) out += cat("место   #", op.id, " [", op.where.begin, "..", op.where.end, "]");
      else if constexpr(std::is_same_v<T, j_gap>) out += cat("пропуск узлов ", op.nodes, ", импортов ", op.imports);
      else if constexpr(std::is_same_v<T, j_derived>) out += cat("производный #", op.id);
      else out += cat("импорт  текста ", op.text.size(), " байт");
    },
        e.op);
    out += '\n';
  }
  return out;
}

} // namespace vish
