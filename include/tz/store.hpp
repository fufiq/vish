// Storing an AST database on a disk: binary journal operations (.tzdb).
//
// Format: Magic "TZDB0001", root module number, then journal records, each
// serialized generically through reflection (serial.hpp).
#pragma once

#include "ast.hpp"
#include "serial.hpp"

#include <fstream>
#include <sstream>

namespace tz {

constexpr std::string_view tzdb_magic = "TZDB0001";

void save_db(const ast_db& db, node_id root, const std::string& path) {
    writer w;
    for (char c : tzdb_magic) w.put(c);
    w.put(root.value);
    for (const auto& e : db.journal()) w.put(e);
    std::ofstream out(path, std::ios::binary);
    if (!out) throw diagnostic(std::format("не могу записать {}", path), {});
    out << w.bytes;
}

std::vector<journal_entry> read_journal(const std::string& path, node_id& root) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw diagnostic(std::format("не могу открыть {}", path), {});
    std::stringstream ss;
    ss << in.rdbuf();
    std::string bytes = ss.str();
    reader      r(bytes);
    for (char c : tzdb_magic)
        if (r.get<char>() != c) throw diagnostic(std::format("{}: не файл базы tz", path), {});
    root = node_id(r.get<uint32_t>());
    std::vector<journal_entry> entries;
    while (!r.at_end()) entries.push_back(r.get<journal_entry>());
    return entries;
}

node_id load_db(ast_db& db, const std::string& path) {
    node_id root;
    db.replay(read_journal(path, root));
    return root;
}

// journal in text.
std::string describe_journal(const std::vector<journal_entry>& entries) {
    std::string out;
    for (const auto& e : entries) {
        out += std::format("r{:<5} ", e.revision);
        std::visit([&](const auto& op) {
            using T = std::remove_cvref_t<decltype(op)>;
            if constexpr (std::is_same_v<T, j_intern>) out += std::format("строка  #{} {:?}", op.id, op.text);
            else if constexpr (std::is_same_v<T, j_add>) {
                out += std::format("узел    #{} ", op.id);
                out += std::visit([](const auto& k) -> std::string_view {
                    return std::meta::identifier_of(std::meta::remove_cvref(^^decltype(k)));
                }, op.kind);
                out += std::format(" [{}..{}]", op.where.begin, op.where.end);
            } else if constexpr (std::is_same_v<T, j_replace>) out += std::format("замена  #{}", op.id);
            else if constexpr (std::is_same_v<T, j_link>) out += std::format("связи   от #{}", op.root);
            else out += std::format("импорт  текста {} байт", op.text.size());
        }, e.op);
        out += '\n';
    }
    return out;
}

} // namespace tz
