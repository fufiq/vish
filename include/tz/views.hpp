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
  residual_view(ast_db & db, interpreter const & vm) : db(db), vm(vm) {}

  // Solutions are in language (residual_plan prelude, M6 step 3b) over observations of parsing
  // vm; here is input and printing with filter and substitution.
  std::string print(node_id module) {
    node_id rp;
    for(uint32_t i = db.size(); i-- > 0;)
      if(db.is<n_fn>(node_id(i)) && db.strings.text(db.as<n_fn>(node_id(i)).name) == "residual_plan") {
        rp = node_id(i);
        break;
      }
    std::set<node_id> hidden;
    std::map<node_id, node_id> folds;
    std::map<node_id, std::string> values;
    if(rp) {
      // root prelude: the program code (ct, emit) is not executed again
      interpreter lang(db, db.prelude ? db.prelude : module, {"residual"}, mode::run);
      lang.view_subject = &vm;
      value r = lang.analyze_instance(rp, {lang.code_value(module)});
      span where = db[module].where;
      auto list = [&](std::string_view f) -> array_v const & {
        for(auto const & [k, v]: r.as<agg_v>().fields)
          if(db.strings.text(k) == f) return v.as<array_v>();
        throw diagnostic(cat("residual_plan: нет поля ", f), where);
      };
      auto field = [&](value const & x, std::string_view f) -> value const & {
        for(auto const & [k, v]: x.as<agg_v>().fields)
          if(db.strings.text(k) == f) return v;
        throw diagnostic(cat("residual_plan: нет поля ", f), where);
      };
      for(uint32_t k = 0; k < list("hidden").size(); ++k) hidden.insert(lang.code_node(list("hidden").get(k), where));
      for(uint32_t k = 0; k < list("folds").size(); ++k) {
        value x = list("folds").get(k);
        folds[lang.code_node(field(x, "at"), where)] = lang.code_node(field(x, "other"), where);
      }
      for(uint32_t k = 0; k < list("values").size(); ++k) {
        value x = list("values").get(k);
        values[lang.code_node(field(x, "at"), where)] = field(x, "text").as<str_v>().str();
      }
    }
    printer::substitution subst;
    subst.node = [&](node_id n) {
      auto it = folds.find(n);
      return it == folds.end() ? node_id{} : it->second;
    };
    subst.text = [&](node_id n) -> std::optional<std::string> {
      auto it = values.find(n);
      if(it == values.end()) return std::nullopt;
      return it->second;
    };
    printer pr(db, [&](node_id n) { return !hidden.contains(n); }, subst);
    pr.comments = false; // Comments - on the written text; in the derivative program can lie (my)
    return pr.print(module);
  }

  ast_db & db;
  interpreter const & vm;
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
