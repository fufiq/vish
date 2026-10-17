// tz - VM king. One unit of translation: all code is headers (HO-Build).
//
// The source of truth is AST (D7). Any command accepts either text (.tz - import text
// projection to the database) or the base itself (.tzdb - journal).
#include "tz/base.hpp"
#include "tz/os.hpp"
#include "tz/lexer.hpp"
#include "tz/ast.hpp"
#include "tz/parser.hpp"
#include "tz/prelude.hpp"
#include "tz/printer.hpp"
#include "tz/dump.hpp"
#include "tz/interp.hpp"
#include "tz/test_runner.hpp"
#include "tz/views.hpp"
#include "tz/store.hpp"
#include "tz/session.hpp"
#include "tz/server.hpp"
#include "tz/merge.hpp"
#include "tz/machine.hpp"
#include "tz/modules.hpp"
#include "tz/jit/jit.hpp"

namespace {

using tz::cat;
using tz::put;

int usage() {
  put(stderr, std::string("использование (<программа> — файл .tz или база .tzdb):\n  tz fmt <программа>        каноническая текстовая проекция\n  tz fmt --ct <программа>   проекция после элаборации: мутации ct-кода и порождённое emit\n  tz fmt --projection <ключ> <программа>   проекция — модуль записей syntax (по ключу, как use)\n  tz ast <программа>        дерево AST\n  tz run [--instances] [--jit] [--jit-stats] [--a64] <программа> [аргументы...]   исполнить main(); --a64 — код AArch64 параллельно, в qemu-aarch64\n  tz run --machine [--dump-after N файл.tzproc] <программа> [аргументы...]   машина процессов; дамп после N шагов\n  tz resume <файл.tzproc>   возобновить процесс из дампа (в другой VM)\n  tz check <программа>      статический разбор: стадии кт/рт, расщепления, экземпляры\n  tz residual <программа> [--stdin СТРОКА]... [аргументы...]   программа для заданных входов\n  tz slice <программа> <функция> <переменная>   только то, что связано с переменной\n  tz save <файл.tz> <база.tzdb>   импортировать текст в базу (журнал на диск)\n  tz log <база.tzdb>         журнал операций базы в тексте\n  tz apply <программа> <функция> [база.tzdb]   применить функцию правки (code_rename…): новый текст, база\n  tz vm <программа>          сессия VM: команды из stdin (check, run, edit <функция> … ., report, save)\n  tz serve <программа> <сокет>   VM для редактора: протокол на Unix-сокете\n  tz schema                  схема протокола редактора (JSON)\n  tz diff <a.tzdb> <b.tzdb>  семантический дифф баз (по элементам модуля)\n  tz merge <мой.tzdb> <их.tzdb> [итог.tzdb]   трёхстороннее слияние; конфликты — слить руками\n  tz test <каталог>          прогнать эталонные тесты\n"));
  return 2;
}

// Downloaded program: base, root, text for diagnostics, errors parsing.
struct program {
  tz::ast_db db;
  tz::node_id root;
  std::vector<tz::diagnostic> parse_errors;
  std::string path;

  std::string const & src() const { return db.source(db[root].where.source); }
};

void load(program & p, std::string const & path) {
  p.path = path;
  if(path.ends_with(".tzdb")) {
    p.root = tz::load_db(p.db, path);
    return;
  }
  tz::os::mapped_file file(path);
  p.root = tz::parse_program(p.db, file.text(), std::filesystem::path(path).parent_path(), p.parse_errors); // use k; next to the program
}

int report_error(program const & p, tz::diagnostic const & d) {
  put(stderr, tz::render(d, p.path, p.db));
  return 1;
}

} // namespace

int run(int argc, char ** argv);

// All work is on the VM 256MB stack (os::with_stack): depth is not the limit of the host stack.
int main(int argc, char ** argv) {
  return tz::os::with_stack(size_t(256) << 20, [&] { return run(argc, argv); });
}

int run(int argc, char ** argv) {
  if(argc == 2 && std::string_view(argv[1]) == "schema") { // Editor's Protocol Scheme (JSON)
    put(stdout, tz::proto::schema());
    return 0;
  }
  if(argc < 3) return usage();
  std::string cmd = argv[1];
  if(cmd == "fmt" && std::string_view(argv[2]) == "--ct") { // tz fmt --ct <program >>
    if(argc < 4) return usage();
    cmd = "fmt-ct";
    ++argv;
    --argc;
  }
  std::string projection; // tz fmt --projection <key> <program>: projection - Record Module (§4.4)
  if(cmd == "fmt" && std::string_view(argv[2]) == "--projection") {
    if(argc < 5) return usage();
    projection = argv[3];
    argv += 2;
    argc -= 2;
  }
  try {
    // Assembling native “cpp” { … } (§3.3): Bodies from prelude and data files into a
    // header that VM compiles into itself (build.sh). The file is rewritten only if it
    // has changed: the exit code 3 is “reassemble”.
    if(cmd == "native") {
      tz::ast_db db;
      tz::load_prelude(db);
      for(int i = 3; i < argc; ++i) {
        tz::os::mapped_file f(argv[i]);
        tz::parser p(db, f.text(), argv[i]);
        p.parse_module();
        if(!p.errors().empty()) throw p.errors().front();
      }
      std::string out = "// Сгенерировано `tz native` из native \"cpp\" { … } библиотек — не править.\n#pragma once\n\nnamespace tz {\n\n";
      std::string table;
      uint32_t k = 0;
      for(uint32_t i = 0; i < db.size(); ++i) {
        if(!db.is<tz::n_native>(tz::node_id(i))) continue;
        tz::node_id fn = tz::native_owner(db, tz::node_id(i));
        std::string code = db.as<tz::n_native>(tz::node_id(i)).code;
        auto const & ps = db.as<tz::n_fn>(fn).params;
        for(std::size_t j = 0; j < ps.size(); ++j) { // #x - parameter x
          std::string name = cat("#", db.strings.text(db.as<tz::n_param>(ps[j]).name));
          for(std::size_t at = code.find(name); at != std::string::npos; at = code.find(name, at)) {
            char next = at + name.size() < code.size() ? code[at + name.size()] : ' ';
            if(std::isalnum(static_cast<unsigned char>(next)) || next == '_') {
              at += name.size();
              continue;
            }
            std::string arg = cat("(a[", j, "])");
            code.replace(at, name.size(), arg);
            at += arg.size();
          }
        }
        std::string key = tz::native_key(db, fn);
        out += cat("// ", key, "\nvalue native_", k, "(interpreter & vm, std::vector<value> & a, span where) {", code, "}\n\n");
        table += cat("    {\"", key, "\", native_", k, "},\n");
        ++k;
      }
      out += k ? cat("native_entry const native_entries[] = {\n", table, "};\n\nstd::span<native_entry const> native_table() { return native_entries; }\n")
               : std::string("std::span<native_entry const> native_table() { return {}; }\n");
      out += "\n} // namespace tz\n";
      std::string old;
      if(std::filesystem::exists(argv[2])) old = std::string(tz::os::mapped_file(argv[2]).text());
      if(old == out) return 0;
      tz::os::write_file(argv[2], out);
      return 3;
    }
    if(cmd == "test") {
      tz::test_runner r;
      r.a64 = argc > 3 && std::string_view(argv[3]) == "--a64";
      return r.run(argv[2]);
    }
    // Versions (§9.4): Semantic diff and triangular .tzdb fusion.
    if(cmd == "diff" && argc == 4) {
      tz::db_copy a, b;
      tz::node_id ra, rb;
      auto ea = tz::read_journal(argv[2], ra), eb = tz::read_journal(argv[3], rb);
      tz::load_journal(a, ea, ra);
      tz::load_journal(b, eb, rb);
      put(stdout, tz::describe_diff(a.db, a.root, b.db, b.root));
      return 0;
    }
    if(cmd == "merge" && argc >= 4) {
      tz::db_copy mine;
      tz::node_id rm, rt;
      auto em = tz::read_journal(argv[2], rm), et = tz::read_journal(argv[3], rt);
      tz::load_journal(mine, em, rm);
      auto r = tz::merge_into(mine.db, mine.root, em, et, rt);
      for(auto const & k: r.taken) put(stdout, cat("взято у них: ", k, "\n"));
      for(auto const & c: r.conflicts)
        put(stdout, cat("конфликт: ", c.key, "\n  база: ", c.base.empty() ? "—" : c.base, "\n  мой:  ", c.mine.empty() ? "—" : c.mine, "\n  их:   ", c.theirs.empty() ? "—" : c.theirs, "\n"));
      std::string out = argc >= 5 ? argv[4] : argv[2];
      tz::save_db(mine.db, mine.root, out);
      put(stdout, cat("записано: ", out, r.conflicts.empty() ? "" : " (конфликты — мой вариант; слить руками)", "\n"));
      return r.conflicts.empty() ? 0 : 1;
    }
    // Renew the process from the dump - "to another VM" (M5): another tz process.
    if(cmd == "resume") {
      tz::os::mapped_file f(argv[2]);
      int fd = ::open(argv[2], O_RDONLY | O_CLOEXEC); // Dump pages - displaying the file
      tz::restored_process r(f.text(), fd);
      r.m->run();
      return int(r.m->st.exit_code);
    }
    if(cmd == "log") {
      tz::node_id root;
      put(stdout, tz::describe_journal(tz::read_journal(argv[2], root)));
      return 0;
    }

    int at = 2;
    bool show_instances = false, use_jit = false, jit_stats = false, machine = false, check_a64 = false;
    uint64_t dump_after = 0;
    std::string dump_path;
    while(cmd == "run" && at < argc && std::string_view(argv[at]).starts_with("--")) {
      std::string_view f = argv[at++];
      if(f == "--instances") show_instances = true;
      else if(f == "--jit") use_jit = true;
      else if(f == "--jit-stats") use_jit = jit_stats = true;
      else if(f == "--a64") use_jit = check_a64 = true; // code AArch64 - parallel to host, in qemu
      else if(f == "--machine") machine = true;
      else if(f == "--dump-after" && at + 1 < argc) { // -dump-after N file.tzproc
        machine = true;
        std::string_view n = argv[at++];
        std::from_chars(n.data(), n.data() + n.size(), dump_after);
        dump_path = argv[at++];
      } else return usage();
    }
    if(at >= argc) return usage();

    program p;
    load(p, argv[at++]);
    for(auto const & e: p.parse_errors) put(stderr, tz::render(e, p.path, p.db));
    bool parsed = p.parse_errors.empty();

    if(cmd == "save") {
      if(at >= argc) return usage();
      tz::save_db(p.db, p.root, argv[at]);
      return parsed ? 0 : 1;
    }
    if(cmd == "fmt") {
      tz::printer pr(p.db);
      if(!projection.empty()) {
        auto errors = tz::preload_uses(p.db, cat("use ", projection, ";"), std::filesystem::path(p.path).parent_path());
        for(auto const & e: errors) put(stderr, tz::render(e, projection, p.db));
        auto it = p.db.strings.index.find(projection);
        if(it != p.db.strings.index.end()) pr.projection = tz::find_module(p.db, it->second);
        if(!pr.projection) {
          put(stderr, cat("нет модуля проекции ", projection, "\n"));
          return 1;
        }
      }
      put(stdout, pr.print(p.root));
      for(auto const & n: pr.notes) put(stderr, cat(n, "\n")); // The choice of printing language fell - conservatively
      return parsed ? 0 : 1;
    }
    if(cmd == "fmt-ct") { // tz fmt --ct: after elaboration (ct code mutation generated by emit)
      if(!parsed) return 1;
      put(stdout, tz::elaborated_text(p.db, p.root, p.path));
      return 0;
    }
    if(cmd == "ast") {
      put(stdout, tz::dumper(p.db).dump(p.root));
      return parsed ? 0 : 1;
    }
    if(cmd == "slice") {
      if(at + 1 >= argc) return usage();
      auto out = tz::slice_view(p.db).print(p.root, argv[at], argv[at + 1]);
      if(out.empty()) {
        put(stderr, cat("не найдены функция ", argv[at], " или переменная ", argv[at + 1], "\n"));
        return 1;
      }
      put(stdout, out);
      return 0;
    }
    if(!parsed) return 1; // We don't do the whole thing - M0

    // Program editing function in language (§7.4): tz apply program function [base.tzdb] - new
    // projection in stdout, base - with journal editing.
    if(cmd == "apply") {
      if(at >= argc) return usage();
      tz::vm_session session(p.db, p.root, p.path);
      std::string r = session.exec(cat("apply ", argv[at]));
      put(stderr, r);
      if(!r.starts_with("применена")) return 1;
      put(stdout, tz::printer(p.db).print(p.root));
      if(at + 1 < argc) tz::save_db(p.db, p.root, argv[at + 1]);
      return 0;
    }
    if(cmd == "vm") {
      tz::vm_session session(p.db, p.root, p.path);
      auto & in = tz::os::host_stdin();
      for(std::string line; in.next(line);) {
        if(line.empty() || line.starts_with("//")) continue;
        if(line == "quit") break;
        std::string body;
        if(line.starts_with("edit ")) // editing body - up to a line from one point
          for(std::string l; in.next(l) && l != ".";) body += l + "\n";
        put(stdout, cat("> ", line, "\n", session.exec(line, body)));
        std::fflush(stdout);
      }
      return 0;
    }

    if(cmd == "serve") {
      if(at >= argc) return usage();
      tz::vm_session session(p.db, p.root, p.path);
      tz::editor_server server(session);
      server.listen(argv[at]);
      put(stderr, cat("tz serve: ", p.path, " на ", argv[at], "\n"));
      for(;;) server.step();
    }
    // Process machine (M5): execution without recursion, memory - data; c -dump-after
    // - stop after N steps and write down the dump (tz resume).
    if(cmd == "run" && machine) {
      tz::process_machine m(p.db, p.root, p.path);
      try {
        m.start(std::vector<std::string>(argv + at, argv + argc)); // parsing before execution - its errors with place
        if(dump_path.empty()) m.run();
        else {
          m.run(dump_after);
          if(m.st.done) put(stderr, cat("процесс закончился раньше шага ", dump_after, "\n"));
          else {
            tz::os::write_file(dump_path, m.dump());
            put(stderr, cat("дамп: ", dump_path, " (шаг ", m.st.steps, ")\n"));
          }
          return 0;
        }
      } catch(tz::diagnostic const & d) {
        return report_error(p, d);
      }
      put(stderr, cat("шагов: ", m.st.steps, "\n"));
      return int(m.st.exit_code);
    }
    if(cmd == "run") {
      std::vector<std::string> args{p.path};
      args.insert(args.end(), argv + at, argv + argc);
      tz::interpreter vm(p.db, p.root, std::move(args));
      tz::jit_engine jit(p.db, p.root);
      if(use_jit) vm.use_jit(&jit);
      tz::a64_check a64(p.db, p.root);
      if(check_a64) vm.a64 = &a64;
      int code = 0;
      try {
        code = vm.run_main();
      } catch(tz::diagnostic const & d) {
        code = report_error(p, d);
      }
      if(show_instances) put(stderr, cat("экземпляры:\n", vm.instances()));
      if(jit_stats) put(stderr, cat("JIT:\n", jit.stats()));
      if(check_a64) {
        put(stderr, a64.report());
        if(!a64.mismatches.empty() && !code) code = 1;
      }
      return code;
    }
    if(cmd == "check") {
      tz::interpreter vm(p.db, p.root, {p.path}, tz::mode::check);
      int code = 0;
      try {
        vm.run_main();
      } catch(tz::diagnostic const & d) {
        code = report_error(p, d);
      }
      put(stdout, vm.report(p.path, p.src()));
      return code;
    }
    if(cmd == "residual") {
      tz::interpreter::known_inputs known;
      std::vector<std::string> args{p.path};
      for(int i = at; i < argc; ++i) {
        if(std::string_view(argv[i]) == "--stdin" && i + 1 < argc) known.stdin_lines.push_back(argv[++i]);
        else args.push_back(argv[i]);
      }
      tz::interpreter vm(p.db, p.root, args, tz::mode::check, known);
      int code = 0;
      try {
        vm.run_main();
      } catch(tz::diagnostic const & d) {
        code = report_error(p, d);
      }
      put(stdout, tz::residual_view(p.db, vm).print(p.root));
      return code;
    }
    return usage();
  } catch(tz::diagnostic const & d) {
    put(stderr, cat(d.what(), "\n"));
    return 1;
  } catch(std::exception const & e) {
    put(stderr, cat(e.what(), "\n"));
    return 1;
  }
}
