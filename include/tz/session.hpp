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
// Proc start [arg...] live process of the machine (§9.3): started, is up to steps
// proc step N | proc run N steps | to the end; process output from last time
// proc where step and footage
// proc call <expression> sample in the clone (fork): expression in the current frame of the stopped
// process; the process itself does not change
// Proc stop finishing the edit process in a live process - a hot replacement: new calls - in a new
// version, started frames live in the old one; editing struct with living values of the old layout
// - only with migrate(T old) in the same text.
//
#pragma once

#include "interp.hpp"
#include "jit/jit.hpp"
#include "machine.hpp"
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
      if(cmd == "apply" && args.size() == 1) return apply(args[0]);
      if(cmd == "stats") return stats();
      if(cmd == "roundtrip") return roundtrip();
      if(cmd == "proc") return proc_cmd(args, line);
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
    node_values = vm.node_values(); // and values known in compilation
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

  // Function correction: the text is disassembled into the base (new import), the node of
  // the old function is replaced with new content - id and all references to the function
  // are saved. Apply the editing function of the program (code_replace, code_rename,
  // code_remove, code_insert): edits - to the database as human editing (§7.4).
  std::string apply(std::string const & fn_name) {
    node_id f;
    for(node_id item: db.as<n_module>(root).items)
      if(db.is<n_fn>(item) && db.strings.text(db.as<n_fn>(item).name) == fn_name && db.as<n_fn>(item).params.empty()) f = item;
    if(!f) return cat("нет функции правки ", fn_name, "() без параметров\n");
    interpreter vm(db, root, {name}, mode::run);
    uint32_t n = vm.apply_edit(f);
    db.link_parents(root);
    return cat("применена ", fn_name, ": правок ", n, "\n");
  }

  // --- living ---
  std::string proc_cmd(std::vector<std::string> const & args, std::string const & line) {
    std::string sub = args.empty() ? std::string() : args[0];
    if(sub == "start") {
      proc.reset();
      proc_out.clear();
      proc = std::make_unique<process_machine>(db, root, name);
      proc->captured = &proc_out;
      proc->start({args.begin() + 1, args.end()}, stdin_lines);
      return cat("процесс запущен\n");
    }
    if(!proc) return "нет живого процесса: proc start\n";
    if(sub == "step" || sub == "run") {
      uint64_t n = sub == "run" ? ~uint64_t{0} : args.size() > 1 ? std::stoull(args[1]) : 1;
      try {
        proc->run(n);
      } catch(diagnostic const & d) {
        proc_out += render(d, name, db);
        proc->st.done = true;
      }
      std::string out = std::exchange(proc_out, {});
      return out + proc->where_now();
    }
    if(sub == "where") return proc->where_now();
    if(sub == "stop") {
      proc.reset();
      return "процесс завершён\n";
    }
    if(sub == "call") {
      std::size_t at = line.find("call");
      return probe(line.substr(at + 4));
    }
    return cat("proc: start, step N, run, where, call <выражение>, stop\n");
  }

  // Sample - in a clone: fork host copies the VM as a whole (pages - a copy when writing), the clone disassembles
  // the expression in its database and counts it, the original waits for the result on the channel.
// 
//.
  std::string probe(std::string const & text) {
    if(proc->st.done) return "процесс завершён: пробовать нечего\n";
    int fd[2];
    if(pipe(fd) != 0) return "проба: нет канала\n";
    std::fflush(stdout);
    pid_t pid = fork();
    if(pid < 0) return "проба: fork не удался\n";
    if(pid == 0) {
      close(fd[0]);
      std::string r;
      try {
        parser p(db, cat("probe__() => ", text, ";"), "<проба>");
        p.place(root, {});
        node_id snippet = p.parse_module();
        auto errs = p.errors();
        if(!errs.empty())
          for(auto & e: errs) r += render(e, "проба", db);
        else {
          db.link_parents(snippet);
          r = proc->probe_here(db.as<n_fn>(db.as<n_module>(snippet).items[0]).body, 1'000'000);
        }
      } catch(diagnostic const & d) {
        r += render(d, name, db);
      } catch(std::exception const & e) {
        r += cat("проба: ", e.what(), "\n");
      }
      for(std::size_t at = 0; at < r.size();) {
        ssize_t k = write(fd[1], r.data() + at, r.size() - at);
        if(k <= 0) break;
        at += std::size_t(k);
      }
      _exit(0);
    }
    close(fd[1]);
    std::string r;
    char buf[4096];
    for(ssize_t k; (k = read(fd[0], buf, sizeof buf)) > 0;) r.append(buf, std::size_t(k));
    close(fd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return r;
  }

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
    for(node_id item: db.as<n_module>(snippet).items)
      if(db.is_struct(item) && db.strings.text(db.as<n_extend>(item).name) == fn_name) return edit_struct(item, snippet);
    node_id fresh;
    for(node_id item: db.as<n_module>(snippet).items)
      if(db.is<n_fn>(item) && db.strings.text(db.as<n_fn>(item).name) == fn_name) fresh = item;
    if(!fresh) return cat("в тексте правки нет функции ", fn_name, "\n");

    for(node_id item: db.as<n_module>(root).items)
      if(db.is<n_fn>(item) && db.strings.text(db.as<n_fn>(item).name) == fn_name) {
        db.replace(item, db[fresh].kind);
        db.adopt_notes(fresh, item); // Commentary on the new text - the preserved id
        db.link_parents(root);
        if(proc && !proc->st.done) proc->refresh_ct(); // ct new body - in place in canonical order
        return cat("функция ", fn_name, " заменена (узел #", item.value, ", ревизия ", db.revision, ")\n");
      }
    // No new function was added to the module (changes the set of overloads)
    auto m = db.as<n_module>(root);
    m.items.push_back(fresh);
    db.replace(root, m);
    db.link_parents(root);
    if(proc) proc->helper.register_item(fresh); // Living process sees new overload
    if(proc && !proc->st.done) proc->refresh_ct();
    return cat("функция ", fn_name, " добавлена\n");
  }

  // Edit struct: node saves id, fields new. The living values of the old layout are via
  // migrate(T old) from the same text (§9.3); no, editing rejected:
//.
  std::string edit_struct(node_id fresh, node_id snippet) {
    symbol type_name = db.as<n_extend>(fresh).name;
    std::string tn(db.strings.text(type_name));
    node_id item;
    for(node_id x: db.as<n_module>(root).items)
      if(db.is_struct(x) && db.as<n_extend>(x).name == type_name) item = x;
    if(!item) return cat("нет struct ", tn, "\n");
    std::vector<process_machine::field_decl> fields; // layout - names and types of fields
    for(node_id x: db.as<n_extend>(fresh).items)
      if(db.is<n_var>(x)) {
        node_id ty = db.as<n_var>(x).type;
        fields.push_back({db.as<n_var>(x).name, ty && db.is<n_ident>(ty) ? std::string(db.strings.text(db.as<n_ident>(ty).name)) : std::string()});
      }
    auto takes = [&](node_id f) {
      auto const & fn = db.as<n_fn>(f);
      if(db.strings.text(fn.name) != "migrate" || fn.params.size() != 1) return false;
      node_id t = db.as<n_param>(fn.params[0]).type;
      return t && db.is<n_ident>(t) && db.as<n_ident>(t).name == type_name;
    };
    node_id mig;
    for(node_id x: db.as<n_module>(snippet).items)
      if(db.is<n_fn>(x) && takes(x)) mig = x;
    type_id t = proc ? proc->helper.types.named(tn) : type_id{};
    uint64_t stale = proc && !proc->st.done ? proc->stale_values(t, fields) : 0;
    if(stale && !mig)
      return cat("правка отклонена: живой процесс держит значений ", tn, " старой раскладки: ", stale,
        " — нужна migrate(", tn, " old) => ", tn, "{…} в том же тексте (или proc stop)\n");
    db.replace(item, db[fresh].kind);
    db.adopt_notes(fresh, item);
    auto m = db.as<n_module>(root);
    if(mig) { // The previous migrate of the same type is replaced
      std::erase_if(m.items, [&](node_id x) { return db.is<n_fn>(x) && takes(x); });
      m.items.push_back(mig);
    }
    db.replace(root, m);
    db.link_parents(root);
    std::string out = cat("struct ", tn, " заменена (узел #", item.value, ", ревизия ", db.revision, ")\n");
    if(!proc || proc->st.done) return out;
    if(mig) { // overload migrate in the assistant - as in the module
      proc->helper.functions[db.as<n_fn>(mig).name].clear();
      for(node_id x: db.as<n_module>(root).items)
        if(db.is<n_fn>(x) && db.as<n_fn>(x).name == db.as<n_fn>(mig).name) proc->helper.register_item(x);
    }
    if(stale) out += cat("перенесено migrate: ", proc->migrate_values(t, fields, mig), "\n");
    return out;
  }

  ast_db & db;
  node_id root;
  std::string name;
  std::string dir; // program directory: key modules (use k, projection); empty - name directory
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
  std::map<uint32_t, std::vector<std::string>> node_values; // "Type\literal" by instance; literal empty - runtime
  std::map<uint32_t, std::vector<uint32_t>> call_targets; // call node → overload selected in instance
  std::vector<interpreter::trace_rec> trace; // launch-track
  std::map<uint32_t, std::map<std::string, interpreter::layer_info>> layers; // function → layers (contexts)
  std::vector<diagnostic> errors;
  std::unique_ptr<process_machine> proc; // living process of the machine (proc...)
  std::string proc_out; // his withdrawal from the previous team
};

} // namespace tz
