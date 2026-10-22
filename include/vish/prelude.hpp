// prelude: for/while/try is the language code (lib/prelude.vish) built into the VM build.
// It is scraped into the same database by the first module: its ads set the syntax of
// calls (clauses, tails) for all subsequent texts.
#pragma once

#include "parser.hpp"

namespace vish {

constexpr unsigned char prelude_bytes[] = {
#embed "../../lib/prelude.vish"
};

// prelude and node AST (struct of the program)
std::string const & prelude_text() {
  static std::string const text = std::string(reinterpret_cast<char const *>(prelude_bytes), sizeof prelude_bytes) + "\n" + ast_structs_text();
  return text;
}

node_id load_prelude(ast_db & db) {
  if(db.prelude || db.loading_prelude) return db.prelude;
  db.loading_prelude = true;
  parser p(db, prelude_text(), "prelude.vish");
  node_id root = p.parse_module();
  db.loading_prelude = false;
  if(!p.errors().empty()) { // place in the prelude text (there is no error in the file)
    diagnostic d = p.errors().front();
    auto lc = locate(prelude_text(), d.where.begin);
    throw diagnostic(cat("prelude.vish:", lc.line, ":", lc.col, ": ", d.what()), d.where);
  }
  return root;
}

} // namespace vish
