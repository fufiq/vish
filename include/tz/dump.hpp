// Debugging output of the AST tree is one implementation for all types of node
// through reflection: new types of node are printed without editing this file.
#pragma once

#include "ast.hpp"

namespace tz {

class dumper {
public:
    explicit dumper(const ast_db& db) : db_(db) {}

    std::string dump(node_id n) {
        out_.clear();
        visit(n, 0);
        return out_;
    }

private:
    void visit(node_id n, uint32_t depth) {
        out_.append(depth * 2, ' ');
        if (!n) { out_ += "·\n"; return; }
        out_ += kind_name(db_[n]);
        std::visit([&](const auto& k) { scalars(k); }, db_[n].kind);
        out_ += '\n';
        std::visit([&](const auto& k) { children(k, depth + 1); }, db_[n].kind);
    }

    void scalars(const auto& k) {
        template for (constexpr auto m : fields_of<std::remove_cvref_t<decltype(k)>>()) {
            using field_t = [:std::meta::type_of(m):];
            constexpr auto name = std::meta::identifier_of(m);
            if constexpr (std::is_same_v<field_t, symbol>) {
                out_ += std::format(" {}={}", name, db_.strings.text(k.[:m:]));
            } else if constexpr (std::is_same_v<field_t, bool>) {
                if (k.[:m:]) out_ += std::format(" {}", name);
            } else if constexpr (std::is_same_v<field_t, int64_t> || std::is_same_v<field_t, double>) {
                out_ += std::format(" {}={}", name, k.[:m:]);
            } else if constexpr (std::is_same_v<field_t, std::string>) {
                out_ += std::format(" {}={:?}", name, k.[:m:]);
            } else if constexpr (std::is_enum_v<field_t>) {
                out_ += std::format(" {}={}", name, uint32_t(k.[:m:]));
            }
        }
    }

    void children(const auto& k, uint32_t depth) {
        template for (constexpr auto m : fields_of<std::remove_cvref_t<decltype(k)>>()) {
            using field_t = [:std::meta::type_of(m):];
            if constexpr (std::is_same_v<field_t, node_id>) {
                if (k.[:m:]) {
                    out_.append(depth * 2, ' ');
                    out_ += std::format("{}:\n", std::meta::identifier_of(m));
                    visit(k.[:m:], depth + 1);
                }
            } else if constexpr (std::is_same_v<field_t, node_list>) {
                for (node_id c : k.[:m:]) visit(c, depth);
            }
        }
    }

    const ast_db& db_;
    std::string   out_;
};

} // namespace tz
