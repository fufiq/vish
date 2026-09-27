// parsing text projection to the AST base (§4 stack).
//
// Phase 1: table of brackets -- . Phase 2: operators. An error inside brackets turns
// only these brackets into n_hole; parsing continues after the closing bracket. Any
// text gives you AST.
//
//
// M0: The set of operators and their groups is sewn into a table. Definition of operators
// by code in language (§4.4) - later, the same table will be filled from the program.
#pragma once

#include "ast.hpp"
#include "lexer.hpp"

#include <algorithm>
#include <array>
#include <optional>

namespace tz {

class parser {
public:
    // The parser reads a copy of the text stored by the database when importing: tokens and
    // holes refer to it, not to the caller string (which may be temporary).
    parser(ast_db& db, std::string_view src, std::string label = {}) : db_(db) {
        source_ = db_.record_import(std::string(src), std::move(label));
        src_    = db_.source(source_);
        try {
            toks_ = lexer(src_).run();
        } catch (const diagnostic& d) {
            errors_.push_back(with_source(d));
            toks_.assign(1, token{});
        }
        match_brackets();
    }

    node_id parse_module() {
        u32       start = here().where.begin;
        n_module  m;
        if (is_word("module")) {
            advance();
            m.name = db_.strings.intern(dotted_name());
            expect_op(";");
        }
        while (!at_end()) m.items.push_back(recover_statement([&] { return item(); }));
        auto id = db_.add(std::move(m), {start, here().where.end});
        db_.link_parents(id);
        return id;
    }

    // parsing errors with import number in positions.
    std::vector<diagnostic> errors() const {
        std::vector<diagnostic> out;
        for (auto d : errors_) out.push_back(with_source(d));
        return out;
    }

private:
    // --- Phase 1: paired brackets ---

    void match_brackets() {
        match_.assign(toks_.size(), ~u32{0});
        std::vector<u32> stack;
        for (u32 i = 0; i < toks_.size(); ++i) {
            if (toks_[i].kind == tok::open) stack.push_back(i);
            else if (toks_[i].kind == tok::close) {
                // Unpaired Closing: Remove unclosed openers until suitable so that one lost `(`
                // does not break the enveloping `{ }`.
                auto fits = [&](u32 o) { return pairs(toks_[o].ch(), toks_[i].ch()); };
                if (std::ranges::none_of(stack, fits)) {
                    errors_.push_back(diagnostic("непарная скобка", toks_[i].where));
                    continue;
                }
                while (!fits(stack.back())) {
                    errors_.push_back(diagnostic("незакрытая скобка", toks_[stack.back()].where));
                    stack.pop_back();
                }
                match_[stack.back()] = i;
                match_[i]            = stack.back();
                stack.pop_back();
            }
        }
        for (u32 i : stack) errors_.push_back(diagnostic("незакрытая скобка", toks_[i].where));
    }

    static bool pairs(char o, char c) { return (o == '(' && c == ')') || (o == '[' && c == ']') || (o == '{' && c == '}'); }

    // --- navigation ---

    const token& here() const { return toks_[pos_]; }
    const token& ahead(u32 k) const { return toks_[std::min<u32>(pos_ + k, toks_.size() - 1)]; }
    bool         at_end() const { return here().kind == tok::end; }
    void         advance() { if (!at_end()) ++pos_; }

    bool is_word(std::string_view w, u32 k = 0) const { return ahead(k).kind == tok::ident && ahead(k).text == w; }
    bool is_open(char c, u32 k = 0) const { return ahead(k).kind == tok::open && ahead(k).ch() == c; }
    bool is_close(char c) const { return here().kind == tok::close && here().ch() == c; }

    // The operator in position k is the longest matching of glued symbols (`==` is
    // not read as `=` `=`, `=>` is not read as `=` `>`).
    bool is_op(std::string_view op, u32 k = 0) const { return longest_op(k) == op; }

    std::string_view longest_op(u32 k) const {
        static constexpr std::array ops{"...", "==", "!=", "<=", ">=", "&&", "||", "=>", "+=", "-=", "*=", "/=",
                                        "..", "=", "<", ">", "+", "-", "*", "/", "%", "!", "~", "?", ":",
                                        ".", ",", ";", "&", "|", "^", "@", "#", "$", "\\"};
        for (std::string_view o : ops) {
            bool ok = true;
            for (u32 i = 0; i < o.size() && ok; ++i) {
                const token& t = ahead(k + i);
                ok = t.kind == tok::op && t.ch() == o[i] && (i == 0 || t.glued);
            }
            if (ok) return o;
        }
        return {};
    }

    void eat_op(std::string_view op) { for (u32 i = 0; i < op.size(); ++i) advance(); }

    void expect_op(std::string_view op) {
        if (!is_op(op)) throw diagnostic(std::format("ожидалось '{}'", op), here().where);
        eat_op(op);
    }

    void expect_open(char c) {
        if (!is_open(c)) throw diagnostic(std::format("ожидалось '{}'", c), here().where);
        advance();
    }

    void expect_close(char c) {
        if (!is_close(c)) throw diagnostic(std::format("ожидалось '{}'", c), here().where);
        advance();
    }

    symbol ident() {
        if (here().kind != tok::ident) throw diagnostic("ожидалось имя", here().where);
        auto s = db_.strings.intern(here().text);
        advance();
        return s;
    }

    std::string dotted_name() {
        std::string name(here().text);
        ident();
        while (is_op(".")) {
            advance();
            name += '.';
            name += here().text;
            ident();
        }
        return name;
    }

    // --- Recovery: a hole instead of a failure ---

    // Disassemble one structure; in case of an error - n_hole to the end of the structure
    // (up to `;` at this level of brackets or to the end of the nearest group).
    template <class fn>
    node_id recover_statement(fn&& f) {
        u32 start_tok = pos_;
        try {
            return f();
        } catch (const diagnostic& d) {
            errors_.push_back(d);
            u32 start = toks_[start_tok].where.begin;
            pos_      = start_tok;
            skip_to_statement_end();
            u32 end = toks_[pos_ > 0 ? pos_ - 1 : 0].where.end;
            return db_.add(n_hole{std::string(src_.substr(start, end - start)), d.what()}, {start, end});
        }
    }

    void skip_to_statement_end() {
        while (!at_end()) {
            if (here().kind == tok::close) return;             // terminus
            if (here().kind == tok::open) {                    // skip the whole group
                u32 m = match_[pos_];
                if (m == ~u32{0}) { advance(); continue; }
                pos_ = m + 1;
                if (toks_[m].ch() == '}') return;              // block completes the design
                continue;
            }
            bool semi = is_op(";");
            advance();
            if (semi) return;
        }
    }

    // --- module elements and instructions ---

    node_id item() {
        if (auto f = try_fn_decl(false)) return *f;
        return statement();
    }

    // Announcement of function: [outline] [type] name(s) (=> e; | { ... }) On the
    // upper level, a form without the type on the left is allowed; in the block, only
    // with the sign of the declaration (§4.2).
    std::optional<node_id> try_fn_decl(bool in_block) {
        u32  k       = 0;
        bool outline = false;
        if (is_word("outline")) { outline = true; ++k; }
        // [type] name
        u32 name_at;
        if (ahead(k).kind == tok::ident && ahead(k + 1).kind == tok::ident && is_open('(', k + 2)) name_at = k + 1;
        else if (!in_block && ahead(k).kind == tok::ident && is_open('(', k + 1) && !is_keyword(ahead(k).text)) name_at = k;
        else return std::nullopt;
        // after ) shall be => or {
        u32 close = match_[pos_ + name_at + 1];
        if (close == ~u32{0}) return std::nullopt;
        u32 after = close + 1 - pos_;
        if (!is_op("=>", after) && !is_open('{', after)) return std::nullopt;

        u32  start = here().where.begin;
        n_fn f;
        f.outline = outline;
        if (outline) advance();
        if (name_at == (outline ? 2u : 1u)) f.result_type = type_expr();
        f.name = ident();
        f.params = param_list();
        if (is_op("=>")) {
            eat_op("=>");
            f.body      = expression();
            f.expr_body = true;
            if (is_op(";")) eat_op(";");
        } else {
            f.body = block();
        }
        return db_.add(std::move(f), {start, toks_[pos_ - 1].where.end});
    }

    static bool is_keyword(std::string_view w) {
        return w == "if" || w == "for" || w == "while" || w == "return" || w == "require" || w == "try_compile"
            || w == "ct" || w == "else" || w == "auto" || w == "const" || w == "outline" || w == "module";
    }

    node_list param_list() {
        expect_open('(');
        node_list ps;
        while (!is_close(')')) {
            ps.push_back(param());
            if (is_op(",")) eat_op(",");
            else break;
        }
        expect_close(')');
        return ps;
    }

    node_id param() {
        u32     start = here().where.begin;
        n_param p;
        if (is_word("mut")) { p.mode = param_mode::mut; advance(); }
        else if (is_word("own")) { p.mode = param_mode::own; advance(); }
        else if (is_word("out")) { p.mode = param_mode::out; advance(); }
        else if (is_word("expr")) { p.mode = param_mode::expr; advance(); }
        else if (is_word("ct")) { p.mode = param_mode::ct; advance(); }
        // [type] [...] name | name
        if (here().kind == tok::ident && (ahead(1).kind == tok::ident || is_op("...", 1))) {
            p.type = type_expr();
            if (is_op("...")) { eat_op("..."); p.pack = true; }
        }
        p.name = ident();
        return db_.add(std::move(p), {start, toks_[pos_ - 1].where.end});
    }

    node_id type_expr() {
        u32 start = here().where.begin;
        if (is_word("auto")) {
            advance();
            return db_.add(n_auto{}, {start, toks_[pos_ - 1].where.end});
        }
        return db_.add(n_ident{ident()}, {start, toks_[pos_ - 1].where.end});
    }

    node_id block() {
        u32 start = here().where.begin;
        expect_open('{');
        n_block b;
        while (!is_close('}') && !at_end()) {
            node_id s = recover_statement([&] { return statement(); });
            b.items.push_back(s);
            if (is_close('}') && last_was_value_) b.has_value = true;
        }
        expect_close('}');
        return db_.add(std::move(b), {start, toks_[pos_ - 1].where.end});
    }

    // Instructions. last_was_value_ is an expression without `;`.
    node_id statement() {
        last_was_value_ = false;
        u32 start       = here().where.begin;

        if (auto f = try_fn_decl(true)) return *f;

        if (is_word("return")) {
            advance();
            // return(цель) value - close; return (x) with space - value
            node_id target;
            if (is_open('(') && here().glued) {
                advance();
                u32 t0 = here().where.begin;
                target = db_.add(n_ident{ident()}, {t0, toks_[pos_ - 1].where.end});
                expect_close(')');
            }
            node_id v;
            if (!is_op(";") && !is_close('}')) v = expression();
            end_statement();
            return db_.add(n_return{v, target}, {start, toks_[pos_ - 1].where.end});
        }
        if (is_word("require") && is_open('(', 1)) {
            advance();
            expect_open('(');
            node_id c = expression();
            expect_close(')');
            end_statement();
            return db_.add(n_require{c}, {start, toks_[pos_ - 1].where.end});
        }
        if (is_word("try_compile") && is_open('{', 1)) {
            advance();
            return db_.add(n_try_compile{block()}, {start, toks_[pos_ - 1].where.end});
        }
        if (is_word("for") && is_open('(', 1)) return for_statement();
        if (is_word("while") && is_open('(', 1)) {
            advance();
            expect_open('(');
            node_id c = expression();
            expect_close(')');
            node_id body = statement_or_block();
            return db_.add(n_while{c, body}, {start, toks_[pos_ - 1].where.end});
        }
        if (auto v = try_var_decl()) return *v;

        node_id e = expression();
        if (is_op(";")) eat_op(";");
        else if (is_close('}')) last_was_value_ = true;
        else if (!ends_with_block(e)) throw diagnostic("ожидалось ';'", here().where);
        return e;
    }

    bool ends_with_block(node_id e) const {
        return db_.is<n_if>(e) || db_.is<n_block>(e) || db_.is<n_while>(e) || db_.is<n_for>(e)
            || (db_.is<n_call>(e) && !db_.as<n_call>(e).args.empty() && db_.is<n_block>(db_.as<n_call>(e).args.back()));
    }

    void end_statement() {
        if (is_op(";")) eat_op(";");
        else if (!is_close('}')) throw diagnostic("ожидалось ';'", here().where);
    }

    node_id statement_or_block() {
        if (is_open('{')) return block();
        node_id s = statement();
        return s;
    }

    // [const] (auto | type) name = expression;
    std::optional<node_id> try_var_decl() {
        u32  k        = 0;
        bool is_const = false;
        if (is_word("const")) { is_const = true; ++k; }
        bool typed = ahead(k).kind == tok::ident && ahead(k + 1).kind == tok::ident && is_op("=", k + 2);
        if (!typed) return std::nullopt;
        u32 start = here().where.begin;
        if (is_const) advance();
        n_var v;
        v.is_const = is_const;
        v.type     = type_expr();
        v.name     = ident();
        expect_op("=");
        v.init = expression();
        end_statement();
        return db_.add(std::move(v), {start, toks_[pos_ - 1].where.end});
    }

    node_id for_statement() {
        u32 start = here().where.begin;
        advance();
        expect_open('(');
        if (is_word("auto")) advance();
        symbol var = ident();
        expect_op(":");
        node_id range = expression();
        expect_close(')');
        node_id body = statement_or_block();
        return db_.add(n_for{var, range, body}, {start, toks_[pos_ - 1].where.end});
    }

    // --- Expression: parsing by priority n ---

    struct binop {
        std::string_view op;
        int              prec;
        bool             right = false;
    };

    // M0: A stitched group table (§4.4). More connects more.
    std::optional<binop> peek_binop() const {
        static constexpr std::array table{
            binop{"=", 1, true},  binop{"+=", 1, true}, binop{"-=", 1, true}, binop{"*=", 1, true},
            binop{"/=", 1, true}, binop{"||", 2},       binop{"&&", 3},       binop{"==", 4},
            binop{"!=", 4},       binop{"<=", 4},       binop{">=", 4},       binop{"<", 4},
            binop{">", 4},        binop{"+", 6},        binop{"-", 6},        binop{"*", 7},
            binop{"/", 7},        binop{"%", 7},
        };
        std::string_view longest = longest_op(0);
        if (longest.empty()) return std::nullopt;
        for (const auto& b : table)
            if (b.op == longest) return b;
        return std::nullopt;
    }

    node_id expression(int min_prec = 0) {
        node_id lhs = unary();
        while (auto b = peek_binop()) {
            if (b->prec < min_prec) break;
            u32 start = db_[lhs].where.begin;
            eat_op(b->op);
            node_id rhs = expression(b->right ? b->prec : b->prec + 1);
            lhs = db_.add(n_op{db_.strings.intern(b->op), {lhs, rhs}}, {start, db_[rhs].where.end});
        }
        return lhs;
    }

    node_id unary() {
        u32 start = here().where.begin;
        for (std::string_view op : {"-", "!", "~"}) {
            if (is_op(op) && !is_op("=>")) {
                eat_op(op);
                node_id x = unary();
                return db_.add(n_op{db_.strings.intern(op), {x}}, {start, db_[x].where.end});
            }
        }
        return postfix(primary());
    }

    node_id postfix(node_id e) {
        for (;;) {
            u32 start = db_[e].where.begin;
            if (is_open('(') && here().glued) {
                node_list args = call_args();
                // tail unit: f(x) { ... } - last expr argument (§4.4)
                if (is_open('{')) args.push_back(block());
                e = db_.add(n_call{e, std::move(args)}, {start, toks_[pos_ - 1].where.end});
            } else if (is_open('[') && here().glued) {
                advance();
                node_id i = expression();
                expect_close(']');
                e = db_.add(n_index{e, i}, {start, toks_[pos_ - 1].where.end});
            } else if (is_open('{') && here().glued && db_.is<n_ident>(e)) {
                e = db_.add(n_construct{e, brace_items()}, {start, toks_[pos_ - 1].where.end});
            } else if (is_op(".") && !is_op("..")) {
                advance();
                symbol name = ident();
                e = db_.add(n_member{e, name}, {start, toks_[pos_ - 1].where.end});
            } else if (is_op("?") && here().glued) {
                advance();
                e = db_.add(n_op{db_.strings.intern("?"), {e}}, {start, toks_[pos_ - 1].where.end});
            } else {
                return e;
            }
        }
    }

    node_list call_args() {
        expect_open('(');
        node_list args;
        while (!is_close(')')) {
            args.push_back(expression());
            if (is_op(",")) eat_op(",");
            else break;
        }
        expect_close(')');
        return args;
    }

    node_list brace_items() {
        expect_open('{');
        node_list items;
        while (!is_close('}')) {
            items.push_back(expression());
            if (is_op(",")) eat_op(",");
            else break;
        }
        expect_close('}');
        return items;
    }

    node_id primary() {
        const token& t     = here();
        u32          start = t.where.begin;
        switch (t.kind) {
        case tok::int_lit: {
            advance();
            return db_.add(n_int{parse_int(t.text)}, t.where);
        }
        case tok::float_lit: {
            advance();
            return db_.add(n_float{parse_float(t.text)}, t.where);
        }
        case tok::str_lit: {
            advance();
            return db_.add(n_str{unescape(t.text)}, t.where);
        }
        case tok::ident: {
            if (t.text == "if" && is_open('(', 1)) return if_expression();
            if (t.text == "ct") {
                advance();
                node_id body = is_open('{') ? block() : unary();
                return db_.add(n_ct{body}, {start, toks_[pos_ - 1].where.end});
            }
            advance();
            return db_.add(n_ident{db_.strings.intern(t.text)}, t.where);
        }
        case tok::open: {
            if (t.ch() == '{') return block();
            if (t.ch() == '(') {
                // lambda: (parameters) => body
                u32 close = match_[pos_];
                if (close != ~u32{0} && is_op("=>", close + 1 - pos_)) {
                    n_lambda l;
                    l.params = param_list();
                    eat_op("=>");
                    l.body = is_open('{') ? block() : expression();
                    return db_.add(std::move(l), {start, toks_[pos_ - 1].where.end});
                }
                advance();
                node_id e = expression();
                expect_close(')');
                return e;
            }
            break;
        }
        default: break;
        }
        throw diagnostic("ожидалось выражение", t.where);
    }

    node_id if_expression() {
        u32 start = here().where.begin;
        advance();
        expect_open('(');
        node_id c = expression();
        expect_close(')');
        node_id then_ = branch();
        node_id else_;
        if (is_op(";") && is_word("else", 1)) eat_op(";"); // if(c) x = 1; else x = 2;
        if (is_word("else")) {
            advance();
            else_ = branch();
        }
        return db_.add(n_if{c, then_, else_}, {start, toks_[pos_ - 1].where.end});
    }

    // Branch if: Block, nested if or expression/instruction.
    node_id branch() {
        if (is_open('{')) return block();
        if (is_word("return") || is_word("require")) return statement();
        return expression();
    }

    // --- literary ---

    static i64 parse_int(std::string_view s) {
        i64 v = 0;
        for (char c : s) if (c != '_') v = v * 10 + (c - '0');
        return v;
    }

    static f64 parse_float(std::string_view s) {
        std::string clean;
        for (char c : s) if (c != '_') clean += c;
        return std::stod(clean);
    }

    static std::string unescape(std::string_view s) {
        std::string out;
        for (u32 i = 1; i + 1 < s.size(); ++i) {
            if (s[i] == '\\' && i + 2 < s.size()) {
                char n = s[++i];
                out += n == 'n' ? '\n' : n == 't' ? '\t' : n;
            } else {
                out += s[i];
            }
        }
        return out;
    }

    diagnostic with_source(diagnostic d) const {
        span w   = d.where();
        w.source = source_;
        diagnostic out(d.what(), w, d.kind());
        for (auto f : d.trace()) { f.where.source = source_; out.note(f.what, f.where); }
        return out;
    }

    ast_db&                 db_;
    u32                     source_ = 0;
    std::string_view        src_;
    std::vector<token>      toks_;
    std::vector<u32>        match_;
    u32                     pos_ = 0;
    bool                    last_was_value_ = false;
    std::vector<diagnostic> errors_;
};

} // namespace tz
