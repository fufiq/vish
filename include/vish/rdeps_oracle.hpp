// RA-1: an independent reference of the AST reverse index (docs/reactive-ast.md, I1).
//
// Mine. The reference does NOT call ast_db::ref_keys/index_refs/users_of/dependents_of_fn/impact_of: it collects
// "all uses" anew by reflection over the fields of each node kind (any symbol / list<symbol> field; the target number
// `target`; child n_ident). A forgotten node kind or a new symbol field cannot be "forgotten": the reference sees it itself,
// and a "reference != index" mismatch is a violation until the field is entered in the index or in the explicit exclusion list below.
// Before, I1 compared the index with a recount by the same ref_names - a tautology that did not catch omissions of the general extraction.
#pragma once
#include "vish/ast.hpp"
#include <algorithm>
#include <array>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>

namespace vish::rdeps_oracle {

using entry = std::array<uint32_t, 3>; // a table (0 - name, 1 - the target's node number), a key (a symbol or a number), the use node

// Symbol fields that are NOT a use of someone else's name (every other symbol field is a use and must
// be in the index). The list is explicit and short; extend it only with a recorded reason.
constexpr bool declaration_field(std::string_view kind, std::string_view field) {
  return (kind == "n_module" && field == "name")   // a module declaration
      || (kind == "n_fn" && field == "name")       // a function/operator declaration
      || (kind == "n_fn" && field == "form")       // the words of a syntax record: the declaration of a parse rule (RA-1b)
      || (kind == "n_param" && field == "name")    // the name of its own parameter
      || (kind == "n_param" && field == "clause")  // a clause word: try(...) catch(...) - syntax, not an entity
      || (kind == "n_var" && field == "name")      // the name of its own variable/field
      || (kind == "n_call" && field == "words")    // the words of call argument clauses
      || (kind == "n_construct" && field == "names") // the field names of an anonymous literal {x: 1}
      || (kind == "n_native" && field == "lang");  // the language of a native block ("cpp")
}

// The reference: all (table, key, node) triples over all database nodes with a number below limit.
inline std::vector<entry> expected_entries(ast_db const & db, uint32_t limit = ~uint32_t{0}) {
  std::vector<entry> out;
  for(uint32_t i = 0; i < db.size() && i < limit; ++i) {
    std::visit([&](auto const & x) {
      using K = std::remove_cvref_t<decltype(x)>;
      constexpr std::string_view kind = std::meta::identifier_of(std::meta::dealias(^^K));
      template for(constexpr auto m: fields_of<K>()) {
        using F = [:std::meta::type_of(m):];
        constexpr std::string_view field = std::meta::identifier_of(m);
        if constexpr(std::is_same_v<F, symbol>) {
          if constexpr(!declaration_field(kind, field))
            if(x.[:m:]) out.push_back({0u, x.[:m:].value, i});
        } else if constexpr(std::is_same_v<F, list<symbol>>) {
          if constexpr(!declaration_field(kind, field))
            for(std::size_t k = 0; k < x.[:m:].size(); ++k)
              if(x.[:m:][k]) out.push_back({0u, x.[:m:][k].value, i});
        } else if constexpr(std::is_same_v<F, uint32_t> && field == "target") {
          if(x.[:m:]) out.push_back({1u, x.[:m:], i}); // a reference by declaration number: n_ref, n_extend
        } else if constexpr(std::is_same_v<F, node_id>) {
          // a child n_ident is a use of a name recorded at the parent (the same name as the ident itself)
          node_id c = x.[:m:];
          if(c && c.value < db.size() && c.value < limit)
            if(auto const * id = std::get_if<n_ident>(&db[c].kind); id && id->name) out.push_back({0u, id->name.value, c.value});
        } else if constexpr(std::is_same_v<F, list<node_id>>) {
          for(std::size_t k = 0; k < x.[:m:].size(); ++k) {
            node_id c = x.[:m:][k];
            if(c && c.value < db.size() && c.value < limit)
              if(auto const * id = std::get_if<n_ident>(&db[c].kind); id && id->name) out.push_back({0u, id->name.value, c.value});
          }
        }
      }
    }, db[node_id(i)].kind);
  }
  std::ranges::sort(out);
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

// The contents of the live index - straight from its tables.
inline std::vector<entry> index_entries(ast_db const & db, uint32_t limit = ~uint32_t{0}) {
  std::vector<entry> out;
  for(uint32_t t = 0; t < 2; ++t) {
    auto const & by = t ? db.refs_by_target : db.refs_by_name;
    for(uint32_t s = 0; s < by.size(); ++s)
      for(auto i: by[s]) if(i < limit) out.push_back({t, s, i});
  }
  std::ranges::sort(out);
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

inline std::string describe(ast_db const & db, entry const & e) {
  std::string key = e[0] ? cat("узел ", e[1]) : cat("имя ", db.strings.text(symbol(e[1])));
  return cat(key, " → использование ", e[2], " (вид ", db[node_id(e[2])].kind.index(), ")");
}

// "" - the index equals the reference; otherwise the first mismatches. missing - reference is not a subset of the index (a missed dependency),
// extra - the index is not a subset of the reference (a stale or an extra entry).
inline std::string compare_index(ast_db const & db, uint32_t limit = ~uint32_t{0}, std::size_t * missing_count = nullptr) {
  auto want = expected_entries(db, limit), have = index_entries(db, limit);
  std::vector<entry> missing, extra;
  std::ranges::set_difference(want, have, std::back_inserter(missing));
  std::ranges::set_difference(have, want, std::back_inserter(extra));
  if(missing_count) *missing_count = missing.size();
  std::string out;
  for(std::size_t k = 0; k < missing.size() && k < 3; ++k) out += cat("эталон ⊄ индекс: ", describe(db, missing[k]), "\n");
  for(std::size_t k = 0; k < extra.size() && k < 3; ++k) out += cat("индекс ⊄ эталон: ", describe(db, extra[k]), "\n");
  if(missing.size() > 3 || extra.size() > 3) out += cat("всего: пропусков ", missing.size(), ", лишних ", extra.size(), "\n");
  return out;
}

// Reference queries: the same answers by the specification, but through the reference (a search by symbol text, a walk over parent, without
// refs_by_name). A comparison with ast_db::users_of / dependents_of_fn / impact_of checks the query layer (operators, `.field`,
// references, inheritance) that the dump comparison does not see.
struct model {
  ast_db const & db;
  std::vector<entry> uses;
  std::unordered_map<std::string, std::vector<uint32_t>> by_text; // a name text -> the use nodes
  std::unordered_map<uint32_t, std::vector<uint32_t>> by_target;
  std::vector<uint32_t> fns;
  explicit model(ast_db const & d) : db(d), uses(expected_entries(d)) {
    for(auto const & e: uses) {
      if(e[0]) by_target[e[1]].push_back(e[2]);
      else by_text[std::string(db.strings.text(symbol(e[1])))].push_back(e[2]);
    }
    for(uint32_t i = 0; i < db.size(); ++i) if(std::holds_alternative<n_fn>(db[node_id(i)].kind)) fns.push_back(i);
  }
  static node_list sorted(node_list xs) {
    std::ranges::sort(xs, {}, &node_id::value);
    xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
    return xs;
  }
  node_id fn_of(node_id n) const { // the nearest function up the parent chain
    for(; n; n = db[n].parent) if(std::holds_alternative<n_fn>(db[n].kind)) return n;
    return {};
  }
  bool inside(node_id n, node_id top) const {
    for(; n; n = db[n].parent) if(n == top) return true;
    return false;
  }
  std::string name_of(node_id d) const { // the name of a declaration entity: the name field of these kinds
    auto const & k = db[d].kind;
    symbol s;
    if(auto p = std::get_if<n_fn>(&k)) s = p->name;
    else if(auto p = std::get_if<n_module>(&k)) s = p->name;
    else if(auto p = std::get_if<n_extend>(&k)) s = p->name;
    else if(auto p = std::get_if<n_ident>(&k)) s = p->name;
    else if(auto p = std::get_if<n_var>(&k)) s = p->name;
    return s ? std::string(db.strings.text(s)) : std::string();
  }
  void add_text(std::string const & t, node_list & out) const {
    if(auto it = by_text.find(t); it != by_text.end()) for(auto i: it->second) out.push_back(node_id(i));
  }
  node_list users(node_id d) const {
    node_list out;
    auto name = name_of(d);
    if(!name.empty()) {
      add_text(name, out);
      if(name[0] != '.') add_text("." + name, out); // a.name is the postfix operator `.name`
    }
    if(auto it = by_target.find(d.value); it != by_target.end()) for(auto i: it->second) out.push_back(node_id(i)); // ref(f)
    if(std::holds_alternative<n_fn>(db[d].kind)) // an operator function: infix+ - the uses of the operator `+` (any role and arity)
      for(std::string_view role: {"infix", "prefix", "postfix"})
        if(name.size() > role.size() && name.starts_with(role)) { add_text(name.substr(role.size()), out); break; }
    return sorted(std::move(out));
  }
  node_list dependents(node_id d) const {
    node_list out;
    for(auto u: users(d)) {
      if(auto f = fn_of(u)) out.push_back(f);
      bool extend = std::holds_alternative<n_extend>(db[u].kind), use = std::holds_alternative<n_use>(db[u].kind);
      if(extend || use) { // an extension or use changes any function of its scope: an extension inside itself, use - the module
        node_id top = u;
        if(use) while(db[top].parent && !std::holds_alternative<n_module>(db[top].kind)) top = db[top].parent;
        for(auto f: fns) if(inside(node_id(f), top)) out.push_back(node_id(f));
      }
    }
    // destroy/copy (the value's life) and implicit (a conversion) are called implicitly, by the value's type: any function of the program (not of the prelude) is dependent
    auto name = name_of(d);
    if(std::holds_alternative<n_fn>(db[d].kind) && (name == "destroy" || name == "copy" || name == "implicit"))
      for(auto f: fns) if(!in_prelude(node_id(f))) out.push_back(node_id(f));
    return sorted(std::move(out));
  }
  bool in_prelude(node_id n) const {
    for(; n && !std::holds_alternative<n_module>(db[n].kind); n = db[n].parent) {}
    return n && n == db.prelude;
  }
  // impact: functions and modules - up the parent chain, the dependents, and the descendants of an entity as its dependents.
  node_list impact(node_list const & changed) const {
    node_list todo = changed, out;
    std::vector<uint8_t> seen(db.size(), 0);
    for(std::size_t i = 0; i < todo.size(); ++i) {
      auto n = todo[i];
      if(!n || n.value >= db.size() || seen[n.value]) continue;
      seen[n.value] = 1;
      if(std::holds_alternative<n_fn>(db[n].kind) || std::holds_alternative<n_module>(db[n].kind)) out.push_back(n);
      if(db[n].parent) todo.push_back(db[n].parent);
      for(auto d: dependents(n)) todo.push_back(d);
      auto name = name_of(n);
      if(!name.empty())
        for(auto u: users(n))
          if(auto const * e = std::get_if<n_extend>(&db[u].kind); e && e->base && std::string(db.strings.text(e->base)) == name) todo.push_back(u);
    }
    return sorted(std::move(out));
  }
};

// A comparison of the live index's answers with the reference model for the probes declarations. Returns the number of mismatches;
// the description of the first is in report.
inline int compare_queries(ast_db const & db, model const & m, node_list const & probes, std::string & report) {
  int bad = 0;
  auto show = [&](node_list const & xs) { std::string s; for(auto x: xs) s += cat(x.value, " "); return s; };
  for(auto d: probes) {
    auto u = db.users_of(d), eu = m.users(d);
    auto dep = db.dependents_of_fn(d), edep = m.dependents(d);
    node_list ch{d};
    auto imp = db.impact_of(ch), eimp = m.impact(ch);
    auto note = [&](char const * what, node_list const & got, node_list const & want) {
      ++bad;
      if(report.size() < 600) report += cat(what, "(", d.value, " ", m.name_of(d), "): индекс [", show(got), "] ≠ эталон [", show(want), "]\n");
    };
    if(u != eu) note("users_of", u, eu);
    if(dep != edep) note("dependents_of", dep, edep);
    if(imp != eimp) note("impact_of", imp, eimp);
  }
  return bad;
}

// A snapshot of query answers over a set of nodes - for I3: after a rollback the answers must match the earlier ones.
inline std::string query_snapshot(ast_db const & db, node_list const & probes) {
  std::string out;
  for(auto d: probes) {
    node_list ch{d};
    out += cat("q ", d.value, ":");
    for(auto x: db.users_of(d)) out += cat(" u", x.value);
    for(auto x: db.dependents_of_fn(d)) out += cat(" d", x.value);
    for(auto x: db.impact_of(ch)) out += cat(" i", x.value);
    out += "\n";
  }
  return out;
}

} // namespace vish::rdeps_oracle
