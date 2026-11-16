// RA-1: independent expected answers (written by hand, not from the index), not only the equality of two dumps.
// Each block is a reproduction of a review defect: B1 (late interning of .field), B2 (ref), B3 (operators), B4 (inheritance).
// A build with -DVISH_RA1_MUTANT_NO_REF / -DVISH_RA1_MUTANT_NO_MEMBER must fail (a check of the oracle itself).
#include "vish/ast.hpp"
#include "vish/rdeps_oracle.hpp"
#include <cassert>
using namespace vish;
namespace ro = vish::rdeps_oracle;

// The index, the reference model and the query answers agree: an empty report means no mismatches.
static void agree(ast_db & db, node_list probes) {
  assert(db.rdeps_dump() == db.rdeps_dump(true));
  auto report = ro::compare_index(db);
  if(!report.empty()) fprintf(stderr, "%s", report.c_str());
  assert(report.empty());
  ro::model m(db);
  std::string q;
  int bad = ro::compare_queries(db, m, probes, q);
  if(bad) fprintf(stderr, "%s", q.c_str());
  assert(bad == 0);
}

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
  agree(db, {fn, gn, root});
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
  assert(!db[root].parent); // the mutation does not touch the parents of detached children: they are fixed by the caller's link_parents (the database behaviour did not change)
  assert(db.rdeps_dump() == db.rdeps_dump(true));
  db.rollback(root.value + 1);
  assert(db[gn].parent == root);
  assert(db.rdeps_dump() == before);

  // B1. .f is a use of name f without a child ident; a later interning of the name must NOT change the index (I1).
  n_op member; member.op = sym(".f"); member.postfix = true; member.args = {f_use};
  auto field = db.add(member, {});
  assert(std::ranges::contains(db.users_of(fn), field));
  {
    // `.fresh` was added BEFORE fresh was interned: a recount from scratch must give the same index, and users_of(fresh) must find the node.
    n_op late; late.op = sym(".ra1_fresh"); late.postfix = true; late.args = {f_use};
    auto late_node = db.add(late, {});
    auto live = db.rdeps_dump();
    auto fresh = sym("ra1_fresh"); // interned later than the node
    assert(db.rdeps_dump() == live);
    assert(db.rdeps_dump() == db.rdeps_dump(true));
    assert(db.users_of(fresh) == node_list{late_node});
    assert(ro::compare_index(db).empty());
    // a name that is not in the database: nothing is created and the answer is empty
    uint32_t symbols = db.strings.size();
    assert(db.users_of(sym("ra1_never_used")).empty());
    assert(db.strings.size() == symbols + 1); // only sym() itself interned; users_of added nothing
  }

  // B2. ref(f): a use by the target's number, not by name (renaming the target does not go stale).
  n_ref r; r.target = fn.value;
  auto ref_node = db.add(r, {});
  n_fn via; via.name = sym("via"); via.body = ref_node;
  auto via_fn = db.add(via, {});
  n_module m2; m2.name = sym("m2"); m2.items = {via_fn};
  auto root2 = db.add(m2, {});
  db.link_parents(root2);
  assert(std::ranges::contains(db.users_of(fn), ref_node));
  assert(std::ranges::contains(db.dependents_of_fn(fn), via_fn));
  assert(std::ranges::contains(db.impact_of(node_list{fn}), via_fn));
  {
    auto old = db.copy_of<n_fn>(fn);
    auto renamed = old; renamed.name = sym("f_renamed");
    db.mutate(fn, renamed, root.value + 1);
    assert(std::ranges::contains(db.dependents_of_fn(fn), via_fn)); // by reference, the target's name does not matter
    db.rollback(root.value + 1);
  }
  agree(db, {fn, gn, via_fn, root, root2});

  // B3. An operator: the declaration infix+ finds the uses of `+` (n_op and n_chain), of any role and arity.
  n_ident a_use{sym("a")}, b_use{sym("b")};
  auto a_node = db.add(a_use, {}), b_node = db.add(b_use, {});
  n_op plus; plus.op = sym("+"); plus.args = {a_node, b_node};
  auto plus_node = db.add(plus, {});
  n_fn user; user.name = sym("plus_user"); user.body = plus_node;
  auto user_fn = db.add(user, {});
  n_chain chain; chain.atoms = {a_node, b_node}; chain.ops = {sym("+")};
  auto chain_node = db.add(chain, {});
  n_fn chain_user; chain_user.name = sym("chain_user"); chain_user.body = chain_node;
  auto chain_fn = db.add(chain_user, {});
  n_op minus; minus.op = sym("-"); minus.args = {a_node}; // the prefix -a
  auto minus_node = db.add(minus, {});
  n_fn neg_user; neg_user.name = sym("neg_user"); neg_user.body = minus_node;
  auto neg_fn = db.add(neg_user, {});
  n_fn infix_plus; infix_plus.name = sym("infix+");
  auto infix_plus_fn = db.add(infix_plus, {});
  n_fn prefix_minus; prefix_minus.name = sym("prefix-");
  auto prefix_minus_fn = db.add(prefix_minus, {});
  n_module m3; m3.name = sym("m3"); m3.items = {user_fn, chain_fn, neg_fn, infix_plus_fn, prefix_minus_fn};
  auto root3 = db.add(m3, {});
  db.link_parents(root3);
  assert(db.users_of(infix_plus_fn) == (node_list{plus_node, chain_node}));
  assert((db.dependents_of_fn(infix_plus_fn) == node_list{user_fn, chain_fn}));
  assert(db.users_of(prefix_minus_fn) == node_list{minus_node}); // conservatively: the role and the arity are not distinguished
  assert(std::ranges::contains(db.impact_of(node_list{infix_plus_fn}), user_fn));
  agree(db, {infix_plus_fn, prefix_minus_fn, user_fn, root3});

  // B4. Inheritance: a change to Base affects the consumers of Derived (and the descendant of the descendant).
  auto sym_base = sym("Base"), sym_derived = sym("Derived"), sym_leaf = sym("Leaf");
  n_extend base; base.name = sym_base; base.of = sym("struct");
  auto base_node = db.add(base, {});
  n_extend derived; derived.name = sym_derived; derived.of = sym("struct"); derived.base = sym_base;
  auto derived_node = db.add(derived, {});
  n_extend leaf; leaf.name = sym_leaf; leaf.of = sym("struct"); leaf.base = sym_derived;
  auto leaf_node = db.add(leaf, {});
  auto consume_type = db.add(n_ident{sym_derived}, {});
  n_param consume_param; consume_param.name = sym("d"); consume_param.type = consume_type;
  auto consume_p = db.add(consume_param, {});
  n_fn consume; consume.name = sym("consume"); consume.params = {consume_p};
  auto consume_fn = db.add(consume, {});
  auto leaf_type = db.add(n_ident{sym_leaf}, {});
  n_param leaf_param; leaf_param.name = sym("l"); leaf_param.type = leaf_type;
  auto leaf_p = db.add(leaf_param, {});
  n_fn eat; eat.name = sym("eat_leaf"); eat.params = {leaf_p};
  auto eat_fn = db.add(eat, {});
  n_fn unrelated; unrelated.name = sym("unrelated");
  auto unrelated_fn = db.add(unrelated, {});
  n_module m4; m4.name = sym("m4"); m4.items = {base_node, derived_node, leaf_node, consume_fn, eat_fn, unrelated_fn};
  auto root4 = db.add(m4, {});
  db.link_parents(root4);
  auto base_impact = db.impact_of(node_list{base_node});
  assert(std::ranges::contains(base_impact, consume_fn));
  assert(std::ranges::contains(base_impact, eat_fn)); // through Derived -> Leaf
  assert(!std::ranges::contains(base_impact, unrelated_fn));
  auto leaf_impact = db.impact_of(node_list{leaf_node});
  assert(std::ranges::contains(leaf_impact, eat_fn) && !std::ranges::contains(leaf_impact, consume_fn)); // the dependency does not go up the bases
  agree(db, {base_node, derived_node, leaf_node, consume_fn, eat_fn, root4});

  // destroy/copy/implicit are called implicitly (by the value's type): all the functions of the program are dependent, but not those of the prelude.
  n_fn hook; hook.name = sym("destroy");
  auto hook_fn = db.add(hook, {});
  n_module m5; m5.name = sym("m5"); m5.items = {hook_fn};
  auto root5 = db.add(m5, {});
  db.link_parents(root5);
  auto hook_dependents = db.dependents_of_fn(hook_fn);
  assert(std::ranges::contains(hook_dependents, consume_fn) && std::ranges::contains(hook_dependents, user_fn));
  n_fn conv; conv.name = sym("implicit");
  auto conv_fn = db.add(conv, {});
  n_module m6; m6.name = sym("m6"); m6.items = {conv_fn};
  auto root6 = db.add(m6, {});
  db.link_parents(root6);
  assert(std::ranges::contains(db.dependents_of_fn(conv_fn), consume_fn));
  agree(db, {hook_fn, conv_fn});

  // use: a list of names - uses of names.
  n_use use; use.key = sym("geo"); use.names = {sym("norm_fn")};
  auto use_node = db.add(use, {});
  assert(db.users_of(sym("norm_fn")) == node_list{use_node});
  assert(ro::compare_index(db).empty());

  // The oracle sees an omission: we delete an entry from the live index - the reference is not a subset of the index (a check of the oracle itself).
  {
    auto victim = db.refs_by_name[f_name.value];
    db.refs_by_name[f_name.value].clear();
    std::size_t missing = 0;
    auto report = ro::compare_index(db, ~uint32_t{0}, &missing);
    assert(missing >= 1 && !report.empty());
    db.refs_by_name[f_name.value] = victim;
    assert(ro::compare_index(db).empty());
  }

  ast_db replay; replay.replay(db.journal());
  assert(replay.rdeps_dump() == db.rdeps_dump());
  assert(ro::compare_index(replay).empty());
}
