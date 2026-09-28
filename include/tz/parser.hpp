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
#include <map>
#include <optional>

namespace tz {

node_id load_prelude(ast_db & db); // prelude.hpp: a module of constructions written in the language

struct parser {
  // The parser reads a copy of the text stored by the database when importing: tokens and
  // holes refer to it, not to the caller string (which may be temporary).
  parser(ast_db & db, std::string_view text, std::string label = {}) : db(db) {
    load_prelude(db); // before import: node of this text from this import
    source = db.record_import(std::string(text), std::move(label));
    src = db.source(source);
    try {
      toks = lexer(src).run();
    } catch(diagnostic const & d) {
      diags.push_back(with_source(d));
      toks.assign(1, token{});
    }
    match_brackets();
    for(uint32_t i = 0; i < db.size(); ++i)
      if(db.is<n_fn>(node_id(i))) learn(db.as<n_fn>(node_id(i)));
  }

  // --- designs declared code (§4.4) ---
  //
  // The syntax of the call is taken from the ads in the area:
// Function
  // try(expr body) catch(expr... hs) makes `catch` a try call clause, a function with the
  // latest expr parameter - tail design: while(c) instruction.
  struct construct {
    std::vector<std::pair<symbol, bool>> clauses; // Clause word; repeated (pak)
    bool tail = false; // The tail of the head can be an instruction, not just a block.
    bool bare = false; // head without brackets: try { … }
  };

  void learn(n_fn const & f) {
    if(!f.name) return;
    learn_operator(f);
    bool any_clause = false;
    uint32_t head = 0;
    bool last_expr = false;
    for(node_id pn: f.params) {
      auto const & p = db.as<n_param>(pn);
      if(p.clause) any_clause = true;
      else {
        ++head;
        last_expr = p.mode == param_mode::expr;
      }
    }
    if(!any_clause && !last_expr) return;
    construct & c = constructs[f.name.value];
    if(last_expr) c.tail = true;
    if(head == 1 && last_expr) c.bare = true;
    for(node_id pn: f.params) {
      auto const & p = db.as<n_param>(pn);
      if(!p.clause) continue;
      bool known = std::ranges::any_of(c.clauses, [&](auto const & w) { return w.first == p.clause; });
      if(!known) c.clauses.push_back({p.clause, p.pack});
    }
  }

  construct const * construct_of(node_id callee) const {
    if(!db.is<n_ident>(callee)) return nullptr;
    auto it = constructs.find(db.as<n_ident>(callee).name.value);
    return it == constructs.end() ? nullptr : &it->second;
  }

  node_id parse_module() {
    uint32_t start = here().where.begin;
    n_module m;
    if(is_word("module")) {
      advance();
      m.name = db.strings.intern(dotted_name());
      expect_op(";");
    }
    while(!at_end()) m.items.push_back(recover_statement([&] { return item(); }));
    auto id = db.add(std::move(m), {start, here().where.end});
    db.link_parents(id);
    return id;
  }

  // parsing errors with import number in positions.
  std::vector<diagnostic> errors() const {
    std::vector<diagnostic> out;
    for(auto d: diags) out.push_back(with_source(d));
    return out;
  }
  // --- Phase 1: paired brackets ---

  void match_brackets() {
    match.assign(toks.size(), ~uint32_t{0});
    std::vector<uint32_t> stack;
    for(uint32_t i = 0; i < toks.size(); ++i) {
      if(toks[i].kind == tok::open) stack.push_back(i);
      else if(toks[i].kind == tok::close) {
        // Unpaired Closing: Remove unclosed openers until suitable so that one lost `(`
        // does not break the enveloping `{ }`.
        auto fits = [&](uint32_t o) { return pairs(toks[o].ch(), toks[i].ch()); };
        if(std::ranges::none_of(stack, fits)) {
          diags.push_back(diagnostic("непарная скобка", toks[i].where));
          continue;
        }
        while(!fits(stack.back())) {
          diags.push_back(diagnostic("незакрытая скобка", toks[stack.back()].where));
          stack.pop_back();
        }
        match[stack.back()] = i;
        match[i] = stack.back();
        stack.pop_back();
      }
    }
    for(uint32_t i: stack) diags.push_back(diagnostic("незакрытая скобка", toks[i].where));
  }

  static bool pairs(char o, char c) { return (o == '(' && c == ')') || (o == '[' && c == ']') || (o == '{' && c == '}'); }

  // --- navigation ---

  token const & here() const { return toks[pos]; }
  token const & ahead(uint32_t k) const { return toks[std::min<uint32_t>(pos + k, toks.size() - 1)]; }
  bool at_end() const { return here().kind == tok::end; }
  void advance() {
    if(!at_end()) ++pos;
  }

  bool is_word(std::string_view w, uint32_t k = 0) const { return ahead(k).kind == tok::ident && ahead(k).text == w; }
  bool is_open(char c, uint32_t k = 0) const { return ahead(k).kind == tok::open && ahead(k).ch() == c; }
  bool is_close(char c) const { return here().kind == tok::close && here().ch() == c; }

  // The operator in position k is the longest matching of glued symbols (`==` is
  // not read as `=` `=`, `=>` is not read as `=` `>`).
  bool is_op(std::string_view op, uint32_t k = 0) const { return longest_op(k) == op; }

  std::string_view longest_op(uint32_t k) const {
    static constexpr std::array ops{"...", "==", "!=", "<=", ">=", "&&", "||", "=>", "+=", "-=", "*=", "/=",
        "..", "=", "<", ">", "+", "-", "*", "/", "%", "!", "~", "?", ":",
        ".", ",", ";", "&", "|", "^", "@", "#", "$", "\\"};
    for(std::string_view o: ops) {
      bool ok = true;
      for(uint32_t i = 0; i < o.size() && ok; ++i) {
        token const & t = ahead(k + i);
        ok = t.kind == tok::op && t.ch() == o[i] && (i == 0 || t.glued);
      }
      if(ok) return o;
    }
    return {};
  }

  void eat_op(std::string_view op) {
    for(uint32_t i = 0; i < op.size(); ++i) advance();
  }

  void expect_op(std::string_view op) {
    if(!is_op(op)) throw diagnostic(cat("ожидалось '", op, "'"), here().where);
    eat_op(op);
  }

  void expect_open(char c) {
    if(!is_open(c)) throw diagnostic(cat("ожидалось '", c, "'"), here().where);
    advance();
  }

  void expect_close(char c) {
    if(!is_close(c)) throw diagnostic(cat("ожидалось '", c, "'"), here().where);
    advance();
  }

  symbol ident() {
    if(here().kind != tok::ident) throw diagnostic("ожидалось имя", here().where);
    auto s = db.strings.intern(here().text);
    advance();
    return s;
  }

  std::string dotted_name() {
    std::string name(here().text);
    ident();
    while(is_op(".")) {
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
  node_id recover_statement(auto && f) {
    uint32_t start_tok = pos;
    try {
      return f();
    } catch(diagnostic const & d) {
      diags.push_back(d);
      uint32_t start = toks[start_tok].where.begin;
      pos = start_tok;
      skip_to_statement_end();
      uint32_t end = toks[pos > 0 ? pos - 1 : 0].where.end;
      return db.add(n_hole{std::string(src.substr(start, end - start)), d.what()}, {start, end});
    }
  }

  void skip_to_statement_end() {
    while(!at_end()) {
      if(here().kind == tok::close) return; // terminus
      if(here().kind == tok::open) { // skip the whole group
        uint32_t m = match[pos];
        if(m == ~uint32_t{0}) {
          advance();
          continue;
        }
        pos = m + 1;
        if(toks[m].ch() == '}') return; // block completes the design
        continue;
      }
      bool semi = is_op(";");
      advance();
      if(semi) return;
    }
  }

  // --- module elements and instructions ---

  node_id item() {
    uint32_t start = here().where.begin;
    node_list attrs = attributes();
    // Attributes enter the node before being added to the database, otherwise journal will not see them.
    if(is_word("struct") && ahead(1).kind == tok::ident && is_open('{', 2)) return struct_decl(std::move(attrs));
    if(is_word("operator")) return operator_decl(std::move(attrs));
    if(auto f = try_fn_decl(false, attrs)) return *f;
    if(!attrs.empty()) throw diagnostic("атрибуты [[…]] — только перед функцией или struct", db[attrs.front()].where);
    // Name { … } - extension of entity
    if(ahead(0).kind == tok::ident && !is_keyword(ahead(0).text) && is_open('{', 1)) return extension(start);
    return statement();
  }

  // [[a, b]] [[c]] - Attributes: Common expressions, computed in compilation
  node_list attributes() {
    node_list out;
    while(is_open('[') && is_open('[', 1)) {
      advance();
      advance();
      while(!is_close(']')) {
        out.push_back(expression());
        if(is_op(",")) eat_op(",");
        else break;
      }
      expect_close(']');
      expect_close(']');
    }
    return out;
  }

  node_id extension(uint32_t start) {
    n_extend e;
    e.name = ident();
    expect_open('{');
    while(!is_close('}') && !at_end()) {
      uint32_t i0 = here().where.begin;
      node_list attrs = attributes();
      if(auto f = try_fn_decl(false, attrs)) {
        e.items.push_back(*f);
        continue;
      }
      if(ahead(0).kind == tok::ident && ahead(1).kind == tok::ident) { // field: T name [= v];
        n_var v;
        v.type = db.add(n_ident{ident()}, {i0, toks[pos - 1].where.end});
        v.name = ident();
        if(is_op("=")) {
          eat_op("=");
          v.init = expression();
        }
        end_statement();
        e.items.push_back(db.add(std::move(v), {i0, toks[pos - 1].where.end}));
        for(node_id a: attrs) e.attrs.push_back(a);
        continue;
      }
      if(attrs.empty()) throw diagnostic("в расширении ожидались поле, функция или [[атрибут]]", here().where);
      for(node_id a: attrs) e.attrs.push_back(a);
      if(is_op(";")) eat_op(";");
    }
    expect_close('}');
    if(is_op(";")) eat_op(";");
    return db.add(std::move(e), {start, toks[pos - 1].where.end});
  }

  // struct Name { T поле [= значение]; … } [;]
  node_id struct_decl(node_list attrs = {}) {
    uint32_t start = here().where.begin;
    advance();
    n_struct st;
    st.attrs = std::move(attrs);
    st.name = ident();
    expect_open('{');
    while(!is_close('}')) {
      uint32_t f0 = here().where.begin;
      n_var f;
      if(is_word("ct") && ahead(1).kind == tok::ident && ahead(2).kind == tok::ident) { // ct field: compile state
        f.ct = true;
        advance();
      }
      f.type = db.add(n_ident{ident()}, {f0, toks[pos - 1].where.end});
      f.name = ident();
      if(is_op("=")) {
        eat_op("=");
        f.init = expression();
      }
      end_statement();
      st.fields.push_back(db.add(std::move(f), {f0, toks[pos - 1].where.end}));
    }
    expect_close('}');
    if(is_op(";")) eat_op(";");
    return db.add(std::move(st), {start, toks[pos - 1].where.end});
  }

  // Announcement of function: [outline] [type] name(s) (=> e; | { ... }) On the
  // upper level, a form without the type on the left is allowed; in the block, only
  // with the sign of the declaration (§4.2).
  std::optional<node_id> try_fn_decl(bool in_block, node_list attrs = {}) {
    uint32_t k = 0;
    bool outline = false;
    if(is_word("outline")) {
      outline = true;
      ++k;
    }
    // [type] name
    uint32_t name_at;
    if(ahead(k).kind == tok::ident && ahead(k + 1).kind == tok::ident && is_open('(', k + 2)) name_at = k + 1;
    else if(!in_block && ahead(k).kind == tok::ident && is_open('(', k + 1) && !is_keyword(ahead(k).text)) name_at = k;
    else return std::nullopt;
    // after ) - clauses word(...) and then => or {
    uint32_t close = match[pos + name_at + 1];
    if(close == ~uint32_t{0}) return std::nullopt;
    uint32_t after = close + 1 - pos;
    while(ahead(after).kind == tok::ident && is_open('(', after + 1) && !is_keyword(ahead(after).text)) {
      uint32_t c2 = match[pos + after + 1];
      if(c2 == ~uint32_t{0}) return std::nullopt;
      after = c2 + 1 - pos;
    }
    if(!is_op("=>", after) && !is_open('{', after)) return std::nullopt;

    uint32_t start = here().where.begin;
    n_fn f;
    f.attrs = std::move(attrs);
    f.outline = outline;
    if(outline) advance();
    if(name_at == (outline ? 2u : 1u)) f.result_type = type_expr();
    f.name = ident();
    f.params = param_list();
    while(here().kind == tok::ident && is_open('(', 1)) { // Clause: catch(expr... hs)
      symbol word = ident();
      for(node_id pn: param_list(word)) f.params.push_back(pn); // word before being added to the database (journal)
    }
    if(is_op("=>")) {
      eat_op("=>");
      f.body = expression();
      f.expr_body = true;
      if(is_op(";")) eat_op(";");
    } else {
      f.body = block();
    }
    learn(f);
    return db.add(std::move(f), {start, toks[pos - 1].where.end});
  }

  static bool is_keyword(std::string_view w) {
    return w == "if" || w == "loop" || w == "break" || w == "continue" || w == "return" || w == "require" || w == "try_compile" || w == "ct" || w == "else" || w == "auto" || w == "const" || w == "outline" || w == "module" || w == "throw" || w == "struct" || w == "operator";
  }

  node_list param_list(symbol clause = {}) {
    expect_open('(');
    node_list ps;
    while(!is_close(')')) {
      ps.push_back(param(clause));
      if(is_op(",")) eat_op(",");
      else break;
    }
    expect_close(')');
    return ps;
  }

  node_id param(symbol clause = {}) {
    uint32_t start = here().where.begin;
    n_param p;
    p.clause = clause;
    if(is_word("mut")) {
      p.mode = param_mode::mut;
      advance();
    } else if(is_word("own")) {
      p.mode = param_mode::own;
      advance();
    } else if(is_word("out")) {
      p.mode = param_mode::out;
      advance();
    } else if(is_word("expr")) {
      p.mode = param_mode::expr;
      advance();
    } else if(is_word("ct")) {
      p.mode = param_mode::ct;
      advance();
    }
    if(p.mode != param_mode::let && is_op("...")) { // expr. hs - pack (repeated clause)
      eat_op("...");
      p.pack = true;
    }
    // [type] [...] name | name
    if(here().kind == tok::ident && (ahead(1).kind == tok::ident || is_op("...", 1))) {
      p.type = type_expr();
      if(is_op("...")) {
        eat_op("...");
        p.pack = true;
      }
    }
    p.name = ident();
    return db.add(std::move(p), {start, toks[pos - 1].where.end});
  }

  node_id type_expr() {
    uint32_t start = here().where.begin;
    if(is_word("auto")) {
      advance();
      return db.add(n_auto{}, {start, toks[pos - 1].where.end});
    }
    return db.add(n_ident{ident()}, {start, toks[pos - 1].where.end});
  }

  node_id block() {
    uint32_t start = here().where.begin;
    expect_open('{');
    n_block b;
    while(!is_close('}') && !at_end()) {
      node_id s = recover_statement([&] { return statement(); });
      b.items.push_back(s);
      if(is_close('}') && last_was_value) b.has_value = true;
    }
    expect_close('}');
    return db.add(std::move(b), {start, toks[pos - 1].where.end});
  }

  // Instructions. last_was_value_ is an expression without `;`.
  node_id statement() {
    last_was_value = false;
    uint32_t start = here().where.begin;

    if(auto f = try_fn_decl(true)) return *f;

    if(is_word("return")) {
      advance();
      // return(цель) value - close; return (x) with space - value
      node_id target;
      if(is_open('(') && here().glued) {
        advance();
        uint32_t t0 = here().where.begin;
        target = db.add(n_ident{ident()}, {t0, toks[pos - 1].where.end});
        expect_close(')');
      }
      node_id v;
      if(!is_op(";") && !is_close('}')) v = expression();
      end_statement();
      return db.add(n_return{v, target}, {start, toks[pos - 1].where.end});
    }
    if(is_word("require") && is_open('(', 1)) {
      advance();
      expect_open('(');
      node_id c = expression();
      expect_close(')');
      end_statement();
      return db.add(n_require{c}, {start, toks[pos - 1].where.end});
    }
    if(is_word("try_compile") && is_open('{', 1)) {
      advance();
      return db.add(n_try_compile{block()}, {start, toks[pos - 1].where.end});
    }
    if(is_word("hide") && ahead(1).kind == tok::ident) {
      advance();
      symbol name = ident();
      end_statement();
      return db.add(n_hide{name}, {start, toks[pos - 1].where.end});
    }
    if(is_word("break") || is_word("continue")) {
      bool brk = is_word("break");
      advance();
      end_statement();
      return brk ? db.add(n_break{}, {start, toks[pos - 1].where.end}) : db.add(n_continue{}, {start, toks[pos - 1].where.end});
    }
    if(auto v = try_var_decl()) return *v;

    uint32_t head_saved = std::exchange(statement_head, here().where.begin); // Tail instruction - only at the call at the beginning of the instruction
    node_id e = expression();
    statement_head = head_saved;
    if(tail_statement) { // The tail instructions ate their own `;`.
      tail_statement = false;
      return e;
    }
    if(is_op(";")) eat_op(";");
    else if(is_close('}')) last_was_value = true;
    else if(!ends_with_block(e)) throw diagnostic("ожидалось ';'", here().where);
    return e;
  }

  bool if_closed(node_id n) const {
    auto const & i = db.as<n_if>(n);
    node_id last = i.else_ ? i.else_ : i.then_;
    if(db.is<n_if>(last)) return if_closed(last);
    return db.is<n_block>(last) || db.is<n_return>(last) || db.is<n_require>(last) || db.is<n_break>(last) || db.is<n_continue>(last);
  }

  bool ends_with_block(node_id e) const {
    return db.is<n_if>(e) || db.is<n_loop>(e) || (db.is<n_ct>(e) && db.is<n_block>(db.as<n_ct>(e).body)) || db.is<n_block>(e) || (db.is<n_call>(e) && (db.as<n_call>(e).tail || !db.as<n_call>(e).words.empty()));
  }

  void end_statement() {
    if(is_op(";")) eat_op(";");
    else if(!is_close('}')) throw diagnostic("ожидалось ';'", here().where);
  }

  node_id statement_or_block() {
    if(is_open('{')) return block();
    node_id s = statement();
    return s;
  }

  // [const] (auto | type) name = expression;
  std::optional<node_id> try_var_decl() {
    uint32_t k = 0;
    bool is_const = false;
    if(is_word("const")) {
      is_const = true;
      ++k;
    }
    bool typed = ahead(k).kind == tok::ident && ahead(k + 1).kind == tok::ident && is_op("=", k + 2);
    if(!typed) return std::nullopt;
    uint32_t start = here().where.begin;
    if(is_const) advance();
    n_var v;
    v.is_const = is_const;
    v.type = type_expr();
    v.name = ident();
    expect_op("=");
    v.init = expression();
    end_statement();
    return db.add(std::move(v), {start, toks[pos - 1].where.end});
  }

  // --- expressions: code operators (§4.4) ---
  //
  // Operators - ads in the area (prelude - first):
// 
// 
//  
// No Priorities: Multi-Tree Chain is a Node
  // n_chain, the tree selects the types at instantiation. The same
  // associative operator in a row - one tree
// ibid. Prefix and postfix operators are part of
  // operanda. Attribution (= += -= *= /=) is the form of the kernel, not the operator.

  static bool op_char(char c) { return std::string_view("+-*/%<>=!&|^~@$\\").contains(c); }

  void learn_operator(n_fn const & f) {
    auto name = db.strings.text(f.name);
    for(std::string_view role: {"infix", "prefix", "postfix"})
      if(name.size() > role.size() && name.starts_with(role) && op_char(name[role.size()])) {
        op_forms & o = operators[std::string(name.substr(role.size()))];
        (role == "infix" ? o.infix : role == "prefix" ? o.prefix
                                                      : o.postfix) = true;
        for(node_id a: f.attrs)
          if(db.is<n_ident>(a) && db.strings.text(db.as<n_ident>(a).name) == "associative") o.assoc = true;
      }
  }

  bool is_role(std::string const & sym, int role) const { // 0 prefix, 1 infix, 2 postfix
    auto it = operators.find(sym);
    if(it == operators.end()) return false;
    return role == 0 ? it->second.prefix : role == 1 ? it->second.infix
                                                     : it->second.postfix;
  }

  // operator -(a) ... | operator (a) + (b) ... | operator (a)++ ... - the ad repeats the
  // entry: the parameter is on the side of its branch (
// ). entity is a function of infix+/prefix-/postfix++.
  node_id operator_decl(node_list attrs) {
    uint32_t start = here().where.begin;
    advance();
    n_fn f;
    f.attrs = std::move(attrs);
    std::string role, sym;
    if(is_open('(')) {
      f.params = param_list();
      sym = symbol_run();
      if(is_open('(')) {
        role = "infix";
        for(node_id pn: param_list()) f.params.push_back(pn);
      } else role = "postfix";
    } else {
      role = "prefix";
      sym = symbol_run();
      f.params = param_list();
    }
    if(sym == "=" || sym == "+=" || sym == "-=" || sym == "*=" || sym == "/=")
      throw diagnostic(cat("присваивание ", sym, " — форма ядра, не оператор"), toks[pos - 1].where);
    if(f.params.size() != (role == "infix" ? 2u : 1u)) throw diagnostic(cat("у оператора ", role, " ", sym, " неверное число параметров"), here().where);
    f.name = db.strings.intern(role + sym);
    if(is_op("=>")) {
      eat_op("=>");
      f.body = expression();
      f.expr_body = true;
      if(is_op(";")) eat_op(";");
    } else {
      f.body = block();
    }
    learn(f);
    return db.add(std::move(f), {start, toks[pos - 1].where.end});
  }

  std::string symbol_run() { // carrier
    std::string s;
    while(here().kind == tok::op && op_char(here().ch()) && (s.empty() || here().glued) && !(s.empty() && is_op("=>"))) {
      s += here().ch();
      advance();
    }
    if(s.empty()) throw diagnostic("ожидались символы оператора", here().where);
    return s;
  }

  // A series of operator symbols from the position of pos + k (without eating): to a
  // non-operator token or `=>`.
  uint32_t run_length(uint32_t k = 0) const {
    uint32_t n = 0;
    while(ahead(k + n).kind == tok::op && op_char(ahead(k + n).ch()) && !is_op("=>", k + n)) ++n;
    return n;
  }

  // Dividing the series into characters of roles: postfixes*, [infix, prefixes*]. Glued
  // symbols can be cut, space - boundary. You need exactly one break.
  struct run_split {
    std::vector<std::string> post, pre;
    std::string infix;
  };
  void splits(uint32_t from, uint32_t i, uint32_t n, int phase, bool want_infix, run_split & cur, std::vector<run_split> & out) const {
    if(out.size() > 1) return;
    if(i == n) {
      if(!want_infix || phase == 1) out.push_back(cur);
      return;
    }
    std::string sym;
    for(uint32_t len = 1; i + len <= n; ++len) {
      if(len > 1 && !ahead(from + i + len - 1).glued) break;
      sym += ahead(from + i + len - 1).ch();
      if(phase == 0 && is_role(sym, 2)) {
        cur.post.push_back(sym);
        splits(from, i + len, n, 0, want_infix, cur, out);
        cur.post.pop_back();
      }
      if(phase == 0 && want_infix && is_role(sym, 1)) {
        cur.infix = sym;
        splits(from, i + len, n, 1, want_infix, cur, out);
        cur.infix.clear();
      }
      if(phase == 1 && is_role(sym, 0)) {
        cur.pre.push_back(sym);
        splits(from, i + len, n, 1, want_infix, cur, out);
        cur.pre.pop_back();
      }
    }
  }

  std::string run_text(uint32_t n) const {
    std::string s;
    for(uint32_t i = 0; i < n; ++i) {
      if(i && !ahead(i).glued) s += ' ';
      s += ahead(i).ch();
    }
    return s;
  }

  bool starts_operand(uint32_t k) const {
    token const & t = ahead(k);
    if(t.kind == tok::ident) return !is_keyword(t.text) || t.text == "if" || t.text == "throw" || t.text == "ct";
    if(t.kind == tok::int_lit || t.kind == tok::float_lit || t.kind == tok::str_lit) return true;
    if(t.kind == tok::open) return t.ch() == '(' || t.ch() == '{';
    return t.kind == tok::op && t.ch() == '#';
  }

  // Attribution is the shape of the nucleus on top of the chain: place = expression (right to left).
  node_id expression() {
    node_id lhs = chain();
    if(tail_statement || (db.is<n_if>(lhs) && if_closed(lhs))) return lhs;
    for(std::string_view a: {"+=", "-=", "*=", "/=", "="})
      if(is_op(a)) {
        uint32_t start = db[lhs].where.begin;
        eat_op(a);
        node_id rhs = expression();
        return db.add(n_op{db.strings.intern(a), {lhs, rhs}}, {start, db[rhs].where.end});
      }
    return lhs;
  }

  node_id chain() {
    uint32_t start = here().where.begin;
    std::vector<std::string> pre;
    if(uint32_t n = run_length(); n > 0 && !is_op("=>")) { // first operand prefix
      std::vector<run_split> v;
      run_split cur;
      splits(0, 0, n, 1, false, cur, v);
      if(v.empty()) throw diagnostic(cat("нет префиксного оператора «", run_text(n), "»"), here().where);
      if(v.size() > 1) throw diagnostic(cat("запись операторов «", run_text(n), "» неоднозначна — разделите пробелом"), here().where);
      pre = v[0].pre;
      for(uint32_t i = 0; i < n; ++i) advance();
    }
    node_list atoms;
    std::vector<std::string> infix;
    for(;;) {
      uint32_t a0 = here().where.begin;
      node_id a = unary_core();
      bool closed = tail_statement || (db.is<n_if>(a) && if_closed(a));
      uint32_t n = closed ? 0 : run_length();
      run_split sp;
      bool more = false;
      if(n > 0) {
        more = starts_operand(n);
        std::vector<run_split> v;
        run_split cur;
        splits(0, 0, n, 0, more, cur, v);
        bool assign = is_op("=") || is_op("+=") || is_op("-=") || is_op("*=") || is_op("/=");
        if(v.empty() && assign) n = 0, more = false; // further assignment - the form of the nucleus
        else if(v.empty()) {
          std::vector<run_split> w;
          if(!more) splits(0, 0, n, 0, true, cur, w);
          if(!w.empty()) throw diagnostic("ожидалось выражение", ahead(n).where); // a + ;
          throw diagnostic(cat("нет оператора «", run_text(n), "»"), here().where);
        } else if(v.size() > 1) throw diagnostic(cat("запись операторов «", run_text(n), "» неоднозначна — разделите пробелом"), here().where);
        else {
          sp = v[0];
          for(uint32_t i = 0; i < n; ++i) advance();
        }
      }
      for(auto const & p: sp.post) a = db.add(n_op{db.strings.intern(p), {a}, true}, {a0, toks[pos - 1].where.end});
      for(auto it = pre.rbegin(); it != pre.rend(); ++it) a = db.add(n_op{db.strings.intern(*it), {a}}, {a0, db[a].where.end});
      atoms.push_back(a);
      if(!more) break;
      infix.push_back(sp.infix);
      pre = sp.pre;
    }
    if(atoms.size() == 1) return atoms[0];
    bool same = std::ranges::all_of(infix, [&](auto const & s) { return s == infix[0]; });
    if(infix.size() == 1 || (same && operators[infix[0]].assoc)) { // tree
      node_id t = atoms[0];
      for(std::size_t k = 0; k < infix.size(); ++k)
        t = db.add(n_op{db.strings.intern(infix[k]), {t, atoms[k + 1]}}, {start, db[atoms[k + 1]].where.end});
      return t;
    }
    n_chain c;
    c.atoms = std::move(atoms);
    for(auto const & s: infix) c.ops.push_back(db.strings.intern(s));
    return db.add(std::move(c), {start, toks[pos - 1].where.end});
  }

  node_id unary_core() {
    uint32_t start = here().where.begin;
    if(is_op("#") && ahead(1).kind == tok::ident) { // #x - inserting a value into a quote
      eat_op("#");
      symbol name = ident();
      return db.add(n_splice{name}, {start, toks[pos - 1].where.end});
    }
    if(is_word("throw")) { // throw
      advance();
      node_id x = expression();
      return db.add(n_throw{x}, {start, db[x].where.end});
    }
    return postfix(primary());
  }

  node_id postfix(node_id e) {
    for(;;) {
      uint32_t start = db[e].where.begin;
      if(is_open('(') && here().glued) {
        e = call(e, start);
        if(tail_statement) return e; // Instruction-tail completed the instruction
      } else if(is_open('{') && !here().glued && construct_of(e) && construct_of(e)->bare) { // try { … }
        e = call(e, start);
      } else if(is_open('[') && here().glued) {
        advance();
        node_id i = expression();
        expect_close(']');
        e = db.add(n_index{e, i}, {start, toks[pos - 1].where.end});
      } else if(is_open('{') && here().glued && db.is<n_ident>(e)) {
        e = db.add(n_construct{e, brace_items()}, {start, toks[pos - 1].where.end});
      } else if(is_op(".") && !is_op("..")) {
        advance();
        symbol name = ident();
        e = db.add(n_member{e, name}, {start, toks[pos - 1].where.end});
      } else if(is_op("?") && here().glued) {
        advance();
        e = db.add(n_op{db.strings.intern("?"), {e}}, {start, toks[pos - 1].where.end});
      } else {
        return e;
      }
    }
  }

  // Call: head (arguments) [tail] and clauses word (arguments) { … } from the
  // design announcement.
//  
// announcement in brackets - `auto x: xs`, `E e` is a parameter
  // The tailblock, not the expression.
  node_id call(node_id callee, uint32_t start) {
    construct const * con = construct_of(callee);
    bool head_of_statement = db[callee].where.begin == statement_head;
    n_call c;
    c.callee = callee;
    group(c, symbol{}, con, head_of_statement);
    while(con && !tail_statement && here().kind == tok::ident && (is_open('(', 1) || is_open('{', 1))) {
      symbol word = db.strings.intern(here().text);
      if(std::ranges::none_of(con->clauses, [&](auto const & w) { return w.first == word; })) break;
      advance();
      group(c, word, con, false);
    }
    // repetitive clause - one argument-pack in place of the first
    if(con)
      for(auto [word, pack]: con->clauses) {
        if(!pack) continue;
        n_pack pk;
        uint32_t at = ~uint32_t{0};
        span w{};
        for(uint32_t i = 0; i < c.args.size();)
          if(c.words[i] == word) {
            if(at == ~uint32_t{0}) at = i, w = db[c.args[i]].where;
            w.end = db[c.args[i]].where.end;
            pk.items.push_back(c.args[i]);
            c.args.erase(c.args.begin() + i);
            c.words.erase(c.words.begin() + i);
          } else ++i;
        if(at == ~uint32_t{0}) continue;
        c.args.insert(c.args.begin() + at, db.add(std::move(pk), w));
        c.words.insert(c.words.begin() + at, word);
      }
    if(std::ranges::all_of(c.words, [](symbol w) { return !w; })) c.words.clear();
    return db.add(std::move(c), {start, toks[pos - 1].where.end});
  }

  // One group of call: (arguments) and tail - block or (at the beginning of the
  // instruction) instruction.
  void group(n_call & c, symbol word, construct const * con, bool head_of_statement) {
    std::vector<n_param> binders;
    std::vector<span> binder_at;
    uint32_t first = uint32_t(c.args.size());
    if(is_open('(')) {
      advance();
      while(!is_close(')')) {
        if(auto b = binder()) {
          span at{b->second, 0};
          if(is_op(":")) {
            eat_op(":");
            b->first.source = int64_t(c.args.size() - first);
            c.args.push_back(expression());
            c.words.push_back(word);
          }
          at.end = toks[pos - 1].where.end;
          binders.push_back(std::move(b->first));
          binder_at.push_back(at);
        } else {
          c.args.push_back(expression());
          c.words.push_back(word);
        }
        if(is_op(",")) eat_op(",");
        else break;
      }
      expect_close(')');
    }
    bool packed = word && con && std::ranges::any_of(con->clauses, [&](auto const & w) { return w.first == word && w.second; });
    if(packed && c.args.size() > first) // Repetitive Clause - a block with parameters: catch(E e), catch(auto e)
      throw diagnostic(cat("в скобках клаузы ", db.strings.text(word), " — только объявления: ", db.strings.text(word), "(auto e)"), db[c.args[first]].where);
    node_id body;
    if(is_open('{')) body = block();
    else if(word == symbol{} && con && con->tail && head_of_statement && here().kind == tok::ident && !is_word("else")) {
      body = statement();
      tail_statement = true;
    }
    if(!body) {
      if(!binders.empty()) throw diagnostic("объявление в скобках вводит имя в блок — нужен блок", here().where);
      if(word) throw diagnostic(cat("после клаузы ", db.strings.text(word), " ожидался блок"), here().where);
      return;
    }
    if(!binders.empty()) {
      n_lambda l;
      for(std::size_t k = 0; k < binders.size(); ++k) l.params.push_back(db.add(std::move(binders[k]), binder_at[k]));
      l.body = body;
      l.block = true;
      body = db.add(std::move(l), db[body].where);
    }
    c.args.push_back(body);
    c.words.push_back(word);
    if(!word) c.tail = true; // The block is always the last.
  }

  // auto x | Type x before `:`, `,` or `)` is a block parameter (not added to the base).
  std::optional<std::pair<n_param, uint32_t>> binder() {
    if(ahead(0).kind != tok::ident || ahead(1).kind != tok::ident) return std::nullopt;
    bool ends = is_op(":", 2) || is_op(",", 2) || (ahead(2).kind == tok::close && ahead(2).ch() == ')');
    if(!ends) return std::nullopt;
    uint32_t start = here().where.begin;
    n_param p;
    p.type = type_expr();
    p.name = ident();
    return std::pair{std::move(p), start};
  }

  node_list brace_items() {
    expect_open('{');
    node_list items;
    while(!is_close('}')) {
      items.push_back(expression());
      if(is_op(",")) eat_op(",");
      else break;
    }
    expect_close('}');
    return items;
  }

  node_id primary() {
    token const & t = here();
    uint32_t start = t.where.begin;
    switch(t.kind) {
      case tok::int_lit: {
        advance();
        return db.add(n_int{parse_int(t.text)}, t.where);
      }
      case tok::float_lit: {
        advance();
        return db.add(n_float{parse_float(t.text)}, t.where);
      }
      case tok::str_lit: {
        advance();
        return db.add(n_str{unescape(t.text)}, t.where);
      }
      case tok::ident: {
        if(t.text == "if" && is_open('(', 1)) return if_expression();
        if(t.text == "loop" && is_open('{', 1)) {
          advance();
          node_id body = block();
          return db.add(n_loop{body}, {start, toks[pos - 1].where.end});
        }
        if(t.text == "quote" && is_open('{', 1)) { // quote { announcements }
          advance();
          expect_open('{');
          n_quote q;
          while(!is_close('}') && !at_end()) q.items.push_back(recover_statement([&] { return item(); }));
          expect_close('}');
          return db.add(std::move(q), {start, toks[pos - 1].where.end});
        }
        if(t.text == "ct") {
          advance();
          node_id body = is_open('{') ? block() : unary_core();
          return db.add(n_ct{body}, {start, toks[pos - 1].where.end});
        }
        advance();
        return db.add(n_ident{db.strings.intern(t.text)}, t.where);
      }
      case tok::open: {
        if(t.ch() == '{') return block();
        if(t.ch() == '(') {
          // lambda: (parameters) => body
          uint32_t close = match[pos];
          if(close != ~uint32_t{0} && is_op("=>", close + 1 - pos)) {
            n_lambda l;
            l.params = param_list();
            eat_op("=>");
            l.body = is_open('{') ? block() : expression();
            return db.add(std::move(l), {start, toks[pos - 1].where.end});
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
    uint32_t start = here().where.begin;
    advance();
    expect_open('(');
    node_id c = expression();
    expect_close(')');
    node_id then_ = branch();
    node_id else_;
    if(is_op(";") && is_word("else", 1)) eat_op(";"); // if(c) x = 1; else x = 2;
    if(is_word("else")) {
      advance();
      else_ = branch();
    }
    return db.add(n_if{c, then_, else_}, {start, toks[pos - 1].where.end});
  }

  // Branch if: Block, nested if or expression/instruction.
  node_id branch() {
    if(is_open('{')) return block();
    if(is_word("return") || is_word("require") || is_word("break") || is_word("continue")) return statement();
    return expression();
  }

  // --- literary ---

  static int64_t parse_int(std::string_view s) {
    int64_t v = 0;
    for(char c: s)
      if(c != '_') v = v * 10 + (c - '0');
    return v;
  }

  static double parse_float(std::string_view s) {
    std::string clean;
    for(char c: s)
      if(c != '_') clean += c;
    return std::stod(clean);
  }

  static std::string unescape(std::string_view s) {
    std::string out;
    for(uint32_t i = 1; i + 1 < s.size(); ++i) {
      if(s[i] == '\\' && i + 2 < s.size()) {
        char n = s[++i];
        out += n == 'n' ? '\n' : n == 't' ? '\t'
                                          : n;
      } else {
        out += s[i];
      }
    }
    return out;
  }

  diagnostic with_source(diagnostic d) const {
    span w = d.where;
    w.source = source;
    diagnostic out(d.what(), w, d.kind);
    for(auto f: d.trace) {
      f.where.source = source;
      out.note(f.what, f.where);
    }
    return out;
  }

  ast_db & db;
  uint32_t source = 0;
  std::string_view src;
  std::vector<token> toks;
  std::vector<uint32_t> match;
  uint32_t pos = 0;
  bool last_was_value = false;
  uint32_t statement_head = ~uint32_t{0}; // starting-point
  bool tail_statement = false; // The tail of the design - the instruction, she finished the instruction
  std::unordered_map<uint32_t, construct> constructs; // head-name
  struct op_forms {
    bool prefix = false, infix = false, postfix = false, assoc = false;
  };
  std::map<std::string, op_forms> operators; // symbolically
  std::vector<diagnostic> diags;
};

} // namespace tz
