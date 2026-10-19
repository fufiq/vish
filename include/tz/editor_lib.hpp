// Editor Module (lib/editor.tz): Editor solutions are code in a language embedded in the VM build as
// prelude. It is loaded only by the editor's server - programs do not disassemble it (user solution
// 2026-10-19). In the database, which is edited by the editor, it lies like prelude: in journal there
// is, the program does not see it (does not use).
#pragma once

#include "parser.hpp"

namespace tz {

constexpr unsigned char editor_bytes[] = {
#embed "../../lib/editor.tz"
};

std::string const & editor_text() {
  static std::string const text(reinterpret_cast<char const *>(editor_bytes), sizeof editor_bytes);
  return text;
}

// Base editor module: already there (base saved by editor) - it, otherwise parsing text
node_id load_editor(ast_db & db) {
  for(uint32_t i = 0; i < db.size(); ++i)
    if(db.is<n_module>(node_id(i)) && db.as<n_module>(node_id(i)).name && db.strings.text(db.as<n_module>(node_id(i)).name) == "editor" && !db[node_id(i)].parent)
      return node_id(i);
  parser p(db, editor_text(), "editor.tz");
  node_id m = p.parse_module();
  if(!p.errors().empty()) {
    diagnostic d = p.errors().front();
    auto lc = locate(editor_text(), d.where.begin);
    throw diagnostic(cat("editor.tz:", lc.line, ":", lc.col, ": ", d.what()), d.where);
  }
  return m;
}

// The interpreter calls the names of entity operators (infix+=, postfix.size...) at the first execution of
// the operator: the editor’s decisions in the language execute their names in the database from the
// opening, and not in journal of the first edit of the program (*my*).
void intern_operator_entities(ast_db & db) {
  for(uint32_t i = 0; i < db.size(); ++i) {
    node_id n(i);
    if(db.is<n_op>(n)) {
      auto const & o = db.as<n_op>(n);
      db.strings.intern(cat(o.args.size() == 2 ? "infix" : o.postfix ? "postfix" : "prefix", db.strings.text(o.op)));
    } else if(db.is<n_chain>(n))
      for(symbol op: db.as<n_chain>(n).ops) db.strings.intern(cat("infix", db.strings.text(op)));
  }
}

} // namespace tz
