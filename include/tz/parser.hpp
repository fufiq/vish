// parsing text projection to the AST base (§4 stack).
//
// Phase 1: the table of paired brackets - unpaired are repaired by indentation. Phase
// 2: operators. An error inside brackets turns only these brackets into n_hole; parsing
// continues after the closing bracket. Any text gives you AST.
//
//
// Phase 2 rules (operators, syntax records, construction) - from ads in the context
// of the place: module, block, prelude as external (§4.4, “contexts”).
#pragma once

#include "ast.hpp"
#include "lexer.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <set>

namespace tz {

node_id load_prelude(ast_db & db); // prelude.hpp: a module of constructions written in the language

struct parser {
  // The parser reads a copy of the text stored by the database when importing: tokens and
  // holes refer to it, not to the caller string (which may be temporary).
  parser(ast_db & db, std::string_view text, std::string label = {}) : db(db) {
    load_prelude(db); // before import: node of this text from this import
    source = db.record_import(std::string(text), std::move(label));
    src = db.source(source);
    lexer lx(src);
    toks = lx.run();
    fold_ast_names();
    diags = std::move(lx.errors); // token::err - Diag numbers
    if(!diags.empty()) lex_error = diags.front().what();
    take_comments(lx.comments, 0);
    match_brackets();
  }

  // The section of the already imported text is a sub-block of the raw block given to the
  // standard parsing (§4.4, "parsing code"): the positions node - in the same text, without new
  // imports (the import of new node is specified by the calling: ast_db::pinned_source).
  parser(ast_db & db, uint32_t from, span range) : db(db) {
    source = from;
    src = db.source(source);
    lexer lx(src.substr(range.begin, range.end - range.begin));
    toks = lx.run();
    for(auto & t: toks) {
      t.where.begin += range.begin;
      t.where.end += range.begin;
    }
    fold_ast_names();
    for(auto const & d: lx.errors) diags.push_back(diagnostic(d.what(), {d.where.begin + range.begin, d.where.end + range.begin}));
    if(!diags.empty()) lex_error = diags.front().what();
    take_comments(lx.comments, range.begin);
    match_brackets();
  }

  // The name node AST is one name: `ast.if`, `ast.call` (module ast, author's decision 2026-10-17).
  // The `ast` `.` word is one lexeme-name: the words of the node species (if, return, use...) after
  // `ast.` are not words of the language. `ast` is the name of the module, not the variable.
  void fold_ast_names() {
    std::vector<token> out;
    for(std::size_t i = 0; i < toks.size(); ++i) {
      token t = toks[i];
      bool after_dot = !out.empty() && out.back().kind == tok::op && out.back().ch() == '.' && t.glued;
      if(t.kind == tok::ident && t.text == "ast" && !after_dot && i + 2 < toks.size() && toks[i + 1].kind == tok::op && toks[i + 1].ch() == '.' && toks[i + 1].glued &&
         toks[i + 2].kind == tok::ident && toks[i + 2].glued) {
        t.text = std::string_view(t.text.data(), std::size_t(toks[i + 2].text.data() + toks[i + 2].text.size() - t.text.data()));
        t.where.end = toks[i + 2].where.end;
        i += 2;
      }
      out.push_back(t);
    }
    toks = std::move(out);
  }

  // --- Comments: Context Elements (§4.0) ---
  //
  // Author's Solution (2026-10-05): Comments and empty lines are in the language, not lost
  // projection. *My:* Comment is the text of a person with a context element (node_note): the
  // lines before the element, the tail of its line, the lines before the end of the context;
  // within the expression, the line before the element. The `/* дыра: … */` parsing hole does
  // not print:
//.
  void take_comments(std::vector<span> const & cs, uint32_t shift) {
    for(span c: cs) {
      c.begin += shift;
      c.end += shift;
      if(src.substr(c.begin, c.end - c.begin).starts_with("/* дыра:")) continue;
      comments.push_back(c);
    }
    claimed.assign(comments.size(), 0);
  }
  uint32_t first_comment(uint32_t at) const {
    return uint32_t(std::ranges::partition_point(comments, [&](span c) { return c.begin < at; }) - comments.begin());
  }
  uint32_t newlines(uint32_t a, uint32_t b) const {
    uint32_t n = 0;
    for(uint32_t k = a; k < b && k < src.size(); ++k) n += src[k] == '\n';
    return n;
  }
  // The comments of subtree n are the contexts inserted first. Hole, raw block and native
  // type the text as written - their comments are already in it.
  void attach(node_id n) {
    span w = db[n].where;
    if(w.source != source) return;
    if(db.is<n_hole>(n) || db.is<n_raw>(n) || db.is<n_native>(n)) {
      for(uint32_t i = first_comment(w.begin); i < comments.size() && comments[i].begin < w.end; ++i) claimed[i] = 1;
      return;
    }
    db.for_each_child(n, [&](node_id c) { attach(c); });
    if(db.is<n_block>(n)) assign(n, db.as<n_block>(n).items, w.begin, w.end);
    else if(db.is<n_module>(n)) assign(n, db.as<n_module>(n).items, 0, uint32_t(src.size()));
  }
  // parsing root: The remaining comments are the root (an element with no context).
  void attach_root(node_id n) {
    if(comments.empty() || !n) return;
    attach(n);
    if(!db.is_context(n)) assign({}, {n}, 0, uint32_t(src.size()));
  }
  // Commentaries of the context of ctx (empty - the string of the set: the end - to the tail of the
  // latter) in [from, to], not yet taken by the nested: on the line of the end of the element - its
  // tail, on its lines or inside the element - the lines in front of it, after the last - before the
  // end. rest - draft editor: comments with their lines after the last element of the line of the
  // set - not the tail of the latter, but here (they - the next element).
  void assign(node_id ctx, node_list const & items, uint32_t from, uint32_t to, std::vector<std::string> * rest = nullptr) {
    std::vector<node_note> ns(items.size() + 1);
    std::vector<uint32_t> last(items.size() + 1, ~uint32_t{0}); // end of last comment before k
    std::vector<std::vector<std::string>> inner(items.size()); // Comments on the element k
    bool any = false;
    for(uint32_t i = first_comment(from); i < comments.size() && comments[i].begin < to; ++i) {
      if(claimed[i]) continue;
      claimed[i] = 1;
      any = true;
      span c = comments[i];
      std::string text(src.substr(c.begin, c.end - c.begin));
      uint32_t k = uint32_t(std::ranges::partition_point(items, [&](node_id x) { return db[x].where.begin <= c.begin; }) - items.begin());
      if(k > 0 && c.begin < db[items[k - 1]].where.end) { // inside the element (in expression) - the line in front of it
        inner[k - 1].push_back(std::move(text));
        continue;
      }
      bool tail = k > 0 && last[k] == ~uint32_t{0} && newlines(db[items[k - 1]].where.end, c.begin) == 0;
      if(!ctx && !rest && k == items.size() && k > 0) tail = true;
      if(tail) {
        std::string & t = ns[k - 1].tail;
        t += t.empty() ? text : cat(" ", text);
        continue;
      }
      auto & lines = k < items.size() ? ns[k].lead : ns[k].end;
      if(last[k] != ~uint32_t{0})
        for(uint32_t n = newlines(last[k], c.begin); n > 1; --n) lines.push_back("");
      lines.push_back(std::move(text));
      last[k] = c.end;
    }
    if(!any) return;
    for(uint32_t k = 0; k < items.size(); ++k) // Empty lines between comments and elements
      if(last[k] != ~uint32_t{0})
        for(uint32_t n = newlines(last[k], db[items[k]].where.begin); n > 1; --n) ns[k].lead.push_back("");
    for(uint32_t k = 0; k < items.size(); ++k) ns[k].lead.insert(ns[k].lead.end(), inner[k].begin(), inner[k].end());
    if(ctx && db.is<n_block>(ctx) && last.back() != ~uint32_t{0})
      for(uint32_t n = newlines(last.back(), to - 1); n > 1; --n) ns.back().end.push_back("");
    auto merge = [&](node_id x, node_note const & add) {
      if(add.lead.empty() && add.tail.empty() && add.end.empty()) return;
      node_note now = db.note_of(x) ? *db.note_of(x) : node_note{};
      if(!add.lead.empty()) now.lead = add.lead;
      if(!add.tail.empty()) now.tail = now.tail.empty() ? add.tail : cat(add.tail, " ", now.tail);
      if(!add.end.empty()) now.end = add.end;
      db.set_note(x, std::move(now));
    };
    for(uint32_t k = 0; k < items.size(); ++k) merge(items[k], ns[k]);
    if(ctx) merge(ctx, ns.back());
    else if(rest) *rest = std::move(ns.back().end);
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
    std::vector<node_id> decls; // announcements (overload): their body rules for call blocks
  };

  void learn(n_fn const & f, node_id id = {}) {
    if(!f.name || raw_rule_form(db, f)) return; // Syntax {r} => is a rule of blocks of construction, not of its context.
    if(!f.form.empty()) { // Syntax - a form of recording, not a call design
      if(!id) return;
      touch();
      rules.syntaxes[f.name.value].push_back(id);
      if(record_of(db, id)) // Write words are not names: `if(c) { … }` does not declare an if function
        for(symbol w: f.form)
          if(lexer::is_ident_start(db.strings.text(w)[0])) rules.record_words.insert(std::string(db.strings.text(w)));
      return;
    }
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
    touch();
    construct & c = rules.constructs[f.name.value];
    if(id && !std::ranges::contains(c.decls, id)) c.decls.push_back(id);
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
    auto it = rules.constructs.find(db.as<n_ident>(callee).name.value);
    return it == rules.constructs.end() ? nullptr : &it->second;
  }

  // --- Contexts: parsing rules from ads in an area (§4.4) ---
  //
  // A syntax, operator or construct ad is valid from itself to the end of its context
  // (module, block - n_context) and in nested:
// 
// 
// prelude is the external context of all modules.
  // use k; brings the rules of modulus k to the use point (module-include, §9.1).

  // The rule changes in the nested context - the old rules remain until the end.
  void touch() {
    if(depth && (saved.empty() || saved.back().first != depth)) saved.push_back({depth, rules});
  }
  // The block is the context of the rules: the output of the rule is as before it (and with an error inside).
  struct context_scope {
    parser & p;
    explicit context_scope(parser & p) : p(p) { ++p.depth; }
    ~context_scope() {
      if(!p.saved.empty() && p.saved.back().first == p.depth) {
        p.rules = std::move(p.saved.back().second);
        p.saved.pop_back();
      }
      --p.depth;
    }
  };

  // The design specifies the parsing rules of its block from its body (user solution
  // 2026-10-02): the syntax, operator, and design ads in the declaration body operate in the
  // blocks transmitted to it at the call point over the rules of the place; the nested inserts
  // receive the rules of both.
// 
//;
  // 
// 
// My: * The overloads are theirs
  // unification (which is chosen - decide the types at instantiation, parsing before).
  void learn_body(node_id d) {
    if(!db.is<n_fn>(d) || !db.is<n_block>(db.as<n_fn>(d).body)) return;
    for(node_id x: db.as<n_block>(db.as<n_fn>(d).body).items) learn_item(x);
  }
  // Block (or tail instruction) of a call: its context with decls body rules.
  node_id ruled(std::vector<node_id> const & decls, auto && parse) {
    context_scope scope(*this);
    for(node_id d: decls) learn_body(d);
    return parse();
  }

  // Rules that declare the context element: function (design, operator, syntax), extension
  // function Name { … }, use k (module k rules).
  void learn_item(node_id x) {
    if(db.is<n_fn>(x)) learn(db.as<n_fn>(x), x);
    else if(db.is<n_extend>(x)) {
      for(node_id f: db.as<n_extend>(x).items)
        if(db.is<n_fn>(f)) learn(db.as<n_fn>(f), f);
    } else if(db.is<n_use>(x)) learn_use(db.as<n_use>(x));
  }
  // use k; / use k. {a, b}; - rules of module k to its end (selective - only called
  // entity; operators - always: module - semantic inclusion,
// The operator has no name - the decision
  // user 2026-10-02; use inside k as written in k. The module is not yet
  // disassembled (use cycle) - without rules.
  void learn_use(n_use const & u) {
    node_id m = find_module(db, u.key);
    if(!m || std::ranges::contains(learning, m)) return;
    learning.push_back(m);
    for(node_id x: db.as<n_module>(m).items) {
      symbol name = db.is<n_fn>(x) ? db.as<n_fn>(x).name : db.is<n_extend>(x) ? db.as<n_extend>(x).name : symbol{};
      std::string_view nm = name ? db.strings.text(name) : std::string_view{};
      bool op = nm.starts_with("infix") || nm.starts_with("prefix") || nm.starts_with("postfix");
      if(db.is<n_use>(x) || op || u.names.empty() || std::ranges::contains(u.names, name)) learn_item(x);
    }
    learning.pop_back();
  }
  node_list learning; // Modules whose rules are taken (use cycle)

  // The trace of an element's rules is what learn_item takes from it: the edit that changes the
  // trail changes parsing after the element in its context (the editor re-examines the following).
  static std::string rule_trace(ast_db const & db, node_id x) {
    std::string out;
    auto body = [&](node_id f) { // The rules of the body of the structure - the rules of its blocks
      if(!db.is<n_block>(db.as<n_fn>(f).body)) return;
      std::string in;
      for(node_id x: db.as<n_block>(db.as<n_fn>(f).body).items) in += rule_trace(db, x);
      if(!in.empty()) out += cat("{", in, "}");
    };
    auto fn = [&](node_id f) {
      auto const & d = db.as<n_fn>(f);
      if(!d.name) return;
      std::string_view name = db.strings.text(d.name);
      if(!d.form.empty()) {
        out += "syntax";
        for(symbol w: d.form) out += cat(" ", db.strings.text(w));
        if(auto r = record_of(db, f)) {
          out += cat(" =", r->kind);
          for(uint32_t s: r->slot) out += cat(" ", s);
        }
        body(f);
        out += ";";
        return;
      }
      bool rule = false;
      for(std::string_view role: {"infix", "prefix", "postfix"})
        rule = rule || (name.size() > role.size() && name.starts_with(role) && !lexer::is_ident_start(name[role.size()]));
      for(node_id pn: d.params)
        rule = rule || db.as<n_param>(pn).clause || db.as<n_param>(pn).mode == param_mode::expr;
      if(!rule) return;
      out += cat(name, "(");
      for(node_id pn: d.params) {
        auto const & p = db.as<n_param>(pn);
        out += cat(p.clause ? db.strings.text(p.clause) : "", p.pack ? "..." : "", p.mode == param_mode::expr ? "expr" : "", ",");
      }
      out += ")";
      for(node_id a: d.attrs)
        if(db.is<n_ident>(a)) out += cat("[[", db.strings.text(db.as<n_ident>(a).name), "]]");
      body(f);
      out += ";";
    };
    if(db.is<n_fn>(x)) fn(x);
    else if(db.is<n_extend>(x)) {
      for(node_id f: db.as<n_extend>(x).items)
        if(db.is<n_fn>(f)) fn(f);
    } else if(db.is<n_use>(x)) {
      out += cat("use ", db.strings.text(db.as<n_use>(x).key));
      for(symbol s: db.as<n_use>(x).names) out += cat(" ", db.strings.text(s));
      out += ";";
    }
    return out;
  }

  // Place rules: prelude, then contexts from the outside to the place - announcements of each
  // to the item on the way to the place (in ctx - before; empty - everything). The block of a
  // design call on the way is the rules of its body (according to the rules of the call place).
  void place(node_id ctx, node_id before = {}) {
    placed = true;
    std::vector<std::pair<node_id, node_id>> chain; // (context or challenge, its element on the way)
    for(node_id c = ctx, at = before; c; at = c, c = db[c].parent)
      if(db.is_context(c) || (at && call_block_arg(c, at))) chain.push_back({c, at});
    if(db.prelude && (chain.empty() || chain.back().first != db.prelude))
      for(node_id x: db.as<n_module>(db.prelude).items) learn_item(x);
    for(auto it = chain.rbegin(); it != chain.rend(); ++it) {
      if(!db.is_context(it->first)) {
        for(node_id d: call_decls(it->first)) learn_body(d);
        continue;
      }
      for(node_id x: db.context_items(it->first)) {
        if(x == it->second) break;
        learn_item(x);
      }
    }
  }
  // a - call block c (block, block with parameters, tail instruction; clause - and in pack)
  bool call_block_arg(node_id c, node_id a) const {
    if(db.is<n_construct>(c) && db.is<n_call>(db[c].parent) && !db.as<n_call>(db[c].parent).words.empty()) // Pack Clause: catch { } catch { }
      return std::ranges::contains(db.as<n_construct>(c).items, a) && block_like(a);
    if(!db.is<n_call>(c)) return false;
    auto const & k = db.as<n_call>(c);
    if(!std::ranges::contains(k.args, a)) return false;
    return block_like(a) || (k.tail && k.args.back() == a);
  }
  bool block_like(node_id a) const { return db.is<n_block>(a) || db.is<n_raw>(a) || (db.is<n_lambda>(a) && db.as<n_lambda>(a).block); }
  // Announcement of the design of the call c according to the rules of the place (the syntax record is its announcement).
  std::vector<node_id> call_decls(node_id c) const {
    if(db.is<n_construct>(c)) c = db[c].parent;
    auto const & k = db.as<n_call>(c);
    if(k.form) return {node_id(k.form - 1)};
    if(auto con = construct_of(k.callee)) return con->decls;
    return {};
  }

  node_id parse_module() {
    if(!placed) place({}); // new module: prelude rules
    uint32_t start = here().where.begin;
    n_module m;
    // sealed removed (author's decision 2026-10-05):
// - a tip error,
    // module disassembles without a word
    if(is_word("sealed") && is_word("module", 1)) {
      diags.push_back(diagnostic("sealed убран: контроль расширений — [[extension(own_only)]] module …; (§9.1)", here().where));
      advance();
    }
    if(module_attrs_ahead()) m.attrs = attributes(); // [[extension]] module k;
    if(is_word("module")) {
      advance();
      m.name = db.strings.intern(dotted_name());
      expect_op(";");
    }
    while(!at_end()) {
      m.blank.push_back(m.items.empty() ? 0 : blank_lines(2)); // Canonically between the elements is an empty line.
      m.items.push_back(recover_statement([&] { return item(); }));
    }
    node_list scope = m.items;
    scope.insert(scope.end(), outer_items.begin(), outer_items.end());
    resolve_refs(scope);
    trim_blank(m.blank);
    auto id = db.add(std::move(m), {start, here().where.end});
    db.link_parents(id);
    attach_root(id);
    return id;
  }

  // [[…]] Before Module: Module Attributes, Not First Element
  bool module_attrs_ahead() const {
    uint32_t k = pos;
    while(k + 1 < toks.size() && toks[k].kind == tok::open && toks[k].ch() == '[' && toks[k + 1].kind == tok::open && toks[k + 1].ch() == '[') k = match[k] + 1;
    return k != pos && k < toks.size() && toks[k].kind == tok::ident && toks[k].text == "module";
  }

  // Editor unit (§13.1): `{…}` block or entire module element.
  // 
// Empty - text not
  // It forms such a unit (its own brackets are broken): the edit goes up to the outer
  // brackets. Mistakes inside are holes, like everywhere else.
  node_id parse_unit(bool as_block) {
    try {
      node_id n = as_block ? block() : item();
      if(!at_end()) return {};
      db.link_parents(n);
      attach_root(n);
      return n;
    } catch(diagnostic const &) {
      return {};
    }
  }

  // Raw block - editing unit (§4.4, §13.1): edit text - the entire block with brackets, only
  // phase 1 (atoms and sub-blocks) is disassembled, no place rules are needed.
  // 
// Empty - text not
  // It forms one block (its parentheses or lexemes are broken): editing - to the external unit.
  node_id parse_raw(uint32_t rule) {
    if(!lex_error.empty() || bracket_error || at_end() || here().kind != tok::open) return {};
    try {
      node_id n = raw_tree(rule);
      if(!at_end()) return {};
      db.link_parents(n);
      return n;
    } catch(diagnostic const &) {
      return {};
    }
  }

  // The typing line in the editor (author's decision: the line is like a new block, not
  // yet disassembled): its instructions (or module elements) are disassembled
  // separately from neighboring ones - the unwritten becomes a hole only for this line
  // and does not capture the next instruction. Broken brackets - empty: editing rises
  // to outer brackets. Value is the last instruction without `;` (block value).
  //
  std::optional<node_list> parse_line(bool module_level, bool & value) {
    value = false;
    if(!lex_error.empty()) return node_list{db.add(n_hole{std::string(src), lex_error}, {0, uint32_t(src.size())})};
    if(bracket_error) return std::nullopt;
    line_mode = true;
    node_list out;
    while(!at_end()) {
      out.push_back(recover_statement([&] { return module_level ? item() : statement(); }));
      value = !module_level && at_end() && last_was_value;
    }
    for(node_id n: out) db.link_parents(n);
    if(!comments.empty()) { // line comments - its elements (without elements - disappear: debt)
      for(node_id n: out) attach(n);
      assign({}, out, 0, uint32_t(src.size()));
    }
    return out;
  }

  // Draft: Instruction without `;`, followed by a new line on the same or smaller indentation,
  // is not written: it ends on its line (empty `;` with the cause - it will become one hole),
  // and does not capture the next one -
// (author's decision 2026-09-29). Only in
  // Instruction contexts (`{…}` and text), not within `(…)` and `[…]`; line continuation
  // is after `;`, `,`, opening, `}`, and `]]` attributes.
  void end_unfinished_by_lines() {
    struct level {
      bool statements; // `{…}` or all text
      uint32_t indent; // indentation of the beginning line of the current instruction (~0 - not started)
    };
    std::vector<level> st{{true, ~uint32_t{0}}};
    std::vector<token> out;
    bool added = false;
    for(token const & t: toks) {
      bool first = t.kind != tok::end && line_begin(t.where.begin) + indent_of(t.where.begin) == t.where.begin;
      if(st.back().statements && first && st.back().indent != ~uint32_t{0} && t.kind != tok::close && !out.empty() && indent_of(t.where.begin) <= st.back().indent) {
        token const & p = out.back();
        bool attr = p.kind == tok::close && p.ch() == ']' && out.size() > 1 && out[out.size() - 2].kind == tok::close && out[out.size() - 2].ch() == ']';
        bool cont = p.kind == tok::open || (p.kind == tok::close && p.ch() == '}') || attr || (p.kind == tok::op && (p.ch() == ';' || p.ch() == ','));
        if(!cont) {
          token semi;
          semi.kind = tok::op;
          semi.text = ";";
          semi.where = {p.where.end, p.where.end};
          semi.glued = true;
          semi.err = uint32_t(diags.size());
          diags.push_back(diagnostic("инструкция не дописана: нет ';' в конце строки", p.where));
          out.push_back(semi);
          st.back().indent = ~uint32_t{0};
          added = true;
        }
      }
      if(t.kind == tok::open) {
        if(st.back().indent == ~uint32_t{0}) st.back().indent = indent_of(t.where.begin);
        st.push_back({t.ch() == '{', ~uint32_t{0}});
      } else if(t.kind == tok::close) {
        if(st.size() > 1) st.pop_back();
        if(t.ch() == '}') st.back().indent = ~uint32_t{0};
      } else if(t.kind == tok::op && t.ch() == ';') st.back().indent = ~uint32_t{0};
      else if(t.kind != tok::end && st.back().indent == ~uint32_t{0}) st.back().indent = indent_of(t.where.begin);
      out.push_back(t);
    }
    if(!added) return;
    toks = std::move(out);
    rematch();
  }

  // Draft editor (author's decision 2026-10-12): elements of typed text as is. Unpaired when
  // typing, the bracket closes by indentation (§4.1) - the edit does not rise to external
  // brackets; the unwritten and non-lexemic (unclosed line - to the end of the line of text) -
  // the hole of its instructions. Empty lines - for the canon when leaving: before the first
  // element (the text of the draft begins after the line of the previous element), between the
  // elements and after the last (before the next element of the module); comments with their
  // own lines after the last - rest.
  struct draft_items {
    node_list items;
    std::vector<uint32_t> blank; // empty lines before the element k; the last after the elements
    std::vector<std::string> rest;
  };
  draft_items parse_draft(bool module_level) {
    draft_items out;
    auto gap_lines = [&](uint32_t from, uint32_t to) { // line translations before the first interval comment
      uint32_t i = first_comment(from);
      if(i < comments.size() && comments[i].begin < to) to = comments[i].begin;
      return newlines(from, to);
    };
    end_unfinished_by_lines();
    line_mode = true;
    uint32_t end = 0;
    while(!at_end()) {
      if(out.items.empty()) {
        uint32_t n = gap_lines(0, here().where.begin);
        out.blank.push_back(n > 1 ? n - 1 : 0);
      } else out.blank.push_back(blank_lines(2));
      out.items.push_back(recover_statement([&] { return module_level ? item() : statement(); }));
      end = toks[pos - 1].where.end;
    }
    uint32_t n = gap_lines(end, uint32_t(src.size()));
    uint32_t canon = out.items.empty() ? 1 : 2;
    out.blank.push_back(n > canon ? n - canon : 0);
    for(node_id x: out.items) db.link_parents(x);
    if(!comments.empty()) {
      for(node_id x: out.items) attach(x);
      assign({}, out.items, 0, uint32_t(src.size()), &out.rest);
    }
    return out;
  }

  // parsing errors with import number in positions.
  std::vector<diagnostic> errors() const {
    std::vector<diagnostic> out;
    for(auto d: diags) out.push_back(with_source(d));
    return out;
  }
  // --- Phase 1: paired brackets ---

  // Brackets are unpaired - first fixed (close_by_indent), then always paired.
  void match_brackets() {
    std::vector<char> stack;
    for(auto const & t: toks) {
      if(t.kind == tok::open) stack.push_back(t.ch());
      else if(t.kind == tok::close) {
        if(stack.empty() || !pairs(stack.back(), t.ch())) bracket_error = true;
        else stack.pop_back();
      }
    }
    if(!stack.empty()) bracket_error = true;
    if(bracket_error) close_by_indent();
    rematch();
  }
  void rematch() {
    match.assign(toks.size(), ~uint32_t{0});
    std::vector<uint32_t> open;
    for(uint32_t i = 0; i < toks.size(); ++i) {
      if(toks[i].kind == tok::open) open.push_back(i);
      else if(toks[i].kind == tok::close) {
        match[open.back()] = i;
        match[i] = open.back();
        open.pop_back();
      }
    }
  }

  // Repair of unpaired brackets (§4.1):
// ). Unclosed opening closes
  // before the first line with an indentation ≤ indentation of his line (or in front of
  // someone else's closing on the same line, or at the end of the text) - an empty
  // closing with the cause "unclosed bracket" is inserted; parsing makes a hole that it
  // closed. Extra closing - token-hole "unpaired bracket". The rest is as it was. *my*:
  // indentation - only when brackets are unpaired (in pairs, indentation means nothing);
  // indentation - the number of gaps and tabulations at the beginning of the line.
  //
  void close_by_indent() {
    std::vector<token> out;
    std::vector<uint32_t> open; // room
    std::vector<uint32_t> indent; // opening-line
    auto close_top = [&] {
      token const & o = out[open.back()];
      token t;
      t.kind = tok::close;
      t.text = o.ch() == '(' ? ")" : o.ch() == '[' ? "]" : "}";
      uint32_t at = out.back().where.end;
      t.where = {at, at};
      t.glued = true;
      t.err = uint32_t(diags.size());
      diags.push_back(diagnostic("незакрытая скобка", o.where));
      out.push_back(t);
      open.pop_back();
      indent.pop_back();
    };
    for(token t: toks) {
      uint32_t ind = indent_of(t.where.begin);
      bool first = line_begin(t.where.begin) + ind == t.where.begin;
      bool own_close = t.kind == tok::close && !open.empty() && pairs(out[open.back()].ch(), t.ch());
      while(!open.empty() && !own_close && (t.kind == tok::end || (first && ind <= indent.back()))) {
        close_top();
        own_close = t.kind == tok::close && !open.empty() && pairs(out[open.back()].ch(), t.ch());
      }
      if(t.kind == tok::open) {
        open.push_back(uint32_t(out.size()));
        indent.push_back(ind);
      } else if(t.kind == tok::close) {
        auto fits = [&](uint32_t o) { return pairs(out[o].ch(), t.ch()); };
        if(std::ranges::none_of(open, fits)) {
          t.kind = tok::bad;
          t.err = uint32_t(diags.size());
          diags.push_back(diagnostic("непарная скобка", t.where));
        } else {
          while(!fits(open.back())) close_top(); // `{ ( }` - `(` closes before `}`
          open.pop_back();
          indent.pop_back();
        }
      }
      out.push_back(t);
    }
    toks = std::move(out);
  }

  // Retreat of the line of text on which the position at.
  uint32_t indent_of(uint32_t at) const {
    uint32_t b = line_begin(at), ind = 0;
    while(b + ind < src.size() && (src[b + ind] == ' ' || src[b + ind] == '\t')) ++ind;
    return ind;
  }
  uint32_t line_begin(uint32_t at) const {
    while(at > 0 && src[at - 1] != '\n') --at;
    return at;
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
  // not read as `=` `=`, `=>` is not read as `=` `>`). Sluices - from statements
  // of operators in the area (operator (a) ==(b) - prelude) and signs parsing of
  // the core (`...`, `=>`, assignment):
//.
  bool is_op(std::string_view op, uint32_t k = 0) const { return longest_op(k) == op; }

  std::string_view longest_op(uint32_t k) const {
    static constexpr std::array core{"...", "=>", "+=", "-=", "*=", "/=", ".."};
    token const & t0 = ahead(k);
    if(t0.kind != tok::op) return {};
    std::string_view best = t0.text.substr(0, 1);
    auto glued_as = [&](std::string_view o) {
      for(uint32_t i = 0; i < o.size(); ++i) {
        token const & t = ahead(k + i);
        if(t.kind != tok::op || t.ch() != o[i] || (i && !t.glued)) return false;
      }
      return true;
    };
    for(std::string_view o: core)
      if(o.size() > best.size() && glued_as(o)) best = o;
    for(auto const & [o, forms]: rules.operators)
      if(o.size() > best.size() && glued_as(o)) best = o;
    return best;
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
    if(here().err != ~uint32_t{0}) throw diagnostic(diags[here().err].what(), here().where); // Closed by repair - closed will become a hole
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
      node_id r = f();
      // draft: the instruction ended with an empty `;` end of the line - not written, the hole
      if(pos > start_tok + 1 && toks[pos - 1].err != ~uint32_t{0} && toks[pos - 1].kind == tok::op && toks[pos - 1].where.begin == toks[pos - 1].where.end) {
        uint32_t start = toks[start_tok].where.begin, end = toks[pos - 2].where.end;
        return db.add(n_hole{std::string(src.substr(start, end - start)), diags[toks[pos - 1].err].what()}, {start, end});
      }
      return r;
    } catch(diagnostic const & d) {
      uint32_t start = toks[start_tok].where.begin;
      pos = start_tok;
      skip_to_statement_end();
      // extra closing bracket: the pass is on it - the hole takes at least it,
      // otherwise parsing does not move
      if(pos == start_tok && !at_end()) advance();
      uint32_t end = toks[pos > 0 ? pos - 1 : 0].where.end;
      // the reason is a token hole or bracket closed by repair (her error is already
      // in diags); the parsing error before it is its own, it is also in diags
      std::string why = d.what();
      auto cause = std::ranges::find_if(toks.begin() + start_tok, toks.begin() + pos, [&](token const & t) { return t.err != ~uint32_t{0}; });
      if(cause != toks.begin() + pos) why = diags[cause->err].what();
      if(cause == toks.begin() + pos || d.where.begin < cause->where.begin) diags.push_back(d);
      return db.add(n_hole{std::string(src.substr(start, end - start)), why}, {start, end});
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
        if(toks[m].err != ~uint32_t{0}) return; // Closed by repair: next line
        continue;
      }
      // The unclosed line ate the end of the line of text (and its `;`) - the instruction ends on it
      bool semi = is_op(";") || (here().kind == tok::bad && (here().text[0] == '"' || here().text[0] == '\''));
      advance();
      if(semi) return;
    }
  }

  // --- module elements and instructions ---

  node_id item() {
    uint32_t start = here().where.begin;
    node_list attrs = attributes();
    // Attributes enter the node before being added to the database, otherwise journal will not see them.
    if(is_word("operator")) return operator_decl(std::move(attrs));
    if(is_word("syntax") && ahead(1).kind == tok::ident) return syntax_decl(std::move(attrs));
    // T Name { … } - Name is instance T:
// 
// What T is - knows VM (struct).
    if(ahead(0).kind == tok::ident && !is_keyword(ahead(0).text) && ahead(1).kind == tok::ident && (is_open('{', 2) || (is_op(":", 2) && ahead(3).kind == tok::ident && is_open('{', 4))))
      return instance_decl(std::move(attrs));
    if(auto f = try_fn_decl(false, attrs)) return *f;
    if(!attrs.empty()) { // [[state]] x = e is the state of the program (§12.2): the variable of the module
      node_id s = statement();
      bool assign = db.is<n_op>(s) && db.strings.text(db.as<n_op>(s).op) == "=" && db.is<n_ident>(db.as<n_op>(s).args[0]);
      if(!assign && !(db.is<n_var>(s) && !db.as<n_var>(s).ct)) throw diagnostic("атрибуты [[…]] — перед функцией, struct или переменной состояния [[state]] x = …", db[attrs.front()].where);
      n_var v = assign ? n_var{db.as<n_ident>(db.as<n_op>(s).args[0]).name, {}, db.as<n_op>(s).args[1]} : db.as<n_var>(s);
      v.attrs = std::move(attrs);
      for(node_id a: v.attrs)
        if(!db.is<n_ident>(a) || db.strings.text(db.as<n_ident>(a).name) != "state") throw diagnostic("у переменной модуля — только [[state]]", db[a].where);
      return db.add(std::move(v), db[s].where);
    }
    // Name { … } - extension of entity
    if(ahead(0).kind == tok::ident && !is_keyword(ahead(0).text) && is_open('{', 1)) return extension(start);
    if(is_op("&") && ahead(1).kind == tok::ident && is_open('(', 2)) return fn_extension(start);
    if(is_word("use") && ahead(1).kind == tok::ident) { // use geo.vec; / use geo.vec.{norm, dot};
      advance();
      n_use u;
      std::string key(here().text);
      ident();
      while(is_op(".") && ahead(1).kind == tok::ident) {
        advance();
        key += '.';
        key += here().text;
        ident();
      }
      if(is_op(".") && is_open('{', 1)) {
        advance();
        expect_open('{');
        while(!is_close('}')) {
          u.names.push_back(ident());
          if(is_op(",")) eat_op(",");
          else break;
        }
        expect_close('}');
      }
      u.key = db.strings.intern(key);
      end_statement();
      learn_use(u); // module forms - from this place (module - inclusion, §9.1 )
      return db.add(std::move(u), {start, toks[pos - 1].where.end});
    }
    node_id s = statement();
    // There are no global process variables outside the functions - only ct (author's decision 2026-09-30)
    bool var = db.is<n_var>(s) && !db.as<n_var>(s).ct;
    bool assign = db.is<n_op>(s) && db.strings.text(db.as<n_op>(s).op) == "=" && db.is<n_ident>(db.as<n_op>(s).args[0]);
    if(var || assign) {
      symbol name = var ? db.as<n_var>(s).name : db.as<n_ident>(db.as<n_op>(s).args[0]).name;
      throw diagnostic(cat("вне функций — только ct-переменные: ct ", db.strings.text(name), " = …"), db[s].where);
    }
    return s;
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

  // f(auto, point) {f(auto out, point p) { … } } - overload of a specific function: a
  // reference to its node by signature (parameter types, auto - without type); is allowed
  // when the entire module (resolve_refs) is known.
  struct fn_ref {
    node_id ext;
    symbol name;
    std::vector<symbol> sig; // empty
  };
  std::vector<fn_ref> refs;
  node_list outer_items; // Where else do the links of the piece look (an element transferred to another module)

  node_id fn_extension(uint32_t start) {
    eat_op("&");
    n_extend e;
    e.name = ident();
    std::vector<symbol> sig;
    expect_open('(');
    while(!is_close(')') && !at_end()) {
      if(is_word("auto")) {
        advance();
        sig.push_back(symbol{});
      } else sig.push_back(ident());
      if(is_op(",")) eat_op(",");
      else break;
    }
    expect_close(')');
    expect_open('{');
    while(!is_close('}') && !at_end()) {
      node_list attrs = attributes();
      auto f = try_fn_decl(false, attrs);
      if(!f) throw diagnostic("в расширении функции — только её перегрузки", here().where);
      e.items.push_back(*f);
    }
    expect_close('}');
    symbol name = e.name;
    node_id id = db.add(std::move(e), {start, toks[pos - 1].where.end});
    refs.push_back({id, name, std::move(sig)});
    return id;
  }

  // References & f(…) to node module functions (and prelude). Not found or
  // ambiguous - the element becomes a hole with a cause.
  void resolve_refs(node_list const & items) {
    for(auto const & r: std::exchange(refs, {})) {
      std::vector<node_id> found;
      auto fits = [&](node_id f) {
        if(!db.is<n_fn>(f) || db.as<n_fn>(f).name != r.name || db.as<n_fn>(f).params.size() != r.sig.size()) return false;
        for(std::size_t i = 0; i < r.sig.size(); ++i) {
          node_id t = db.as<n_param>(db.as<n_fn>(f).params[i]).type;
          bool untyped = !t;
          if(r.sig[i] ? untyped || !db.is<n_ident>(t) || db.as<n_ident>(t).name != r.sig[i] : !untyped) return false;
        }
        return true;
      };
      for(node_id f: items)
        if(fits(f)) found.push_back(f);
      if(db.prelude)
        for(node_id f: db.as<n_module>(db.prelude).items)
          if(fits(f)) found.push_back(f);
      if(found.size() == 1) {
        n_extend e = db.as<n_extend>(r.ext);
        e.target = found[0].value;
        db.replace(r.ext, std::move(e));
        continue;
      }
      std::string sig;
      for(symbol s: r.sig) sig += cat(sig.empty() ? "" : ", ", s ? db.strings.text(s) : "auto");
      diagnostic d(cat(found.empty() ? "нет функции " : "неоднозначно: несколько функций ", db.strings.text(r.name), "(", sig, ")"), db[r.ext].where);
      diags.push_back(d);
      span w = db[r.ext].where;
      db.replace(r.ext, n_hole{std::string(src.substr(w.begin, w.end - w.begin)), d.what()});
    }
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
        symbol tn = ident(); // name - before span: no order of arguments
        v.type = db.add(n_ident{tn}, {i0, toks[pos - 1].where.end});
        v.name = ident();
        if(is_op("=")) {
          eat_op("=");
          v.init = value_expr(); // foo f = {} - the default value of the field type
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

  // T Name [: base] { [ct] T поле [= значение]; … } [;] - extension Name, instance T
  node_id instance_decl(node_list attrs = {}) {
    uint32_t start = here().where.begin;
    n_extend st;
    st.of = ident();
    st.attrs = std::move(attrs);
    st.name = ident();
    if(is_op(":")) { // struct method error : http { … }
      eat_op(":");
      st.base = ident();
    }
    expect_open('{');
    while(!is_close('}')) {
      uint32_t f0 = here().where.begin;
      n_var f;
      if(is_word("ct") && ahead(1).kind == tok::ident && ahead(2).kind == tok::ident) { // ct field: compile state
        f.ct = true;
        advance();
      }
      symbol tn = ident();
      f.type = db.add(n_ident{tn}, {f0, toks[pos - 1].where.end});
      f.name = ident();
      if(is_op("=")) {
        eat_op("=");
        f.init = value_expr(); // foo f = {} - the default value of the field type
      }
      end_statement();
      st.items.push_back(db.add(std::move(f), {f0, toks[pos - 1].where.end}));
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
    else if(ahead(k).kind == tok::ident && is_open('(', k + 1) && !is_keyword(ahead(k).text)) name_at = k;
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
    if(in_block && name_at == k && !is_op("=>", after)) return std::nullopt; // f(x) { … } in the block - a call with a tail

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
      f.body = expr_body();
      f.expr_body = true;
      end_expr_body();
    } else {
      f.body = block();
    }
    node_id id = db.add(std::move(f), {start, toks[pos - 1].where.end});
    learn(db.as<n_fn>(id), id); // node - for the rules of the body of the structure in its blocks
    return id;
  }

  // Words that are not names: kernel parsing (ads) and words of syntax entries from ads in
  // the area (if, else, loop, return... - prelude):
// 
//.
  bool is_keyword(std::string_view w) const {
    return w == "ct" || w == "const" || w == "outline" || w == "module" || w == "operator" || rules.record_words.contains(std::string(w));
  }
  // w is the first word of the declared syntax record (begins the expression)
  bool heads_syntax(std::string_view w) const {
    auto it = db.strings.index.find(std::string(w));
    return it != db.strings.index.end() && rules.syntaxes.contains(it->second.value);
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
    if(is_word("explicit") && ahead(1).kind == tok::ident && ahead(2).kind == tok::ident) { // explicit T x - strictly
      advance();
      p.strict = true;
    }
    if(is_op("...")) { // expr. hs - pac clauses; ...xs - tail of arguments
      eat_op("...");
      p.pack = true;
    }
    // sample: foo(array{a, b, ...xs}) - parameter without a name, names give unpacking; f({x:
    // a, y: b}) - field by name of any unit (§4.3)
    if(p.mode == param_mode::let && ((here().kind == tok::ident && is_open('{', 1) && ahead(1).glued) || record_ahead())) {
      p.type = pattern();
      return db.add(std::move(p), {start, toks[pos - 1].where.end});
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

  // The name {…} ... = - in front of the sample unpacking (after his `}` stands a single `=`).
  bool pattern_ahead() const {
    if(record_ahead()) { // {x: a, y: b} = v is a sample of fields by name
      uint32_t m = match[pos];
      return m != ~uint32_t{0} && is_op("=", m + 1 - pos);
    }
    if(here().kind != tok::ident || !is_open('{', 1) || !ahead(1).glued) return false;
    uint32_t m = match[pos + 1];
    if(m == ~uint32_t{0}) return false;
    return is_op("=", m + 1 - pos);
  }

  node_id pattern() {
    uint32_t start = here().where.begin;
    n_pat p;
    if(record_ahead()) { // {x: a, y: b} - without type: .x reference to value - subsample
      expect_open('{');
      while(!is_close('}') && !at_end()) {
        uint32_t i0 = here().where.begin;
        symbol name = ident();
        node_id e = db.add(n_subject{}, {i0, i0});
        e = db.add(n_op{db.strings.intern(cat(".", db.strings.text(name))), {e}, true}, {i0, toks[pos - 1].where.end});
        expect_op(":");
        node_id sub = pattern_element();
        p.items.push_back(db.add(n_pat_field{e, sub}, {i0, toks[pos - 1].where.end}));
        if(is_op(",")) eat_op(",");
        else break;
      }
      expect_close('}');
      return db.add(std::move(p), {start, toks[pos - 1].where.end});
    }
    symbol tn = ident();
    p.type = db.add(n_ident{tn}, {start, toks[pos - 1].where.end});
    expect_open('{');
    while(!is_close('}') && !at_end()) {
      uint32_t i0 = here().where.begin;
      if((is_op(".") && !is_op("...")) || is_open('[')) { // .field: subsample - value interface
        node_id e = db.add(n_subject{}, {i0, i0});
        if(is_open('[')) {
          advance();
          node_id k = expression();
          expect_close(']');
          e = db.add(n_op{db.strings.intern("[]"), {e, k}}, {i0, toks[pos - 1].where.end});
        }
        e = postfix(e);
        expect_op(":");
        node_id sub = pattern_element();
        p.items.push_back(db.add(n_pat_field{e, sub}, {i0, toks[pos - 1].where.end}));
        if(is_op(",")) eat_op(",");
        else break;
        continue;
      }
      if(is_op("...")) { // leftover
        eat_op("...");
        n_param r;
        r.pack = true;
        r.name = ident();
        p.items.push_back(db.add(std::move(r), {i0, toks[pos - 1].where.end}));
      } else if(is_open('(')) { // (value) - the equality of the element
        p.items.push_back(expression());
      } else if(here().kind == tok::ident && is_open('{', 1) && ahead(1).glued) {
        p.items.push_back(pattern());
      } else {
        p.items.push_back(pattern_element());
      }
      if(is_op(",")) eat_op(",");
      else break;
    }
    expect_close('}');
    return db.add(std::move(p), {start, toks[pos - 1].where.end});
  }

  // Subsample: name, T name, , (value) or nested sample.
  node_id pattern_element() {
    uint32_t i0 = here().where.begin;
    if(is_open('(')) return expression();
    if((here().kind == tok::ident && is_open('{', 1) && ahead(1).glued) || record_ahead()) return pattern();
    n_param b;
    if(is_word("explicit") && ahead(1).kind == tok::ident && ahead(2).kind == tok::ident) { // explicit T name - strictly
      advance();
      b.strict = true;
    }
    if(ahead(1).kind == tok::ident) b.type = type_expr(); // T-name
    b.name = ident();
    return db.add(std::move(b), {i0, toks[pos - 1].where.end});
  }

  node_id unpack(bool test = false) {
    uint32_t start = here().where.begin;
    node_id p = pattern();
    eat_op("=");
    node_id v = expression();
    return db.add(n_unpack{p, v, test}, {start, toks[pos - 1].where.end});
  }

  // Type is name. auto no (author's decision 2026-09-28):
// Tag with
  // any name is a value (§5.1), so the prohibition is here, one place.
  node_id type_expr() {
    uint32_t start = here().where.begin;
    if(is_word("auto")) throw diagnostic("auto не нужен: без типа имя принимает всё, x = … выводит тип", here().where);
    symbol name = ident(); // to span: the order of calculation of arguments is not specified
    return db.add(n_ident{name}, {start, toks[pos - 1].where.end});
  }

  // Translations of lines between the previous lexeme and the current one (for empty lines)
  // - before the first interval comment (comment lines are not empty; the tail of the line
  // of the previous lexeme is omitted): empty lines after it - in node_note.
  uint32_t newlines_before() const {
    if(pos == 0 || pos >= toks.size()) return 0;
    uint32_t from = toks[pos - 1].where.end, to = toks[pos].where.begin;
    uint32_t i = first_comment(from);
    if(i < comments.size() && comments[i].begin < to && newlines(from, comments[i].begin) == 0) from = comments[i++].end;
    if(i < comments.size() && comments[i].begin < to) to = comments[i].begin;
    return newlines(from, to);
  }
  // empty lines beyond canonical `canon` line translations
  uint32_t blank_lines(uint32_t canon) const {
    uint32_t n = newlines_before();
    return n > canon ? n - canon : 0;
  }
  static void trim_blank(std::vector<uint32_t> & blank) {
    if(std::ranges::all_of(blank, [](uint32_t x) { return x == 0; })) blank.clear();
  }

  node_id block() {
    context_scope scope(*this); // declared in the block - until its end
    uint32_t start = here().where.begin;
    expect_open('{');
    n_block b;
    while(!is_close('}') && !at_end()) {
      b.blank.push_back(blank_lines(1));
      node_id s = recover_statement([&] { return statement(); });
      b.items.push_back(s);
      if(is_close('}') && last_was_value) b.has_value = true;
    }
    b.blank.push_back(blank_lines(1)); // before `}` : n empty lines printed n + 1 translations
    trim_blank(b.blank);
    expect_close('}');
    return db.add(std::move(b), {start, toks[pos - 1].where.end});
  }

  // Instructions. Assigns last_was_value_ , if it is an expression without `;` . return,
  // break, continue, hide, try_compile - syntax prelude (§4.4 ): disassembled as
  // expressions, the record with `;` itself ends the instruction.
  node_id statement() {
    last_was_value = false;

    // operator and syntax in the block - the rules of its block (and the blocks of the structure in the body of which)
    if(is_word("operator")) return operator_decl({});
    if(is_word("syntax") && ahead(1).kind == tok::ident) return syntax_decl({});
    if(is_word("syntax") && is_open('{', 1)) return raw_rule_decl();
    if(auto f = try_fn_decl(true)) return *f;

    if(is_word("using") && is_word("operator", 1)) { // Using Operator - Redefinition in Context (§8.2)
      advance();
      return operator_decl({}, true);
    }
    if(is_open('[') && is_open('[', 1)) { // [[associative]] operator ... in the block is a property of the context operator
      node_list attrs = attributes();
      if(is_word("operator")) return operator_decl(std::move(attrs));
      if(is_word("using") && is_word("operator", 1)) {
        advance();
        return operator_decl(std::move(attrs), true);
      }
      throw diagnostic("атрибуты [[…]] в блоке — перед operator или using operator", attrs.empty() ? here().where : db[attrs.front()].where);
    }
    if(pattern_ahead()) { // sample = value;
      node_id u = unpack();
      end_statement();
      db[u].where.end = toks[pos - 1].where.end;
      return u;
    }
    if(auto v = try_var_decl()) return *v;

    uint32_t head_saved = std::exchange(statement_head, here().where.begin); // Tail instruction - only at the call at the beginning of the instruction
    node_id e = expression();
    statement_head = head_saved;
    if(tail_statement) { // The tail instructions ate their own `;`.
      tail_statement = false;
      return e;
    }
    if(is_op(";")) {
      eat_op(";");
      last_was_value = false; // The block argument inside (f({ 1 }) ;) does not make the instruction value n
    } else if(is_close('}') || (line_mode && at_end())) last_was_value = true;
    else if(!ends_with_block(e)) throw diagnostic("ожидалось ';'", here().where);
    return e;
  }

  bool if_closed(node_id n) const {
    auto const & i = db.as<n_if>(n);
    node_id last = i.else_ ? i.else_ : i.then_;
    if(db.is<n_if>(last)) return if_closed(last);
    return db.is<n_block>(last); // Write branch with `;` (return x;) closes through tail_statement
  }

  bool ends_with_block(node_id e) const {
    if(db.is<n_node>(e)) { // Type of body-ending recording program: until (c) {body}
      auto const & items = db.as<n_node>(e).items;
      return !items.empty() && db.is<n_block>(items.back());
    }
    return db.is<n_if>(e) || db.is<n_loop>(e) || (db.is<n_ct>(e) && db.is<n_block>(db.as<n_ct>(e).body)) || db.is<n_block>(e) || (db.is<n_call>(e) && (db.as<n_call>(e).tail || !db.as<n_call>(e).words.empty()));
  }

  // Body end `=> e`: The tail instruction at the end of the body (g() => if(c) for(x : xs) s;) has already
  // eaten the `;` - it is also the end of the announcement; the flag should not live to see the next
  // instruction (there it broke the call: k(…) - the name k and a separate instruction). The `=> e` body is
  // an instruction: the tail of the structure at its beginning (g() => for(x : xs) s;), as in the if branch
  // without brackets (author's decision 2026-10-11). My body is the beginning of the instruction.
  node_id expr_body() {
    uint32_t head_saved = std::exchange(statement_head, here().where.begin);
    node_id e = expression();
    statement_head = head_saved;
    return e;
  }

  void end_expr_body() {
    if(tail_statement) tail_statement = false;
    else if(is_op(";")) eat_op(";");
  }

  void end_statement() {
    if(is_op(";")) eat_op(";");
    else if(!is_close('}')) throw diagnostic("ожидалось ';'", here().where);
  }

  node_id statement_or_block() {
    if(is_open('{') && !record_ahead()) return block();
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
    bool strict = is_word("explicit", k) && ahead(k + 1).kind == tok::ident && ahead(k + 2).kind == tok::ident; // explicit T x = e - strictly (§5.8 )
    k += strict;
    // T x = e is a statement with a restriction; const x = e is a qualifier (type is
    // derived); &e = v[i] is a reference; x = e without a sign is an announcement if
    // there is no name (interpreter parses):
    //
    bool typed = ahead(k).kind == tok::ident && !is_keyword(ahead(k).text) && ahead(k + 1).kind == tok::ident && is_op("=", k + 2); // return x = e - not an announcement
    bool qualified = is_const && ahead(k).kind == tok::ident && is_op("=", k + 1);
    if(k == 0 && is_word("ct") && ahead(1).kind == tok::ident && is_op("=", 2)) { // ct x = e is the compile variable (§7.3)
      uint32_t start = here().where.begin;
      advance();
      n_var v;
      v.ct = true;
      v.name = ident();
      expect_op("=");
      v.init = expression();
      end_statement();
      return db.add(std::move(v), {start, toks[pos - 1].where.end});
    }
    bool ref = k == 0 && is_op("&") && ahead(1).kind == tok::ident && is_op("=", 2);
    if(!typed && !qualified && !ref) return std::nullopt;
    if(strict && !typed) throw diagnostic("explicit — перед типом: explicit T x = e", here().where);
    uint32_t start = here().where.begin;
    if(is_const) advance();
    if(strict) advance();
    n_var v;
    v.is_const = is_const;
    v.strict = strict;
    v.ref = ref;
    if(ref) eat_op("&");
    if(typed) v.type = type_expr();
    v.name = ident();
    expect_op("=");
    v.init = value_expr(); // point p = {}
    end_statement();
    return db.add(std::move(v), {start, toks[pos - 1].where.end});
  }

  // --- expressions: code operators (§4.4) ---
  //
  // Operators - ads in the area (prelude - first):
// 
// 
//  
// No Priorities: A chain of two or more infixes is a node
  // n_chain, the tree selects the semantics for instantiation - by types and operators
  // of the execution context: [[associative]] is the ad property, not the parsing rule
  //
//).
  // Prefix and postfix operators are part of their operand. Attribution (= += -= *=
  // /=) is the form of the kernel, not the operator.

  static bool op_char(char c) { return std::string_view("+-*/%<>=!&|^~?@$\\").contains(c); }

  void learn_operator(n_fn const & f) {
    auto name = db.strings.text(f.name);
    for(std::string_view role: {"infix", "prefix", "postfix"})
      if(name.size() > role.size() && name.starts_with(role) && (op_char(name[role.size()]) || name.substr(role.size()) == "[]")) {
        if(auto it = rules.operators.find(std::string(name.substr(role.size()))); it != rules.operators.end()) { // It is already there, the context does not change.
          op_forms const & o = it->second;
          if(role == "infix" ? o.infix : role == "prefix" ? o.prefix : o.postfix) continue;
        }
        touch();
        op_forms & o = rules.operators[std::string(name.substr(role.size()))];
        (role == "infix" ? o.infix : role == "prefix" ? o.prefix
                                                      : o.postfix) = true;
      }
  }

  bool is_role(std::string const & sym, int role) const { // 0 prefix, 1 infix, 2 postfix
    auto it = rules.operators.find(sym);
    if(it == rules.operators.end()) return false;
    return role == 0 ? it->second.prefix : role == 1 ? it->second.infix
                                                     : it->second.postfix;
  }

  // operator -(a) ... | operator (a) + (b) ... | operator (a)++ ... - the ad repeats the
  // entry: the parameter is on the side of its branch (
// ). entity is a function of infix+/prefix-/postfix++.
  node_id operator_decl(node_list attrs, bool scoped = false) {
    uint32_t start = here().where.begin;
    advance();
    n_fn f;
    f.attrs = std::move(attrs);
    f.scoped = scoped;
    std::string role, sym;
    if(is_open('(')) {
      f.params = param_list();
      if(is_open('[')) { // (a)[i] The pair operator:
        advance();
        f.params.push_back(param());
        expect_close(']');
        sym = "[]";
        role = "infix";
      } else {
        sym = symbol_run();
        if(is_open('(')) {
          role = "infix";
          for(node_id pn: param_list()) f.params.push_back(pn);
        } else role = "postfix";
      }
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
      f.body = expr_body();
      f.expr_body = true;
      end_expr_body();
    } else {
      f.body = block();
    }
    node_id id = db.add(std::move(f), {start, toks[pos - 1].where.end});
    learn(db.as<n_fn>(id), id);
    return id;
  }

  // syntax word (x) {тело} word ... => expression; is a form of writing declared by the
  // code:
//;
  // 
// An announcement
  // It repeats the recording like an operator. An entity is a function with the name of the first
  // word: (x) is a parameter (mode as a parameter), {x} is a block (expr), {[[value]] x} is a slot
  // value: a block or expression, but `{}` There is an empty letter in it (return {}).
  node_id syntax_decl(node_list attrs) {
    uint32_t start = here().where.begin;
    advance();
    n_fn f;
    f.attrs = std::move(attrs);
    f.name = ident();
    f.form.push_back(f.name);
    while(!is_op("=>") && !at_end()) {
      if(is_open('[') && is_open('[', 1)) { // [[line]] part - layout mark: print only
        advance();
        advance();
        for(;;) {
          if(here().kind != tok::ident || !layout_mark_name(here().text)) throw diagnostic("пометки раскладки записи — line, indent, space, glued", here().where);
          f.form.push_back(db.strings.intern(cat("[", here().text, "]")));
          advance();
          if(!is_op(",")) break;
          eat_op(",");
        }
        expect_close(']');
        expect_close(']');
        if(is_op("=>") || at_end()) throw diagnostic("пометка раскладки — перед частью записи", here().where);
      } else if(is_open('(')) {
        bool glued = here().glued; // return(цель) vs. return (x)
        advance();
        f.params.push_back(param());
        bool list = is_op(",") && is_op("...", 1); // (xs, ...) - part-list
        if(list) {
          eat_op(",");
          eat_op("...");
        } else if(is_op(",")) { // (a, b) - several places in the same brackets: part per place
          f.form.push_back(db.strings.intern(glued ? "()+" : "(+"));
          while(is_op(",")) {
            eat_op(",");
            f.params.push_back(param());
            f.form.push_back(db.strings.intern(is_op(",") ? ",+" : ",)"));
          }
          expect_close(')');
          continue;
        }
        expect_close(')');
        f.form.push_back(db.strings.intern(cat(glued ? "()" : "(", list ? "..." : "")));
      } else if(is_op(";")) { // end of instruction: ; (or before )
        advance();
        f.form.push_back(db.strings.intern(";"));
      } else if(is_open('{')) {
        uint32_t p0 = here().where.begin;
        advance();
        bool value = is_open('[') && is_open('[', 1); // {[[value]] x} is the slot value
        if(value) {
          advance();
          advance();
          if(!is_word("value")) throw diagnostic("в слоте записи — только [[value]]", here().where);
          advance();
          expect_close(']');
          expect_close(']');
        }
        n_param p;
        p.mode = param_mode::expr;
        p.name = ident();
        expect_close('}');
        f.params.push_back(db.add(std::move(p), {p0, toks[pos - 1].where.end}));
        f.form.push_back(db.strings.intern(value ? "{=" : "{"));
      } else if(here().kind == tok::ident) {
        f.form.push_back(db.strings.intern(here().text));
        advance();
      } else if(here().kind == tok::op) {
        f.form.push_back(db.strings.intern(symbol_run()));
      } else throw diagnostic("в записи syntax — слова, символы, ;, (параметр) и {тело}", here().where);
    }
    expect_op("=>");
    f.body = is_open('{') && !record_ahead() ? block() : expr_body();
    f.expr_body = true;
    end_expr_body();
    node_id id = db.add(std::move(f), {start, toks[pos - 1].where.end});
    learn(db.as<n_fn>(id), id);
    return id;
  }

  // Record syntax ads with this first word. Forms with a common beginning are parsed
  // together once; on branch, the text in front decides: matching words and symbols
  // (longer row - first), then (parameter) in brackets, then {тело}, then the end of the
  // form. Of equals, the last announcement. Write node kernel (if, loop, return... -
  // prelude) after the start of parsing of its parts does not roll back: the error inside
  // is a hole, like any instruction. Otherwise (form-challenge) did not fit any -
  // nullopt, the position is not shifted.
  std::optional<node_id> syntax_use(uint32_t start) {
    auto it = rules.syntaxes.find(db.strings.intern(here().text).value);
    if(it == rules.syntaxes.end()) return std::nullopt;
    std::vector<node_id> live(it->second.rbegin(), it->second.rend());
    auto piece = [&](node_id d, std::size_t k) { return form_piece(db, db.as<n_fn>(d), k); }; // The parsing layout is not visible.
    auto is_param = [](std::string_view p) { return form_place(p); };
    bool records = std::ranges::all_of(live, [&](node_id d) { auto r = record_of(db, d); return r && r->kind >= 0; });
    uint32_t saved = pos;
    bool committed = false;
    try {
      uint32_t h0 = here().where.begin, h1 = here().where.end;
      advance();
      node_id callee;
      if(std::ranges::any_of(live, [&](node_id d) { return !record_of(db, d); })) callee = db.add(n_ident{db.strings.intern(toks[pos - 1].text)}, {h0, h1});
      node_list args;
      std::size_t k = 1;
      for(;;) {
        // words and symbols ahead: the longest matching row
        uint32_t best_len = 0, best_toks = 0;
        node_id best;
        for(node_id d: live) {
          uint32_t len = 0, off = 0;
          for(std::string_view p = piece(d, k); !p.empty() && !is_param(p); p = piece(d, k + ++len)) {
            uint32_t n = literal_at(p, off);
            if(n == ~uint32_t{0}) {
              len = 0;
              break;
            }
            off += n;
          }
          if(len > best_len) best_len = len, best_toks = off, best = d;
        }
        if(best_len) {
          std::erase_if(live, [&](node_id d) {
            for(uint32_t i = 0; i < best_len; ++i)
              if(piece(d, k + i) != piece(best, k + i)) return true;
            return false;
          });
          for(uint32_t i = 0; i < best_toks; ++i) advance();
          tail_statement = piece(best, k + best_len - 1) == ";"; // ; completed the instruction
          k += best_len;
          continue;
        }
        auto with = [&](auto && fits) {
          std::vector<node_id> v;
          for(node_id d: live)
            if(fits(piece(d, k))) v.push_back(d);
          return v;
        };
        if(auto v = with([&](std::string_view p) { return form_paren(p) && is_open('(') && (!p.starts_with("()") || here().glued); }); !v.empty()) {
          live = std::move(v);
          committed = records;
          tail_statement = false;
          uint32_t p0 = here().where.begin;
          advance();
          // (x) is one expression; (xs, ...) is a comma list, place is array {…}
          auto lists = [&](bool list) { std::erase_if(live, [&](node_id d) { return form_list(piece(d, k)) != list; }); };
          node_list items;
          if(!is_close(')')) items.push_back(expression());
          while(is_op(",")) {
            eat_op(",");
            items.push_back(expression());
          }
          // (a, b): as many places as expressions are parts (+,+...)
          auto multi = [&](node_id d) {
            if(items.size() < 2 || !form_open(piece(d, k))) return false;
            for(std::size_t i = 1; i < items.size(); ++i)
              if(piece(d, k + i) != (i + 1 == items.size() ? ",)" : ",+")) return false;
            return true;
          };
          if(std::ranges::any_of(live, multi)) {
            std::erase_if(live, [&](node_id d) { return !multi(d); });
            expect_close(')');
            for(node_id x: items) args.push_back(x);
            k += items.size();
            continue;
          }
          std::erase_if(live, [&](node_id d) { return form_open(piece(d, k)); });
          bool one = items.size() == 1 && std::ranges::any_of(live, [&](node_id d) { return !form_list(piece(d, k)); });
          lists(!one);
          if(live.empty()) throw diagnostic(items.empty() ? "ожидалось выражение" : "ожидалось одно выражение в скобках", here().where);
          expect_close(')');
          if(one) args.push_back(items[0]);
          else args.push_back(db.add(n_construct{db.add(n_ident{db.strings.intern("array")}, {p0, p0 + 1}), std::move(items), {}}, {p0, toks[pos - 1].where.end}));
          ++k;
          continue;
        }
        if(auto v = with([](std::string_view p) { return p == "{" || p == "{="; }); !v.empty()) {
          // {} in the slot value is an empty literal, the type is from the place:
//  
          bool literal = empty_value_ahead() && std::ranges::any_of(v, [&](node_id d) { return piece(d, k) == "{="; });
          if(literal) std::erase_if(v, [&](node_id d) { return piece(d, k) != "{="; });
          live = std::move(v);
          committed = records;
          tail_statement = false;
          // body: block (with the rules of body forms - as in the structure; raw - if the form
          // disassembles it with the code) or expression (branch)
          if(literal) args.push_back(record());
          else if(is_open('{') && !record_ahead()) args.push_back(ruled_block(live));
          else { // Branch without brackets - instruction: the tail of the structure at its beginning (if(c) for(x : xs) s);
            uint32_t head_saved = std::exchange(statement_head, here().where.begin);
            args.push_back(expression());
            statement_head = head_saved;
          }
          ++k;
          continue;
        }
        auto v = with([](std::string_view p) { return p.empty(); });
        if(v.empty()) throw diagnostic(cat("ожидалось ", piece(live.front(), k)), here().where);
        node_id d = v.front();
        auto const & form = db.as<n_fn>(d).form;
        bool tail = db.strings.text(form.back()).starts_with('{') && !args.empty() && db.is<n_block>(args.back());
        span w{start, toks[pos - 1].where.end};
        if(auto r = record_of(db, d)) {
          std::vector<node_id> places(r->places);
          for(std::size_t i = 0; i < args.size(); ++i) places[r->slot[i]] = args[i];
          if(r->kind >= 0) return db.add(make_node(r->kind, places, r->program), w);
          n_call c; // kernel call recording: try_compile { … }, hide x
          c.args = std::move(places);
          c.callee = db.add(n_ident{form[0]}, {h0, h1});
          c.tail = tail;
          return db.add(std::move(c), w);
        }
        n_call c;
        c.callee = callee;
        c.form = d.value + 1;
        c.args = std::move(args);
        c.tail = tail;
        return db.add(std::move(c), w);
      }
    } catch(diagnostic const &) {
      if(committed) throw;
    }
    pos = saved;
    tail_statement = false;
    return std::nullopt;
  }

  // Word or glued symbols p at a distance off in front: how many lexemes it occupies
  // (~0 is not the same). `;` is the end of the instruction: `;`, and before `}` and
  // at the end of the line of the set - without the token.
  uint32_t literal_at(std::string_view p, uint32_t off) const {
    if(p == ";") {
      if(is_op(";", off)) return 1;
      bool closes = ahead(off).kind == tok::close && ahead(off).ch() == '}';
      return closes || (line_mode && ahead(off).kind == tok::end) ? 0 : ~uint32_t{0};
    }
    if(lexer::is_ident_start(p[0])) return is_word(p, off) ? 1 : ~uint32_t{0};
    uint32_t n = run_length(off);
    if(n < p.size()) return ~uint32_t{0};
    for(uint32_t i = 0; i < p.size(); ++i)
      if(ahead(off + i).ch() != p[i] || (i && !ahead(off + i).glued)) return ~uint32_t{0};
    if(n > p.size() && ahead(off + uint32_t(p.size())).glued) return ~uint32_t{0}; // glued on - another row
    return uint32_t(p.size());
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
    if(t.kind == tok::ident) return !is_keyword(t.text) || heads_syntax(t.text) || t.text == "ct"; // (if, throw, return...) - operand
    if(t.kind == tok::int_lit || t.kind == tok::float_lit || t.kind == tok::str_lit || t.kind == tok::char_lit) return true;
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
        node_id rhs = a == "=" ? value_expr() : expression(); // p = {} - value of place type
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
      // The instruction ending in a block (for(…) { … }, loop { … }, until (c) { … }) ends in `}`: the next
      // line of `-1` is a new instruction, not a minus.
// *Mine: * - Just at the beginning of the instruction
      bool closed = tail_statement || (db.is<n_if>(a) && if_closed(a)) ||
          (!db.is<n_if>(a) && db[a].where.begin == statement_head && ends_with_block(a));
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
    if(infix.size() == 1) return db.add(n_op{db.strings.intern(infix[0]), {atoms[0], atoms[1]}}, {start, db[atoms[1]].where.end}); // tree
    n_chain c; // a + b + c is also a chain: associativity - in the context of execution
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
    return postfix(primary());
  }

  node_id postfix(node_id e) {
    for(;;) {
      uint32_t start = db[e].where.begin;
      if(tail_statement) return e; // recording with `;` (return x;) finished the instruction
      if(is_open('(') && here().glued) {
        e = call(e, start);
        if(tail_statement) return e; // Instruction-tail completed the instruction
      } else if(is_open('{') && !here().glued && construct_of(e) && construct_of(e)->bare) { // try { … }
        e = call(e, start);
      } else if(is_open('[') && here().glued && is_role("[]", 1)) { // Pair operator - by declaration of operator (a)[i]
        advance();
        node_id i = expression();
        expect_close(']');
        e = db.add(n_op{db.strings.intern("[]"), {e, i}}, {start, toks[pos - 1].where.end});
      } else if(is_open('{') && here().glued && db.is<n_ident>(e)) {
        node_list items = brace_items();
        e = db.add(n_construct{e, std::move(items)}, {start, toks[pos - 1].where.end});
      } else if(is_op(".") && is_open('[', 1)) { // v.[f] - descriptor field
        advance();
        advance();
        node_id key = expression();
        expect_close(']');
        e = db.add(n_op{db.strings.intern(".[]"), {e, key}}, {start, toks[pos - 1].where.end});
      } else if(is_op(".") && !is_op("..")) {
        advance();
        symbol name = ident();
        e = db.add(n_op{db.strings.intern(cat(".", db.strings.text(name))), {e}, true}, {start, toks[pos - 1].where.end});
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
    // copy: block rules (context) change and restore the table of designs
    construct own;
    construct const * con = construct_of(callee);
    if(con) con = &(own = *con);
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
        n_construct pk; // pack
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
        pk.type = db.add(n_ident{db.strings.intern("array")}, {w.begin, w.begin});
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
    bool packed = word && con && std::ranges::any_of(con->clauses, [&](auto const & w) { return w.first == word && w.second; });
    if(is_open('(')) {
      advance();
      while(!is_close(')')) {
        if(auto b = binder(packed)) {
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
        } else if(!packed && is_op("...")) { // ...t - disclosure in arguments
          uint32_t s0 = here().where.begin;
          eat_op("...");
          node_id v = expression();
          c.args.push_back(db.add(n_spread{v}, {s0, toks[pos - 1].where.end}));
          c.words.push_back(word);
        } else {
          c.args.push_back(value_expr()); // m({}) is the value of the parameter type
          c.words.push_back(word);
        }
        if(is_op(",")) eat_op(",");
        else break;
      }
      expect_close(')');
    }
    if(packed && c.args.size() > first) // Repetitive Clause - a block with parameters: catch(E e), catch(auto e)
      throw diagnostic(cat("в скобках клаузы ", db.strings.text(word), " — только имена: ", db.strings.text(word), "(e)"), db[c.args[first]].where);
    node_id body;
    std::vector<node_id> decls = con ? con->decls : std::vector<node_id>{};
    if(is_open('{')) body = ruled_block(decls);
    else if(word == symbol{} && con && con->tail && head_of_statement && here().kind == tok::ident && (!rules.record_words.contains(std::string(here().text)) || heads_syntax(here().text))) {
      body = ruled(decls, [&] { return statement(); });
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

  // Design call block decls: raw if the body of the structure declares the rule of its parsing
  // (syntax {r} =>...), otherwise - the standard parsing with the rules of the bodies (§4.4).
  node_id ruled_block(std::vector<node_id> const & decls) {
    for(node_id d: decls)
      if(raw_rule_of(db, d)) return raw_tree(d.value + 1);
    return ruled(decls, [&] { return block(); });
  }

  // Raw block (§4.4, "parsing code"): the result of phase 1 is brackets, atoms and sub-blocks
  // with positions. ;
  // 
  // . node from it makes the rule of construction on the language - in analysis.
  node_id raw_tree(uint32_t rule = 0) {
    span o = here().where;
    bool glued = here().glued;
    char open = here().ch();
    uint32_t close = match[pos];
    if(close == ~uint32_t{0}) throw diagnostic("незакрытая скобка", o);
    if(toks[close].err != ~uint32_t{0}) throw diagnostic(diags[toks[close].err].what(), toks[close].where); // closed by repair - hole
    n_raw x;
    x.open = std::string(1, open);
    x.glued = glued;
    x.rule = rule;
    advance();
    while(pos < close) {
      token const & t = here();
      if(t.kind == tok::open) {
        x.items.push_back(raw_tree());
        continue;
      }
      switch(t.kind) {
        case tok::ident: x.items.push_back(db.add(n_ident{db.strings.intern(t.text)}, t.where)); break;
        case tok::int_lit: x.items.push_back(db.add(n_int{parse_int(t.text), false, t.text.starts_with("0x") || t.text.starts_with("0X")}, t.where)); break;
        case tok::float_lit: x.items.push_back(db.add(n_float{parse_float(t.text)}, t.where)); break;
        case tok::str_lit: x.items.push_back(db.add(n_str{unescape(t.text)}, t.where)); break;
        case tok::char_lit: {
          std::string b = unescape(t.text);
          if(b.size() != 1) x.items.push_back(db.add(n_hole{std::string(t.text), "символ '…' — один байт"}, t.where));
          else x.items.push_back(db.add(n_int{int64_t(uint8_t(b[0])), true}, t.where));
          break;
        }
        case tok::op: x.items.push_back(db.add(n_sym{std::string(t.text), t.glued}, t.where)); break;
        default: x.items.push_back(db.add(n_hole{std::string(t.text), t.err != ~uint32_t{0} ? diags[t.err].what() : "не лексема"}, t.where)); break;
      }
      advance();
    }
    span w{o.begin, toks[close].where.end};
    advance();
    x.text = std::string(src.substr(w.begin, w.end - w.begin));
    return db.add(std::move(x), w);
  }

  // Syntax {r} => f(r) is a rule of parsing blocks of construction declared in her body: the
  // call block is stored raw (n_raw), node from it makes f - a function on the language.
  //  
// 
// *My:* recording is like syntax
  // ("announcement repeats record": {r} is the entire block), place is the body of the structure (as
  // the rules of its blocks); entity is a function "{}" with the parameter r, not called by name.
  node_id raw_rule_decl() {
    uint32_t start = here().where.begin;
    advance();
    uint32_t p0 = here().where.begin;
    expect_open('{');
    n_param p;
    p.name = ident();
    expect_close('}');
    n_fn f;
    f.name = db.strings.intern("{}");
    f.form.push_back(db.strings.intern("{"));
    f.params.push_back(db.add(std::move(p), {p0, toks[pos - 1].where.end}));
    expect_op("=>");
    f.body = is_open('{') && !record_ahead() ? block() : expr_body();
    f.expr_body = true;
    end_expr_body();
    return db.add(std::move(f), {start, toks[pos - 1].where.end});
  }

  // auto x | Type x before `:`, `,` or `)` is a block parameter (not added to the base).
  // The block parameter in the call brackets: `x : xs`, `T x : xs`; in the block-close
  // (catch) - and `e`, `T e` (type is not written - it is output:).
  //
  std::optional<std::pair<n_param, uint32_t>> binder(bool packed) {
    if(ahead(0).kind != tok::ident) return std::nullopt;
    auto closes = [&](uint32_t k) { return is_op(",", k) || (ahead(k).kind == tok::close && ahead(k).ch() == ')'); };
    bool typed = ahead(1).kind == tok::ident && (is_op(":", 2) || closes(2));
    bool bare = is_op(":", 1) || (packed && closes(1));
    if(!typed && !bare) return std::nullopt;
    uint32_t start = here().where.begin;
    n_param p;
    if(typed) p.type = type_expr();
    p.name = ident();
    return std::pair{std::move(p), start};
  }

  // {имя: e, …} is an anonymous structural literal, not a block: a block does not begin with `имя :`.
  // 
//;.
  bool record_ahead(uint32_t k = 0) const { return is_open('{', k) && ahead(k + 1).kind == tok::ident && is_op(":", k + 2); }

  // {} is the entire expression of meaning (argument, literal field or name {…}, right side `=`,
  // default field value): literal without fields, value of place type.
// 
// Instruction and body `{}` is a block.
  bool empty_value_ahead() const {
    if(!is_open('{') || ahead(1).kind != tok::close || ahead(1).ch() != '}') return false;
    token const & t = ahead(2);
    return (t.kind == tok::close && (t.ch() == ')' || t.ch() == '}')) || is_op(",", 2) || is_op(";", 2) || (line_mode && t.kind == tok::end);
  }
  node_id value_expr() { return empty_value_ahead() ? record() : expression(); }

  node_id record() {
    uint32_t start = here().where.begin;
    expect_open('{');
    n_construct c;
    while(!is_close('}')) {
      c.names.push_back(ident());
      expect_op(":");
      c.items.push_back(value_expr()); // {f: {}} is the value of the field type
      if(is_op(",")) eat_op(",");
      else break;
    }
    expect_close('}');
    return db.add(std::move(c), {start, toks[pos - 1].where.end});
  }

  node_list brace_items() {
    expect_open('{');
    node_list items;
    while(!is_close('}')) {
      if(is_op("...")) { // array {...a, x} - Disclosure of the array into elements
        uint32_t s0 = here().where.begin;
        eat_op("...");
        node_id v = expression();
        items.push_back(db.add(n_spread{v}, {s0, toks[pos - 1].where.end}));
      } else items.push_back(value_expr()); // Name {{}, ...} is the value of the field type
      if(is_op(",")) eat_op(",");
      else break;
    }
    expect_close('}');
    return items;
  }

  node_id primary() {
    if(pattern_ahead()) return unpack(true); // sample = value of the expression - predicate
    token const & t = here();
    uint32_t start = t.where.begin;
    switch(t.kind) {
      case tok::int_lit: {
        advance();
        return db.add(n_int{parse_int(t.text), false, t.text.starts_with("0x") || t.text.starts_with("0X")}, t.where);
      }
      case tok::float_lit: {
        advance();
        return db.add(n_float{parse_float(t.text)}, t.where);
      }
      case tok::char_lit: { // 'c' - byte: strings - bytes (§4.0)
        advance();
        std::string b = unescape(t.text);
        if(b.size() != 1) throw diagnostic("символ '…' — один байт", t.where);
        return db.add(n_int{int64_t(uint8_t(b[0])), true}, t.where);
      }
      case tok::str_lit: {
        advance();
        return db.add(n_str{unescape(t.text)}, t.where);
      }
      case tok::ident: {
        if(ahead(1).kind == tok::str_lit && ahead(1).glued) return template_string();
        if(auto u = syntax_use(start)) return *u; // If, loop, return, throw... - prelude
        if(t.text == "native" && ahead(1).kind == tok::str_lit && is_open('{', 2)) { // native "cpp" {code as is }
          advance();
          symbol lang = db.strings.intern(unescape(here().text));
          advance();
          uint32_t close = match[pos];
          if(close == ~uint32_t{0} || toks[close].err != ~uint32_t{0}) throw diagnostic("native: незакрытый блок", here().where);
          uint32_t b = here().where.end, e = toks[close].where.begin;
          pos = close;
          advance();
          return db.add(n_native{lang, std::string(src.substr(b, e - b))}, {start, toks[pos - 1].where.end});
        }
        if(t.text == "quote" && is_open('{', 1)) { // quote { announcements }
          advance();
          expect_open('{');
          n_quote q;
          context_scope scope(*this); // workpiece - its context
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
        if(record_ahead()) return record();
        if(t.ch() == '{') return block();
        if(t.ch() == '(') {
          // lambda: (parameters) => body
          uint32_t close = match[pos];
          if(close != ~uint32_t{0} && is_op("=>", close + 1 - pos)) {
            n_lambda l;
            l.params = param_list();
            eat_op("=>");
            l.body = is_open('{') && !record_ahead() ? block() : expression();
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

  // --- literary ---

  static int64_t parse_int(std::string_view s) {
    int64_t v = 0;
    if(s.starts_with("0x") || s.starts_with("0X")) {
      uint64_t u = 0; // 0x... - bits i64 (0x8000000000000000000 - iconic bit), without UB overflow
      for(char c: s.substr(2))
        if(c != '_') u = u * 16 + uint64_t(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
      return int64_t(u);
    }
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

  // Tag "text {выражение} text": expressions are common expressions of the language, their lexemes
  // are from the same source (places node indicate in the text). {{ и } } are literal brackets.
  node_id template_string() {
    uint32_t start = here().where.begin;
    symbol tag = ident();
    n_call tp;
    tp.callee = db.add(n_ident{tag}, {start, toks[pos - 1].where.end});
    tp.tagged = true;
    token s = here();
    advance();
    std::string_view raw = s.text.substr(1, s.text.size() - 2);
    uint32_t base = s.where.begin + 1;
    std::string chunk; // raw text before expression (with screening)
    auto text_part = [&] {
      std::string lit;
      for(std::size_t i = 0; i < chunk.size(); ++i) {
        if((chunk[i] == '{' || chunk[i] == '}') && i + 1 < chunk.size() && chunk[i + 1] == chunk[i]) ++i;
        lit += chunk[i];
      }
      tp.args.push_back(db.add(n_str{unescape(cat("\"", lit, "\""))}, {base, base}));
      chunk.clear();
    };
    for(std::size_t i = 0; i < raw.size(); ++i) {
      char c = raw[i];
      if(c == '\\' && i + 1 < raw.size()) {
        chunk += c;
        chunk += raw[++i];
        continue;
      }
      if((c == '{' || c == '}') && i + 1 < raw.size() && raw[i + 1] == c) {
        chunk += c;
        chunk += c;
        ++i;
        continue;
      }
      if(c != '{') {
        chunk += c;
        continue;
      }
      std::size_t j = i + 1;
      for(int depth = 1; j < raw.size(); ++j) {
        if(raw[j] == '{') ++depth;
        else if(raw[j] == '}' && --depth == 0) break;
      }
      if(j >= raw.size()) throw diagnostic("шаблон: незакрытая {", {uint32_t(base + i), uint32_t(base + i + 1), source});
      text_part();
      tp.args.push_back(embedded(uint32_t(base + i + 1), uint32_t(base + j)));
      i = j;
    }
    text_part();
    return db.add(std::move(tp), {start, toks[pos - 1].where.end});
  }

  // And then there are the verses [b, e], and then there are the verses.
  node_id embedded(uint32_t b, uint32_t e) {
    auto saved_toks = std::move(toks);
    auto saved_match = std::move(match);
    uint32_t saved_pos = pos;
    toks = lexer(src.substr(b, e - b)).run();
    for(auto & t: toks) {
      t.where.begin += b;
      t.where.end += b;
    }
    match_brackets();
    pos = 0;
    node_id x;
    try {
      x = expression();
      if(!at_end()) throw diagnostic("шаблон: в {…} — одно выражение", here().where);
    } catch(...) {
      toks = std::move(saved_toks);
      match = std::move(saved_match);
      pos = saved_pos;
      throw;
    }
    toks = std::move(saved_toks);
    match = std::move(saved_match);
    pos = saved_pos;
    return x;
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
  std::vector<span> comments; // text comments in order (without marking holes)
  std::vector<uint8_t> claimed; // Comment already given to node
  std::vector<token> toks;
  std::string lex_error; // The first error of the lexer: the line of the editor's set - the whole hole
  bool bracket_error = false; // unpaired brackets (editor's unit - to outer brackets)
  bool line_mode = false; // typing line: end of text - as `}` for value
  std::vector<uint32_t> match;
  uint32_t pos = 0;
  bool last_was_value = false;
  uint32_t statement_head = ~uint32_t{0}; // starting-point
  bool tail_statement = false; // The tail of the design - the instruction, she finished the instruction
  struct op_forms {
    bool prefix = false, infix = false, postfix = false;
  };
  // parsing place rules (§4.4) - from ads in context and external contexts
  struct rule_set {
    std::unordered_map<uint32_t, construct> constructs; // head-name
    std::unordered_map<uint32_t, std::vector<node_id>> syntaxes; // Syntax announcements by first word
    std::set<std::string> record_words; // words of records (heads and words of forms)
    std::map<std::string, op_forms> operators; // symbolically
  };
  rule_set rules;
  uint32_t depth = 0; // nesting of context-blocks (module - 0)
  std::vector<std::pair<uint32_t, rule_set>> saved; // rules of embedded context before the first announcement in it
  bool placed = false; // Place rules already taken (place)
  std::vector<diagnostic> diags;
};

} // namespace tz
