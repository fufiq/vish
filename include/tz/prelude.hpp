// prelude: for/while/try is a language code (lib/prelude.tz) built into the VM build. It
// is scraped into the same database by the first module: its ads set the syntax of calls
// (clauses, tails) for all subsequent texts.
#pragma once

#include "parser.hpp"

namespace tz {

constexpr unsigned char prelude_bytes[] = {
#embed "../../lib/prelude.tz"
};

node_id load_prelude(ast_db & db) {
  if(db.prelude || db.loading_prelude) return db.prelude;
  db.loading_prelude = true;
  parser p(db, std::string_view(reinterpret_cast<char const *>(prelude_bytes), sizeof prelude_bytes), "prelude.tz");
  node_id root = p.parse_module();
  db.loading_prelude = false;
  if(!p.errors().empty()) throw p.errors().front();
  return root;
}

} // namespace tz
