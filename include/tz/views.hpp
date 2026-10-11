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

// The tail of the design instruction: while(c) x = 1; - x = 1 is the instruction.
bool is_tail(ast_db const & db, node_id call, node_id n) {
  auto const & c = db.as<n_call>(call);
  return c.tail && !c.args.empty() && c.args.back() == n;
}

// Instruction means a block/module element or a structure/branch body that is not a block.
bool is_statement(ast_db const & db, node_id n) {
  node_id p = db[n].parent;
  if(!p) return false;
  if(db.is<n_block>(p) || db.is<n_module>(p)) return true;
  if(db.is<n_call>(p)) return is_tail(db, p, n) && !db.is<n_lambda>(n);
  if(db.is<n_lambda>(p)) return db.as<n_lambda>(p).block && db.as<n_lambda>(p).body == n;
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
// projection after elaboration (tz fmt --ct): ct module executed - node, modified by the
// ct code, printed modified, generated emit ads - after the module. The written text is tz
// fmt; this is .
//
//
std::string elaborated_text(ast_db & db, node_id root, std::string const & name) {
  interpreter vm(db, root, {name}, mode::check);
  vm.elaborate();
  std::string out = printer(db).print(root);
  if(!vm.emitted.empty()) out += "// порождено ct (emit):\n";
  for(node_id e: vm.emitted) out += printer(db).print(e) + "\n";
  return out;
}

struct residual_view {
  residual_view(ast_db const & db, interpreter const & vm) : db(db), vm(vm) {}

  std::string print(node_id module) {
    find_used_names(module);
    printer::substitution subst;
    subst.node = [this](node_id n) { return fold_if(n); };
    subst.text = [this](node_id n) { return fold_value(n); };
    printer pr(db, [this](node_id n) { return keep(n); }, subst);
    pr.comments = false; // Comments - on the written text; in the derivative program can lie (my)
    return pr.print(module);
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
    if(vm.declares(n) && !used.contains(db.as<n_ident>(db.as<n_op>(n).args[0]).name)) { // x = e, which declared x
      auto const * init = vm.observed(db.as<n_op>(n).args[1]);
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
    if(is_statement(db, n) && !db.is<n_var>(n) && !vm.declares(n) && !db.is<n_fn>(n) && (!keep_ignoring_dead(n) || pure_noop(n))) return;
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
    if(db.is<n_var>(n) || db.is<n_fn>(n) || db.is<n_param>(n) || db.is<n_lambda>(n) || db.is<n_block>(n) || db.is<n_return>(n) || db.require_cond(n) || db.is<n_loop>(n) || db.is<n_break>(n) || db.is<n_continue>(n) || db.clause_pack(n))
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
    if(db.is_index(p) && db.index_of(p).object == n) return is_lvalue(p);
    return false;
  }

  ast_db const & db;
  interpreter const & vm;
  std::set<symbol> used;
};

// --- срез -----------------------------------------------------------------------------
//
// slice logic is in language (slice_hidden prelude, M6 step 3): This is
// the entrance and the filter print.
struct slice_view {
  slice_view(ast_db & db) : db(db) {}

  // Empty string - no function or variable found.
  std::string print(node_id module, std::string_view fn_name, std::string_view var) {
    node_id sh;
    for(uint32_t i = db.size(); i-- > 0;)
      if(db.is<n_fn>(node_id(i)) && db.strings.text(db.as<n_fn>(node_id(i)).name) == "slice_hidden") {
        sh = node_id(i);
        break;
      }
    if(!sh) return {};
    // root - prelude : slice - pure analysis, program code (ct, emit) is not executed
    interpreter vm(db, db.prelude ? db.prelude : module, {"slice"}, mode::run);
    value r = vm.analyze_instance(sh, {vm.code_value(module), value(std::string(fn_name)), value(std::string(var))});
    if(!r.is<array_v>() || r.as<array_v>().size() == 0) return {};
    auto const & xs = r.as<array_v>();
    node_id fn = vm.code_node(xs.get(0), db[module].where);
    std::set<node_id> hidden;
    for(uint32_t k = 1; k < xs.size(); ++k) hidden.insert(vm.code_node(xs.get(k), db[module].where));
    printer pr(db, [&](node_id n) {
      if(db[n].parent && db.is<n_module>(db[n].parent)) return n == fn;
      return !hidden.contains(n);
    });
    pr.comments = false; // residual program
    return pr.print(module);
  }

  ast_db & db;
};

} // namespace tz
