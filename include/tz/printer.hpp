// Text projection: AST base → canonical text (§4 sequel, §9.4).
//
// projection is deterministic: the same AST is always printed the same way. It is
// also used by slice and residual program (§13.2) - they just need to pass the node
// filter.
#pragma once

#include "ast.hpp"

#include <functional>
#include <optional>

namespace tz {

// projection node: Your text or child's place.
using piece = std::variant<std::string, node_id>;
struct node_layout {
  node_id node;
  std::vector<piece> pieces;
};

struct printer {
  // keep(n) == False - node drops out of projection (slice).
  using filter = std::function<bool(node_id)>;

  // Substitutions of residual program: instead of node, another node (folded branch)
  // or finished text (known value).
  struct substitution {
    std::function<node_id(node_id)> node;
    std::function<std::optional<std::string>(node_id)> text;
  };

  explicit printer(ast_db const & db, filter keep = {}, substitution subst = {})
      : db(db), keep(std::move(keep)), subst(std::move(subst)) {}

  std::string print(node_id n) {
    out.clear();
    item(n);
    return out;
  }
  bool kept(node_id n) const { return !keep || keep(n); }

  std::string_view str(symbol s) const { return db.strings.text(s); }

  void line() {
    out += '\n';
    out.append(indent * 2, ' ');
  }

  void item(node_id n) {
    if(!n || !kept(n)) return;
    if(!layouts) return item_body(n);
    enter(n);
    item_body(n);
    leave();
  }

  // projection layout by node (editor, M5): Each node has chunks of its own text and
  // children's place. The syntax is described once - here in projection; the editor
  // gets the finished chunks, not his copy of the grammar.
  void enter(node_id n) {
    if(!open.empty()) {
      flush();
      (*layouts)[open.back()].pieces.push_back(n);
    }
    open.push_back(uint32_t(layouts->size()));
    layouts->push_back({n, {}});
    mark = out.size();
  }
  void leave() {
    flush();
    open.pop_back();
  }
  void flush() {
    if(out.size() > mark) (*layouts)[open.back()].pieces.push_back(out.substr(mark));
    mark = out.size();
  }

  void item_body(node_id n) {
    if(subst.node) {
      if(node_id other = subst.node(n); other && other != n) {
        item(other);
        return;
      }
    }
    if(subst.text) {
      if(auto t = subst.text(n)) {
        out += *t;
        return;
      }
    }
    std::visit([&](auto const & k) { print_node(n, k); }, db[n].kind);
  }

  // --- instruction ---

  void print_node(node_id, n_module const & m) {
    if(m.name) out += cat("module ", str(m.name), ";\n");
    bool first = !m.name;
    for(uint32_t k = 0; k < m.items.size(); ++k) {
      node_id i = m.items[k];
      if(!kept(i)) continue;
      if(!first) out += '\n';
      if(!first) blank_lines(m.blank, k);
      first = false;
      item(i);
      if(!db.is<n_fn>(i)) terminate(i);
      out += '\n';
    }
  }

  void attrs(node_list const & as) {
    if(as.empty()) return;
    out += "[[";
    for(uint32_t i = 0; i < as.size(); ++i) {
      if(i) out += ", ";
      item(as[i]);
    }
    out += "]] ";
  }

  void print_node(node_id, n_fn const & f) {
    attrs(f.attrs);
    if(f.outline) out += "outline ";
    if(f.result_type) {
      item(f.result_type);
      out += ' ';
    }
    auto name = str(f.name);
    for(std::string_view role: {"infix", "prefix", "postfix"})
      if(name.size() > role.size() && name.starts_with(role) && !std::isalnum(static_cast<unsigned char>(name[role.size()])) && name[role.size()] != '_') {
        auto sym = name.substr(role.size());
        out += f.scoped ? "using operator " : "operator ";
        if(role == "prefix") out += sym;
        params({f.params[0]});
        if(role == "postfix") out += sym;
        if(role == "infix") {
          out += cat(" ", sym, " ");
          params({f.params[1]});
        }
        if(f.expr_body) {
          out += " => ";
          item(f.body);
          out += ';';
        } else {
          out += ' ';
          item(f.body);
        }
        return;
      }
    out += str(fn_name ? fn_name : f.name); // overload from & f(…) { … } under the target name
    fn_name = symbol{};
    // head and clauses: try(expr body) catch(expr... handlers)
    for(uint32_t i = 0; i < f.params.size() || i == 0;) {
      symbol word = f.params.empty() ? symbol{} : db.as<n_param>(f.params[i]).clause;
      if(word) out += cat(" ", str(word));
      node_list ps;
      while(i < f.params.size() && db.as<n_param>(f.params[i]).clause == word) ps.push_back(f.params[i++]);
      params(ps);
      if(f.params.empty()) break;
    }
    if(f.expr_body) {
      out += " => ";
      item(f.body);
      out += ';';
    } else {
      out += ' ';
      item(f.body);
    }
  }

  void params(node_list const & ps) {
    out += '(';
    for(uint32_t i = 0; i < ps.size(); ++i) {
      if(i) out += ", ";
      item(ps[i]);
    }
    out += ')';
  }

  void print_node(node_id, n_pat const & p) {
    item(p.type);
    out += '{';
    for(std::size_t i = 0; i < p.items.size(); ++i) {
      node_id x = p.items[i];
      out += i ? ", " : "";
      if(db.is<n_param>(x) && db.as<n_param>(x).pack) out += cat("...", str(db.as<n_param>(x).name));
      else if(db.is<n_param>(x) || db.is<n_pat>(x) || db.is<n_pat_field>(x)) item(x);
      else { // meaning in brackets (otherwise it is a name)
        out += '(';
        item(x);
        out += ')';
      }
    }
    out += '}';
  }

  void print_node(node_id, n_pat_field const & f) {
    item(f.access);
    out += ": ";
    if(db.is<n_param>(f.sub) || db.is<n_pat>(f.sub)) item(f.sub);
    else {
      out += '(';
      item(f.sub);
      out += ')';
    }
  }

  void print_node(node_id, n_subject const &) {} // implicit root: .size, [1].flags

  void print_node(node_id, n_spread const & s) {
    out += "...";
    item(s.value);
  }

  void print_node(node_id n, n_unpack const & u) {
    bool paren = db.is<n_op>(db[n].parent) || db.is<n_chain>(db[n].parent); // operand: (p = e) &&...
    if(paren) out += '(';
    item(u.pattern);
    out += " = ";
    item(u.value);
    if(paren) out += ')';
  }

  void print_node(node_id, n_param const & p) {
    static constexpr std::array modes{"", "mut ", "own ", "out ", "expr ", "ct "};
    if(p.type && db.is<n_pat>(p.type)) return item(p.type); // parameter-sample: names give unpacking
    out += modes[uint32_t(p.mode)];
    if(p.strict) out += "explicit ";
    if(p.type && !db.is<n_auto>(p.type)) {
      item(p.type);
      out += p.pack ? "... " : " ";
    } else if(p.pack && p.mode == param_mode::let) { // xs - the tail of arguments
      out += "...";
    } else if(p.pack) {
      out.pop_back();
      out += "... ";
    }
    out += str(p.name);
  }

  void print_node(node_id, n_var const & v) {
    if(v.is_const) out += "const ";
    if(v.ct) out += "ct ";
    if(v.ref) out += "&";
    if(v.type && !db.is<n_auto>(v.type)) {
      item(v.type);
      out += ' ';
    }
    out += cat(str(v.name), " = ");
    item(v.init);
    out += ';';
  }

  // Empty lines set by a person; in the editor - with indentation (the place of the cursor
  // on an empty line - in the code column), in the text - without tail spaces.
  void blank_lines(std::vector<uint32_t> const & blank, std::size_t k) {
    for(uint32_t n = k < blank.size() ? blank[k] : 0; n > 0; --n) {
      out += '\n';
      if(layouts) out.append(indent * 2, ' ');
    }
  }

  void print_node(node_id, n_block const & b) {
    std::vector<uint32_t> at; // Numbers of output elements in b.items
    for(uint32_t k = 0; k < b.items.size(); ++k)
      if(kept(b.items[k])) at.push_back(k);
    uint32_t tail = uint32_t(b.items.size());
    if(at.empty() && (tail >= b.blank.size() || b.blank[tail] == 0)) {
      out += "{}";
      return;
    }
    out += '{';
    ++indent;
    for(uint32_t k: at) {
      blank_lines(b.blank, k);
      line();
      item(b.items[k]);
      bool value = b.has_value && k + 1 == b.items.size();
      if(!value) terminate(b.items[k]);
    }
    blank_lines(b.blank, tail);
    --indent;
    line();
    out += '}';
  }

  // `;` after the instruction, unless it ends in a block and puts it itself.
  void terminate(node_id s) {
    if(db.is<n_var>(s) || db.is<n_fn>(s) || db.is<n_require>(s) || db.is<n_return>(s)) return;
    if(db.is<n_hole>(s)) { // the hole prints the source code as is; the string of the set in the block - without `;`
      if(!db[s].parent || !db.is<n_block>(db[s].parent)) return;
      std::string_view t = db.as<n_hole>(s).text;
      while(!t.empty() && (t.back() == ' ' || t.back() == '\n' || t.back() == '\t')) t.remove_suffix(1);
      if(t.empty() || t.back() == ';' || t.back() == '}') return;
    }
    if(db.is<n_loop>(s) || db.is<n_block>(s) || db.is<n_try_compile>(s) || db.is<n_struct>(s) || db.is<n_extend>(s)) return;
    if(db.is<n_break>(s) || db.is<n_continue>(s) || db.is<n_hide>(s)) return;
    if(db.is<n_ct>(s) && db.is<n_block>(db.as<n_ct>(s).body)) return;
    if(db.is<n_if>(s) && ends_in_block(s)) return;
    if(db.is<n_call>(s)) {
      auto const & c = db.as<n_call>(s);
      if(!c.words.empty() || c.tail) return; // Block or tail instruction sets `;` itself
    }
    out += ';';
  }

  bool ends_in_block(node_id n) const {
    auto const & i = db.as<n_if>(n);
    node_id last = i.else_ ? i.else_ : i.then_;
    if(db.is<n_if>(last)) return ends_in_block(last);
    // Branch-instruction itself puts `;`
    return db.is<n_block>(last) || db.is<n_return>(last) || db.is<n_require>(last) || db.is<n_break>(last) || db.is<n_continue>(last);
  }

  void print_node(node_id, n_return const & r) {
    out += "return";
    if(r.target) {
      out += '(';
      item(r.target);
      out += ')';
    }
    if(r.value) {
      out += ' ';
      item(r.value);
    }
    out += ';';
  }

  void print_node(node_id, n_require const & r) {
    out += "require(";
    item(r.cond);
    out += ");";
  }

  void print_node(node_id, n_try_compile const & t) {
    out += "try_compile ";
    item(t.body);
  }

  void print_node(node_id, n_extend const & e) {
    if(e.target) { // &f(types) {overload } - name and signature - current at the target
      auto const & f = db.as<n_fn>(node_id(e.target));
      out += cat("&", str(f.name), "(");
      for(std::size_t i = 0; i < f.params.size(); ++i) {
        node_id t = db.as<n_param>(f.params[i]).type;
        out += i ? ", " : "";
        if(!t || db.is<n_auto>(t)) out += "auto";
        else item(t);
      }
      out += ") {";
      for(node_id i: e.items) {
        out += ' ';
        fn_name = f.name; // Takes the head of this overload
        item(i);
      }
      out += " }";
      return;
    }
    out += cat(str(e.name), " {");
    if(!e.attrs.empty()) {
      out += ' ';
      attrs(e.attrs);
      out.pop_back();
    }
    for(node_id i: e.items) {
      out += ' ';
      if(db.is<n_var>(i)) {
        auto const & v = db.as<n_var>(i);
        item(v.type);
        out += cat(" ", str(v.name));
        if(v.init) {
          out += " = ";
          item(v.init);
        }
        out += ';';
      } else item(i);
    }
    out += " }";
  }

  void print_node(node_id, n_struct const & st) {
    attrs(st.attrs);
    out += cat("struct ", str(st.name), " {");
    for(node_id f: st.fields) {
      auto const & v = db.as<n_var>(f);
      out += v.ct ? " ct " : " ";
      item(v.type);
      out += cat(" ", str(v.name));
      if(v.init) {
        out += " = ";
        item(v.init);
      }
      out += ';';
    }
    out += " }";
  }

  void print_node(node_id, n_quote const & q) {
    out += "quote {";
    for(node_id i: q.items) {
      out += ' ';
      item(i);
      terminate(i);
    }
    out += " }";
  }

  void print_node(node_id, n_splice const & s) { out += cat("#", str(s.name)); }

  void print_node(node_id, n_throw const & t) {
    out += "throw ";
    item(t.value);
  }

  void print_node(node_id, n_loop const & l) {
    out += "loop ";
    item(l.body);
  }

  void print_node(node_id, n_break const &) { out += "break;"; }
  void print_node(node_id, n_hide const & h) { out += cat("hide ", str(h.name), ";"); }
  void print_node(node_id, n_continue const &) { out += "continue;"; }

  void print_node(node_id, n_pack const & p) { // PAC outside the call (not in the text)
    for(uint32_t i = 0; i < p.items.size(); ++i) {
      if(i) out += ' ';
      item(p.items[i]);
    }
  }

  // --- expression ---

  void print_node(node_id, n_if const & i) {
    out += "if(";
    item(i.cond);
    out += ") ";
    item(i.then_);
    if(i.else_) {
      if(db.is<n_block>(i.then_)) out += " else ";
      else out += " else ";
      item(i.else_);
    }
  }

  void print_node(node_id, n_ident const & i) { out += str(i.name); }
  void print_node(node_id, n_auto const &) { out += "auto"; }
  void print_node(node_id, n_int const & i) { out += std::to_string(i.value); }

  void print_node(node_id, n_float const & f) {
    auto s = cat(f.value);
    if(!s.contains('.') && !s.contains('e')) s += ".0";
    out += s;
  }

  void print_node(node_id, n_str const & s) {
    out += '"';
    for(char c: s.value) {
      if(c == '"' || c == '\\') {
        out += '\\';
        out += c;
      } else if(c == '\n') out += "\\n";
      else out += c;
    }
    out += '"';
  }

  void print_node(node_id n, n_op const & o) {
    auto op = str(o.op);
    if(o.args.size() == 1) {
      if(o.postfix) {
        operand(o.args[0], false, {});
        out += op;
        return;
      }
      out += op;
      operand(o.args[0], false, {});
      return;
    }
    if(op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=") { // appropriation
      operand(o.args[0], false, {});
      out += cat(" ", op, " ");
      item(o.args[1]);
      return;
    }
    operand(o.args[0], true, o.op);
    out += cat(" ", op, " ");
    operand(o.args[1], false, {});
  }

  void print_node(node_id, n_chain const & c) {
    for(uint32_t k = 0; k < c.atoms.size(); ++k) {
      if(k) out += cat(" ", str(c.ops[k - 1]), " ");
      operand(c.atoms[k], false, {});
    }
  }

  // Brackets - where without them, parsing would give another tree: there are no
  // priorities, so around any nested infix except the left operand of the same
  // associative operator (a + b + c).
  void operand(node_id child, bool left, symbol parent_op) {
    bool infix = (db.is<n_op>(child) && db.as<n_op>(child).args.size() == 2) || db.is<n_chain>(child);
    bool paren = infix || db.is<n_if>(child);
    if(infix && left && db.is<n_op>(child) && db.as<n_op>(child).op == parent_op && associative(parent_op)) paren = false;
    if(paren) out += '(';
    item(child);
    if(paren) out += ')';
  }

  bool associative(symbol op) const {
    for(uint32_t i = 0; i < db.size(); ++i) {
      node_id f(i);
      if(!db.is<n_fn>(f) || db.strings.text(db.as<n_fn>(f).name) != cat("infix", str(op))) continue;
      for(node_id a: db.as<n_fn>(f).attrs)
        if(db.is<n_ident>(a) && db.strings.text(db.as<n_ident>(a).name) == "associative") return true;
    }
    return false;
  }

  // Challenge: head and clauses. The parameters of the construction block are printed in
  // parentheses of its group: `auto x : xs` at the place of its source, `E e` at the end.
  void print_node(node_id, n_call const & c) {
    item(c.callee);
    auto word = [&](uint32_t i) { return c.words.empty() ? symbol{} : c.words[i]; };
    uint32_t n = uint32_t(c.args.size());
    uint32_t i = 0;
    uint32_t head_end = 0;
    while(head_end < n && !word(head_end)) ++head_end;
    group(c, 0, head_end, c.tail, !c.words.empty());
    for(i = head_end; i < n; ++i) {
      if(db.is<n_pack>(c.args[i])) {
        for(node_id x: db.as<n_pack>(c.args[i]).items) {
          out += cat(" ", str(word(i)));
          clause(x, {});
        }
        continue;
      }
      uint32_t from = i;
      while(i + 1 < n && word(i + 1) == word(from) && !db.is<n_pack>(c.args[i + 1])) ++i;
      out += cat(" ", str(word(from)));
      group(c, from, i + 1, true, true);
    }
  }

  void group(n_call const & c, uint32_t from, uint32_t to, bool tail, bool bare_ok) {
    node_list args(c.args.begin() + from, c.args.begin() + to - (tail && to > from ? 1 : 0));
    node_id body = tail && to > from ? c.args[to - 1] : node_id{};
    if(!tail) {
      out += '(';
      for(uint32_t k = 0; k < args.size(); ++k) {
        if(k) out += ", ";
        item(args[k]);
      }
      out += ')';
      return;
    }
    clause(body, args, bare_ok);
  }

  // (Arguments and parameters of the block) body
  void clause(node_id body, node_list const & args, bool bare_ok = true) {
    n_lambda const * lam = db.is<n_lambda>(body) && db.as<n_lambda>(body).block ? &db.as<n_lambda>(body) : nullptr;
    // Print immediately in order, without intermediate lines: the layout of
    // projection by node (editor) requires the text to go out linearly
    uint32_t count = uint32_t(args.size());
    if(lam)
      for(node_id pn: lam->params)
        if(db.as<n_param>(pn).source < 0) ++count;
    if(count || !bare_ok) {
      out += '(';
      bool first = true;
      auto sep = [&] {
        if(!first) out += ", ";
        first = false;
      };
      for(uint32_t k = 0; k < args.size(); ++k) {
        sep();
        if(lam)
          for(node_id pn: lam->params)
            if(db.as<n_param>(pn).source == int64_t(k)) {
              item(pn);
              out += " : ";
            }
        item(args[k]);
      }
      if(lam)
        for(node_id pn: lam->params)
          if(db.as<n_param>(pn).source < 0) {
            sep();
            item(pn);
          }
      out += ')';
    }
    node_id b = lam ? lam->body : body;
    out += ' ';
    item(b);
    if(!db.is<n_block>(b)) terminate(b);
  }

  void print_node(node_id, n_member const & m) {
    item(m.object);
    out += '.';
    out += str(m.name);
  }

  void print_node(node_id, n_field const & f) {
    item(f.object);
    out += ".[";
    item(f.key);
    out += ']';
  }

  void print_node(node_id, n_index const & i) {
    item(i.object);
    out += '[';
    item(i.index);
    out += ']';
  }

  void print_node(node_id, n_ct const & c) {
    out += "ct ";
    item(c.body);
  }

  void print_node(node_id, n_lambda const & l) {
    params(l.params);
    out += " => ";
    item(l.body);
  }

  void print_node(node_id, n_construct const & c) {
    item(c.type);
    out += '{';
    for(uint32_t i = 0; i < c.items.size(); ++i) {
      if(i) out += ", ";
      item(c.items[i]);
    }
    out += '}';
  }

  // For the editor (layout), the hole is only your text: the reason is the node note,
  // the scenery, not the editable text.
  void print_node(node_id, n_hole const & h) { out += layouts ? h.text : cat("/* дыра: ", h.reason, " */ ", h.text); }

  ast_db const & db;
  symbol fn_name; // name for the head of the following function (overload)
  filter keep;
  substitution subst;
  std::string out;
  uint32_t indent = 0;
  std::vector<node_layout> * layouts = nullptr; // It's not empty.
  std::vector<uint32_t> open; // node, which is printed (numbers in layouts)
  std::size_t mark = 0; // beginning of text not yet referred to as node
};

} // namespace tz
