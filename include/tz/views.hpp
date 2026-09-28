// projection environment (§13.2 steak): residual program for inputs and slice.
//
//;
// 
//.
//
// Both projections are the same AST base and the same print with filter and substitutions,
// not a separate representation: edits in the projection are placed in the original nodes.
#pragma once

#include "interp.hpp"
#include "printer.hpp"

#include <set>

namespace tz {

// Instruction: A block/module element or a cycle/branch body that is not a block.
bool is_statement(ast_db const & db, node_id n) {
  node_id p = db[n].parent;
  if(!p) return false;
  if(db.is<n_block>(p) || db.is<n_module>(p)) return true;
  if(db.is<n_for>(p)) return db.as<n_for>(p).body == n;
  if(db.is<n_while>(p)) return db.as<n_while>(p).body == n;
  if(db.is<n_if>(p)) {
    auto const & i = db.as<n_if>(p);
    return (i.then_ == n || i.else_ == n) && is_statement(db, p);
  }
  return false;
}

// residual program -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
//
// Partial execution as a global convolution constant:
// 
// A static parsing (check) with predetermined inputs observes the values of node; known
// and pure expressions are replaced by values, branch with a known condition is folded
// into its branch, unexecuted code and functions needed only in folded form are removed.
//
struct residual_view {
  residual_view(ast_db const & db, interpreter const & vm) : db(db), vm(vm) {}

  std::string print(node_id module) {
    find_used_names(module);
    printer::substitution subst;
    subst.node = [this](node_id n) { return fold_if(n); };
    subst.text = [this](node_id n) { return fold_value(n); };
    return printer(db, [this](node_id n) { return keep(n); }, subst).print(module);
  }
  bool keep(node_id n) const {
    node_id p = db[n].parent;
    if(db.is<n_fn>(n) && p && db.is<n_module>(p)) // module
      return db.strings.text(db.as<n_fn>(n).name) == "main" || vm.live(n);
    if(!is_statement(db, n)) return true;
    auto const * o = vm.observed(n);
    if(!o || !o->present) return false; // It was not performed at these entrances.
    // dead variable: after convolution it is not read anywhere, and initialization
    // without runtime effect
    if(db.is<n_var>(n) && !used.contains(db.as<n_var>(n).name)) {
      auto const * init = vm.observed(db.as<n_var>(n).init);
      if(!init || !init->effect) return false;
    }
    if(pure_noop(n)) return false;
    if(db.is<n_if>(n)) { // if(ложь) without more - nothing remains
      auto c = cond_value(n);
      if(c && !*c && !db.as<n_if>(n).else_) return false;
    }
    return true;
  }

  // An expression instruction that does nothing at these inputs: the result is known
  // and discarded, there are no runtime effects, no variable parameters.
  bool pure_noop(node_id n) const {
    if(!db.is<n_call>(n) && !db.is<n_op>(n) && !db.is<n_ident>(n)) return false;
    if(db.is<n_op>(n)) {
      auto op = db.strings.text(db.as<n_op>(n).op);
      if(op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=") return false;
    }
    auto const * o = vm.observed(n);
    if(!o || !o->seen || o->effect || o->diverted || !o->v.known()) return false;
    if(db.is<n_call>(n) && db.is<n_ident>(db.as<n_call>(n).callee)) {
      symbol name = db.as<n_ident>(db.as<n_call>(n).callee).name;
      for(node_id item: db.as<n_module>(module_of(n)).items)
        if(db.is<n_fn>(item) && db.as<n_fn>(item).name == name)
          for(node_id p: db.as<n_fn>(item).params) {
            auto m = db.as<n_param>(p).mode;
            if(m == param_mode::mut || m == param_mode::own || m == param_mode::out) return false;
          }
    }
    return true;
  }

  node_id module_of(node_id n) const {
    while(db[n].parent) n = db[n].parent;
    return n;
  }

  std::optional<bool> cond_value(node_id if_node) const {
    auto const * o = vm.observed(db.as<n_if>(if_node).cond);
    if(!o || !o->seen || o->effect || !o->v.is<bool>()) return std::nullopt;
    return o->v.as<bool>();
  }

  node_id fold_if(node_id n) const {
    if(!db.is<n_if>(n)) return {};
    auto c = cond_value(n);
    if(!c) return {};
    return *c ? db.as<n_if>(n).then_ : db.as<n_if>(n).else_;
  }

  std::optional<std::string> fold_value(node_id n) const {
    if(is_statement(db, n) || !foldable_position(n)) return std::nullopt;
    auto const * o = vm.observed(n);
    if(!o || !o->seen || o->diverted || o->effect) return std::nullopt;
    if(db.is<n_int>(n) || db.is<n_float>(n) || db.is<n_str>(n)) return std::nullopt; // literary
    return vm.literal(o->v);
  }

  // Names that are read in residual program: identifiers that will be printed (not
  // folded, not in a discarded instruction or branch).
  void find_used_names(node_id n) {
    if(!n) return;
    if(db.is<n_fn>(n) && db[n].parent && db.is<n_module>(db[n].parent) && !keep(n)) return;
    if(is_statement(db, n) && !db.is<n_var>(n) && !db.is<n_fn>(n) && (!keep_ignoring_dead(n) || pure_noop(n))) return;
    if(fold_value(n)) return;
    if(node_id other = fold_if(n)) {
      find_used_names(other);
      return;
    }
    if(db.is<n_if>(n) && fold_if_drops(n)) return;
    if(db.is<n_ident>(n) && !is_lvalue(n)) used.insert(db.as<n_ident>(n).name);
    db.for_each_child(n, [&](node_id c) { find_used_names(c); });
  }

  bool keep_ignoring_dead(node_id n) const {
    auto const * o = vm.observed(n);
    return o && o->present;
  }
  bool fold_if_drops(node_id n) const {
    auto c = cond_value(n);
    return c && !*c && !db.as<n_if>(n).else_;
  }

  // Do not replace: the purpose of assignment, called, announcements and parameters.
  bool foldable_position(node_id n) const {
    if(db.is<n_var>(n) || db.is<n_fn>(n) || db.is<n_param>(n) || db.is<n_lambda>(n) || db.is<n_block>(n) || db.is<n_return>(n) || db.is<n_require>(n) || db.is<n_for>(n) || db.is<n_while>(n))
      return false;
    node_id p = db[n].parent;
    if(!p) return false;
    if(db.is<n_call>(p) && db.as<n_call>(p).callee == n) return false;
    if(db.is<n_var>(p) && db.as<n_var>(p).type == n) return false;
    if(db.is<n_construct>(p) && db.as<n_construct>(p).type == n) return false;
    if(is_lvalue(n)) return false;
    return true;
  }

  bool is_lvalue(node_id n) const {
    node_id p = db[n].parent;
    if(!p) return false;
    if(db.is<n_op>(p)) {
      auto const & o = db.as<n_op>(p);
      auto op = db.strings.text(o.op);
      return (op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=") && o.args[0] == n;
    }
    if(db.is<n_index>(p) && db.as<n_index>(p).object == n) return is_lvalue(p);
    return false;
  }

  ast_db const & db;
  interpreter const & vm;
  std::set<symbol> used;
};

// --- срез -----------------------------------------------------------------------------
//
// Instructions of a function that use a variable (and everything that is computed from
// it), plus what it depends on, to keep slice executable. Covering if/for/while are
// stored along with the terms.
struct slice_view {
  slice_view(ast_db const & db) : db(db) {}

  // Empty string - no function or variable found.
  std::string print(node_id module, std::string_view fn_name, std::string_view var) {
    node_id fn;
    for(node_id item: db.as<n_module>(module).items)
      if(db.is<n_fn>(item) && db.strings.text(db.as<n_fn>(item).name) == fn_name) fn = item;
    if(!fn) return {};

    collect(db.as<n_fn>(fn).body);
    for(node_id p: db.as<n_fn>(fn).params) param_names.insert(db.as<n_param>(p).name);

    std::set<symbol> forward{find_symbol(var)}, backward{find_symbol(var)};
    if(!*forward.begin()) return {};
    // Forward: Who uses x and all that is computed
    for(bool changed = true; changed;) {
      changed = false;
      for(auto & s: stmts)
        if(!kept.contains(s.node) && intersects(s.uses, forward)) {
          kept.insert(s.node);
          for(symbol d: s.defs) changed |= forward.insert(d).second;
          changed = true;
        }
    }
    // Back: Where does x come from and everything that saves instructions use?
    for(auto & s: stmts)
      if(kept.contains(s.node))
        for(symbol u: s.uses) backward.insert(u);
    for(bool changed = true; changed;) {
      changed = false;
      for(auto & s: stmts)
        if(intersects(s.defs, backward) && kept.insert(s.node).second) {
          for(symbol u: s.uses) backward.insert(u);
          changed = true;
        }
    }
    // control structures around and their conditions
    for(bool changed = true; changed;) {
      changed = false;
      for(node_id k: std::vector<node_id>(kept.begin(), kept.end()))
        for(node_id p = db[k].parent; p && p != fn; p = db[p].parent)
          if(is_statement(db, p) && kept.insert(p).second) {
            for(auto & s: stmts)
              if(s.node == p)
                for(symbol u: s.uses) backward.insert(u);
            changed = true;
          }
      for(auto & s: stmts)
        if(intersects(s.defs, backward) && kept.insert(s.node).second) {
          for(symbol u: s.uses) backward.insert(u);
          changed = true;
        }
    }
    return printer(db, [&](node_id n) {
      if(db[n].parent && db.is<n_module>(db[n].parent)) return n == fn;
      return !is_statement(db, n) || kept.contains(n) || has_kept_descendant(n);
    }).print(module);
  }
  struct stmt {
    node_id node;
    std::set<symbol> defs, uses;
  };

  symbol find_symbol(std::string_view name) const {
    for(auto & s: stmts) {
      for(symbol d: s.defs)
        if(db.strings.text(d) == name) return d;
      for(symbol u: s.uses)
        if(db.strings.text(u) == name) return u;
    }
    return {};
  }

  static bool intersects(std::set<symbol> const & a, std::set<symbol> const & b) {
    for(symbol x: a)
      if(b.contains(x)) return true;
    return false;
  }

  bool has_kept_descendant(node_id n) const {
    for(node_id k: kept)
      for(node_id p = db[k].parent; p; p = db[p].parent)
        if(p == n) return true;
    return false;
  }

  // Bypass the body: For each instruction, what it defines and uses.
  void collect(node_id n) {
    if(!n) return;
    if(is_statement(db, n) && !db.is<n_block>(n)) {
      stmt s{n, {}, {}};
      analyze(n, s);
      stmts.push_back(std::move(s));
    }
    // nested instructions (block bodies, branches, cycles)
    db.for_each_child(n, [&](node_id c) {
      if(!db.is<n_lambda>(c) && !db.is<n_fn>(c)) collect(c);
    });
  }

  void analyze(node_id n, stmt & s) {
    if(db.is<n_var>(n)) {
      s.defs.insert(db.as<n_var>(n).name);
      uses(db.as<n_var>(n).init, s.uses);
    } else if(db.is<n_for>(n)) {
      // The cycle variable is visible only in his body: the cycle enters slice as
      // encompassing the construction of stored instructions, not by name.
      uses(db.as<n_for>(n).range, s.uses);
    } else if(db.is<n_if>(n)) {
      uses(db.as<n_if>(n).cond, s.uses);
    } else if(db.is<n_while>(n)) {
      uses(db.as<n_while>(n).cond, s.uses);
    } else if(db.is<n_op>(n) && is_assign(db.as<n_op>(n))) {
      auto const & o = db.as<n_op>(n);
      node_id t = o.args[0];
      while(db.is<n_index>(t)) {
        uses(db.as<n_index>(t).index, s.uses);
        t = db.as<n_index>(t).object;
      }
      if(db.is<n_ident>(t)) {
        s.defs.insert(db.as<n_ident>(t).name);
        if(db.strings.text(o.op) != "=" || t != o.args[0]) s.uses.insert(db.as<n_ident>(t).name);
      }
      uses(o.args[1], s.uses);
    } else {
      uses(n, s.uses);
      // The call can change the arguments - alias (§6.2)
      if(db.is<n_call>(n))
        for(node_id a: db.as<n_call>(n).args)
          if(db.is<n_ident>(a)) s.defs.insert(db.as<n_ident>(a).name);
    }
  }

  bool is_assign(n_op const & o) const {
    auto op = db.strings.text(o.op);
    return op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=";
  }

  // Names readable by the expression (without nested instructions-blocks).
  void uses(node_id n, std::set<symbol> & out) const {
    if(!n) return;
    if(db.is<n_ident>(n)) {
      out.insert(db.as<n_ident>(n).name);
      return;
    }
    if(db.is<n_block>(n) || db.is<n_lambda>(n)) return; // blocks have their own instructions
    db.for_each_child(n, [&](node_id c) { uses(c, out); });
  }

  ast_db const & db;
  std::vector<stmt> stmts;
  std::set<node_id> kept;
  std::set<symbol> param_names;
};

} // namespace tz
