// Supplement: context identifiers (§13.1, M5).
//
// 
// 
// 
// 
// Therefore, the context is the path from the cursor's place up the AST: the block
// gives the declared to the place, the function and the block of construction -
// parameters, module and prelude - their entity.
//
// The list is alive: nothing is
// inserted by itself.
//
//
#pragma once

#include "ast.hpp"
#include "interp.hpp"

#include <algorithm>

namespace tz {

struct completion_item {
  std::string name;
  std::string kind; // variable, parameter, function, type
  std::string detail;
  uint32_t depth = 0; // closer to place, less
};

// Identifiers visible at `at` (node where the cursor stands).
std::vector<completion_item> context_identifiers(ast_db const & db, node_id at) {
  std::vector<completion_item> out;
  auto add_name = [&](std::string_view name, std::string_view kind, std::string detail, uint32_t depth) {
    std::string n(name);
    if(n.empty() || n.starts_with("infix") || n.starts_with("prefix") || n.starts_with("postfix")) return; // operators are not names
    for(auto & x: out)
      if(x.name == n) {
        if(x.kind == "функция" && kind == "функция" && x.depth == depth) x.detail = "перегрузки";
        return; // Short-term ad hides long-range
      }
    out.push_back({std::move(n), std::string(kind), std::move(detail), depth});
  };
  auto add = [&](symbol name, std::string_view kind, std::string detail, uint32_t depth) {
    if(name) add_name(db.strings.text(name), kind, std::move(detail), depth);
  };
  auto params = [&](node_list const & ps) {
    std::string s = "(";
    for(node_id p: ps) {
      if(s.size() > 1) s += ", ";
      s += db.strings.text(db.as<n_param>(p).name);
    }
    return s + ")";
  };
  auto entities = [&](node_id module, uint32_t depth) {
    if(!module || !db.is<n_module>(module)) return;
    for(node_id item: db.as<n_module>(module).items) {
      if(db.is<n_fn>(item)) add(db.as<n_fn>(item).name, "функция", params(db.as<n_fn>(item).params), depth);
      else if(db.is<n_struct>(item)) add(db.as<n_struct>(item).name, "тип", "struct", depth);
      else if(db.is<n_var>(item)) add(db.as<n_var>(item).name, "переменная", "модуль", depth);
    }
  };

  uint32_t depth = 0;
  node_id child = at;
  for(node_id n = at; n; child = n, n = db[n].parent, ++depth) {
    if(db.is<n_block>(n)) {
      for(node_id s: db.as<n_block>(n).items) {
        if(s == child) break; // post-sitting
        if(db.is<n_var>(s)) add(db.as<n_var>(s).name, "переменная", {}, depth);
        // `x = e` announces if the name is not yet available (author's decision: no auto)
        if(db.is<n_op>(s) && db.strings.text(db.as<n_op>(s).op) == "=" && !db.as<n_op>(s).args.empty() && db.is<n_ident>(db.as<n_op>(s).args[0]))
          add(db.as<n_ident>(db.as<n_op>(s).args[0]).name, "переменная", {}, depth);
      }
    } else if(db.is<n_fn>(n)) {
      for(node_id p: db.as<n_fn>(n).params) add(db.as<n_param>(p).name, "параметр", {}, depth);
    } else if(db.is<n_lambda>(n)) {
      for(node_id p: db.as<n_lambda>(n).params) add(db.as<n_param>(p).name, "параметр", "блок конструкции", depth);
    } else if(db.is<n_module>(n)) {
      entities(n, depth);
    }
  }
  entities(db.prelude, depth + 1);
  for(auto const & [name, sig]: interpreter::visible_builtins) add_name(name, "функция", cat(sig, " — VM"), depth + 2);
  return out;
}

// Live filter: prefix, then substring, then subsequence (without taking into account the
// register); with equality - near, then by name. That's all.
std::vector<completion_item> filter_completions(std::vector<completion_item> items, std::string_view word, std::size_t limit = 50) {
  auto lower = [](std::string_view s) {
    std::string r(s);
    for(char & c: r)
      if(c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return r;
  };
  std::string w = lower(word);
  auto rank = [&](std::string const & name) -> int {
    std::string n = lower(name);
    if(n.starts_with(w)) return 0;
    if(n.contains(w)) return 1;
    std::size_t k = 0;
    for(char c: n)
      if(k < w.size() && c == w[k]) ++k;
    return k == w.size() ? 2 : -1;
  };
  std::vector<std::pair<int, completion_item>> scored;
  for(auto & x: items)
    if(int r = rank(x.name); r >= 0) scored.push_back({r, std::move(x)});
  std::ranges::sort(scored, [](auto const & a, auto const & b) {
    if(a.first != b.first) return a.first < b.first;
    if(a.second.depth != b.second.depth) return a.second.depth < b.second.depth;
    return a.second.name < b.second.name;
  });
  std::vector<completion_item> out;
  for(auto & [r, x]: scored)
    if(out.size() < limit) out.push_back(std::move(x));
  return out;
}

} // namespace tz
