// RA-1b: hand-written expectations for every real form of a rule, independent of the model.
#include "vish/ast.hpp"
#include "vish/rdeps_oracle.hpp"
#include <cassert>
#include "rdeps_impact_reference.hpp"
#include <chrono>
using namespace vish;
int main() {
  for(int category = 0; category < 8; ++category) for(bool nested: {false, true}) {
    ast_db db;
    auto sym = [&](char const * s) { return db.strings.intern(s); };
    auto before = db.add(n_int{1}, {});
    auto after = db.add(n_var{.name = sym("following")}, {});
    n_fn f; f.name = sym("later");
    auto later = db.add(f, {});
    n_block child; child.items = {later};
    auto block = db.add(child, {});
    node_kind k;
    if(category < 3) {
      f.name = sym(category == 0 ? "syntax_rule" : category == 1 ? "infix+" : "ordinary_shadow");
      if(category == 0) f.form = {f.name};
      k = f;
    } else if(category == 3) k = n_use{.key = sym("dep")};
    else if(category == 4) k = n_extend{.name = sym("E")};
    else if(category == 5) k = n_ct{};
    else if(category == 6) k = n_var{.name = sym("state"), .ct = true};
    else { f.name = sym("infix-"); f.scoped = true; k = f; }
    auto rule = db.add(k, {});
    n_block inner; inner.items = {before, rule, after, block};
    n_module mod; mod.items = inner.items;
    auto context = nested ? db.add(inner, {}) : db.add(mod, {});
    node_id root = context;
    if(nested) { n_module outer; outer.items = {context}; root = db.add(outer, {}); }
    db.link_parents(root);
    auto check = [&]() {
      assert(db.rule_suffix(rule) == (node_list{after, block}));
      auto impact = db.impact_of(node_list{rule});
      assert(std::ranges::contains(impact, after));
      assert(std::ranges::contains(impact, block));
      assert(std::ranges::contains(impact, later));
      assert(!std::ranges::contains(impact, before));
      assert(impact == rdeps_oracle::model(db).impact(node_list{rule}));
      assert(rdeps_oracle::compare_index(db).empty());
      rdeps_oracle::impact_reference original(db);
      rdeps_oracle::model fast(db);
      for(uint32_t id = 0; id < db.size(); ++id) {
        node_list delta{node_id(id)};
        assert(original.impact(delta) == fast.impact(delta));
        assert(db.impact_of(delta) == fast.impact(delta));
      }
      for(node_list delta: {node_list{}, node_list{before, rule, after}, node_list{rule, rule}})
        assert(original.impact(delta) == fast.impact(delta));
    };
    check();
    auto snapshot = rdeps_oracle::query_snapshot(db, {rule, context});
    db.mutate(rule, n_int{0}, 100);
    assert(db.rule_suffix(rule).empty());
    db.rollback(100); check();
    assert(snapshot == rdeps_oracle::query_snapshot(db, {rule, context}));
    // A deletion and an insertion in the middle of a context; the delta is taken before and after respectively.
    auto replacement = db.copy_kind(context);
    std::visit([&](auto & c) {
      using K = std::remove_cvref_t<decltype(c)>;
      if constexpr(std::is_same_v<K, n_module> || std::is_same_v<K, n_block>) c.items = {before, after, block};
    }, replacement);
    db.mutate(context, replacement, 101); db.link_parents(root);
    db.rollback(101); check();
    auto added = db.add(k, {});
    replacement = db.copy_kind(context);
    std::visit([&](auto & c) {
      using K = std::remove_cvref_t<decltype(c)>;
      if constexpr(std::is_same_v<K, n_module> || std::is_same_v<K, n_block>) c.items = {before, added, rule, after, block};
    }, replacement);
    db.mutate(context, replacement, 102); db.link_parents(root);
    assert(std::ranges::contains(db.impact_of(node_list{added}), later));
    assert(db.impact_of(node_list{added}) == rdeps_oracle::model(db).impact(node_list{added}));
    db.rollback(102); check();
    ast_db replay; replay.replay(db.journal());
    assert(replay.impact_of(node_list{rule}) == db.impact_of(node_list{rule}));
    assert(replay.rdeps_dump() == db.rdeps_dump());
  }
  { // Cost localization on an independent, ordered graph; exact answers retained.
    ast_db db;
    n_module module;
    for(int i = 0; i < 120; ++i) {
      n_fn f; f.name = db.strings.intern(cat("f", i));
      if(i) f.body = db.add(n_ident{db.strings.intern(cat("f", i - 1))}, {});
      module.items.push_back(db.add(f, {}));
    }
    auto root = db.add(module, {}); db.link_parents(root);
    auto run = [&](auto const & model, char const * label) {
      auto start = std::chrono::steady_clock::now();
      uint64_t total = 0;
      for(auto f: module.items) total += model.impact(node_list{f}).size();
      double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      fprintf(stderr, "%s: %.3f ms, answers %lu\n", label, ms, total);
      return total;
    };
    auto old = run(rdeps_oracle::impact_reference(db), "original oracle");
    assert(old == run(rdeps_oracle::model(db), "fast oracle"));
    struct live { ast_db const & db; node_list impact(node_list const & d) const { return db.impact_of(d); } };
    assert(old == run(live{db}, "live impact"));
  }
  fprintf(stderr, "RA-1b: 8 категорий × 2 области; изменение/удаление/вставка/rollback/replay OK\n");
}
