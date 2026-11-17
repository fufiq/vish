// A VM regression: a wrong shape of a rule result gives a diagnostic at the call site.
#include "vish/base.hpp"
#include "vish/os.hpp"
#include "vish/lexer.hpp"
#include "vish/ast.hpp"
#include "vish/rdeps_oracle.hpp"
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
// Wrong rule results must keep the call site and the diagnostic's owner.
int main() {
  struct sample { char const * rule; char const * result; char const * message; };
  sample cases[] = {
    {"overload_pick", "99", "ожидалась запись"},
    {"overload_pick", "{order: 1, reach: 0, ambiguous: false, tied: 0}", "неверные типы"},
    {"overload_pick", "{order: array{}, reach: false, ambiguous: false, tied: 0}", "неверные типы"},
    {"overload_pick", "{order: array{}, reach: 0, ambiguous: 1, tied: 0}", "неверные типы"},
    {"overload_pick", "{order: array{}, reach: 0, ambiguous: false, tied: false}", "неверные типы"},
    {"overload_pick", "{order: array{false}, reach: 0, ambiguous: false, tied: 0}", "order должен"},
    {"chain_resolve", "99", "ожидалась запись"},
    {"chain_resolve", "{status: false, tree: array{}, notes: array{}}", "неверные типы"},
    {"chain_resolve", "{status: 0, tree: 1, notes: array{}}", "неверные типы"},
    {"chain_resolve", "{status: 0, tree: array{}, notes: 1}", "неверные типы"},
    {"chain_resolve", "{status: 0, tree: array{false}, notes: array{}}", "tree должен"},
    {"chain_resolve", "{status: 3, tree: array{}, notes: array{false}}", "notes должен"},
  };
  for(auto const & c: cases) {
    ast_db db; load_prelude(db);
    parser p(db, cat("replacement() => ", c.result, "; f(i64 x) => x; f(x) => x; main() => ",
      std::string_view(c.rule) == "overload_pick" ? "f(1);" : "1 % 2 == 0;"), "rule_results.vish");
    auto root = p.parse_module();
    assert(p.errors().empty());
    node_id replacement, main, rule;
    for(uint32_t i = 0; i < db.size(); ++i) if(db.is<n_fn>(node_id(i))) {
      auto name = db.strings.text(db.as<n_fn>(node_id(i)).name);
      if(name == "replacement") replacement = node_id(i);
      if(name == "main") main = node_id(i);
      if(name == c.rule) rule = node_id(i);
    }
    assert(rule && main && replacement);
    auto f = db.copy_of<n_fn>(rule);
    f.body = db.as<n_fn>(replacement).body; f.expr_body = true;
    db.replace(rule, f); db.link_parents(rule);
    bool caught = false;
    try { interpreter lang(db, root, {"rule_results.vish"}, mode::check); lang.run_main(); }
    catch(diagnostic const & d) {
      fprintf(stderr, "%s: %s owner=%u main=%u\n", c.rule, d.what(), rdeps_oracle::diagnostic_owner(db, d.where).value, main.value);
      assert(std::string_view(d.what()).find(c.rule) != std::string_view::npos);
      assert(std::string_view(d.what()).find(c.message) != std::string_view::npos);
      assert(rdeps_oracle::diagnostic_owner(db, d.where) == main);
      caught = true;
    }
    assert(caught);
  }
  std::puts("12/12 rule diagnostics with call owner");
}
