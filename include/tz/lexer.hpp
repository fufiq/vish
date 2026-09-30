// Lexer text projection (spec §4.1).
//
// “The operator is always single-character”: multi-character operators (`==`, `=>`,
// `<=`) are chains of single-character operators without spaces. The lexer gives each
// character separately and marks whether it is glued to the previous one.
#pragma once

#include "base.hpp"

namespace tz {

enum class tok : uint8_t {
  ident,
  int_lit,
  float_lit,
  str_lit,
  char_lit, // 'c' - byte (i64)
  op, // operator
  open, // ( [ {
  close, // ) ] }
  end,
};

struct token {
  tok kind = tok::end;
  span where;
  bool glued = false; // close to the previous token (without space)

  char ch() const { return kind == tok::op || kind == tok::open || kind == tok::close ? text[0] : 0; }
  std::string_view text;
};

struct lexer {
  explicit lexer(std::string_view src) : src(src) {}

  std::vector<token> run() {
    std::vector<token> out;
    bool glued = false;
    while(pos < src.size()) {
      char c = src[pos];
      if(is_space(c)) {
        ++pos;
        glued = false;
        continue;
      }
      if(c == '/' && peek(1) == '/') {
        skip_line();
        glued = false;
        continue;
      }
      if(c == '/' && peek(1) == '*') {
        skip_block_comment();
        glued = false;
        continue;
      }

      uint32_t start = pos;
      token t;
      if(is_ident_start(c)) {
        while(pos < src.size() && is_ident(src[pos])) ++pos;
        t.kind = tok::ident;
      } else if(is_digit(c)) {
        t.kind = lex_number();
      } else if(c == '"') {
        lex_string();
        t.kind = tok::str_lit;
      } else if(c == '\'') {
        lex_string('\'');
        t.kind = tok::char_lit;
      } else if(c == '(' || c == '[' || c == '{') {
        ++pos;
        t.kind = tok::open;
      } else if(c == ')' || c == ']' || c == '}') {
        ++pos;
        t.kind = tok::close;
      } else if(is_op(c)) {
        ++pos;
        t.kind = tok::op;
      } else {
        throw diagnostic(cat("неизвестный символ '", c, "'"), {start, start + 1});
      }
      t.where = {start, pos};
      t.text = src.substr(start, pos - start);
      t.glued = glued;
      out.push_back(t);
      glued = true;
    }
    token e;
    e.kind = tok::end;
    e.where = {pos, pos};
    out.push_back(e);
    return out;
  }
  static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
  static bool is_digit(char c) { return c >= '0' && c <= '9'; }
  static bool is_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
  static bool is_ident(char c) { return is_ident_start(c) || is_digit(c); }
  static bool is_op(char c) { return std::string_view("+-*/%<>=!&|^~?:.,;@#$\\").contains(c); }

  char peek(uint32_t k) const { return pos + k < src.size() ? src[pos + k] : '\0'; }

  void skip_line() {
    while(pos < src.size() && src[pos] != '\n') ++pos;
  }

  void skip_block_comment() {
    uint32_t start = pos;
    pos += 2;
    while(pos + 1 < src.size() && !(src[pos] == '*' && src[pos + 1] == '/')) ++pos;
    if(pos + 1 >= src.size()) throw diagnostic("незакрытый комментарий", {start, start + 2});
    pos += 2;
  }

  tok lex_number() {
    while(pos < src.size() && (is_digit(src[pos]) || src[pos] == '_')) ++pos;
    // 1.5 - fractional; 1..5 and 1.foo() - none
    if(peek(0) == '.' && is_digit(peek(1))) {
      ++pos;
      while(pos < src.size() && (is_digit(src[pos]) || src[pos] == '_')) ++pos;
      return tok::float_lit;
    }
    return tok::int_lit;
  }

  void lex_string(char quote = '"') {
    uint32_t start = pos++;
    while(pos < src.size() && src[pos] != quote) {
      if(src[pos] == '\\') ++pos;
      ++pos;
    }
    if(pos >= src.size()) throw diagnostic(quote == '"' ? "незакрытая строка" : "незакрытый символ", {start, start + 1});
    ++pos;
  }

  std::string_view src;
  uint32_t pos = 0;
};

} // namespace tz
