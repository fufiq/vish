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
// stats base size: node, journal records, .tzdb bytes; growth from past stats
// roundtrip journal to new base: same projection and run output
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
      if(cmd == "stats") return stats();
      if(cmd == "roundtrip") return roundtrip();
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
    call_targets = vm.resolved_calls;
    layers = vm.layers;
    last_report = vm.report(name, db.source(db[root].where.source));
    std::string names;
    for(auto & n: cache.recomputed_names) names += (names.empty() ? "" : ", ") + n;
    return err + cat("check: пересчитано ", cache.recomputed, " экземпл. (", names.empty() ? "—" : names, "), из кэша ", cache.hits, "\n");
  }

  // Base size and growth from the past stats: edits grow the base to disassemble text, while
  // elaboration, parsing recalculation and new interpreters with the same result do not (test 217)
  std::string stats() {
    writer w;
    auto entries = db.journal();
    for(auto const & e: entries) w.put(e);
    std::string out = cat("база: узлов ", db.size(), ", журнал ", entries.size(), ", tzdb ", w.bytes.size(), " байт\n");
    if(seen_size) out += cat("рост: узлов +", db.size() - seen_size, ", журнал +", entries.size() - seen_log, "\n");
    seen_size = db.size();
    seen_log = entries.size();
    return out;
  }

  // journal → new base (as .tzdb): same projection and same run output (tests 218, 219)
  std::string roundtrip() {
    writer w;
    for(auto const & e: db.journal()) w.put(e);
    reader r(w.bytes);
    std::vector<journal_entry> entries;
    while(!r.at_end()) entries.push_back(r.get<journal_entry>());
    ast_db db2;
    db2.replay(entries);
    std::string out;
    if(printer(db).print(root) != printer(db2).print(root)) out += "roundtrip: проекция отличается\n";
    std::string a = run({}), b;
    interpreter vm(db2, root, {name}, mode::run, interpreter::known_inputs{stdin_lines});
    vm.capture_output(&b);
    try {
      vm.run_main();
    } catch(diagnostic const & d) {
      b += render(d, name, db2);
    }
    if(a != b) out += cat("roundtrip: вывод отличается: ", in_quotes(b), " против ", in_quotes(a), "\n");
    return out.empty() ? cat("roundtrip: совпадает (узлов ", db2.size(), ")\n") : out;
  }

  std::string run(std::vector<std::string> const & args) {
    bool ok = true;
    return run(args, ok);
  }
  std::string run(std::vector<std::string> const & args, bool & ok) {
    std::vector<std::string> a{name};
    a.insert(a.end(), args.begin(), args.end());
    interpreter vm(db, root, a, mode::run, interpreter::known_inputs{stdin_lines});
    vm.use_jit(&jit);
    std::string out;
    vm.capture_output(&out);
    ok = true;
    try {
      vm.run_main();
    } catch(diagnostic const & d) {
      out += render(d, name, db);
      ok = false;
    }
    return out;
  }

  // Run with track: interpreter (machine code does not write the track), the same main().
  std::string trace_run(std::vector<std::string> const & args, bool & ok, std::string & error) {
    std::vector<std::string> a{name};
    a.insert(a.end(), args.begin(), args.end());
    interpreter vm(db, root, a, mode::run, interpreter::known_inputs{stdin_lines});
    trace.clear();
    vm.tracer = &trace;
    std::string out;
    vm.capture_output(&out);
    ok = true;
    error.clear();
    try {
      vm.run_main();
    } catch(diagnostic const & d) {
      error = render(d, name, db);
      ok = false;
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
    node_id at; // parsing rules of the function location (§4.4): announced before it; new - the entire module
    for(node_id item: db.as<n_module>(root).items)
      if(!at && db.is<n_fn>(item) && db.strings.text(db.as<n_fn>(item).name) == fn_name) at = item;
    p.place(root, at);
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
  uint32_t seen_size = 0; // stats: sizes on previous call
  std::size_t seen_log = 0;
  std::string last_report;
  // Last check: node types in all contexts and errors (editor, §13.1).
  std::map<uint32_t, std::vector<std::string>> node_types;
  std::map<uint32_t, std::vector<uint32_t>> call_targets; // call node → overload selected in instance
  std::vector<interpreter::trace_rec> trace; // launch-track
  std::map<uint32_t, std::map<std::string, interpreter::layer_info>> layers; // function → layers (contexts)
  std::vector<diagnostic> errors;
};

} // namespace tz
