// Lexer text projection (spec §4.1).
//
// “The operator is always single-character”: multi-character operators (`==`, `=>`,
// `<=`) are chains of single-character operators without spaces. The lexer gives each
// character separately and marks whether it is glued to the previous one.
//
// Lexer doesn't fall: Unreadable -
// token-hole
// tok::bad с причиной в errors; парсер делает дырой только его инструкцию.
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
  bad, // non-lexeme: unknown characters, unclosed line or comment
  end,
};

struct token {
  tok kind = tok::end;
  span where;
  bool glued = false; // close to the previous token (without space)
  uint32_t err = ~uint32_t{0}; // Cause error number: tok::bad and closed bracket parsing

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
      if(c == '/' && peek(1) == '/') { // Comment - not a lexeme, but not lost: layout (§4.0)
        uint32_t start = pos;
        skip_line();
        uint32_t end = pos;
        if(end > start && src[end - 1] == '\r') --end; // text as written, without \r
        comments.push_back({start, end});
        glued = false;
        continue;
      }
      if(c == '/' && peek(1) == '*') {
        uint32_t start = pos;
        if(skip_block_comment()) {
          comments.push_back({start, pos});
          glued = false;
          continue;
        }
      }

      uint32_t start = pos;
      token t;
      if(is_ident_start(c)) {
        while(pos < src.size() && is_ident(src[pos])) ++pos;
        t.kind = tok::ident;
      } else if(is_digit(c)) {
        t.kind = lex_number();
      } else if(c == '/' && peek(1) == '*') { // Unclosed comment - a hole to the end of the text
        pos = uint32_t(src.size());
        while(is_space(src[pos - 1])) --pos; // The gaps at the end are not a hole (fmtаfmt = fmt)
        t.kind = tok::bad;
        t.err = fail("незакрытый комментарий", {start, start + 2});
      } else if(c == '"' || c == '\'') {
        t.kind = c == '"' ? tok::str_lit : tok::char_lit;
        if(!lex_string(c)) { // Unclosed - a hole to the end of a line of text
          t.kind = tok::bad;
          t.err = fail(c == '"' ? "незакрытая строка" : "незакрытый символ", {start, start + 1});
        }
      } else if(c == '(' || c == '[' || c == '{') {
        ++pos;
        t.kind = tok::open;
      } else if(c == ')' || c == ']' || c == '}') {
        ++pos;
        t.kind = tok::close;
      } else if(is_op(c)) {
        ++pos;
        t.kind = tok::op;
      } else { // Unknown Symbols: One Hole
        while(pos < src.size() && !known(src[pos])) ++pos;
        t.kind = tok::bad;
        t.err = fail(cat("неизвестный символ '", src.substr(start, pos - start), "'"), {start, pos});
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
  // UTF-8 bytes ≥ 0x80 - letters of the name (Cyrillic, etc.); *my *
  static bool is_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || static_cast<unsigned char>(c) >= 0x80; }
  static bool is_ident(char c) { return is_ident_start(c) || is_digit(c); }
  static bool is_op(char c) { return std::string_view("+-*/%<>=!&|^~?:.,;@#$\\").contains(c); }
  static bool known(char c) { return is_space(c) || is_ident(c) || is_op(c) || std::string_view("\"'()[]{}").contains(c); }

  char peek(uint32_t k) const { return pos + k < src.size() ? src[pos + k] : '\0'; }

  uint32_t fail(std::string what, span where) {
    errors.push_back(diagnostic(std::move(what), where));
    return uint32_t(errors.size() - 1);
  }

  void skip_line() {
    while(pos < src.size() && src[pos] != '\n') ++pos;
  }

  // Unclosed - false, the position is not shifted.
  bool skip_block_comment() {
    uint32_t end = pos + 2;
    while(end + 1 < src.size() && !(src[end] == '*' && src[end + 1] == '/')) ++end;
    if(end + 1 >= src.size()) return false;
    pos = end + 2;
    return true;
  }

  tok lex_number() {
    if(src[pos] == '0' && (peek(1) == 'x' || peek(1) == 'X')) { // 0x2A - hexadecimal
      pos += 2;
      while(pos < src.size() && (std::isxdigit(static_cast<unsigned char>(src[pos])) || src[pos] == '_')) ++pos;
      return tok::int_lit;
    }
    while(pos < src.size() && (is_digit(src[pos]) || src[pos] == '_')) ++pos;
    // 1.5 - fractional; 1..5 and 1.foo() - none
    if(peek(0) == '.' && is_digit(peek(1))) {
      ++pos;
      while(pos < src.size() && (is_digit(src[pos]) || src[pos] == '_')) ++pos;
      return tok::float_lit;
    }
    return tok::int_lit;
  }

  // Line - within a line of text (translation inside - `\n`); *my*: so the unclosed
  // does not eat the next lines. Unclosed - false, position - at the end of the line
  // of text.
  bool lex_string(char quote) {
    ++pos;
    while(pos < src.size() && src[pos] != quote && src[pos] != '\n') {
      if(src[pos] == '\\' && peek(1) != '\n') ++pos;
      ++pos;
    }
    if(pos >= src.size() || src[pos] != quote) {
      pos = std::min<uint32_t>(pos, uint32_t(src.size()));
      return false;
    }
    ++pos;
    return true;
  }

  std::string_view src;
  uint32_t pos = 0;
  std::vector<diagnostic> errors; // Reasons for token-holes (token::err)
  std::vector<span> comments; // Comments in order: `//…` to the end of the line, `/*…*/`
};

} // namespace tz
