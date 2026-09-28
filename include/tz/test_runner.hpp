// Reference test runner: `tz test <каталог>`. No external dependencies.
//
// Comments at the beginning of the .tz file:
// // args: a b c arguments of the program
// // stdin: 1 2 3 line stdin (may be several)
// // out: text expected line stdout (in order)
// // error: adjustment expected error (diagnosis adjustment)
// // instances: dot(...) expected instance
// // check: substrate expected line of `tz check` report
// Check-error: Predicted static parsing error
// // residual: ab construct residual program for arguments
// // residual-has: text... it has a line
// // residual-lacks: text ... there is no line in it
// // slice: function variable
// // slice-has: text / slice-lacks: text
// // fmt-stable check fmtffmt = fmt
// // store-roundtrip save to .tzdb, download: same projection and same output
#pragma once

#include "interp.hpp"
#include "os.hpp"
#include "parser.hpp"
#include "printer.hpp"
#include "views.hpp"
#include "store.hpp"
#include "session.hpp"
#include "jit/jit.hpp"

#include <filesystem>

namespace tz {

struct test_runner {
  int run(std::string const & dir) {
    std::vector<std::filesystem::path> files;
    for(auto & e: std::filesystem::directory_iterator(dir))
      if(e.path().extension() == ".tz" || e.path().extension() == ".session") files.push_back(e.path());
    std::ranges::sort(files);
    uint32_t passed = 0;
    for(auto & f: files) {
      std::string why = run_one(f);
      if(why.empty()) {
        ++passed;
        put(stdout, cat("  ok   ", f.filename().string(), "\n"));
      } else {
        put(stdout, cat("  FAIL ", f.filename().string(), "\n", why, "\n"));
      }
    }
    put(stdout, cat(passed, "/", files.size(), " тестов прошло\n"));
    return passed == files.size() ? 0 : 1;
  }
  struct expect {
    std::vector<std::string> args, stdin_lines, out, errors, instances, check, check_lacks, check_errors;
    std::vector<std::string> residual, residual_has, residual_lacks, slice, slice_has, slice_lacks, jit_compiled, unknown;
    bool fmt_stable = false;
    bool store_roundtrip = false;
  };

  static expect read_header(std::string const & src) {
    expect e;
    for(std::string_view line: lines_of(src)) {
      if(!line.starts_with("//")) {
        if(line.empty()) continue;
        break;
      }
      auto body = line.substr(2);
      while(body.starts_with(' ')) body.remove_prefix(1);
      auto take = [&](std::string_view key, std::vector<std::string> & to) {
        if(!body.starts_with(key)) return false;
        auto rest = body.substr(key.size());
        while(rest.starts_with(' ')) rest.remove_prefix(1);
        to.emplace_back(rest);
        return true;
      };
      std::vector<std::string> args_line;
      if(take("args:", args_line)) {
        for(auto a: words_of(args_line[0])) e.args.emplace_back(a);
      } else if(take("stdin:", e.stdin_lines) || take("out:", e.out) || take("error:", e.errors) || take("instances:", e.instances) || take("check-error:", e.check_errors) || take("check-lacks:", e.check_lacks) || take("check:", e.check) || take("residual-has:", e.residual_has) || take("residual-lacks:", e.residual_lacks) || take("residual:", e.residual) || take("slice-has:", e.slice_has) || take("slice-lacks:", e.slice_lacks) || take("slice:", e.slice) || take("jit-compiled:", e.jit_compiled)) {
      } else if(body.starts_with("fmt-stable")) {
        e.fmt_stable = true;
      } else if(body.starts_with("store-roundtrip")) {
        e.store_roundtrip = true;
      } else if(auto c = body.find(':'); c != std::string::npos && c > 0 && std::ranges::all_of(body.substr(0, c), [](char ch) { return (ch >= 'a' && ch <= 'z') || ch == '-'; })) {
        e.unknown.emplace_back(body.substr(0, c)); // a typo in the directive shall not silently turn off the check
      }
    }
    return e;
  }

  // Scenario of the VM session: commands and after each waiting for its conclusion.
  // // program: file.tz program (relative to script directory)
  // // expect: substring // expect-not: substring
  std::string run_session(std::filesystem::path const & path) {
    os::mapped_file text(path.string());
    std::vector<std::string> lines;
    for(auto l: lines_of(text.text())) lines.emplace_back(l);
    ast_db db;
    node_id root;
    std::optional<vm_session> session;
    std::string last, why;
    for(std::size_t i = 0; i < lines.size(); ++i) {
      auto const & l = lines[i];
      if(l.starts_with("// program:")) {
        auto file = path.parent_path() / std::string_view(l).substr(11 + (l.size() > 11 && l[11] == ' '));
        os::mapped_file f(file.string());
        parser p(db, f.text());
        root = p.parse_module();
        session.emplace(db, root, file.filename().string());
        continue;
      }
      if(l.starts_with("// expect:")) {
        auto want = std::string_view(l).substr(10);
        while(want.starts_with(' ')) want.remove_prefix(1);
        if(!last.contains(want)) why += cat("    строка ", i + 1, ": нет ", in_quotes(want), " в выводе:\n", last);
        continue;
      }
      if(l.starts_with("// expect-not:")) {
        auto bad = std::string_view(l).substr(14);
        while(bad.starts_with(' ')) bad.remove_prefix(1);
        if(last.contains(bad)) why += cat("    строка ", i + 1, ": лишнее ", in_quotes(bad), " в выводе:\n", last);
        continue;
      }
      if(l.empty() || l.starts_with("//") || !session) continue;
      std::string body;
      if(l.starts_with("edit "))
        while(++i < lines.size() && lines[i] != ".") body += lines[i] + "\n";
      last = session->exec(l, body);
    }
    return why;
  }

  std::string run_one(std::filesystem::path const & path) {
    if(path.extension() == ".session") return run_session(path);
    os::mapped_file file(path.string());
    std::string src(file.text());
    expect e = read_header(src);

    ast_db db;
    parser p(db, src);
    node_id root = p.parse_module();

    std::string errors;
    for(auto const & d: p.errors()) errors += render(d, path.filename().string(), db);

    if(e.fmt_stable) {
      std::string once = printer(db).print(root);
      ast_db db2;
      parser p2(db2, once);
      std::string twice = printer(db2).print(p2.parse_module());
      if(once != twice) return "    проекция нестабильна: fmt∘fmt ≠ fmt\n";
    }

    // stdin - queue of interpreter lines (as in a session), output - in a row
    auto input = [&] { return interpreter::known_inputs{e.stdin_lines}; };

    std::string out;
    std::string instances;
    if(errors.empty()) {
      std::vector<std::string> args{path.filename().string()};
      args.insert(args.end(), e.args.begin(), e.args.end());
      interpreter vm(db, root, args, mode::run, input());
      vm.capture_output(&out);
      try {
        vm.run_main();
      } catch(diagnostic const & d) {
        errors += render(d, path.filename().string(), db);
      }
      instances = vm.instances();
    }

    std::string why;
    for(auto & d: e.unknown) why += cat("    неизвестная директива теста: ", d, "\n");
    // Differential (§10.5): With JIT, the output and error locations coincide with the interpreter.
    if(p.errors().empty()) {
      std::string out3, err3;
      std::vector<std::string> args{path.filename().string()};
      args.insert(args.end(), e.args.begin(), e.args.end());
      interpreter vm3(db, root, args, mode::run, input());
      jit_engine jit(db, root);
      vm3.use_jit(&jit);
      vm3.capture_output(&out3);
      try {
        vm3.run_main();
      } catch(diagnostic const & d) {
        err3 = render(d, path.filename().string(), db);
      }
      auto first_loc = [](std::string const & t) { return t.substr(0, t.find(": ошибка")); };
      if(out3 != out) why += cat("    JIT: вывод отличается: ", in_quotes(out3), " против ", in_quotes(out), "\n", jit.stats());
      for(auto & fn: e.jit_compiled)
        if(!jit.stats().contains(cat("jit  ", fn, "("))) why += cat("    JIT не скомпилировал ", fn, ":\n", jit.stats());
      if(first_loc(err3) != first_loc(errors) && !(errors.empty() && err3.empty()))
        why += cat("    JIT: ошибка отличается:\n", err3, "против\n", errors);
    }
    if(!e.check.empty() || !e.check_lacks.empty() || !e.check_errors.empty()) {
      std::string check_errors, report;
      if(p.errors().empty()) {
        interpreter chk(db, root, {path.filename().string()}, mode::check);
        try {
          chk.run_main();
        } catch(diagnostic const & d) {
          check_errors = render(d, path.filename().string(), db);
        }
        report = chk.report(path.filename().string(), src);
      }
      for(auto & want: e.check)
        if(!report.contains(want)) why += cat("    нет в отчёте check: ", in_quotes(want), "\n", report);
      for(auto & bad: e.check_lacks)
        if(report.contains(bad)) why += cat("    лишнее в отчёте check: ", in_quotes(bad), "\n", report);
      for(auto & want: e.check_errors)
        if(!check_errors.contains(want)) why += cat("    нет ошибки check: ", in_quotes(want), "; есть: ", check_errors, "\n");
      if(e.check_errors.empty() && !check_errors.empty()) why += "    неожиданная ошибка check:\n" + check_errors;
    }
    if(!e.residual.empty() && p.errors().empty()) {
      std::vector<std::string> args{path.filename().string()};
      for(auto a: words_of(e.residual[0])) args.emplace_back(a);
      interpreter vm(db, root, args, mode::check, interpreter::known_inputs{});
      try {
        vm.run_main();
      } catch(diagnostic const &) {
      }
      std::string text = residual_view(db, vm).print(root);
      for(auto & want: e.residual_has)
        if(!text.contains(want)) why += cat("    нет в остаточной программе: ", in_quotes(want), "\n", text);
      for(auto & bad: e.residual_lacks)
        if(text.contains(bad)) why += cat("    лишнее в остаточной программе: ", in_quotes(bad), "\n", text);
    }
    if(!e.slice.empty() && p.errors().empty()) {
      auto w = words_of(e.slice[0]);
      std::string fn(w.size() > 0 ? w[0] : ""), var(w.size() > 1 ? w[1] : "");
      std::string text = slice_view(db).print(root, fn, var);
      for(auto & want: e.slice_has)
        if(!text.contains(want)) why += cat("    нет в срезе: ", in_quotes(want), "\n", text);
      for(auto & bad: e.slice_lacks)
        if(text.contains(bad)) why += cat("    лишнее в срезе: ", in_quotes(bad), "\n", text);
    }
    if(e.store_roundtrip) {
      auto tmp = std::filesystem::temp_directory_path() / cat("tz-test-", path.stem().string(), ".tzdb");
      save_db(db, root, tmp.string());
      ast_db db2;
      node_id root2 = load_db(db2, tmp.string());
      std::filesystem::remove(tmp);
      if(printer(db).print(root) != printer(db2).print(root2)) why += "    проекция после загрузки базы отличается\n";
      std::string out2;
      std::vector<std::string> args{path.filename().string()};
      args.insert(args.end(), e.args.begin(), e.args.end());
      interpreter vm2(db2, root2, args, mode::run, input());
      vm2.capture_output(&out2);
      try {
        vm2.run_main();
      } catch(diagnostic const &) {
      }
      if(out2 != out) why += cat("    вывод из базы отличается: ", in_quotes(out2), " против ", in_quotes(out), "\n");
    }
    uint32_t i = 0;
    for(std::string_view line: lines_of(out)) {
      if(i >= e.out.size()) {
        why += cat("    лишний вывод: ", line, "\n");
        continue;
      }
      if(line != e.out[i]) why += cat("    строка ", i + 1, ": ожидалось ", in_quotes(e.out[i]), ", получено ", in_quotes(line), "\n");
      ++i;
    }
    for(; i < e.out.size(); ++i) why += cat("    не выведено: ", in_quotes(e.out[i]), "\n");
    for(auto & want: e.errors)
      if(!errors.contains(want)) why += cat("    нет ожидаемой ошибки: ", in_quotes(want), "\n");
    if(e.errors.empty() && !errors.empty()) why += "    неожиданная ошибка:\n" + errors;
    for(auto & want: e.instances)
      if(!instances.contains(want)) why += cat("    нет экземпляра ", in_quotes(want), "; есть:\n", instances);
    return why;
  }
};

} // namespace tz
