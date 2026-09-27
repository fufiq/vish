// tz - VM king. One unit of translation: all code is headers (HO-Build).
#include "tz/base.hpp"
#include "tz/lexer.hpp"
#include "tz/ast.hpp"
#include "tz/parser.hpp"
#include "tz/printer.hpp"
#include "tz/dump.hpp"
#include "tz/interp.hpp"
#include "tz/test_runner.hpp"

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
    std::println(stderr, "использование:\n"
                         "  tz fmt <файл.tz>   разобрать и напечатать каноническую проекцию\n"
                         "  tz ast <файл.tz>   напечатать дерево AST\n"
                         "  tz run [--instances] <файл.tz> [аргументы...]   исполнить main()\n"
                         "  tz test <каталог>  прогнать эталонные тесты");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    std::string cmd  = argv[1];
    if (cmd == "test") return tz::test_runner().run(argv[2]);
    int         at   = 2;
    bool        show_instances = false;
    if (cmd == "run" && std::string_view(argv[at]) == "--instances") { show_instances = true; ++at; }
    if (at >= argc) return usage();
    std::string path = argv[at++];
    try {
        std::string src = read_file(path);
        tz::ast_db  db;
        tz::parser  p(db, src);
        tz::node_id root = p.parse_module();
        for (const auto& e : p.errors()) std::print(stderr, "{}", tz::render(e, path, src));

        if (cmd == "fmt") std::print("{}", tz::printer(db).print(root));
        else if (cmd == "ast") std::print("{}", tz::dumper(db).dump(root));
        else if (cmd == "run") {
            if (!p.errors().empty()) return 1; // We do not perform the whole hole - M0
            std::vector<std::string> args{path};
            args.insert(args.end(), argv + at, argv + argc);
            tz::interpreter vm(db, root, std::move(args));
            int code = 0;
            try {
                code = vm.run_main();
            } catch (const tz::diagnostic& d) {
                std::print(stderr, "{}", tz::render(d, path, src));
                code = 1;
            }
            if (show_instances) std::print(stderr, "экземпляры:\n{}", vm.instances());
            return code;
        }
        else return usage();
        return p.errors().empty() ? 0 : 1;
    } catch (const tz::diagnostic& d) {
        std::println(stderr, "{}", d.what());
        return 1;
    } catch (const std::exception& e) {
        std::println(stderr, "{}", e.what());
        return 1;
    }
}
