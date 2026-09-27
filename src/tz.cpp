// tz - VM king. One unit of translation: all code is headers (HO-Build).
//
// The source of truth is AST (D7). Any command accepts either text (.tz - import text
// projection to the database) or the base itself (.tzdb - journal).
#include "tz/base.hpp"
#include "tz/lexer.hpp"
#include "tz/ast.hpp"
#include "tz/parser.hpp"
#include "tz/printer.hpp"
#include "tz/dump.hpp"
#include "tz/interp.hpp"
#include "tz/test_runner.hpp"
#include "tz/views.hpp"
#include "tz/store.hpp"
#include "tz/session.hpp"

#include <iostream>

#include <fstream>
#include <sstream>

namespace {

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error(std::format("не могу открыть {}", path));
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int usage() {
    std::println(stderr, "использование (<программа> — файл .tz или база .tzdb):\n"
                         "  tz fmt <программа>        каноническая текстовая проекция\n"
                         "  tz ast <программа>        дерево AST\n"
                         "  tz run [--instances] <программа> [аргументы...]   исполнить main()\n"
                         "  tz check <программа>      статический разбор: стадии кт/рт, расщепления, экземпляры\n"
                         "  tz residual <программа> [--stdin СТРОКА]... [аргументы...]   программа для заданных входов\n"
                         "  tz slice <программа> <функция> <переменная>   только то, что связано с переменной\n"
                         "  tz save <файл.tz> <база.tzdb>   импортировать текст в базу (журнал на диск)\n"
                         "  tz log <база.tzdb>         журнал операций базы в тексте\n"
                         "  tz vm <программа>          сессия VM: команды из stdin (check, run, edit <функция> … ., report, save)\n"
                         "  tz test <каталог>          прогнать эталонные тесты");
    return 2;
}

// Downloaded program: base, root, text for diagnostics, errors parsing.
struct program {
    tz::ast_db                  db;
    tz::node_id                 root;
    std::vector<tz::diagnostic> parse_errors;
    std::string                 path;

    const std::string& src() const { return db.source(0); }
};

void load(program& p, const std::string& path) {
    p.path = path;
    if (path.ends_with(".tzdb")) {
        p.root = tz::load_db(p.db, path);
        return;
    }
    std::string text = read_file(path);
    tz::parser  parser(p.db, text);
    p.root         = parser.parse_module();
    p.parse_errors = parser.errors();
}

int report_error(const program& p, const tz::diagnostic& d) {
    std::print(stderr, "{}", tz::render(d, p.path, p.db));
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    std::string cmd = argv[1];
    try {
        if (cmd == "test") return tz::test_runner().run(argv[2]);
        if (cmd == "log") {
            tz::node_id root;
            std::print("{}", tz::describe_journal(tz::read_journal(argv[2], root)));
            return 0;
        }

        int  at             = 2;
        bool show_instances = false;
        if (cmd == "run" && std::string_view(argv[at]) == "--instances") { show_instances = true; ++at; }
        if (at >= argc) return usage();

        program p;
        load(p, argv[at++]);
        for (const auto& e : p.parse_errors) std::print(stderr, "{}", tz::render(e, p.path, p.db));
        bool parsed = p.parse_errors.empty();

        if (cmd == "save") {
            if (at >= argc) return usage();
            tz::save_db(p.db, p.root, argv[at]);
            return parsed ? 0 : 1;
        }
        if (cmd == "fmt") {
            std::print("{}", tz::printer(p.db).print(p.root));
            return parsed ? 0 : 1;
        }
        if (cmd == "ast") {
            std::print("{}", tz::dumper(p.db).dump(p.root));
            return parsed ? 0 : 1;
        }
        if (cmd == "slice") {
            if (at + 1 >= argc) return usage();
            auto out = tz::slice_view(p.db).print(p.root, argv[at], argv[at + 1]);
            if (out.empty()) {
                std::println(stderr, "не найдены функция {} или переменная {}", argv[at], argv[at + 1]);
                return 1;
            }
            std::print("{}", out);
            return 0;
        }
        if (!parsed) return 1; // We don't do the whole thing - M0

        if (cmd == "vm") {
            tz::vm_session session(p.db, p.root, p.path);
            for (std::string line; std::getline(std::cin, line);) {
                if (line.empty() || line.starts_with("//")) continue;
                if (line == "quit") break;
                std::string body;
                if (line.starts_with("edit ")) // editing body - up to a line from one point
                    for (std::string l; std::getline(std::cin, l) && l != ".";) body += l + "\n";
                std::print("> {}\n{}", line, session.exec(line, body));
                std::fflush(stdout);
            }
            return 0;
        }

        if (cmd == "run") {
            std::vector<std::string> args{p.path};
            args.insert(args.end(), argv + at, argv + argc);
            tz::interpreter vm(p.db, p.root, std::move(args));
            int code = 0;
            try {
                code = vm.run_main();
            } catch (const tz::diagnostic& d) {
                code = report_error(p, d);
            }
            if (show_instances) std::print(stderr, "экземпляры:\n{}", vm.instances());
            return code;
        }
        if (cmd == "check") {
            tz::interpreter vm(p.db, p.root, {p.path}, tz::mode::check);
            int code = 0;
            try {
                vm.run_main();
            } catch (const tz::diagnostic& d) {
                code = report_error(p, d);
            }
            std::print("{}", vm.report(p.path, p.src()));
            return code;
        }
        if (cmd == "residual") {
            tz::interpreter::known_inputs known;
            std::vector<std::string>      args{p.path};
            for (int i = at; i < argc; ++i) {
                if (std::string_view(argv[i]) == "--stdin" && i + 1 < argc) known.stdin_lines.push_back(argv[++i]);
                else args.push_back(argv[i]);
            }
            tz::interpreter vm(p.db, p.root, args, tz::mode::check, known);
            int code = 0;
            try {
                vm.run_main();
            } catch (const tz::diagnostic& d) {
                code = report_error(p, d);
            }
            std::print("{}", tz::residual_view(p.db, vm).print(p.root));
            return code;
        }
        return usage();
    } catch (const tz::diagnostic& d) {
        std::println(stderr, "{}", d.what());
        return 1;
    } catch (const std::exception& e) {
        std::println(stderr, "{}", e.what());
        return 1;
    }
}
