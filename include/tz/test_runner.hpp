// Reference test runner: `tz test <каталог>`. No external dependencies.
//
// Comments at the beginning of the .tz file:
// // args: a b c arguments of the program
// // stdin: 1 2 3 line stdin (may be several)
// // out: text expected line stdout (in order)
// // error: adjustment expected error (diagnosis adjustment)
// // instances: dot(...) expected instance
// // fmt-stable check fmtffmt = fmt
#pragma once

#include "interp.hpp"
#include "parser.hpp"
#include "printer.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace tz {

class test_runner {
public:
    int run(const std::string& dir) {
        std::vector<std::filesystem::path> files;
        for (auto& e : std::filesystem::directory_iterator(dir))
            if (e.path().extension() == ".tz") files.push_back(e.path());
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
        std::vector<std::string> args, stdin_lines, out, errors, instances;
        bool                     fmt_stable = false;
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
                       || take("instances:", e.instances)) {
            } else if (body.starts_with("fmt-stable")) {
                e.fmt_stable = true;
            }
        }
        return e;
    }

    std::string run_one(const std::filesystem::path& path) {
        std::ifstream in(path);
        std::stringstream ss;
        ss << in.rdbuf();
        std::string src = ss.str();
        expect      e   = read_header(src);

        ast_db  db;
        parser  p(db, src);
        node_id root = p.parse_module();

        std::string errors;
        for (const auto& d : p.errors()) errors += render(d, path.filename().string(), src);

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
                errors += render(d, path.filename().string(), src);
            }
            instances = vm.instances();
        }
        std::cin.rdbuf(old_in);

        std::string why;
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
