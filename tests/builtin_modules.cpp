// Built-in base modules - from the assembly, the base with a copy of another assembly receives the
// module again, the former copy remains under the name@old; the same assembly is the same module.
// Launches check.sh.
#include "vish/base.hpp"
#include "vish/os.hpp"
#include "vish/lexer.hpp"
#include "vish/ast.hpp"
#include "vish/parser.hpp"
#include "vish/prelude.hpp"
#include "vish/printer.hpp"
#include "vish/dump.hpp"
#include "vish/interp.hpp"
#include "vish/test_runner.hpp"
#include "vish/views.hpp"
#include "vish/store.hpp"
#include "vish/session.hpp"
#include "vish/server.hpp"
#include "vish/editor_lib.hpp"
#include "vish/tools.hpp"
#include "vish/machine.hpp"
#include "vish/modules.hpp"
#include "vish/jit/jit.hpp"
#include "vish/cli.hpp"
#include "native.hpp"
#include <cassert>
#include <cstdio>
using namespace vish;
int main() {
  ast_db db;
  node_id a = load_builtin_module(db, "plug", "module plug;\nf() => 1;\n", "plug.vish");
  assert(find_module(db, db.strings.intern("plug")) == a);
  assert(load_builtin_module(db, "plug", "module plug;\nf() => 1;\n", "plug.vish") == a); // Same assembly, same module
  node_id b = load_builtin_module(db, "plug", "module plug;\nf() => 2;\n", "plug.vish"); // Another assembly - the module again
  assert(b != a && find_module(db, db.strings.intern("plug")) == b);
  assert(db.strings.text(db.as<n_module>(a).name) == "plug@старый");
  assert(load_builtin_module(db, "plug", "module plug;\nf() => 2;\n", "plug.vish") == b);
  node_id c = load_builtin_module(db, "plug", "module plug;\nf() => 1;\n", "plug.vish"); // back to the first assembly: copy b is someone else's
  assert(c != b && find_module(db, db.strings.intern("plug")) == c);
  // prelude is a copy of the build that created: native, which is not in this one, is called when opening.
  ast_db old;
  load_builtin_module(old, "prelude", "module prelude;\nlost_native(i64 x) => native \"cpp\" { return value(int64_t(0)); };\n", "prelude.vish");
  assert(old.prelude && missing_prelude_native(old).find("lost_native") == 0);
  ast_db fine;
  load_builtin_module(fine, "prelude", "module prelude;\nplain(i64 x) => x;\n", "prelude.vish");
  assert(missing_prelude_native(fine).empty());
  std::puts("ok");
}
