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
    for(node_id i: m.items) {
      if(!kept(i)) continue;
      if(!first) out += '\n';
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
    out += str(f.name);
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

  void print_node(node_id, n_param const & p) {
    static constexpr std::array modes{"", "mut ", "own ", "out ", "expr ", "ct "};
    out += modes[uint32_t(p.mode)];
    if(p.type) {
      item(p.type);
      out += p.pack ? "... " : " ";
    } else if(p.pack) {
      out.pop_back();
      out += "... ";
    }
    out += str(p.name);
  }

  void print_node(node_id, n_var const & v) {
    if(v.is_const) out += "const ";
    item(v.type);
    out += cat(" ", str(v.name), " = ");
    item(v.init);
    out += ';';
  }

  void print_node(node_id, n_block const & b) {
    std::vector<node_id> items;
    for(node_id i: b.items)
      if(kept(i)) items.push_back(i);
    if(items.empty()) {
      out += "{}";
      return;
    }
    out += '{';
    ++indent;
    for(uint32_t i = 0; i < items.size(); ++i) {
      line();
      item(items[i]);
      bool value = b.has_value && items[i] == b.items.back();
      if(!value) terminate(items[i]);
    }
    --indent;
    line();
    out += '}';
  }

  // `;` after the instruction, unless it ends in a block and puts it itself.
  void terminate(node_id s) {
    if(db.is<n_var>(s) || db.is<n_fn>(s) || db.is<n_require>(s) || db.is<n_return>(s)) return;
    if(db.is<n_hole>(s)) return; // The hole prints the source code as it is.
    if(db.is<n_loop>(s) || db.is<n_block>(s) || db.is<n_try_compile>(s) || db.is<n_struct>(s) || db.is<n_extend>(s)) return;
    if(db.is<n_break>(s) || db.is<n_continue>(s)) return;
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
      out += ' ';
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
      if(op == "?") {
        item(o.args[0]);
        out += '?';
        return;
      }
      out += op;
      operand(n, o.args[0]);
      return;
    }
    operand(n, o.args[0]);
    out += cat(" ", op, " ");
    operand(n, o.args[1]);
  }

  // Brackets around the operand - only if its operator connects weaker.
  void operand(node_id parent, node_id child) {
    bool paren = db.is<n_op>(child) && db.as<n_op>(child).args.size() == 2 && prec(db.as<n_op>(child).op) < prec(db.as<n_op>(parent).op);
    paren = paren || (db.is<n_op>(child) && db.as<n_op>(parent).args.size() == 1 && db.as<n_op>(child).args.size() == 2);
    paren = paren || db.is<n_if>(child);
    if(paren) out += '(';
    item(child);
    if(paren) out += ')';
  }

  int prec(symbol op) const {
    auto s = str(op);
    if(s == "=" || s == "+=" || s == "-=" || s == "*=" || s == "/=") return 1;
    if(s == "||") return 2;
    if(s == "&&") return 3;
    if(s == "==" || s == "!=" || s == "<" || s == ">" || s == "<=" || s == ">=") return 4;
    if(s == "+" || s == "-") return 6;
    if(s == "*" || s == "/" || s == "%") return 7;
    return 9;
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
    std::vector<std::string> parts;
    for(uint32_t k = 0; k < args.size(); ++k) {
      std::string s;
      if(lam)
        for(node_id pn: lam->params)
          if(db.as<n_param>(pn).source == int64_t(k)) s = cat(print_sub(pn), " : ");
      parts.push_back(s + print_sub(args[k]));
    }
    if(lam)
      for(node_id pn: lam->params)
        if(db.as<n_param>(pn).source < 0) parts.push_back(print_sub(pn));
    if(!parts.empty() || !bare_ok) {
      out += '(';
      for(uint32_t k = 0; k < parts.size(); ++k) {
        if(k) out += ", ";
        out += parts[k];
      }
      out += ')';
    }
    node_id b = lam ? lam->body : body;
    out += ' ';
    item(b);
    if(!db.is<n_block>(b)) terminate(b);
  }

  std::string print_sub(node_id n) {
    std::string saved = std::exchange(out, std::string());
    item(n);
    return std::exchange(out, std::move(saved));
  }

  void print_node(node_id, n_member const & m) {
    item(m.object);
    out += '.';
    out += str(m.name);
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

  void print_node(node_id, n_hole const & h) { out += cat("/* дыра: ", h.reason, " */ ", h.text); }

  ast_db const & db;
  filter keep;
  substitution subst;
  std::string out;
  uint32_t indent = 0;
};

} // namespace tz
