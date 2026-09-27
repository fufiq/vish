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
#include "parser.hpp"
#include "printer.hpp"
#include "views.hpp"
#include "store.hpp"
#include "session.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace tz {

class test_runner {
public:
    int run(const std::string& dir) {
        std::vector<std::filesystem::path> files;
        for (auto& e : std::filesystem::directory_iterator(dir))
            if (e.path().extension() == ".tz" || e.path().extension() == ".session") files.push_back(e.path());
        std::ranges::sort(files);
        u32 passed = 0;
        for (auto& f : files) {
            std::string why = run_one(f);
            if (why.empty()) {
                ++passed;
                std::println("  ok   {}", f.filename().string());
            } else {
                std::println("  FAIL {}\n{}", f.filename().string(), why);
            }
        }
        std::println("{}/{} тестов прошло", passed, files.size());
        return passed == files.size() ? 0 : 1;
    }

private:
    struct expect {
        std::vector<std::string> args, stdin_lines, out, errors, instances, check, check_errors;
        std::vector<std::string> residual, residual_has, residual_lacks, slice, slice_has, slice_lacks;
        bool                     fmt_stable = false;
        bool                     store_roundtrip = false;
    };

    static expect read_header(const std::string& src) {
        expect             e;
        std::istringstream in(src);
        std::string        line;
        while (std::getline(in, line)) {
            if (!line.starts_with("//")) {
                if (line.empty()) continue;
                break;
            }
            auto body = std::string_view(line).substr(2);
            while (body.starts_with(' ')) body.remove_prefix(1);
            auto take = [&](std::string_view key, std::vector<std::string>& to) {
                if (!body.starts_with(key)) return false;
                auto rest = body.substr(key.size());
                while (rest.starts_with(' ')) rest.remove_prefix(1);
                to.emplace_back(rest);
                return true;
            };
            std::vector<std::string> args_line;
            if (take("args:", args_line)) {
                std::istringstream as(args_line[0]);
                std::string        a;
                while (as >> a) e.args.push_back(a);
            } else if (take("stdin:", e.stdin_lines) || take("out:", e.out) || take("error:", e.errors)
                       || take("instances:", e.instances) || take("check-error:", e.check_errors) || take("check:", e.check)
                       || take("residual-has:", e.residual_has) || take("residual-lacks:", e.residual_lacks)
                       || take("residual:", e.residual) || take("slice-has:", e.slice_has) || take("slice-lacks:", e.slice_lacks)
                       || take("slice:", e.slice)) {
            } else if (body.starts_with("fmt-stable")) {
                e.fmt_stable = true;
            } else if (body.starts_with("store-roundtrip")) {
                e.store_roundtrip = true;
            }
        }
        return e;
    }

    // Scenario of the VM session: commands and after each waiting for its conclusion.
    // // program: file.tz program (relative to script directory)
    // // expect: substring // expect-not: substring
    std::string run_session(const std::filesystem::path& path) {
        std::ifstream               in(path);
        std::vector<std::string>    lines;
        for (std::string l; std::getline(in, l);) lines.push_back(l);
        ast_db                      db;
        node_id                     root;
        std::optional<vm_session>   session;
        std::string                 last, why;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const auto& l = lines[i];
            if (l.starts_with("// program:")) {
                auto file = path.parent_path() / std::string_view(l).substr(11 + (l.size() > 11 && l[11] == ' '));
                std::ifstream     f(file);
                std::stringstream ss;
                ss << f.rdbuf();
                parser p(db, ss.str());
                root = p.parse_module();
                session.emplace(db, root, file.filename().string());
                continue;
            }
            if (l.starts_with("// expect:")) {
                auto want = std::string_view(l).substr(10);
                while (want.starts_with(' ')) want.remove_prefix(1);
                if (!last.contains(want)) why += std::format("    строка {}: нет {:?} в выводе:\n{}", i + 1, want, last);
                continue;
            }
            if (l.starts_with("// expect-not:")) {
                auto bad = std::string_view(l).substr(14);
                while (bad.starts_with(' ')) bad.remove_prefix(1);
                if (last.contains(bad)) why += std::format("    строка {}: лишнее {:?} в выводе:\n{}", i + 1, bad, last);
                continue;
            }
            if (l.empty() || l.starts_with("//") || !session) continue;
            std::string body;
            if (l.starts_with("edit "))
                while (++i < lines.size() && lines[i] != ".") body += lines[i] + "\n";
            last = session->exec(l, body);
        }
        return why;
    }

    std::string run_one(const std::filesystem::path& path) {
        if (path.extension() == ".session") return run_session(path);
        std::ifstream in(path);
        std::stringstream ss;
        ss << in.rdbuf();
        std::string src = ss.str();
        expect      e   = read_header(src);

        ast_db  db;
        parser  p(db, src);
        node_id root = p.parse_module();

        std::string errors;
        for (const auto& d : p.errors()) errors += render(d, path.filename().string(), db);

        if (e.fmt_stable) {
            std::string once = printer(db).print(root);
            ast_db      db2;
            parser      p2(db2, once);
            std::string twice = printer(db2).print(p2.parse_module());
            if (once != twice) return "    проекция нестабильна: fmt∘fmt ≠ fmt\n";
        }

        // Stdin/stdout redirected to lines
        std::string stdin_text;
        for (auto& l : e.stdin_lines) stdin_text += l + "\n";
        std::istringstream fake_in(stdin_text);
        auto*              old_in = std::cin.rdbuf(fake_in.rdbuf());

        std::string out;
        std::string instances;
        if (errors.empty()) {
            std::vector<std::string> args{path.filename().string()};
            args.insert(args.end(), e.args.begin(), e.args.end());
            interpreter vm(db, root, args);
            vm.capture_output(&out);
            try {
                vm.run_main();
            } catch (const diagnostic& d) {
                errors += render(d, path.filename().string(), db);
            }
            instances = vm.instances();
        }
        std::cin.rdbuf(old_in);

        std::string why;
        if (!e.check.empty() || !e.check_errors.empty()) {
            std::string check_errors, report;
            if (p.errors().empty()) {
                interpreter chk(db, root, {path.filename().string()}, mode::check);
                try {
                    chk.run_main();
                } catch (const diagnostic& d) {
                    check_errors = render(d, path.filename().string(), db);
                }
                report = chk.report(path.filename().string(), src);
            }
            for (auto& want : e.check)
                if (!report.contains(want)) why += std::format("    нет в отчёте check: {:?}\n{}", want, report);
            for (auto& want : e.check_errors)
                if (!check_errors.contains(want)) why += std::format("    нет ошибки check: {:?}; есть: {}\n", want, check_errors);
            if (e.check_errors.empty() && !check_errors.empty()) why += "    неожиданная ошибка check:\n" + check_errors;
        }
        if (!e.residual.empty() && p.errors().empty()) {
            std::vector<std::string> args{path.filename().string()};
            std::istringstream       as(e.residual[0]);
            for (std::string a; as >> a;) args.push_back(a);
            interpreter vm(db, root, args, mode::check, interpreter::known_inputs{});
            try {
                vm.run_main();
            } catch (const diagnostic&) {
            }
            std::string text = residual_view(db, vm).print(root);
            for (auto& want : e.residual_has)
                if (!text.contains(want)) why += std::format("    нет в остаточной программе: {:?}\n{}", want, text);
            for (auto& bad : e.residual_lacks)
                if (text.contains(bad)) why += std::format("    лишнее в остаточной программе: {:?}\n{}", bad, text);
        }
        if (!e.slice.empty() && p.errors().empty()) {
            std::istringstream fs(e.slice[0]);
            std::string        fn, var;
            fs >> fn >> var;
            std::string text = slice_view(db).print(root, fn, var);
            for (auto& want : e.slice_has)
                if (!text.contains(want)) why += std::format("    нет в срезе: {:?}\n{}", want, text);
            for (auto& bad : e.slice_lacks)
                if (text.contains(bad)) why += std::format("    лишнее в срезе: {:?}\n{}", bad, text);
        }
        if (e.store_roundtrip) {
            auto tmp = std::filesystem::temp_directory_path() / std::format("tz-test-{}.tzdb", path.stem().string());
            save_db(db, root, tmp.string());
            ast_db  db2;
            node_id root2 = load_db(db2, tmp.string());
            std::filesystem::remove(tmp);
            if (printer(db).print(root) != printer(db2).print(root2)) why += "    проекция после загрузки базы отличается\n";
            std::istringstream fake2(stdin_text);
            auto*              old2 = std::cin.rdbuf(fake2.rdbuf());
            std::string        out2;
            std::vector<std::string> args{path.filename().string()};
            args.insert(args.end(), e.args.begin(), e.args.end());
            interpreter vm2(db2, root2, args);
            vm2.capture_output(&out2);
            try {
                vm2.run_main();
            } catch (const diagnostic&) {
            }
            std::cin.rdbuf(old2);
            if (out2 != out) why += std::format("    вывод из базы отличается: {:?} против {:?}\n", out2, out);
        }
        std::istringstream out_lines(out);
        std::string        line;
        u32                i = 0;
        while (std::getline(out_lines, line)) {
            if (i >= e.out.size()) { why += std::format("    лишний вывод: {}\n", line); continue; }
            if (line != e.out[i]) why += std::format("    строка {}: ожидалось {:?}, получено {:?}\n", i + 1, e.out[i], line);
            ++i;
        }
        for (; i < e.out.size(); ++i) why += std::format("    не выведено: {:?}\n", e.out[i]);
        for (auto& want : e.errors)
            if (!errors.contains(want)) why += std::format("    нет ожидаемой ошибки: {:?}\n", want);
        if (e.errors.empty() && !errors.empty()) why += "    неожиданная ошибка:\n" + errors;
        for (auto& want : e.instances)
            if (!instances.contains(want)) why += std::format("    нет экземпляра {:?}; есть:\n{}", want, instances);
        return why;
    }
};

} // namespace tz
