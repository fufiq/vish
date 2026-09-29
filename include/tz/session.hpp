// Long-lived VM session: AST base + jet cache parsing (§10.1 steak, §9.3).
//
// function editing - operation on the base (new import + replacement of node in journal);
// the next check recalculates only instance, depending on the changed, the next run goes to
// the new version.
//
// Commands (by line):
// check static parsing; prints what is recalculated and what from the cache
// Full report from the last check
// stdin <line> string stdin for the next run (queue; session stdin - not process stdin)
// run [arguments] execute main() (machine code - JIT session)
// jit instance JIT: compiled and reasons for failure
// Residual [arguments] program for given arguments
// fmt text projection
// edit <function> then the lines of the new function, the end is a line from one point
// Save the way. tzdb> write down journal
#pragma once

#include "interp.hpp"
#include "jit/jit.hpp"
#include "os.hpp"
#include "parser.hpp"
#include "printer.hpp"
#include "store.hpp"
#include "views.hpp"

namespace tz {

struct vm_session {
  vm_session(ast_db & db, node_id root, std::string name) : db(db), root(root), name(std::move(name)) {}

  // Execute one command; to edit transfer the body to `body`.
  std::string exec(std::string const & line, std::string const & body = {}) {
    auto words = words_of(line);
    std::string cmd = words.empty() ? std::string() : std::string(words[0]);
    std::vector<std::string> args;
    for(std::size_t k = 1; k < words.size(); ++k) args.emplace_back(words[k]);
    try {
      if(cmd == "check") return check();
      if(cmd == "stdin") {
        stdin_lines.push_back(line.size() > 6 ? line.substr(6) : "");
        return {};
      }
      if(cmd == "report") return last_report;
      if(cmd == "jit") return jit.stats();
      if(cmd == "run") return run(args);
      if(cmd == "residual") return residual(args);
      if(cmd == "fmt") return printer(db).print(root);
      if(cmd == "edit" && args.size() == 1) return edit(args[0], body);
      if(cmd == "save" && args.size() == 1) {
        save_db(db, root, args[0]);
        return cat("сохранено: ", args[0], "\n");
      }
    } catch(diagnostic const & d) {
      return render(d, name, db);
    }
    return cat("неизвестная команда: ", line, "\n");
  }
  std::string check() {
    cache.hits = cache.recomputed = 0;
    cache.recomputed_names.clear();
    interpreter vm(db, root, {name}, mode::check, std::nullopt, &cache);
    std::string err;
    errors.clear();
    try {
      vm.run_main();
    } catch(diagnostic const & d) {
      err = render(d, name, db);
      errors.push_back(d);
    }
    node_types = vm.node_types(); // For the Editor: Types and Errors in Place
    last_report = vm.report(name, db.source(db[root].where.source));
    std::string names;
    for(auto & n: cache.recomputed_names) names += (names.empty() ? "" : ", ") + n;
    return err + cat("check: пересчитано ", cache.recomputed, " экземпл. (", names.empty() ? "—" : names, "), из кэша ", cache.hits, "\n");
  }

  std::string run(std::vector<std::string> const & args) {
    std::vector<std::string> a{name};
    a.insert(a.end(), args.begin(), args.end());
    interpreter vm(db, root, a, mode::run, interpreter::known_inputs{stdin_lines});
    vm.use_jit(&jit);
    std::string out;
    vm.capture_output(&out);
    try {
      vm.run_main();
    } catch(diagnostic const & d) {
      out += render(d, name, db);
    }
    return out;
  }

  std::string residual(std::vector<std::string> const & args) {
    std::vector<std::string> a{name};
    a.insert(a.end(), args.begin(), args.end());
    interpreter vm(db, root, a, mode::check, interpreter::known_inputs{});
    std::string err;
    try {
      vm.run_main();
    } catch(diagnostic const & d) {
      err = render(d, name, db);
    }
    return err + residual_view(db, vm).print(root);
  }

  // Function correction: the text is disassembled into the base (new import), the node of the old
  // function is replaced with new content - id and all references to the function are saved.
  std::string edit(std::string const & fn_name, std::string const & text) {
    parser p(db, text, cat("<правка ", fn_name, " #", ++edits, ">"));
    node_id snippet = p.parse_module();
    auto errs = p.errors();
    if(!errs.empty()) {
      std::string out;
      for(auto & e: errs) out += render(e, cat("правка ", fn_name), db);
      return out + "правка отклонена\n";
    }
    node_id fresh;
    for(node_id item: db.as<n_module>(snippet).items)
      if(db.is<n_fn>(item) && db.strings.text(db.as<n_fn>(item).name) == fn_name) fresh = item;
    if(!fresh) return cat("в тексте правки нет функции ", fn_name, "\n");

    for(node_id item: db.as<n_module>(root).items)
      if(db.is<n_fn>(item) && db.strings.text(db.as<n_fn>(item).name) == fn_name) {
        db.replace(item, db[fresh].kind);
        db.link_parents(root);
        return cat("функция ", fn_name, " заменена (узел #", item.value, ", ревизия ", db.revision, ")\n");
      }
    // No new function was added to the module (changes the set of overloads)
    auto m = db.as<n_module>(root);
    m.items.push_back(fresh);
    db.replace(root, m);
    db.link_parents(root);
    return cat("функция ", fn_name, " добавлена\n");
  }

  ast_db & db;
  node_id root;
  std::string name;
  // The machine code lives throughout the session: instance checks the revisions of the
  // functions included in it (including embedded ones), and after editing it is recompiled.
  // There are no de-optimizations - this is a recompilation of the new version (§10, M1).
  jit_engine jit{db, root};
  check_cache cache;
  std::vector<std::string> stdin_lines;
  uint32_t edits = 0;
  std::string last_report;
  // Last check: node types in all contexts and errors (editor, §13.1).
  std::map<uint32_t, std::vector<std::string>> node_types;
  std::vector<diagnostic> errors;
};

} // namespace tz
