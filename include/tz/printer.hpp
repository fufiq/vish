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
    if(auto rec = record_for(n)) return print_record(n, *rec);
    std::visit([&](auto const & k) { print_node(n, k); }, db[n].kind);
  }

  // A node without a record (if, loop... without prelude) is a kernel record: type {поля}.
  void print_node(node_id n, auto const &) {
    out += cat(kind_name(db[n]), "{");
    bool first = true;
    db.for_each_child(n, [&](node_id c) {
      out += first ? "" : ", ";
      first = false;
      item(c);
    });
    out += '}';
  }

  // --- instruction ---

  void print_node(node_id, n_module const & m) {
    if(m.name) out += cat(m.sealed ? "sealed " : "", "module ", str(m.name), ";\n");
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
    if(!f.form.empty()) { // syntax - the record repeats the ad
      out += "syntax ";
      form(f.form, [&](std::size_t k, std::string_view piece) {
        auto const & p = db.as<n_param>(f.params[k]);
        if(piece == "{=") out += cat("{[[value]] ", str(p.name), "}");
        else if(piece == "{") out += cat("{", str(p.name), "}");
        else params({f.params[k]});
      }, true);
      out += " => ";
      item(f.body);
      if(!db.is<n_block>(f.body)) out += ';';
      return;
    }
    if(f.outline) out += "outline ";
    if(f.result_type) {
      item(f.result_type);
      out += ' ';
    }
    auto name = str(f.name);
    for(std::string_view role: {"infix", "prefix", "postfix"})
      if(name.size() > role.size() && name.starts_with(role) && !lexer::is_ident(name[role.size()])) {
        auto sym = name.substr(role.size());
        out += f.scoped ? "using operator " : "operator ";
        if(role == "prefix") out += sym;
        params({f.params[0]});
        if(role == "postfix") out += sym;
        if(sym == "[]") { // (a)[i]
          out += '[';
          item(f.params[1]);
          out += ']';
        } else if(role == "infix") {
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

  // Syntax form: word through the space, (...) - close to the word, {…} - through the
  // space, `;` - close; hole(k, часть) prints the k-th hole ("(", "()), "{" - body, "{="
  // - slot value). In the announcement (decl) brackets - as written: "()" close, "("
  // through the space.
  void form(std::vector<symbol> const & pieces, auto && hole, bool decl = false) {
    std::size_t k = 0;
    for(std::size_t i = 0; i < pieces.size(); ++i) {
      auto p = str(pieces[i]);
      bool paren = p == "(" || p == "()";
      bool after_word = i && lexer::is_ident_start(str(pieces[i - 1])[0]);
      bool glued = p == "()" || (!decl && p == "(" && after_word);
      if(i && !glued && p != ";") out += ' ';
      if(paren || p == "{" || p == "{=") hole(k++, p);
      else out += p;
    }
  }

  // prelude entries (§4.4): kernel node and kernel call are printed on the first
  // suitable record -, the record - in prelude.
  std::vector<std::pair<node_id, record_info>> const & records() {
    if(!records_built) {
      records_built = true;
      if(db.prelude)
        for(node_id f: db.as<n_module>(db.prelude).items)
          if(auto r = record_of(db, f)) record_list.push_back({f, *r});
    }
    return record_list;
  }
  // Write node n: Body locations а node field (call arguments); empty no.
  std::pair<node_id, record_info> const * record_for(node_id n) {
    node_id up = db[n].parent; // hide(x) in syntax hide {x} => hide(x) - as written
    // (Form-call body - normal code: `=> if(c) a else b`, not n_if {…})
    if(up && db.is<n_fn>(up) && !db.as<n_fn>(up).form.empty() && db.as<n_fn>(up).body == n && record_of(db, up)) return nullptr;
    for(auto const & rec: records()) {
      auto const & [f, r] = rec;
      if(r.kind >= 0) {
        if(int(db[n].kind.index()) != r.kind) continue;
        bool fits = true;
        for(uint32_t p = 0; p < r.places && fits; ++p)
          fits = bool(node_place(db[n].kind, p)) == std::ranges::contains(r.slot, p);
        if(fits) return &rec;
      } else if(db.is<n_call>(n)) {
        auto const & c = db.as<n_call>(n);
        if(db.is<n_ident>(c.callee) && db.as<n_ident>(c.callee).name == db.as<n_fn>(f).name && c.words.empty() && !c.tagged && !c.form && c.args.size() == r.places) return &rec;
      }
    }
    return nullptr;
  }
  void print_record(node_id n, std::pair<node_id, record_info> const & rec) {
    auto const & [f, r] = rec;
    form(db.as<n_fn>(f).form, [&](std::size_t k, std::string_view piece) {
      bool block = piece[0] == '{';
      node_id x = r.kind >= 0 ? node_place(db[n].kind, r.slot[k]) : db.as<n_call>(n).args[r.slot[k]];
      if(!block) out += '(';
      item(x);
      if(!block) out += ')';
    });
  }
  // node itself sets `;` (return x; break;)
  bool self_terminated(node_id n) {
    auto rec = record_for(n);
    return rec && str(db.as<n_fn>(rec->first).form.back()) == ";";
  }
  bool records_built = false;
  std::vector<std::pair<node_id, record_info>> record_list;

  void params(node_list const & ps) {
    out += '(';
    for(uint32_t i = 0; i < ps.size(); ++i) {
      if(i) out += ", ";
      item(ps[i]);
    }
    out += ')';
  }

  void print_node(node_id, n_pat const & p) {
    if(p.type) item(p.type); // typeless - {x: a, y: b}
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

  void print_node(node_id n, n_pat_field const & f) {
    node_id p = db[n].parent;
    if(p && db.is<n_pat>(p) && !db.as<n_pat>(p).type && db.is<n_op>(f.access)) // {x: a} is the name of a field without a point
      out += str(db.as<n_op>(f.access).op).substr(1);
    else item(f.access);
    out += ": ";
    if(db.is<n_param>(f.sub) || db.is<n_pat>(f.sub)) item(f.sub);
    else {
      out += '(';
      item(f.sub);
      out += ')';
    }
  }

  void print_node(node_id, n_subject const &) {} // implicit root: .size, [1].flags

  void print_node(node_id, n_use const & u) {
    out += cat("use ", str(u.key));
    if(!u.names.empty()) {
      out += ".{";
      for(std::size_t i = 0; i < u.names.size(); ++i) out += cat(i ? ", " : "", str(u.names[i]));
      out += '}';
    }
  }

  void print_node(node_id, n_native const & x) {
    out += cat("native ", in_quotes(str(x.lang)), " {", x.code, "}");
  }

  // raw block (§4.4) - text as written: it is parsed by code in language, not projection
  void print_node(node_id, n_raw const & x) { out += x.text; }
  void print_node(node_id, n_sym const & x) { out += x.text; }

  // call tag(s) with template writing: tag "text {x} text"
  void tagged(n_call const & t) {
    item(t.callee);
    out += '"';
    for(node_id p: t.args) {
      if(!db.is<n_str>(p)) {
        out += '{';
        item(p);
        out += '}';
        continue;
      }
      for(char c: db.as<n_str>(p).value) {
        if(c == '"' || c == '\\') out += '\\';
        if(c == '\n') {
          out += "\\n";
          continue;
        }
        if(c == '{' || c == '}') out += c; // {{ }}
        out += c;
      }
    }
    out += '"';
  }

  void print_node(node_id, n_spread const & s) {
    out += "...";
    item(s.value);
  }

  void print_node(node_id n, n_unpack const & u) {
    bool paren = (db.is<n_op>(db[n].parent) && !db.is_index(db[n].parent) && !db.is_field(db[n].parent)) || db.is<n_chain>(db[n].parent); // operand: (p = e) &&...
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
    if(p.type) {
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
    attrs(v.attrs);
    if(v.is_const) out += "const ";
    if(v.ct) out += "ct ";
    if(v.ref) out += "&";
    if(v.strict) out += "explicit ";
    if(v.type) {
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
    if(db.is<n_var>(s) || db.is<n_fn>(s)) return;
    if(db.is<n_hole>(s)) { // the hole prints the source code as is; the string of the set in the block - without `;`
      if(!db[s].parent || !db.is<n_block>(db[s].parent)) return;
      std::string_view t = db.as<n_hole>(s).text;
      while(!t.empty() && (t.back() == ' ' || t.back() == '\n' || t.back() == '\t')) t.remove_suffix(1);
      if(t.empty() || t.back() == ';' || t.back() == '}') return;
    }
    if(db.is<n_block>(s) || db.is<n_raw>(s) || db.is<n_extend>(s)) return;
    if(db.is<n_ct>(s) && db.is<n_block>(db.as<n_ct>(s).body)) return;
    if(closed(s)) return;
    if(db.is<n_call>(s)) {
      auto const & c = db.as<n_call>(s);
      if(!c.words.empty() || c.tail) return; // Block or tail instruction sets `;` itself
    }
    out += ';';
  }

  // Write node ends with `;` (return x;) or body block (loop { }, if(c) { } else { }) -
  // `;` is not needed after it.
  bool closed(node_id n) {
    auto rec = record_for(n);
    if(!rec) return false;
    auto const & [f, r] = *rec;
    auto last = str(db.as<n_fn>(f).form.back());
    if(last == ";") return true;
    if((last != "{" && last != "{=") || r.slot.empty()) return false;
    node_id x = r.kind >= 0 ? node_place(db[n].kind, r.slot.back()) : db.as<n_call>(n).args[r.slot.back()];
    return db.is<n_block>(x) || db.is<n_raw>(x) || closed(x);
  }

  void print_node(node_id n, n_extend const & e) {
    if(e.of) return instance_decl(n, e);
    if(e.target) { // &f(types) {overload } - name and signature - current at the target
      auto const & f = db.as<n_fn>(node_id(e.target));
      out += cat("&", str(f.name), "(");
      for(std::size_t i = 0; i < f.params.size(); ++i) {
        node_id t = db.as<n_param>(f.params[i]).type;
        out += i ? ", " : "";
        if(!t) out += "auto";
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

  // T Name [: base] { поля } - instance T (struct)
  void instance_decl(node_id, n_extend const & st) {
    attrs(st.attrs);
    out += cat(str(st.of), " ", str(st.name), st.base ? cat(" : ", str(st.base)) : std::string(), " {");
    for(node_id f: st.items) {
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
  // ad link - current target name (link, not name: renaming the target changes the target)
  void print_node(node_id, n_ref const & r) {
    node_id d(r.target);
    symbol s = db.is<n_fn>(d) ? db.as<n_fn>(d).name : db.is<n_extend>(d) ? db.as<n_extend>(d).name : db.is<n_var>(d) ? db.as<n_var>(d).name : symbol{};
    out += s ? std::string(str(s)) : std::string("?");
  }

  // --- If, loop, return, break, continue, throw - from prelude (record_for)
  //

  void print_node(node_id, n_ident const & i) { out += str(i.name); }
  void print_node(node_id, n_int const & i) {
    if(i.hex) {
      char buf[24];
      snprintf(buf, sizeof buf, "0x%llX", static_cast<unsigned long long>(i.value));
      out += buf;
      return;
    }
    if(!i.ch) {
      out += std::to_string(i.value);
      return;
    }
    char c = char(i.value);
    out += c == '\n' ? "'\\n'" : c == '\t' ? "'\\t'" : c == '\'' || c == '\\' ? cat("'\\", c, "'") : cat("'", c, "'");
  }

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
    if(db.is_index(n)) return index(o);
    if(db.is_field(n)) return field(o);
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
      operand(c.atoms[k], k == 0, same_op(c)); // (a + b) + c + d - as a + b + c + d
    }
  }
  // The chain of one operator is its symbol, otherwise it is empty.
  symbol same_op(n_chain const & c) const {
    for(symbol o: c.ops)
      if(o != c.ops[0]) return {};
    return c.ops[0];
  }

  // Brackets - where without them, parsing would give another tree: there are no
  // priorities, so around any nested infix, except the left operand of the same
  // associative operator (a + b + c is a chain, its tree is one).
  void operand(node_id child, bool left, symbol parent_op) {
    bool infix = (db.is<n_op>(child) && db.as<n_op>(child).args.size() == 2 && !db.is_index(child) && !db.is_field(child)) || db.is<n_chain>(child);
    bool paren = infix || db.is<n_if>(child);
    symbol child_op = db.is<n_op>(child) ? db.as<n_op>(child).op : db.is<n_chain>(child) ? same_op(db.as<n_chain>(child)) : symbol{};
    if(infix && left && parent_op && child_op == parent_op && associative(parent_op, child)) paren = false;
    if(paren) out += '(';
    item(child);
    if(paren) out += ')';
  }

  // [[associative]] in the operator, visible in place lexically: module elements and prelude,
  // the operators of the blocks declared above. The context of the execution (using operator
  // of the function-context) the print does not know - there the brackets remain.
  bool associative(symbol op, node_id at) const {
    auto entity = cat("infix", str(op));
    auto fits = [&](node_id f) {
      if(!db.is<n_fn>(f) || db.strings.text(db.as<n_fn>(f).name) != entity) return false;
      for(node_id a: db.as<n_fn>(f).attrs)
        if(db.is<n_ident>(a) && db.strings.text(db.as<n_ident>(a).name) == "associative") return true;
      return false;
    };
    for(uint32_t i = 0; i < db.size(); ++i)
      if(node_id f(i); fits(f) && !db.is<n_block>(db[f].parent)) return true;
    for(node_id b = db[at].parent; b; b = db[b].parent)
      if(db.is<n_block>(b))
        for(node_id x: db.as<n_block>(b).items)
          if(fits(x)) return true;
    return false;
  }

  // Challenge: head and clauses. The parameters of the construction block are printed in
  // parentheses of its group: `auto x : xs` at the place of its source, `E e` at the end.
  void print_node(node_id call, n_call const & c) {
    if(c.tagged) return tagged(c); // hide x, try_compile { … } - prelude records (record_for)
    if(c.form && c.form - 1 < db.size() && db.is<n_fn>(node_id(c.form - 1)) && !db.as<n_fn>(node_id(c.form - 1)).form.empty()) {
      auto const & pieces = db.as<n_fn>(node_id(c.form - 1)).form;
      std::size_t shown = 0;
      form(pieces, [&](std::size_t k, std::string_view piece) {
        bool block = piece[0] == '{';
        if(k >= c.args.size()) return;
        if(!block) out += '(';
        item(c.args[k]);
        if(!block) out += ')';
        shown = k + 1;
      });
      if(shown == c.args.size()) return;
    }
    item(c.callee);
    auto word = [&](uint32_t i) { return c.words.empty() ? symbol{} : c.words[i]; };
    uint32_t n = uint32_t(c.args.size());
    uint32_t i = 0;
    uint32_t head_end = 0;
    while(head_end < n && !word(head_end)) ++head_end;
    group(c, 0, head_end, c.tail, !c.words.empty());
    for(i = head_end; i < n; ++i) {
      if(auto pack = db.clause_pack(c.args[i])) {
        for(node_id x: *pack) {
          out += cat(" ", str(word(i)));
          clause(x, {});
        }
        continue;
      }
      uint32_t from = i;
      while(i + 1 < n && word(i + 1) == word(from) && !db.clause_pack(c.args[i + 1])) ++i;
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
    if(!db.is<n_block>(b) && !db.is<n_raw>(b)) terminate(b);
  }

  void field(n_op const & f) { // v.[f]
    operand(f.args[0], false, {});
    out += ".[";
    item(f.args[1]);
    out += ']';
  }

  void index(n_op const & o) { // (a)[i] The pair operator
    item(o.args[0]);
    out += '[';
    item(o.args[1]);
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
    if(c.type) item(c.type); // without type - anonymous literature {x: 1, y: 2}
    out += '{';
    for(uint32_t i = 0; i < c.items.size(); ++i) {
      if(i) out += ", ";
      if(i < c.names.size()) out += cat(db.strings.text(c.names[i]), ": ");
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
