// Modules (§9.1): Semantic key inclusion.
// 
//;
//.
//
// A module is a n_module node with a key name in the same database. *My:* text module, which is
// not in the database, the tool finds next to the program: geo.vec → <catalogue>/geo/vec.tz - .
//
#pragma once

#include "os.hpp"
#include "parser.hpp"

#include <filesystem>
#include <set>

namespace tz {

// Module with key in the database (last import - valid).
node_id find_module(ast_db const & db, symbol key) {
  node_id found;
  for(uint32_t i = 0; i < db.size(); ++i)
    if(db.is<n_module>(node_id(i)) && db.as<n_module>(node_id(i)).name == key) found = node_id(i);
  return found;
}

// use the root module to the base (recursively). Errors parsing modules and "no module".
std::vector<diagnostic> load_uses(ast_db & db, node_id root, std::filesystem::path const & dir) {
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
      node_id got = find_module(db, key);
      if(!got) {
        std::string rel(db.strings.text(key));
        std::ranges::replace(rel, '.', '/');
        std::filesystem::path file = dir / (rel + ".tz");
        if(!std::filesystem::exists(file)) {
          errors.push_back(diagnostic(cat("нет модуля ", db.strings.text(key), " (", file.string(), ")"), db[item].where));
          continue;
        }
        os::mapped_file text(file.string());
        parser p(db, text.text(), std::string(db.strings.text(key)));
        got = p.parse_module();
        for(auto & e: p.errors()) errors.push_back(e);
        n_module mod = db.as<n_module>(got);
        if(!mod.name) { // Module without header - name by key
          mod.name = key;
          db.replace(got, std::move(mod));
        } else if(mod.name != key)
          errors.push_back(diagnostic(cat("модуль ", db.strings.text(mod.name), " найден по ключу ", db.strings.text(key)), db[got].where));
      }
      todo.push_back(got);
    }
  }
  return errors;
}

} // namespace tz
