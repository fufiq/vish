// Text projection: AST base → canonical text (§4 sequel, §9.4).
//
// projection is deterministic: the same AST is always printed the same way. It is
// also used by slice and residual program (§13.2) - they just need to pass the node
// filter.
#pragma once

#include "ast.hpp"

#include <deque>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>

namespace tz {

// projection node: Your text or child's place.
using piece = std::variant<std::string, node_id>;
struct node_layout {
  node_id node;
  std::vector<piece> pieces;
};

// Argument of the print selection function (§4.4, variant C): a descriptor value or code.
using choice_arg = std::variant<bool, std::string, node_id>;
// Interpreters of the functions of selecting one print - by function module (interp.hpp).
struct interpreter;
struct choice_vms {
  std::list<interpreter> vms; // list: items do not move, type declared below
  node_list mods;
};
// Calling the pure fn selection function in the language (interp.hpp): 1/0; -1 - error (why).
int language_choice(ast_db const & db, choice_vms & pool, node_id fn, std::vector<choice_arg> const & args, std::string & why);

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
    out += cat(lang_kind_of(db[n]), "{");
    bool first = true;
    db.for_each_child(n, [&](node_id c) {
      out += first ? "" : ", ";
      first = false;
      item(c);
    });
    out += '}';
  }

  // --- instruction ---

  void print_node(node_id module, n_module const & m) {
    if(m.name) {
      attrs(m.attrs); // [[extension]] module k;
      out += cat("module ", str(m.name), ";\n");
    }
    bool first = !m.name;
    bool joined = false; // after the draft: the separator of the following element - in its text
    bool top = layouts && open.size() == 1; // module
    if(top) {
      chunk_at.assign(m.items.size(), ~uint32_t{0});
      content_at.assign(m.items.size(), ~uint32_t{0});
    }
    for(uint32_t k = 0; k <= m.items.size(); ++k) {
      if(top && draft && k == draft->at) {
        draft_begin = uint32_t(out.size());
        verbatim_group_text(m.items);
        draft_end = uint32_t(out.size());
        first = false;
        joined = true;
        k += draft->count;
        if(k > m.items.size()) break;
      }
      if(k == m.items.size()) break;
      node_id i = m.items[k];
      if(!kept(i)) continue;
      if(top) chunk_at[k] = uint32_t(out.size());
      if(!first && !joined) out += '\n';
      if(!first && !joined) blank_lines(m.blank, k);
      if(top) content_at[k] = uint32_t(out.size());
      first = false;
      joined = false;
      node_note const * nt = comments ? db.note_of(i) : nullptr;
      if(nt) note_lines(nt->lead, true);
      item(i);
      if(!db.is<n_fn>(i)) terminate(i);
      if(nt && !nt->tail.empty()) out += cat(" ", nt->tail);
      out += '\n';
    }
    if(top) items_end = uint32_t(out.size());
    if(node_note const * nt = comments ? db.note_of(module) : nullptr; nt && !nt->end.empty()) { // comments at the end of the module
      if(!first) out += '\n';
      note_lines(nt->end, true);
    }
  }

  // Comments (node_note, §4.0) - text as written: lines in front of an element or the end of
  // context - each on its line in the indentation of the place (""" is an empty line), the
  // tail - through a space at the end of the line of the element. Printing a comment - like a
  // literal, basic level.
  void note_lines(std::vector<std::string> const & ls, bool module) {
    for(auto const & l: ls) {
      if(module) {
        out += cat(l, "\n");
        continue;
      }
      if(l.empty()) {
        out += '\n';
        if(layouts) out.append(indent * 2, ' ');
        continue;
      }
      line();
      out += l;
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
        else if(form_list(piece)) { // (xs, ...) - part-list
          out += '(';
          item(f.params[k]);
          out += ", ...)";
        } else params({f.params[k]});
      }, [](std::size_t) { return false; }, true);
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

  // Write the syntax form in parts: the word - through the space, (...) - close to the
  // word, {…} - through the space, `;` - close. The layout marks in front of the part
  // (§4.4, projection) change the interval: [[space]] - space, [[glued]] - close, [[line]]
  // - from a new line, [[indent]] - non-block place from a new line to a deeper level (the
  // block recedes itself). hole(k, часть) prints the kth place, block(k) the k place is
  // occupied by the block. In the announcement (decl) brackets - as written ("()" close,
  // "(" through the space), notes - the text [[…]].
  void form(std::vector<symbol> const & pieces, auto && hole, auto && block, bool decl = false) {
    std::size_t k = 0;
    std::string_view prev;
    std::vector<std::string_view> marks;
    for(symbol s: pieces) {
      auto p = str(s);
      if(form_mark(p)) {
        marks.push_back(p.substr(1, p.size() - 2));
        continue;
      }
      bool place = form_paren(p) || p == "{" || p == "{=";
      bool after_word = !prev.empty() && lexer::is_ident_start(prev[0]);
      bool glued = p.starts_with("()") || (!decl && form_paren(p) && after_word);
      bool space = !prev.empty() && !glued && p != ";", newline = false, deeper = false;
      if(decl && !marks.empty()) {
        out += prev.empty() ? "[[" : " [[";
        for(std::size_t i = 0; i < marks.size(); ++i) out += cat(i ? ", " : "", marks[i]);
        out += "]]";
        space = !p.starts_with("()");
      } else
        for(auto m: marks) {
          if(m == "space" || m == "glued") space = m == "space";
          else if(m == "line") newline = true;
          else if(m == "indent") deeper = place && !block(k);
        }
      marks.clear();
      prev = p;
      if(deeper) {
        ++indent;
        line();
        hole(k++, p);
        --indent;
        continue;
      }
      if(newline) line();
      else if(space) out += ' ';
      if(place) hole(k++, p);
      else out += p;
    }
  }

  // The place of writing in its part: (x) - in brackets, (xs, ...) - elements array {…}
  // through the comma, {x} - block or expression as is.
  void part(std::string_view piece, node_id x) {
    if(!form_paren(piece)) return item(x);
    out += '(';
    if(form_list(piece) && db.is<n_construct>(x) && kept(x)) {
      if(layouts) enter(x);
      auto const & xs = db.as<n_construct>(x).items;
      for(std::size_t i = 0; i < xs.size(); ++i) {
        if(i) out += ", ";
        item(xs[i]);
      }
      if(layouts) leave();
    } else item(x);
    out += ')';
  }
  bool block_like(node_id x) const { return db.is<n_block>(x) || db.is<n_raw>(x); }

  // - Printing by field of visibility;§4.4)
  // -------------------------------------------------------------------------------------------- The
  // kernel nodesk and the kernel call are printed from the syntax record visible in the place of the
  // nodesk: context from the nodesk to the nearest first (in context - declared to the element on the
  // way, the first suitable in order of the announcement), `use k` - records of the module k at the
  // use point, blocks of calling the structure - recordings from its body, prelude - the last.
//  
//;.
  // Printing from the record R is analyzed by the same R - reversibility by construction.
  using rec = std::pair<node_id, record_info>;
  struct ctx_rec {
    uint32_t at; // context number
    uint32_t rec; // room
  };

  int rec_of_decl(node_id d) {
    auto [it, fresh] = rec_index.try_emplace(d.value, -1);
    if(fresh)
      if(auto r = record_of(db, d)) {
        rec_pool.push_back({d, *r});
        it->second = int(rec_pool.size() - 1);
      }
    return it->second;
  }
  // Context element entries: syntax recording, entries in the name extension { … }, use k.
  void item_recs(node_id x, std::vector<uint32_t> & out) {
    if(db.is<n_fn>(x)) {
      if(int r = rec_of_decl(x); r >= 0) out.push_back(uint32_t(r));
    } else if(db.is<n_extend>(x)) {
      for(node_id f: db.as<n_extend>(x).items)
        if(db.is<n_fn>(f))
          if(int r = rec_of_decl(f); r >= 0) out.push_back(uint32_t(r));
    } else if(db.is<n_use>(x)) {
      node_id m = find_module(db, db.as<n_use>(x).key);
      if(!m || std::ranges::contains(using_now, m)) return;
      using_now.push_back(m);
      for(node_id y: db.as<n_module>(m).items) item_recs(y, out);
      using_now.pop_back();
    }
  }
  std::vector<ctx_rec> const & context_recs(node_id c) {
    auto [it, fresh] = ctx_recs.try_emplace(c.value);
    if(fresh) {
      auto const & xs = db.context_items(c);
      std::vector<uint32_t> rs;
      for(uint32_t k = 0; k < xs.size(); ++k) {
        item_at[xs[k].value] = k;
        rs.clear();
        item_recs(xs[k], rs);
        for(uint32_t r: rs) it->second.push_back({k, r});
      }
    }
    return it->second;
  }
  // Records from the body of a structure for its blocks (as the rules parsing, §4.4).
  std::vector<uint32_t> const & body_recs(node_id d) {
    auto [it, fresh] = decl_body_recs.try_emplace(d.value);
    if(fresh && db.is<n_fn>(d) && db.is<n_block>(db.as<n_fn>(d).body)) {
      std::vector<uint32_t> rs;
      for(node_id x: db.as<n_block>(db.as<n_fn>(d).body).items) item_recs(x, rs);
      decl_body_recs[d.value] = std::move(rs);
    }
    return decl_body_recs[d.value];
  }
  // Announcements of the call construct c, which have records in the body: the syntax form is its
  // announcement, otherwise functions with the name of the called (living elements of contexts).
  node_list call_rec_decls(node_id c) {
    auto const & k = db.as<n_call>(c);
    if(k.form) return {node_id(k.form - 1)};
    if(!body_scan) {
      body_scan = true;
      for(uint32_t i = 0; i < db.size(); ++i) {
        node_id f(i);
        if(!db.is<n_fn>(f) || !db.is<n_block>(db.as<n_fn>(f).body) || !db.as<n_fn>(f).form.empty() || !db[f].parent) continue;
        node_id up = db[f].parent;
        bool live = db.is_context(up) ? std::ranges::contains(db.context_items(up), f) : db.is<n_extend>(up) && std::ranges::contains(db.as<n_extend>(up).items, f);
        if(live && !body_recs(f).empty()) rec_bodies[db.as<n_fn>(f).name.value].push_back(f);
      }
    }
    if(rec_bodies.empty() || !db.is<n_ident>(k.callee)) return {};
    auto it = rec_bodies.find(db.as<n_ident>(k.callee).name.value);
    return it == rec_bodies.end() ? node_list{} : it->second;
  }
  // a is a call block (as in parsing: block, block with parameters, tail; clause - and in
  // pack): the call itself, otherwise empty.
  node_id block_call(node_id c, node_id a) const {
    if(db.is<n_construct>(c) && db[c].parent && db.is<n_call>(db[c].parent) && !db.as<n_call>(db[c].parent).words.empty())
      return std::ranges::contains(db.as<n_construct>(c).items, a) ? db[c].parent : node_id{};
    if(!db.is<n_call>(c) || !std::ranges::contains(db.as<n_call>(c).args, a)) return {};
    bool block = block_like(a) || (db.is<n_lambda>(a) && db.as<n_lambda>(a).block);
    return block || (db.as<n_call>(c).tail && db.as<n_call>(c).args.back() == a) ? c : node_id{};
  }

  // Write fits node: view and occupied places (node fields or call arguments) and, if the
  // record has [[prefer(f)]], f(узел) in the language: "form selection" of one node (`if …
  // else` or `if …; else`). Error f - no entry is taken (conservatively: next suitable, no
  // condition).
  bool fits(node_id n, rec const & x) {
    if(!shape_fits(n, x)) return false;
    for(node_id a: db.as<n_fn>(x.first).attrs) {
      if(!db.is<n_call>(a) || !db.is<n_ident>(db.as<n_call>(a).callee) || str(db.as<n_ident>(db.as<n_call>(a).callee).name) != "prefer") continue;
      auto const & args = db.as<n_call>(a).args;
      node_id fn = args.size() == 1 && db.is<n_ident>(args[0]) ? visible_fn(str(db.as<n_ident>(args[0]).name), x.first) : node_id{};
      if(!fn) {
        notes.push_back("печать: prefer(f) — функция f не найдена; запись не выбрана");
        return false;
      }
      if(choose(fn, {n}) != 1) return false;
    }
    return true;
  }
  bool shape_fits(node_id n, rec const & x) const {
    auto const & [f, r] = x;
    if(r.kind >= 0) {
      if(int(db[n].kind.index()) != r.kind) return false;
      for(uint32_t p = 0; p < r.places; ++p)
        if(bool(node_place(db[n].kind, p)) != std::ranges::contains(r.slot, p)) return false;
      return true;
    }
    if(!db.is<n_call>(n)) return false;
    auto const & c = db.as<n_call>(n);
    return db.is<n_ident>(c.callee) && db.as<n_ident>(c.callee).name == db.as<n_fn>(f).name && c.words.empty() && !c.tagged && !c.form && c.args.size() == r.places;
  }
  // node, which can be printed by writing: all fields are node (n_if, n_return...) or call.
  bool record_kind(node_id n) const {
    static std::vector<int8_t> const able = [] {
      std::vector<int8_t> v;
      for(int k = 0; k < int(std::variant_size_v<node_kind>); ++k) v.push_back(node_code_fields(k) >= 0);
      return v;
    }();
    return db.is<n_call>(n) || able[db[n].kind.index()];
  }

  // Syntax form call (c.form - link to the ad, it was chosen by parsing): the record - the ad
  // itself, places - arguments in the order of parts (view -2). Print, `;` after instruction and
  // layout marking are the same as node records.
  rec const * form_call(node_id n) {
    auto const & c = db.as<n_call>(n);
    node_id d(c.form - 1);
    if(c.form - 1 >= db.size() || !db.is<n_fn>(d) || db.as<n_fn>(d).form.empty()) return nullptr;
    auto [it, fresh] = rec_index.try_emplace(d.value | 0x80000000u, -1);
    if(fresh) {
      record_info r{-2, {}, uint32_t(db.as<n_fn>(d).params.size())};
      for(uint32_t k = 0; k < r.places; ++k) r.slot.push_back(k);
      rec_pool.push_back({d, std::move(r)});
      it->second = int(rec_pool.size() - 1);
    }
    rec const & x = rec_pool[it->second];
    return c.args.size() == x.second.places ? &x : nullptr; // The number of seats is not the same.
  }

  // Write node n (body locations а node field or call arguments); empty - no.
  rec const * record_for(node_id n) {
    if(!record_kind(n)) return nullptr;
    if(db.is<n_call>(n) && db.as<n_call>(n).form) return form_call(n);
    node_id up = db[n].parent; // hide(x) in syntax hide {x} => hide(x) - as written
    // (Form-call body - normal code: `=> if(c) a else b`, not n_if {…})
    if(up && db.is<n_fn>(up) && !db.as<n_fn>(up).form.empty() && db.as<n_fn>(up).body == n && record_of(db, up)) return nullptr;
    if(auto it = found.find(n.value); it != found.end()) return it->second < 0 ? nullptr : &rec_pool[it->second];
    int r = -1;
    if(projection) // projection Presentation - First: Its Records Replace Place Records
      for(auto const & x: context_recs(projection))
        if(fits(n, rec_pool[x.rec])) {
          r = int(x.rec);
          found[n.value] = r;
          return &rec_pool[r];
        }
    node_id top = n;
    for(node_id at = n, c = up; c && r < 0; at = c, c = db[c].parent) {
      top = c;
      if(db.is_context(c)) {
        auto const & rs = context_recs(c);
        if(rs.empty()) continue;
        auto pos = item_at.find(at.value);
        if(pos == item_at.end()) continue;
        for(auto const & x: rs)
          if(x.at < pos->second && fits(n, rec_pool[x.rec])) {
            r = int(x.rec);
            break;
          }
      } else if(node_id call = block_call(c, at))
        for(node_id d: call_rec_decls(call)) {
          for(uint32_t x: body_recs(d))
            if(fits(n, rec_pool[x])) {
              r = int(x);
              break;
            }
          if(r >= 0) break;
        }
    }
    if(r < 0 && db.prelude && top != db.prelude)
      for(auto const & x: context_recs(db.prelude))
        if(fits(n, rec_pool[x.rec])) {
          r = int(x.rec);
          break;
        }
    found[n.value] = r;
    return r < 0 ? nullptr : &rec_pool[r];
  }
  void print_record(node_id n, rec const & x) {
    auto const & [f, r] = x;
    auto at = [&](std::size_t k) { return r.kind >= 0 ? node_place(db[n].kind, r.slot[k]) : db.as<n_call>(n).args[r.slot[k]]; };
    form(db.as<n_fn>(f).form, [&](std::size_t k, std::string_view piece) { part(piece, at(k)); }, [&](std::size_t k) { return block_like(at(k)); });
  }
  // node itself sets `;` (return x; break;)
  bool self_terminated(node_id n) {
    auto rec = record_for(n);
    return rec && str(db.as<n_fn>(rec->first).form.back()) == ";";
  }
  std::deque<rec> rec_pool; // records encountered by the seal (links are stable)
  std::unordered_map<uint32_t, int> rec_index; // Advertisement → Record Number (-1 - Not Record)
  std::unordered_map<uint32_t, std::vector<ctx_rec>> ctx_recs; // the context of the recording of its elements
  std::unordered_map<uint32_t, uint32_t> item_at; // Context Element → Its Number
  std::unordered_map<uint32_t, std::vector<uint32_t>> decl_body_recs; // construction
  std::unordered_map<uint32_t, node_list> rec_bodies; // designs with records in the body
  bool body_scan = false;
  std::unordered_map<uint32_t, int> found; // Node → its record (-1 - no)
  node_list using_now; // Modules whose records are taken (use cycle)

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

  void print_node(node_id block, n_block const & b) {
    std::vector<uint32_t> at; // Numbers of output elements in b.items
    for(uint32_t k = 0; k < b.items.size(); ++k)
      if(kept(b.items[k])) at.push_back(k);
    uint32_t tail = uint32_t(b.items.size());
    node_note const * end = comments ? db.note_of(block) : nullptr;
    if(at.empty() && (tail >= b.blank.size() || b.blank[tail] == 0) && (!end || end->end.empty())) {
      out += "{}";
      return;
    }
    out += '{';
    ++indent;
    for(uint32_t k: at) {
      blank_lines(b.blank, k);
      node_note const * nt = comments ? db.note_of(b.items[k]) : nullptr;
      if(nt) note_lines(nt->lead, false);
      line();
      item(b.items[k]);
      bool value = b.has_value && k + 1 == b.items.size();
      if(!value) terminate(b.items[k]);
      if(nt && !nt->tail.empty()) out += cat(" ", nt->tail);
    }
    blank_lines(b.blank, tail);
    if(end) note_lines(end->end, false);
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
    // branch - construction with tail-instruction or clauses (if(c) for(x : xs) s;) puts `;` itself
    bool self_closing = db.is<n_call>(x) && (db.as<n_call>(x).tail || !db.as<n_call>(x).words.empty());
    return db.is<n_block>(x) || db.is<n_raw>(x) || self_closing || closed(x);
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

  // The brackets around the operand select the pure `operand_parens` function visible
  // in place (prelude - default):
// Arguments --
  // Describers: the type of operand ("infix", "chain", "if", "), its operator, whether
  // left, the operator of the parent and whether it is associative in place (only when the
  // operand is the same operator). There is no function or it has fallen - conservative:
  // brackets for any infix and if (so parsing gives the same tree).
  void operand(node_id child, bool left, symbol parent_op) {
    bool infix_op = db.is<n_op>(child) && db.as<n_op>(child).args.size() == 2 && !db.is_index(child) && !db.is_field(child);
    std::string_view kind = infix_op ? "infix" : db.is<n_chain>(child) ? "chain" : db.is<n_if>(child) ? "if" : "";
    bool paren = !kind.empty();
    if(paren) {
      symbol child_op = infix_op ? db.as<n_op>(child).op : db.is<n_chain>(child) ? same_op(db.as<n_chain>(child)) : symbol{};
      bool assoc = left && parent_op && child_op == parent_op && kind != "if" && associative(parent_op, child);
      if(node_id fn = visible_fn("operand_parens", child))
        if(int c = choose(fn, {std::string(kind), std::string(child_op ? str(child_op) : ""), left, std::string(parent_op ? str(parent_op) : ""), assoc}); c >= 0) paren = c;
    }
    if(paren) out += '(';
    item(child);
    if(paren) out += ')';
  }

  // Selection of code in the language (§4.4, variant C)
  // --------------------------------------------------------------------------------------------------------- Where the
  // printing needs a solution (bracket, the shape of the nodeciration), it takes a pure function on the language: it only
  // selectselects (bool), printselected. Error or impurity (effect, code change) - conservative choice and notes.

  // The name function seen in place at: contexts from place up, nearest first (the
  // function is visible in its entire context), prelude last; empty - no.
  node_id visible_fn(std::string_view name, node_id at) {
    auto in = [&](node_id c) {
      auto [it, fresh] = fn_in_ctx.try_emplace({c.value, std::string(name)});
      if(fresh)
        for(node_id x: db.context_items(c))
          if(db.is<n_fn>(x) && db.as<n_fn>(x).form.empty() && str(db.as<n_fn>(x).name) == name) it->second = x;
      return it->second;
    };
    if(projection)
      if(node_id f = in(projection)) return f;
    node_id top = at;
    for(node_id c = db[at].parent; c; c = db[c].parent) {
      top = c;
      if(db.is_context(c))
        if(node_id f = in(c)) return f;
    }
    if(db.prelude && top != db.prelude) return in(db.prelude);
    return {};
  }
  // The result of the function of selecting fn over args (0/1); -1 is an error (note in notes).
  // Cash - in the database: function, its revision, base edits, arguments (node - with revision).
  int choose(node_id fn, std::vector<choice_arg> const & args) {
    std::string key = cat(fn.value, "@", db.node_revision(fn), "/", db.edits);
    for(auto const & a: args) {
      if(auto b = std::get_if<bool>(&a)) key += *b ? "|1" : "|0";
      else if(auto s = std::get_if<std::string>(&a)) key += cat("|s", *s);
      else key += cat("|n", std::get<node_id>(a).value, "@", db.node_revision(std::get<node_id>(a)));
    }
    if(auto it = db.print_choices.find(key); it != db.print_choices.end()) return it->second;
    std::string why;
    int r = language_choice(db, vms, fn, args, why);
    if(r < 0) notes.push_back(cat("печать: ", str(db.as<n_fn>(fn).name), " — ", why, "; выбрано консервативно"));
    db.print_choices[key] = int8_t(r);
    return r;
  }
  std::map<std::pair<uint32_t, std::string>, node_id> fn_in_ctx; // (context, name) → function of choice
  std::vector<std::string> notes; // Printing Notes (Errors of Selection Functions)
  choice_vms vms; // Interpreters of Selection Functions - at the Time of Printing
  bool comments = true; // Post comments (slice and residual program are not)
  // projection (§4.4, variant C): A module is a group of syntax records (and selection
  // functions); its records are the first, before the location records. Empty - by default:
  // entries by scope, prelude - C-like projection (D3).
  node_id projection;

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

  // --- Draft editor: typed text as is ---
  //
  // Author's decision (2026-10-12): the text of the module element that rules is exactly as
  // typed; the canon is when leaving the element. The draft is also projection: the layout
  // of its node is built according to their places in the typed text (where parsing), and
  // not by print - the editor's DOM remains the AST tree.
  struct verbatim_group {
    uint32_t at = 0; // first element of the draft module (number in items)
    uint32_t count = 0; // draft
    uint32_t source = 0; // Import of typed text: places node - in it
  };
  verbatim_group const * draft = nullptr;
  // places in the output (module layout): by element number - the beginning of its piece
  // (with a separator) and the beginning of its lines (~0 - not output); end of elements
  // (until the comments of the end of the module); draft text [draft_begin , draft_end )
  std::vector<uint32_t> chunk_at, content_at;
  uint32_t items_end = 0, draft_begin = 0, draft_end = 0;

  void verbatim_group_text(node_list const & items) {
    std::string_view src = db.source(draft->source);
    uint32_t pos = 0;
    for(uint32_t k = draft->at; k < draft->at + draft->count && k < items.size(); ++k) {
      span w = db[items[k]].where;
      if(w.source != draft->source || w.begin < pos || w.end > src.size()) continue;
      out += src.substr(pos, w.begin - pos);
      verbatim(items[k], src);
      pos = w.end;
    }
    out += src.substr(pos);
  }

  // The knot is typed: a piece of it between children is as written; children without their
  // place in this text (born parsing) are not shown separately. whole - your node text - all
  // typed text (root of the function representation).
  void verbatim(node_id n, std::string_view src, bool whole = false) {
    span w = db[n].where;
    span own = whole ? span{0, uint32_t(src.size()), w.source} : w;
    if(!layouts) {
      out += src.substr(own.begin, own.end - own.begin);
      return;
    }
    if(whole) { // children in the place node, the whole text
      enter(n);
      if(w.end <= src.size() && w.begin <= w.end) {
        out += src.substr(0, w.begin);
        verbatim_inside(n, src, w);
        out += src.substr(w.end);
      } else out += src;
      leave();
      return;
    }
    enter(n);
    verbatim_inside(n, src, w);
    leave();
  }
  void verbatim_inside(node_id n, std::string_view src, span w) {
    if(db.is<n_raw>(n) || db.is<n_native>(n) || db.is<n_hole>(n)) { // Text as written - sheet as in projection
      out += src.substr(w.begin, w.end - w.begin);
      return;
    }
    std::vector<node_id> kids;
    db.for_each_child(n, [&](node_id c) {
      span cw = db[c].where;
      if(cw.source == w.source && cw.begin >= w.begin && cw.end <= w.end && cw.begin < cw.end) kids.push_back(c);
    });
    std::ranges::stable_sort(kids, {}, [&](node_id c) { return db[c].where.begin; });
    uint32_t pos = w.begin;
    for(node_id c: kids) {
      span cw = db[c].where;
      if(cw.begin < pos) continue;
      out += src.substr(pos, cw.begin - pos);
      verbatim(c, src);
      pos = cw.end;
    }
    out += src.substr(pos, w.end - pos);
  }

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
