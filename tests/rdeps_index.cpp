// RA-1: independent expected answers (written by hand, not from the index), not only the equality of two dumps.
// Each block is a reproduction of a review defect: B1 (late interning of .field), B2 (ref), B3 (operators), B4 (inheritance).
// B5 (the second review cycle): the core's implicit calls - the table ast_db::implicit_hooks, the rule "all the program's functions", a comparison of the table with the core sources.
// A build with -DVISH_RA1_MUTANT_NO_REF / _NO_MEMBER / _NO_HOOKS must fail (a check of the oracle itself).
#include "vish/ast.hpp"
#include "vish/rdeps_oracle.hpp"
#include <cassert>
#include <fstream>
#include <regex>
#include <set>
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
  { // B6: main -> h -> g -> f; removing the call of h keeps f/g in the union of impact before/after.
    ast_db graph;
    auto add_fn = [&](char const * name, node_id body) {
      n_fn f; f.name = graph.strings.intern(name); f.body = body;
      return graph.add(f, {});
    };
    auto leaf = graph.add(n_int{1}, {});
    auto f = add_fn("f", leaf);
    auto fu = graph.add(n_ident{graph.strings.intern("f")}, {});
    auto g = add_fn("g", fu);
    auto gu = graph.add(n_ident{graph.strings.intern("g")}, {});
    auto h = add_fn("h", gu);
    auto hu = graph.add(n_ident{graph.strings.intern("h")}, {});
    auto main = add_fn("main", hu);
    auto isolated = add_fn("isolated", graph.add(n_int{7}, {}));
    n_module m; m.items = {isolated, f, g, h, main};
    auto root = graph.add(m, {}); graph.link_parents(root);
    auto expected = node_list{f, g, h, main, root};
    // A hand-written expectation checks the downward direction and the absence of an unrelated function.
    assert(graph.impact_of(node_list{hu}) == expected);
    assert(ro::model(graph).impact(node_list{hu}) == expected);
    auto before = graph.impact_of(node_list{hu});
    graph.mutate(hu, n_int{0}, 999);
    auto after = graph.impact_of(node_list{hu});
    assert(!std::ranges::contains(after, f));
    for(auto n: after) before.push_back(n);
    ast_db::unique_nodes(before);
    assert(before == expected);
    agree(graph, {f, g, h, main, root});
    graph.rollback(999);
    assert(graph.impact_of(node_list{hu}) == expected);
    ast_db replay; replay.replay(graph.journal());
    assert(replay.impact_of(node_list{hu}) == expected);
    // Arbitrary ct reflection requires coverage and unrelated functions.
    auto ct = graph.add(n_ct{graph.add(n_int{0}, {})}, {});
    auto module = graph.copy_of<n_module>(root); module.items.push_back(ct);
    graph.replace(root, module); graph.link_parents(root);
    assert(std::ranges::contains(graph.impact_of(node_list{hu}), isolated));
    agree(graph, {hu, isolated, root});
  }
  { // A module-level ct diagnostic: its own origin, without substituting main.
    ast_db d; d.pinned_source = 7;
    auto expr = d.add(n_ident{d.strings.intern("helper")}, {20, 26, 7});
    auto ct = d.add(n_ct{expr}, {15, 30, 7});
    n_fn f; f.name = d.strings.intern("helper");
    auto helper = d.add(f, {1, 10, 7});
    n_module m; m.items = {helper, ct};
    auto module = d.add(m, {0, 40, 7}); d.link_parents(module);
    auto owner = ro::diagnostic_owner(d, {20, 26, 7});
    assert(owner == module);
    assert(ro::diagnostic_owner(d, {2, 4, 7}) == helper);
    assert(!ro::diagnostic_owner(d, {}));
    assert(!ro::diagnostic_owner(d, {20, 26, 8}));
    auto impact = d.impact_of(node_list{helper});
    assert(ro::diagnostic_covered(d, node_list{owner}, impact));
    assert(!ro::diagnostic_covered(d, node_list{helper, module}, node_list{module}));
    assert(ro::diagnostic_covered(d, node_list{helper, module}, node_list{helper}));
    std::erase(impact, module); // a negative check: functions do not replace the module
    assert(!ro::diagnostic_covered(d, node_list{owner}, impact));
    assert(!ro::diagnostic_covered(d, {}, node_list{module}));
  }
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
  n_module m4; m4.name = sym("m4"); m4.items = {unrelated_fn, base_node, derived_node, consume_fn, leaf_node, eat_fn};
  auto root4 = db.add(m4, {});
  db.link_parents(root4);
  auto base_impact = db.impact_of(node_list{base_node});
  assert(std::ranges::contains(base_impact, consume_fn));
  assert(std::ranges::contains(base_impact, eat_fn)); // through Derived -> Leaf
  assert(!std::ranges::contains(base_impact, unrelated_fn)); // stands before the rule
  auto leaf_impact = db.impact_of(node_list{leaf_node});
  assert(std::ranges::contains(leaf_impact, eat_fn) && !std::ranges::contains(leaf_impact, consume_fn)); // the dependency does not go up the bases
  agree(db, {base_node, derived_node, leaf_node, consume_fn, eat_fn, root4});

  // B5. The core's implicit calls (ast_db::implicit_hooks): all the program's functions are dependent, but not the prelude's.
  n_fn prel_user; prel_user.name = sym("prelude_user");
  auto prel_user_fn = db.add(prel_user, {});
  n_module prel; prel.name = sym("prelude"); prel.items = {prel_user_fn};
  auto prel_root = db.add(prel, {});
  db.link_parents(prel_root); // the name "prelude" makes a module the database's prelude
  assert(db.prelude == prel_root && db.in_prelude(prel_user_fn));
  std::vector<std::pair<std::string, node_id>> hook_fns;
  for(auto const & h: ast_db::implicit_hooks) {
    if(h.name.empty()) continue;
    n_fn hook; hook.name = sym(std::string(h.name).c_str());
    auto hook_fn = db.add(hook, {});
    n_module hm; hm.name = sym((std::string("hm_") + std::string(h.name)).c_str()); hm.items = {hook_fn};
    auto hroot = db.add(hm, {});
    db.link_parents(hroot);
    hook_fns.push_back({std::string(h.name), hook_fn});
  }
  assert(hook_fns.size() == 12); // destroy copy implicit text ref_taken ref_used eval + rules in the language: fit_rule join_rule analysis_limit chain_resolve overload_pick
  for(auto const & [name, f]: hook_fns) {
    auto dep = db.dependents_of_fn(f);
    bool is_eval = name == "eval"; // eval outside a declaration of kind ast.X is an ordinary function, not a kind member
    if(is_eval) assert(!db.is_implicit_hook(f) && !std::ranges::contains(dep, consume_fn));
    else {
      assert(db.is_implicit_hook(f));
      assert(std::ranges::contains(dep, consume_fn) && std::ranges::contains(dep, user_fn) && std::ranges::contains(dep, eat_fn)); // the program's functions
      assert(!std::ranges::contains(dep, prel_user_fn)); // the prelude - no
      assert(std::ranges::contains(db.impact_of(node_list{f}), consume_fn));
    }
  }
  { // eval is a kind member: inside an n_extend named ast.X
    n_fn ev; ev.name = sym("eval");
    auto ev_fn = db.add(ev, {});
    n_extend kind_ext; kind_ext.name = sym("ast.twice"); kind_ext.of = sym("struct"); kind_ext.items = {ev_fn};
    auto kind_node = db.add(kind_ext, {});
    n_module km; km.name = sym("km"); km.items = {kind_node};
    auto kroot = db.add(km, {});
    db.link_parents(kroot);
    assert(db.is_implicit_hook(ev_fn));
    assert(std::ranges::contains(db.dependents_of_fn(ev_fn), consume_fn));
    assert(std::ranges::contains(db.impact_of(node_list{ev_fn}), user_fn));
    agree(db, {ev_fn, kind_node, kroot});
  }
  { // [[transform]] is a function with an attribute: called by the core during elaboration on the nodes of all the program's modules
    auto attr = db.add(n_ident{sym("transform")}, {});
    n_fn tr; tr.name = sym("my_rewrite"); tr.attrs = {attr};
    auto tr_fn = db.add(tr, {});
    n_module tm; tm.name = sym("tm"); tm.items = {tr_fn};
    auto troot = db.add(tm, {});
    db.link_parents(troot);
    assert(db.is_implicit_hook(tr_fn));
    assert(std::ranges::contains(db.dependents_of_fn(tr_fn), consume_fn) && std::ranges::contains(db.dependents_of_fn(tr_fn), user_fn));
    assert(!std::ranges::contains(db.dependents_of_fn(tr_fn), prel_user_fn));
    assert(!db.is_implicit_hook(consume_fn));
    agree(db, {tr_fn, troot});
  }
  agree(db, {hook_fns[0].second, hook_fns[4].second, hook_fns[5].second});

  // A field (n_var) is a declaration: users_of(field) finds `.field` (a query by a field's declaration must not lose the n_var branch).
  {
    n_var field; field.name = sym("ra1_field");
    auto field_node = db.add(field, {});
    n_op access; access.op = sym(".ra1_field"); access.postfix = true; access.args = {f_use};
    auto access_node = db.add(access, {});
    n_fn field_user; field_user.name = sym("field_user"); field_user.body = access_node;
    auto field_user_fn = db.add(field_user, {});
    n_extend holder; holder.name = sym("FieldHolder"); holder.of = sym("struct"); holder.items = {field_node};
    auto holder_node = db.add(holder, {});
    n_module fm; fm.name = sym("fm"); fm.items = {holder_node, field_user_fn};
    auto froot = db.add(fm, {});
    db.link_parents(froot);
    assert(db.users_of(field_node) == node_list{access_node});
    assert(std::ranges::contains(db.dependents_of_fn(field_node), field_user_fn));
    agree(db, {field_node, holder_node, field_user_fn});
  }
  // A mutation and a rollback of a node that was not detached but never attached (parent is empty): the index follows its kind.
  {
    auto never = db.add(n_ident{sym("ra1_never_attached")}, {});
    assert(!db[never].parent);
    db.mutate(never, n_ident{sym("ra1_never_other")}, root.value + 3000);
    assert(db.users_of(sym("ra1_never_attached")).empty() && !db.users_of(sym("ra1_never_other")).empty());
    assert(ro::compare_index(db).empty());
    db.rollback(root.value + 3000);
    assert(!db.users_of(sym("ra1_never_attached")).empty() && db.users_of(sym("ra1_never_other")).empty());
    assert(ro::compare_index(db).empty());
  }

  // A use outside a function: an extension's field type and a module's global variable/ct block are not lost.
  {
    auto field_type = db.add(n_ident{sym("Inner")}, {});
    n_var field; field.name = sym("inner"); field.type = field_type;
    auto field_node = db.add(field, {});
    n_extend outer; outer.name = sym("Outer"); outer.of = sym("struct"); outer.items = {field_node};
    auto outer_node = db.add(outer, {});
    n_extend inner; inner.name = sym("Inner"); inner.of = sym("struct");
    auto inner_node = db.add(inner, {});
    auto outer_type = db.add(n_ident{sym("Outer")}, {});
    n_param op_param; op_param.name = sym("o"); op_param.type = outer_type;
    auto op_param_node = db.add(op_param, {});
    n_fn take_outer; take_outer.name = sym("take_outer"); take_outer.params = {op_param_node};
    auto take_outer_fn = db.add(take_outer, {});
    auto global_use = db.add(n_ident{sym("take_outer")}, {}); // a module's ct block/global variable calls take_outer
    n_var global; global.name = sym("g_state"); global.init = global_use;
    auto global_node = db.add(global, {});
    n_fn other_fn_k; other_fn_k.name = sym("module_mate");
    auto module_mate = db.add(other_fn_k, {});
    n_module nm; nm.name = sym("nested_m"); nm.items = {inner_node, outer_node, take_outer_fn, global_node, module_mate};
    auto nroot = db.add(nm, {});
    db.link_parents(nroot);
    assert(std::ranges::contains(db.impact_of(node_list{inner_node}), take_outer_fn)); // Inner -> a field of Outer -> a consumer of Outer
    assert(std::ranges::contains(db.dependents_of_fn(take_outer_fn), module_mate)); // a use outside functions: the module's functions
    agree(db, {inner_node, outer_node, take_outer_fn, nroot});
  }

  // A comparison of the table with the core sources: every name that the core looks up in functions/member_fn_of/hook symbols or compares with a function name
  // must be in the table or in the list of explicit forms below (a name from the node's text is in the index anyway). A new implicit call in the core without
  // an entry in the table breaks this test instead of passing silently.
  {
    std::set<std::string> found;
    std::regex member_fn(R"re(member_fn_of\("([a-z_]+)")re");
    std::regex find_sym(R"re(functions\.(?:find|contains|at)\(.*?sym\("([^"]+)"\)\))re");
    std::regex hook_sym(R"re(symbol h = sym\("([a-z_]+)"\))re");
    std::regex fn_name_eq(R"re(n_fn>\(.*?\)\.name\) == "([a-z_]+)")re"); // iterating the database's functions by name: eval, chain_resolve, overload_pick
    std::regex lang_rule_call(R"re(lang_rule\(\w+, "([a-z_]+)")re");      // fit_rule, join_rule, analysis_limit
    std::regex attr_eq(R"re(n_ident>\(a\)\.name\) == "([a-z_]+)")re");   // a mark attribute: transform, service
    for(char const * file: {"include/vish/interp.hpp", "include/vish/machine.hpp"}) {
      std::ifstream in(file);
      assert(in && "запускать из корня репозитория");
      std::string line;
      while(std::getline(in, line)) {
        std::smatch m;
        for(auto const * re: {&member_fn, &find_sym, &hook_sym, &fn_name_eq, &lang_rule_call, &attr_eq})
          for(auto it = std::sregex_iterator(line.begin(), line.end(), *re); it != std::sregex_iterator(); ++it) found.insert((*it)[1]);
      }
    }
    std::set<std::string> allowed{"main", "service"}; // main is the program's entry (no dependents); service is a mark attribute (it calls no functions)
    for(auto const & h: ast_db::implicit_hooks) if(!h.name.empty()) allowed.insert(std::string(h.name));
    allowed.insert("transform"); // the [[transform]] attribute is a mark in is_implicit_hook
    for(auto const & name: found)
      if(!allowed.contains(name)) fprintf(stderr, "неявный вызов в ядре без записи в ast_db::implicit_hooks: %s\n", name.c_str());
    for(auto const & name: found) assert(allowed.contains(name));
    for(auto const & h: ast_db::implicit_hooks) if(!h.name.empty()) assert(found.contains(std::string(h.name))); // and back: the table entry exists in the core
    assert(found.contains("transform") && found.contains("main"));
  }

  // The full I3 for a free node without the harness restoring the links.
  {
    auto free_node = db.add(n_ident{sym("ra1_free")}, {});
    auto before_tables = db.rdeps_dump();
    auto before_queries = ro::query_snapshot(db, {root4, free_node, consume_fn});
    auto module_body = db.copy_of<n_module>(root4);
    module_body.items.push_back(free_node);
    db.mutate(root4, module_body, root.value + 2000);
    db.link_parents(root4);
    assert(db[free_node].parent == root4 && !db.users_of(sym("ra1_free")).empty());
    db.rollback(root.value + 2000);
    assert(!db[free_node].parent); // the former free node is free again
    assert(db.rdeps_dump() == before_tables);
    assert(ro::query_snapshot(db, {root4, free_node, consume_fn}) == before_queries);
    assert(db.rdeps_dump() == db.rdeps_dump(true) && ro::compare_index(db).empty());
    // a mutation and a rollback of the detached node itself (after detaching it stays in the database): the index follows the node's kind
    db.mutate(free_node, n_ident{sym("ra1_free_other")}, root.value + 2001);
    assert(db.users_of(sym("ra1_free")).empty() && !db.users_of(sym("ra1_free_other")).empty());
    assert(ro::compare_index(db).empty());
    db.rollback(root.value + 2001);
    assert(!db.users_of(sym("ra1_free")).empty() && db.users_of(sym("ra1_free_other")).empty());
    assert(db.rdeps_dump() == before_tables && ro::compare_index(db).empty());
  }

  // Moving an already attached subtree: a rollback from the middle and a replay must restore the parents exactly.
  {
    ast_db d;
    auto leaf = d.add(n_ident{d.strings.intern("leaf")}, {});
    n_fn a; a.name = d.strings.intern("a"); a.body = leaf;
    auto left = d.add(a, {});
    n_fn b; b.name = d.strings.intern("b");
    auto right = d.add(b, {});
    d.link_parents(left);
    auto before = d.rdeps_dump();
    b.body = leaf;
    d.mutate(right, b, 100);
    d.link_parents(right);
    assert(d[leaf].parent == right);
    auto middle = d.rdeps_dump();
    d.mutate(leaf, n_ident{d.strings.intern("other")}, 101);
    d.rollback(101);
    assert(d.rdeps_dump() == middle);
    d.rollback(100);
    assert(d[leaf].parent == left && d.rdeps_dump() == before);
    ast_db replay;
    replay.replay(d.journal());
    assert(replay.rdeps_dump() == d.rdeps_dump());
    assert(replay[leaf].parent == left);
    db.use_pool();
  }

  // A derived node (derive): a node taken anew gets its kind at birth - the index follows it, not the completed kind.
  {
    std::vector<uint32_t> holder;
    auto born = db.derive(n_ident{sym("ra1_born")}, span{}, holder);
    db.mutate(born, n_ident{sym("ra1_built")}, 0); // completion (no source): the former kind - not in the mutations
    assert(ro::compare_index(db).empty() && !db.users_of(sym("ra1_built")).empty());
    db.release(holder);
    std::vector<uint32_t> again;
    auto reborn = db.derive(n_ident{sym("ra1_born")}, span{}, again); // the same kind and place - the same node, the kind at birth
    assert(reborn == born);
    assert(db.rdeps_dump() == db.rdeps_dump(true));
    auto report = ro::compare_index(db);
    if(!report.empty()) fprintf(stderr, "%s", report.c_str());
    assert(report.empty());
    assert(!db.users_of(sym("ra1_born")).empty() && db.users_of(sym("ra1_built")).empty());
    db.release(again);
    assert(ro::compare_index(db).empty());
  }

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
