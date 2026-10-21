// tz commands - in language (lib/tools.tz, cli): parsing trigger lines, flags, order, texts and output
// codes:
//  
// 
// Base one: prelude, tool module, program - tool module does not see (do not use). Here are the
// commands themselves on the program: download, print, execution (interpreter, JIT, machine), check,
// residual, slice, session, editor server, dump renewal.
//
#pragma once

#include "interp.hpp"
#include "jit/jit.hpp"
#include "machine.hpp"
#include "modules.hpp"
#include "printer.hpp"
#include "server.hpp"
#include "session.hpp"
#include "store.hpp"
#include "test_runner.hpp"
#include "tools.hpp"
#include "views.hpp"

namespace tz {

// Team program: base (for .tz - the team base, for .tzdb - its own by journal), root, errors parsing.
struct cli_program {
  std::optional<ast_db> own;
  ast_db * db = nullptr;
  node_id root;
  std::vector<diagnostic> errors;
  std::string path;
  std::optional<vm_session> session;

  std::string const & src() const { return db->source((*db)[root].where.source); }
  int report(diagnostic const & d) const {
    put(stderr, render(d, path, *db));
    return 1;
  }
};

struct cli_host {
  ast_db & db; // command base: prelude, tools module, .tz program
  std::deque<cli_program> programs; // number
  test_host tests; // tz test

  cli_program & at(int64_t h) {
    if(h < 0 || std::size_t(h) >= programs.size()) throw diagnostic("tz: нет программы", {});
    return programs[std::size_t(h)];
  }
  int64_t open(std::string const & path) {
    cli_program & p = programs.emplace_back();
    p.path = path;
    if(path.ends_with(".tzdb")) {
      p.db = &p.own.emplace();
      p.root = load_db(*p.db, path);
    } else {
      p.db = &db;
      os::mapped_file file(path);
      p.root = parse_program(db, file.text(), std::filesystem::path(path).parent_path(), p.errors); // use k; next to the program
    }
    return int64_t(programs.size() - 1);
  }
  std::string errors(int64_t h) {
    cli_program & p = at(h);
    std::string out;
    for(auto const & e: p.errors) out += render(e, p.path, *p.db);
    return out;
  }
  // Canonical projection; key - record module (§4.4): array {текст, ошибки, нашёлся ли модуль}
  std::tuple<std::string, std::string, bool> fmt(int64_t h, std::string const & key) {
    cli_program & p = at(h);
    printer pr(*p.db);
    std::string errs;
    if(!key.empty()) {
      for(auto const & e: preload_uses(*p.db, cat("use ", key, ";"), std::filesystem::path(p.path).parent_path())) errs += render(e, key, *p.db);
      if(auto it = p.db->strings.index.find(key); it != p.db->strings.index.end()) pr.projection = find_module(*p.db, it->second);
      pr.header = true; // The text itself tells you how to disassemble it (§4.5)
      if(!pr.projection) return {"", errs + cat("нет модуля проекции ", key, "\n"), false};
    }
    std::string text = pr.print(p.root);
    for(auto const & n: pr.notes) errs += cat(n, "\n"); // The choice of printing language fell - conservatively
    return {text, errs, true};
  }
  vm_session & session(int64_t h) {
    cli_program & p = at(h);
    if(!p.session) p.session.emplace(*p.db, p.root, p.path);
    return *p.session;
  }
  // tz run: interpreter (with JIT, AArch64 in qemu); output code
  int run(int64_t h, bool instances, bool use_jit, bool jit_stats, bool a64, std::vector<std::string> const & extra) {
    cli_program & p = at(h);
    std::vector<std::string> args{p.path};
    args.insert(args.end(), extra.begin(), extra.end());
    interpreter vm(*p.db, p.root, std::move(args));
    jit_engine jit(*p.db, p.root);
    if(use_jit) vm.use_jit(&jit);
    a64_check check(*p.db, p.root);
    if(a64) vm.a64 = &check;
    int code = 0;
    try {
      code = vm.run_main();
    } catch(diagnostic const & d) {
      code = p.report(d);
    }
    if(instances) put(stderr, cat("экземпляры:\n", vm.instances()));
    if(jit_stats) put(stderr, cat("JIT:\n", jit.stats()));
    if(a64) {
      put(stderr, check.report());
      if(!check.mismatches.empty() && !code) code = 1;
    }
    return code;
  }
  // Process Machine (M5): Execution without recursion, memory - data; dump_path - stop after dump_after steps
  // and write down the dump (tz resume).
  int machine(int64_t h, uint64_t dump_after, std::string const & dump_path, std::vector<std::string> const & args) {
    cli_program & p = at(h);
    process_machine m(*p.db, p.root, p.path);
    try {
      m.start(args); // parsing before execution - its errors with place
      if(dump_path.empty()) m.run();
      else {
        m.run(dump_after);
        if(m.st.done) put(stderr, cat("процесс закончился раньше шага ", dump_after, "\n"));
        else {
          os::write_file(dump_path, m.dump());
          put(stderr, cat("дамп: ", dump_path, " (шаг ", m.st.steps, ")\n"));
        }
        return 0;
      }
    } catch(diagnostic const & d) {
      return p.report(d);
    }
    put(stderr, cat("шагов: ", m.st.steps, "\n"));
    return int(m.st.exit_code);
  }
  int check(int64_t h) {
    cli_program & p = at(h);
    interpreter vm(*p.db, p.root, {p.path}, mode::check);
    int code = 0;
    try {
      vm.run_main();
    } catch(diagnostic const & d) {
      code = p.report(d);
    }
    put(stdout, vm.report(p.path, p.src()));
    return code;
  }
  int residual(int64_t h, std::vector<std::string> const & input, std::vector<std::string> const & extra) {
    cli_program & p = at(h);
    interpreter::known_inputs known;
    known.stdin_lines = input;
    std::vector<std::string> args{p.path};
    args.insert(args.end(), extra.begin(), extra.end());
    interpreter vm(*p.db, p.root, args, mode::check, known);
    int code = 0;
    try {
      vm.run_main();
    } catch(diagnostic const & d) {
      code = p.report(d);
    }
    put(stdout, residual_view(*p.db, vm).print(p.root));
    return code;
  }
  // The key tuning module is in this database (if not) and its function is fn with the words args: exit code.
  int run_module(std::string const & key, std::string const & fn, std::vector<std::string> const & args) {
    node_id m = load_builtin(db, key);
    node_id was = std::exchange(tools_host->module, m); // functions of the module by name (children run tests)
    struct back {
      node_id was;
      ~back() { tools_host->module = was; }
    } restore{was};
    node_id f = tools_host->fn(fn);
    if(!f) throw diagnostic(cat("нет функции ", key, ".", fn), {});
    interpreter vm(db, m, {}, mode::run);
    return int(vm.analyze_instance(f, {strings_value(args)}).as<int64_t>());
  }
  // VM for Editor: Protocol on Unix Socket - Forever
  [[noreturn]] void serve(int64_t h, std::string const & socket) {
    vm_session & s = session(h);
    editor_server server(s);
    server.listen(socket);
    put(stderr, cat("tz serve: ", at(h).path, " на ", socket, "\n"));
    for(;;) server.step();
  }
};

// tz command, whose work is underway: primitive commands of the tool module (native "cpp")
thread_local cli_host * cli_now = nullptr;

// Renew the process from the dump - "to another VM" (M5): another tz process.
int resume_dump(std::string const & path) {
  os::mapped_file f(path);
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC); // Dump pages - displaying the file
  restored_process r(f.text(), fd);
  r.m->run();
  return int(r.m->st.exit_code);
}

// The tz start line is a function of the cli tool module; the command base is prelude, the tools module, and
// the program. Error base or module code - text and code 1.
int run_cli(int argc, char ** argv) {
  ast_db db;
  tools_context ctx;
  ctx.db = &db;
  cli_host host{db, {}, {}};
  struct hold {
    tools_context * was_tools;
    cli_host * was_cli;
    test_host * was_tests;
    ~hold() {
      tools_host = was_tools;
      cli_now = was_cli;
      tests_host = was_tests;
    }
  } h{std::exchange(tools_host, &ctx), std::exchange(cli_now, &host), std::exchange(tests_host, &host.tests)};
  try {
    load_prelude(db);
    ctx.module = load_builtin(db, "cli");
    node_id f = ctx.fn("cli");
    std::vector<value> args;
    for(int i = 1; i < argc; ++i) args.push_back(value(std::string(argv[i])));
    interpreter vm(db, ctx.module, {}, mode::run);
    try {
      return int(vm.analyze_instance(f, {value(array_v::of(std::move(args)))}).as<int64_t>());
    } catch(diagnostic const & d) {
      if(d.where.begin == 0 && d.where.end == 0 && d.where.source == 0) throw; // team error (file, base) - as is
      throw diagnostic(cat("tools: ", render(d, builtin_file(db, d.where.source), db)), {}); // Module code error - with space
    }
  } catch(diagnostic const & d) {
    put(stderr, cat(d.what(), "\n"));
    return 1;
  } catch(std::exception const & e) {
    put(stderr, cat(e.what(), "\n"));
    return 1;
  }
}

} // namespace tz
