// Debugging output of the AST tree is one implementation for all types of node
// through reflection: new types of node are printed without editing this file.
#pragma once

#include "ast.hpp"

namespace tz {

struct dumper {
    explicit dumper(const ast_db& db) : db(db) {}

    std::string dump(node_id n) {
        out.clear();
        visit(n, 0);
        return out;
    }
    void visit(node_id n, uint32_t depth) {
        out.append(depth * 2, ' ');
        if (!n) { out += "·\n"; return; }
        out += kind_name(db[n]);
        std::visit([&](const auto& k) { scalars(k); }, db[n].kind);
        out += '\n';
        std::visit([&](const auto& k) { children(k, depth + 1); }, db[n].kind);
    }

    void scalars(const auto& k) {
        template for (constexpr auto m : fields_of<std::remove_cvref_t<decltype(k)>>()) {
            using field_t = [:std::meta::type_of(m):];
            constexpr auto name = std::meta::identifier_of(m);
            if constexpr (std::is_same_v<field_t, symbol>) {
                out += std::format(" {}={}", name, db.strings.text(k.[:m:]));
            } else if constexpr (std::is_same_v<field_t, bool>) {
                if (k.[:m:]) out += std::format(" {}", name);
            } else if constexpr (std::is_same_v<field_t, int64_t> || std::is_same_v<field_t, double>) {
                out += std::format(" {}={}", name, k.[:m:]);
            } else if constexpr (std::is_same_v<field_t, std::string>) {
                out += std::format(" {}={:?}", name, k.[:m:]);
            } else if constexpr (std::is_enum_v<field_t>) {
                out += std::format(" {}={}", name, uint32_t(k.[:m:]));
            }
        }
    }

    void children(const auto& k, uint32_t depth) {
        template for (constexpr auto m : fields_of<std::remove_cvref_t<decltype(k)>>()) {
            using field_t = [:std::meta::type_of(m):];
            if constexpr (std::is_same_v<field_t, node_id>) {
                if (k.[:m:]) {
                    out.append(depth * 2, ' ');
                    out += std::format("{}:\n", std::meta::identifier_of(m));
                    visit(k.[:m:], depth + 1);
                }
            } else if constexpr (std::is_same_v<field_t, node_list>) {
                for (node_id c : k.[:m:]) visit(c, depth);
            }
        }
    }

    const ast_db& db;
    std::string   out;
};

} // namespace tz
