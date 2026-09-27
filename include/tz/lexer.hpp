// Lexer text projection (spec §4.1).
//
// “The operator is always single-character”: multi-character operators (`==`, `=>`,
// `<=`) are chains of single-character operators without spaces. The lexer gives each
// character separately and marks whether it is glued to the previous one.
#pragma once

#include "base.hpp"

namespace tz {

enum class tok : u8 {
    ident,
    int_lit,
    float_lit,
    str_lit,
    op,      // operator
    open,    // ( [ {
    close,   // ) ] }
    end,
};

struct token {
    tok  kind  = tok::end;
    span where;
    bool glued = false; // close to the previous token (without space)

    char ch() const { return kind == tok::op || kind == tok::open || kind == tok::close ? text[0] : 0; }
    std::string_view text;
};

class lexer {
public:
    explicit lexer(std::string_view src) : src_(src) {}

    std::vector<token> run() {
        std::vector<token> out;
        bool glued = false;
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (is_space(c)) { ++pos_; glued = false; continue; }
            if (c == '/' && peek(1) == '/') { skip_line(); glued = false; continue; }
            if (c == '/' && peek(1) == '*') { skip_block_comment(); glued = false; continue; }

            u32   start = pos_;
            token t;
            if (is_ident_start(c)) {
                while (pos_ < src_.size() && is_ident(src_[pos_])) ++pos_;
                t.kind = tok::ident;
            } else if (is_digit(c)) {
                t.kind = lex_number();
            } else if (c == '"') {
                lex_string();
                t.kind = tok::str_lit;
            } else if (c == '(' || c == '[' || c == '{') {
                ++pos_;
                t.kind = tok::open;
            } else if (c == ')' || c == ']' || c == '}') {
                ++pos_;
                t.kind = tok::close;
            } else if (is_op(c)) {
                ++pos_;
                t.kind = tok::op;
            } else {
                throw diagnostic(std::format("неизвестный символ '{}'", c), {start, start + 1});
            }
            t.where = {start, pos_};
            t.text  = src_.substr(start, pos_ - start);
            t.glued = glued;
            out.push_back(t);
            glued = true;
        }
        token e;
        e.kind  = tok::end;
        e.where = {pos_, pos_};
        out.push_back(e);
        return out;
    }

private:
    static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
    static bool is_digit(char c) { return c >= '0' && c <= '9'; }
    static bool is_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
    static bool is_ident(char c) { return is_ident_start(c) || is_digit(c); }
    static bool is_op(char c) { return std::string_view("+-*/%<>=!&|^~?:.,;@#$\\").contains(c); }

    char peek(u32 k) const { return pos_ + k < src_.size() ? src_[pos_ + k] : '\0'; }

    void skip_line() {
        while (pos_ < src_.size() && src_[pos_] != '\n') ++pos_;
    }

    void skip_block_comment() {
        u32 start = pos_;
        pos_ += 2;
        while (pos_ + 1 < src_.size() && !(src_[pos_] == '*' && src_[pos_ + 1] == '/')) ++pos_;
        if (pos_ + 1 >= src_.size()) throw diagnostic("незакрытый комментарий", {start, start + 2});
        pos_ += 2;
    }

    tok lex_number() {
        while (pos_ < src_.size() && (is_digit(src_[pos_]) || src_[pos_] == '_')) ++pos_;
        // 1.5 - fractional; 1..5 and 1.foo() - none
        if (peek(0) == '.' && is_digit(peek(1))) {
            ++pos_;
            while (pos_ < src_.size() && (is_digit(src_[pos_]) || src_[pos_] == '_')) ++pos_;
            return tok::float_lit;
        }
        return tok::int_lit;
    }

    void lex_string() {
        u32 start = pos_++;
        while (pos_ < src_.size() && src_[pos_] != '"') {
            if (src_[pos_] == '\\') ++pos_;
            ++pos_;
        }
        if (pos_ >= src_.size()) throw diagnostic("незакрытая строка", {start, start + 1});
        ++pos_;
    }

    std::string_view src_;
    u32              pos_ = 0;
};

} // namespace tz
