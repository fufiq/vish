// tz - VM king. One unit of translation: all code is headers (HO-Build).
#include "tz/base.hpp"
#include "tz/lexer.hpp"
#include "tz/ast.hpp"
#include "tz/parser.hpp"
#include "tz/printer.hpp"
#include "tz/dump.hpp"

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
                         "  tz ast <файл.tz>   напечатать дерево AST");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    std::string cmd  = argv[1];
    std::string path = argv[2];
    try {
        std::string src = read_file(path);
        tz::ast_db  db;
        tz::parser  p(db, src);
        tz::node_id root = p.parse_module();
        for (const auto& e : p.errors()) std::print(stderr, "{}", tz::render(e, path, src));

        if (cmd == "fmt") std::print("{}", tz::printer(db).print(root));
        else if (cmd == "ast") std::print("{}", tz::dumper(db).dump(root));
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
