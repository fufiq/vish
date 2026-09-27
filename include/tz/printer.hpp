// Text projection: AST base → canonical text (§4 sequel, §9.4).
//
// projection is deterministic: the same AST is always printed the same way. It is
// also used by slice and residual program (§13.2) - they just need to pass the node
// filter.
#pragma once

#include "ast.hpp"

#include <functional>

namespace tz {

class printer {
public:
    // keep(n) == False - node drops out of projection (slice).
    using filter = std::function<bool(node_id)>;

    explicit printer(const ast_db& db, filter keep = {}) : db_(db), keep_(std::move(keep)) {}

    std::string print(node_id n) {
        out_.clear();
        item(n);
        return out_;
    }

private:
    bool kept(node_id n) const { return !keep_ || keep_(n); }

    std::string_view str(symbol s) const { return db_.strings.text(s); }

    void line() {
        out_ += '\n';
        out_.append(indent_ * 2, ' ');
    }

    void item(node_id n) {
        if (!n || !kept(n)) return;
        std::visit([&](const auto& k) { print_node(n, k); }, db_[n].kind);
    }

    // --- instruction ---

    void print_node(node_id, const n_module& m) {
        if (m.name) out_ += std::format("module {};\n", str(m.name));
        bool first = !m.name;
        for (node_id i : m.items) {
            if (!kept(i)) continue;
            if (!first) out_ += '\n';
            first = false;
            item(i);
            if (!db_.is<n_fn>(i)) terminate(i);
            out_ += '\n';
        }
    }

    void print_node(node_id, const n_fn& f) {
        if (f.outline) out_ += "outline ";
        if (f.result_type) { item(f.result_type); out_ += ' '; }
        out_ += str(f.name);
        params(f.params);
        if (f.expr_body) {
            out_ += " => ";
            item(f.body);
            out_ += ';';
        } else {
            out_ += ' ';
            item(f.body);
        }
    }

    void params(const node_list& ps) {
        out_ += '(';
        for (u32 i = 0; i < ps.size(); ++i) {
            if (i) out_ += ", ";
            item(ps[i]);
        }
        out_ += ')';
    }

    void print_node(node_id, const n_param& p) {
        static constexpr std::array modes{"", "mut ", "own ", "out ", "expr ", "ct "};
        out_ += modes[u32(p.mode)];
        if (p.type) { item(p.type); out_ += p.pack ? "... " : " "; }
        out_ += str(p.name);
    }

    void print_node(node_id, const n_var& v) {
        if (v.is_const) out_ += "const ";
        item(v.type);
        out_ += std::format(" {} = ", str(v.name));
        item(v.init);
        out_ += ';';
    }

    void print_node(node_id, const n_block& b) {
        std::vector<node_id> items;
        for (node_id i : b.items) if (kept(i)) items.push_back(i);
        if (items.empty()) { out_ += "{}"; return; }
        out_ += '{';
        ++indent_;
        for (u32 i = 0; i < items.size(); ++i) {
            line();
            item(items[i]);
            bool value = b.has_value && items[i] == b.items.back();
            if (!value) terminate(items[i]);
        }
        --indent_;
        line();
        out_ += '}';
    }

    // `;` after the instruction, unless it ends in a block and puts it itself.
    void terminate(node_id s) {
        if (db_.is<n_var>(s) || db_.is<n_fn>(s) || db_.is<n_require>(s) || db_.is<n_return>(s)) return;
        if (db_.is<n_hole>(s)) return; // The hole prints the source code as it is.
        if (db_.is<n_for>(s) || db_.is<n_while>(s) || db_.is<n_block>(s) || db_.is<n_try_compile>(s)) return;
        if (db_.is<n_if>(s) && ends_in_block(s)) return;
        if (db_.is<n_call>(s)) {
            const auto& c = db_.as<n_call>(s);
            if (!c.args.empty() && db_.is<n_block>(c.args.back())) return;
        }
        out_ += ';';
    }

    bool ends_in_block(node_id n) const {
        const auto& i = db_.as<n_if>(n);
        node_id     last = i.else_ ? i.else_ : i.then_;
        if (db_.is<n_if>(last)) return ends_in_block(last);
        // Branch-instruction itself puts `;`
        return db_.is<n_block>(last) || db_.is<n_return>(last) || db_.is<n_require>(last);
    }

    void print_node(node_id, const n_return& r) {
        out_ += "return";
        if (r.target) { out_ += '('; item(r.target); out_ += ')'; }
        if (r.value) { out_ += ' '; item(r.value); }
        out_ += ';';
    }

    void print_node(node_id, const n_require& r) {
        out_ += "require(";
        item(r.cond);
        out_ += ");";
    }

    void print_node(node_id, const n_try_compile& t) {
        out_ += "try_compile ";
        item(t.body);
    }

    void print_node(node_id, const n_for& f) {
        out_ += std::format("for(auto {} : ", str(f.var));
        item(f.range);
        out_ += ") ";
        item(f.body);
        if (!db_.is<n_block>(f.body)) terminate(f.body);
    }

    void print_node(node_id, const n_while& w) {
        out_ += "while(";
        item(w.cond);
        out_ += ") ";
        item(w.body);
        if (!db_.is<n_block>(w.body)) terminate(w.body);
    }

    // --- expression ---

    void print_node(node_id, const n_if& i) {
        out_ += "if(";
        item(i.cond);
        out_ += ") ";
        item(i.then_);
        if (i.else_) {
            if (db_.is<n_block>(i.then_)) out_ += " else ";
            else out_ += " else ";
            item(i.else_);
        }
    }

    void print_node(node_id, const n_ident& i) { out_ += str(i.name); }
    void print_node(node_id, const n_auto&) { out_ += "auto"; }
    void print_node(node_id, const n_int& i) { out_ += std::to_string(i.value); }

    void print_node(node_id, const n_float& f) {
        auto s = std::format("{}", f.value);
        if (!s.contains('.') && !s.contains('e')) s += ".0";
        out_ += s;
    }

    void print_node(node_id, const n_str& s) {
        out_ += '"';
        for (char c : s.value) {
            if (c == '"' || c == '\\') { out_ += '\\'; out_ += c; }
            else if (c == '\n') out_ += "\\n";
            else out_ += c;
        }
        out_ += '"';
    }

    void print_node(node_id n, const n_op& o) {
        auto op = str(o.op);
        if (o.args.size() == 1) {
            if (op == "?") { item(o.args[0]); out_ += '?'; return; }
            out_ += op;
            operand(n, o.args[0]);
            return;
        }
        operand(n, o.args[0]);
        out_ += std::format(" {} ", op);
        operand(n, o.args[1]);
    }

    // Brackets around the operand - only if its operator connects weaker.
    void operand(node_id parent, node_id child) {
        bool paren = db_.is<n_op>(child) && db_.as<n_op>(child).args.size() == 2
                  && prec(db_.as<n_op>(child).op) < prec(db_.as<n_op>(parent).op);
        paren = paren || (db_.is<n_op>(child) && db_.as<n_op>(parent).args.size() == 1 && db_.as<n_op>(child).args.size() == 2);
        paren = paren || db_.is<n_if>(child);
        if (paren) out_ += '(';
        item(child);
        if (paren) out_ += ')';
    }

    int prec(symbol op) const {
        auto s = str(op);
        if (s == "=" || s == "+=" || s == "-=" || s == "*=" || s == "/=") return 1;
        if (s == "||") return 2;
        if (s == "&&") return 3;
        if (s == "==" || s == "!=" || s == "<" || s == ">" || s == "<=" || s == ">=") return 4;
        if (s == "+" || s == "-") return 6;
        if (s == "*" || s == "/" || s == "%") return 7;
        return 9;
    }

    void print_node(node_id, const n_call& c) {
        item(c.callee);
        out_ += '(';
        u32 n = u32(c.args.size());
        bool trailing = n > 0 && db_.is<n_block>(c.args.back());
        for (u32 i = 0; i < n - (trailing ? 1 : 0); ++i) {
            if (i) out_ += ", ";
            item(c.args[i]);
        }
        out_ += ')';
        if (trailing) { out_ += ' '; item(c.args.back()); }
    }

    void print_node(node_id, const n_member& m) {
        item(m.object);
        out_ += '.';
        out_ += str(m.name);
    }

    void print_node(node_id, const n_index& i) {
        item(i.object);
        out_ += '[';
        item(i.index);
        out_ += ']';
    }

    void print_node(node_id, const n_ct& c) {
        out_ += "ct ";
        item(c.body);
    }

    void print_node(node_id, const n_lambda& l) {
        params(l.params);
        out_ += " => ";
        item(l.body);
    }

    void print_node(node_id, const n_construct& c) {
        item(c.type);
        out_ += '{';
        for (u32 i = 0; i < c.items.size(); ++i) {
            if (i) out_ += ", ";
            item(c.items[i]);
        }
        out_ += '}';
    }

    void print_node(node_id, const n_hole& h) { out_ += std::format("/* дыра: {} */ {}", h.reason, h.text); }

    const ast_db& db_;
    filter        keep_;
    std::string   out_;
    u32           indent_ = 0;
};

} // namespace tz
