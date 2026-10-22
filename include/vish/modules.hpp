// Modules (§9.1): Semantic key inclusion.
// 
//;
//.
//
// A module is a n_module node with a key name in the same database. *My:* text module, which is
// not in the database, the tool finds next to the program: geo.vec → <catalogue> /geo/vec.vish -
// .
#pragma once

#include "os.hpp"
#include "parser.hpp"

#include <filesystem>
#include <set>

namespace vish {

// Use k keys; top-level text to its parsing.
std::vector<std::string> use_keys(std::string_view text) {
  lexer lx(text);
  auto toks = lx.run();
  std::vector<std::string> out;
  int depth = 0;
  for(std::size_t i = 0; i < toks.size(); ++i) {
    auto const & t = toks[i];
    if(t.kind == tok::open) ++depth;
    else if(t.kind == tok::close) --depth;
    else if(depth == 0 && t.kind == tok::ident && t.text == "use" && i + 1 < toks.size() && toks[i + 1].kind == tok::ident) {
      std::string key(toks[++i].text);
      while(i + 2 < toks.size() && toks[i + 1].ch() == '.' && toks[i + 2].kind == tok::ident) {
        key += '.';
        key += toks[i + 2].text;
        i += 2;
      }
      out.push_back(std::move(key));
    }
  }
  return out;
}

// Keys of qualified text names: geo.vec.norm( - chain prefixes that have a module file
// (geo/vec.vish); the module is taken without use - the name is named in its entirety.
std::vector<std::string> qualified_keys(std::string_view text, std::filesystem::path const & dir) {
  lexer lx(text);
  auto toks = lx.run();
  std::vector<std::string> out;
  for(std::size_t i = 0; i < toks.size(); ++i) {
    if(toks[i].kind != tok::ident || (i > 0 && toks[i - 1].ch() == '.')) continue;
    std::vector<std::string_view> parts{toks[i].text};
    std::size_t j = i;
    while(j + 2 < toks.size() && toks[j + 1].ch() == '.' && toks[j + 2].kind == tok::ident) {
      parts.push_back(toks[j + 2].text);
      j += 2;
    }
    if(parts.size() < 2 || j + 1 >= toks.size() || toks[j + 1].ch() != '(') continue;
    std::string key, rel;
    for(std::size_t k = 0; k + 1 < parts.size(); ++k) {
      key += (k ? "." : "") + std::string(parts[k]);
      rel += (k ? "/" : "") + std::string(parts[k]);
      if(std::filesystem::exists(dir / (rel + ".vish"))) out.push_back(key);
    }
  }
  return out;
}

// Modules from use k; text to the database before the text itself (like prelude): use brings
// module shapes (syntax, operator, design) into context at the use point - the main module's
// parsing sees them. Recursively; the use cycle is a pass. Errors parsing modules; "no module"
// - load_uses (use location known after parsing).
std::vector<diagnostic> preload_uses(ast_db & db, std::string_view text, std::filesystem::path const & dir, std::set<std::string> & loading) {
  std::vector<diagnostic> errors;
  auto keys = use_keys(text);
  for(auto & k: qualified_keys(text, dir)) keys.push_back(std::move(k));
  for(auto const & key: keys) {
    auto it = db.strings.index.find(key);
    if((it != db.strings.index.end() && find_module(db, it->second)) || loading.contains(key)) continue;
    std::string rel = key;
    std::ranges::replace(rel, '.', '/');
    std::filesystem::path file = dir / (rel + ".vish");
    if(!std::filesystem::exists(file)) continue;
    os::mapped_file f(file.string());
    loading.insert(key);
    for(auto & e: preload_uses(db, f.text(), dir, loading)) errors.push_back(e);
    parser p(db, f.text(), key);
    node_id got = p.parse_module();
    loading.erase(key);
    for(auto & e: p.errors()) errors.push_back(e);
    symbol k = db.strings.intern(key);
    n_module mod = db.as<n_module>(got);
    if(!mod.name) { // Module without header - name by key
      mod.name = k;
      db.replace(got, std::move(mod));
    } else if(mod.name != k)
      errors.push_back(diagnostic(cat("модуль ", db.strings.text(mod.name), " найден по ключу ", key), db[got].where));
  }
  return errors;
}
std::vector<diagnostic> preload_uses(ast_db & db, std::string_view text, std::filesystem::path const & dir) {
  std::set<std::string> loading;
  return preload_uses(db, text, dir, loading);
}

// use the root module (recursively) after parsing: "no module" - at the place of use.
std::vector<diagnostic> missing_uses(ast_db const & db, node_id root, std::filesystem::path const & dir) {
  std::vector<diagnostic> errors;
  std::set<uint32_t> seen;
  std::vector<node_id> todo{root};
  while(!todo.empty()) {
    node_id m = todo.back();
    todo.pop_back();
    if(!seen.insert(m.value).second) continue;
    for(node_id item: db.as<n_module>(m).items) {
      if(!db.is<n_use>(item)) continue;
      symbol key = db.as<n_use>(item).key;
      if(node_id got = find_module(db, key)) {
        todo.push_back(got);
        continue;
      }
      std::string rel(db.strings.text(key));
      std::ranges::replace(rel, '.', '/');
      errors.push_back(diagnostic(cat("нет модуля ", db.strings.text(key), " (", (dir / (rel + ".vish")).string(), ")"), db[item].where));
    }
  }
  return errors;
}

// Program with modules: modules from use - before text (preload_uses), then text, then "no
// module". Errors - modules, text, use.
node_id parse_program(ast_db & db, std::string_view text, std::filesystem::path const & dir, std::vector<diagnostic> & errors) {
  auto modules = preload_uses(db, text, dir);
  parser p(db, text);
  node_id root = p.parse_module();
  errors = p.errors();
  for(auto & e: modules) errors.push_back(e);
  for(auto & e: missing_uses(db, root, dir)) errors.push_back(e);
  return root;
}

} // namespace vish
