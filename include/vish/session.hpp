// Long-lived VM session: AST base + jet cache parsing (§10.1 steak, §9.3).
//
// function editing - operation on the base (new import + replacement of node in journal);
// the next check recalculates only instance, depending on the changed, the next run goes to
// the new version.
//
// Session commands (`vish vm`, scripts .session, `vish apply`) - in rows; their parsing, decisions and
// texts - in check, report, stdin <line>, run [arguments], jit, residual [arguments], fmt, edit <function>
// (body - up to a line from one point), <function>, save <path.vishdb>, stats, applytrip, proc start
// [arg...] | step N | run | where | calle | grante | Here is the command base: parsing, execution, base
// editing, live machine process; data (numbers, program texts and errors) - language. What to replace with
// editing, struct and migrate in a live process, which editing function to apply - decides the language.
//
//
//
#pragma once

#include "interp.hpp"
#include "jit/jit.hpp"
#include "machine.hpp"
#include "os.hpp"
#include "parser.hpp"
#include "printer.hpp"
#include "repository.hpp"
#include "store.hpp"
#include "views.hpp"

namespace vish {

// struct n layout of the db base: names and types of fields (living process compares with its values)
std::vector<process_machine::field_decl> struct_fields(ast_db const & db, node_id n) {
  std::vector<process_machine::field_decl> fields;
  for(node_id x: db.as<n_extend>(n).items)
    if(db.is<n_var>(x)) {
      node_id ty = db.as<n_var>(x).type;
      fields.push_back({db.as<n_var>(x).name, ty && db.is<n_ident>(ty) ? std::string(db.strings.text(db.as<n_ident>(ty).name)) : std::string()});
    }
  return fields;
}

struct vm_session {
  vm_session(ast_db & db, node_id root, std::string name) : db(db), root(root), name(std::move(name)) {}

  // Session command: Error parsing or execution - text (with a place), not an exception. After the command,
  // there are no bypasses - the removed views of node won (a long session does not accumulate them).
  std::string guarded(auto && command) {
    std::string out;
    try {
      out = command();
    } catch(diagnostic const & d) {
      out = render(d, name, db);
    }
    db.sweep_retired();
    return out;
  }
  // Reactive check: error (text with place), recalculated instance (their names), from cache.
  struct check_facts {
    std::string error;
    uint32_t recomputed = 0, hits = 0;
    std::vector<std::string> names;
  };
  check_facts check() {
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
    return {err, cache.recomputed, cache.hits, {cache.recomputed_names.begin(), cache.recomputed_names.end()}};
  }

  // Base size and growth from the past stats: edits grow the base on disassembled text, and
  // elaboration, recounting parsing and new interpreters with the same result - no (test 217)
  // (node, journal records, byte .vishdb, past node and entries - 0 if stats were not)
  std::array<uint64_t, 5> stats() {
    writer w;
    auto entries = db.journal();
    for(auto const & e: entries) w.put(e);
    std::array<uint64_t, 5> r{db.size(), entries.size(), w.bytes.size(), seen_size, seen_log};
    seen_size = db.size();
    seen_log = entries.size();
    return r;
  }

  // journal → new base (like .vishdb): same projection, run output here and there, node in new (tests
  // 218, 219)
  struct roundtrip_facts {
    bool same_projection;
    std::string here, there;
    uint64_t nodes;
  };
  roundtrip_facts roundtrip() {
    writer w;
    for(auto const & e: db.journal()) w.put(e);
    reader r(w.bytes);
    std::vector<journal_entry> entries;
    while(!r.at_end()) entries.push_back(r.get<journal_entry>());
    ast_db db2;
    db2.replay(entries);
    bool same = printer(db).print(root) == printer(db2).print(root);
    std::string a = run({}), b;
    interpreter vm(db2, root, {name}, mode::run, interpreter::known_inputs{stdin_lines});
    vm.capture_output(&b);
    try {
      vm.run_main();
    } catch(diagnostic const & d) {
      b += render(d, name, db2);
    }
    return {same, std::move(a), std::move(b), db2.size()};
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

  // The function of editing the program f (code_replace, code_rename, code_remove, code_insert): edits -
  // to the base as human editing (§7.4). The result is the number of edits.
  uint32_t apply(node_id f) {
    interpreter vm(db, root, {name}, mode::run);
    uint32_t n = vm.apply_edit(f);
    db.link_parents(root);
    return n;
  }
  // --- living ---
  void proc_start(std::vector<std::string> const & args) {
    proc.reset();
    proc_out.clear();
    proc = std::make_unique<process_machine>(db, root, name);
    proc->captured = &proc_out;
    proc->start(args, stdin_lines);
  }
  // n steps (~0 - to the end), output from the last time and where
  std::string proc_step(uint64_t n) {
    try {
      proc->run(n);
    } catch(diagnostic const & d) {
      proc_out += render(d, name, db);
      proc->st.done = true;
    }
    std::string out = std::exchange(proc_out, {});
    return out + proc->where_now();
  }
  // "You give": resource on the move
  void proc_grant(std::string const & what, bool give) { proc->grant(what, give); }
  // external management; result - processors
  std::size_t proc_pressure() {
    proc->pressure();
    return proc->st.pressure.size();
  }

  // Sample - in a clone: fork host copies the VM as a whole (pages - a copy when writing), the clone disassembles
  // the expression in its database and counts it, the original waits for the result on the channel.
// 
//.
  std::string probe(std::string const & text) {
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

  // Editing is the primitives of the base; what to replace, struct and migrate in a live process is decided
  // by the language (lib/tools.vish, edit_command). Edit text - parsing according to the rules of place at
  // (§4.4; empty - declared throughout the module): errors parsing text and elements of the disassembled.
  std::pair<std::string, std::vector<node_id>> parse_at(node_id at, std::string const & label, std::string const & text) {
    parser p(db, text, cat("<правка ", label, " #", ++edits, ">"));
    p.place(root, at);
    node_id snippet = p.parse_module();
    std::string errs;
    for(auto & e: p.errors()) errs += render(e, cat("правка ", label), db);
    if(!errs.empty()) return {errs, {}};
    auto const & items = db.as<n_module>(snippet).items;
    return {errs, {items.begin(), items.end()}};
  }
  // The old element becomes fresh: id old and the references to it are saved, the comments of the new text
  // are in it. The result is a base audit.
  uint64_t replace_item(node_id old, node_id fresh) {
    db.replace(old, db[fresh].kind);
    db.adopt_notes(fresh, old);
    db.link_parents(root);
    return db.revision;
  }
  // elements of the program module - this list (one edit of the base)
  void set_items(std::vector<node_id> items) {
    auto m = db.as<n_module>(root);
    m.items = std::move(items);
    db.replace(root, m);
    db.link_parents(root);
  }

  std::optional<repository_state> repository;
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

} // namespace vish
