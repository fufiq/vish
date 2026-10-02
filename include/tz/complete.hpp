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
      else if(db.is_struct(item)) add(db.as<n_extend>(item).name, "тип", "struct", depth);
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

// The terms of the value after `.` for its aggregate field types, the size of the array
// and string, the functions called through the point are `a.f(b) ≡ f(a, b)` (§4.5): the
// first parameter without the type (polymorphic) or with the same type name.
//
std::vector<completion_item> member_completions(ast_db const & db, node_id root, std::vector<std::string> const & types) {
  std::vector<completion_item> out;
  auto add = [&](std::string name, std::string kind, std::string detail, uint32_t depth) {
    for(auto const & x: out)
      if(x.name == name) return;
    out.push_back({std::move(name), std::move(kind), std::move(detail), depth});
  };
  auto type_text = [&](node_id t) -> std::string {
    return t && db.is<n_ident>(t) ? std::string(db.strings.text(db.as<n_ident>(t).name)) : std::string();
  };
  for(auto const & t: types) {
    std::string base = t.substr(0, t.find('<'));
    if(base == "array" || base == "str") add("size", "поле", cat("i64 — ", t), 0);
    // entity is a struct ad and all of its `Имя { … }` extensions:
// 
    // Fields and functions of all
    auto field = [&](node_id f) {
      if(db.is<n_var>(f)) add(std::string(db.strings.text(db.as<n_var>(f).name)), "поле", cat(type_text(db.as<n_var>(f).type), " — ", t), 0);
    };
    for(node_id module: {root, db.prelude}) {
      if(!module || !db.is<n_module>(module)) continue;
      for(node_id n: db.as<n_module>(module).items) {
        if(db.is<n_extend>(n) && db.strings.text(db.as<n_extend>(n).name) == base)
          for(node_id x: db.as<n_extend>(n).items) {
            field(x);
            if(db.is<n_fn>(x) && !db.as<n_fn>(x).params.empty()) {
              std::string rest = "(";
              auto const & ps = db.as<n_fn>(x).params;
              for(uint32_t k = 1; k < ps.size(); ++k) rest += cat(k > 1 ? ", " : "", db.strings.text(db.as<n_param>(ps[k]).name));
              add(std::string(db.strings.text(db.as<n_fn>(x).name)), "функция", rest + ")", 0);
            }
          }
      }
    }
  }
  return out;
}

// Functions called through a point for these types (from root and prelude modules).
// Selection: A function is suitable if its first parameter in some instance received
// this type (first_types(f) is the types of the first parameter for all instance) or is
// explicitly typed by it. instance with this type is not offered.
//
void add_dot_functions(ast_db const & db, node_id root, std::vector<std::string> const & types, std::vector<completion_item> & out,
    auto && first_types) {
  for(node_id module: {root, db.prelude}) {
    if(!module || !db.is<n_module>(module)) continue;
    for(node_id item: db.as<n_module>(module).items) {
      if(!db.is<n_fn>(item)) continue;
      auto const & f = db.as<n_fn>(item);
      std::string name(db.strings.text(f.name));
      if(f.params.empty() || name.starts_with("infix") || name.starts_with("prefix") || name.starts_with("postfix")) continue;
      auto const & first = db.as<n_param>(f.params[0]);
      // design (expr-parameter - block or clause: for, while, try) - syntax, not a call through a point
      if(std::ranges::any_of(f.params, [&](node_id p) { return db.as<n_param>(p).mode == param_mode::expr || db.as<n_param>(p).clause; })) continue;
      bool fits = false;
      if(first.type && db.is<n_ident>(first.type)) {
        std::string want(db.strings.text(db.as<n_ident>(first.type).name));
        fits = std::ranges::any_of(types, [&](std::string const & t) { return t.substr(0, t.find('<')) == want; });
      } else {
        std::vector<std::string> seen = first_types(item);
        fits = std::ranges::any_of(types, [&](std::string const & t) { return std::ranges::contains(seen, t); });
      }
      if(!fits) continue;
      std::string rest = "(";
      for(uint32_t k = 1; k < f.params.size(); ++k) rest += cat(k > 1 ? ", " : "", db.strings.text(db.as<n_param>(f.params[k]).name));
      if(std::ranges::none_of(out, [&](completion_item const & x) { return x.name == name; }))
        out.push_back({name, "функция", rest + ")", module == root ? 1u : 2u});
    }
  }
}

// Live filter: prefix, then substring, then subsequence (without taking into account the
// register); with equality - near, then by name. That's all.
std::vector<completion_item> filter_completions(std::vector<completion_item> items, std::string_view word, std::size_t limit = 100) {
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
  for(auto & x: items) {
    // node AST (struct programs generated from VM) - only on the typed prefix: they are needed by
    // the code that works with the code, the rest - noise (*mine*)
    if(x.kind == "тип" && is_node_kind(x.name) && (w.empty() || !lower(x.name).starts_with(w))) continue;
    if(int r = rank(x.name); r >= 0) scored.push_back({r, std::move(x)});
  }
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
