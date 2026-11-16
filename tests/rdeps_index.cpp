// RA-1: independent expected answers, not only the equality of two dumps.
#include "vish/ast.hpp"
#include <cassert>
using namespace vish;
int main() {
  ast_db db;
  auto sym = [&](char const * s) { return db.strings.intern(s); };
  auto f_name = sym("f"), g_name = sym("g");
  auto f_use = db.add(n_ident{f_name}, {});
  auto g_use = db.add(n_ident{g_name}, {});
  n_fn f; f.name = f_name; f.body = g_use;
  n_fn g; g.name = g_name; g.body = f_use;
  auto fn = db.add(f, {}), gn = db.add(g, {});
  n_module m; m.name = sym("m"); m.items = {fn, gn};
  auto root = db.add(m, {});
  db.link_parents(root);
  assert(db.enclosing_fn(f_use) == gn);
  assert(db.users_of(fn) == node_list{f_use});
  assert(db.dependents_of_fn(fn) == node_list{gn});
  node_list changed_nodes{fn};
  auto impact = db.impact_of(changed_nodes);
  assert(impact == (node_list{fn, gn, root})); // the loop terminates
  auto before = db.rdeps_dump();
  auto rev = db.node_revision(gn);
  db.mutate(f_use, n_ident{sym("other")}, root.value + 1);
  assert(db.users_of(fn).empty());
  assert(db.rdeps_dump() == db.rdeps_dump(true));
  db.rollback(root.value + 1);
  assert(db.rdeps_dump() == before);
  assert(db.node_revision(gn) > rev);
  auto body = db.copy_of<n_module>(root);
  body.items.pop_back();
  db.mutate(root, body, root.value + 1);
  assert(!db[root].parent && !db[gn].parent);
  db.rollback(root.value + 1);
  assert(db[gn].parent == root);
  assert(db.rdeps_dump() == before);
  // .f is a use of the same name f, even without a child ident.
  n_op member; member.op = sym(".f"); member.postfix = true; member.args = {f_use};
  auto field = db.add(member, {});
  assert(std::ranges::contains(db.users_of(fn), field));
  ast_db replay; replay.replay(db.journal());
  assert(replay.rdeps_dump() == db.rdeps_dump());
}
