// Editor server: VM is a single state, editors are clients (§13.1, M5).
//
// 
// 
// Connections - Unix socket, events - stream from io_uring (multishot
// accept/recv, like the network in M3):
//
#pragma once

#include "proto.hpp"
#include "complete.hpp"
#include "merge.hpp"
#include "parser.hpp"
#include "session.hpp"

#include <ranges>

namespace tz {

// Place of the cursor in projection: node, a piece of text, offset in code points.
struct caret {
  node_id node;
  uint32_t piece;
  uint32_t offset;
};

// Module representation: projection layout and cursor. The cursor is in VM, the
// client only draws.
struct editor_view {
  uint32_t id;
  node_id root;
  std::vector<node_layout> layouts;
  std::vector<caret> places; // cursor
  std::string lead; // the first byte of the symbol in each place ('\n' at the end)
  std::string text; // projection
  std::vector<uint32_t> byte_at; // byte
  std::vector<std::pair<uint32_t, uint32_t>> range; // places node [beginning, end] - by layout number
  std::vector<uint32_t> at; // node_id layout number (~0 is not in projection)
  uint32_t cursor = 0;
  // Supplement: is open, options selected, the beginning of the typed word
  bool completing = false;
  std::vector<completion_item> items;
  uint32_t selected = 0;
  uint32_t word_begin = 0;
  std::string kind = "module"; // Module - Module without internal elements; fn - one function
  std::string layer; // Presentation Layer: Key instance, "all contexts"
  // transition list (F12, Ctrl+P): by item - function or signature "add"
  struct nav_target {
    node_id fn;
    std::string add;
  };
  std::vector<nav_target> targets; // not empty - open list - transition, not addition

  uint32_t layout_of(node_id n) const { return n.value < at.size() ? at[n.value] : ~uint32_t{0}; }
  std::string_view text_of(std::pair<uint32_t, uint32_t> r) const {
    return std::string_view(text).substr(byte_at[r.first], byte_at[r.second] - byte_at[r.first]);
  }
};

// The length of the UTF-8 code point per byte k.
std::size_t cp_len(std::string_view s, std::size_t k) {
  std::size_t n = 1;
  while(k + n < s.size() && (static_cast<unsigned char>(s[k + n]) & 0xC0) == 0x80) ++n;
  return n;
}

// The place of `at` (in code points) of the `from` text is in the `to` text, which is
// distinguished by spaces between lexemes (canonical projection after parsing): matching
// characters go together, unnecessary spaces on either side are missed.
uint32_t align(std::string_view from, uint32_t at, std::string_view to) {
  auto space = [](char c) { return c == ' ' || c == '\n' || c == '\t'; };
  std::size_t i = 0, j = 0;
  uint32_t ci = 0, cj = 0;
  while(ci < at && i < from.size()) {
    std::size_t ni = cp_len(from, i), nj = j < to.size() ? cp_len(to, j) : 0;
    if(nj && space(from[i]) && space(to[j])) {
      // space gap - completely against the gap: the cursor inside it rises after as
      // many translations of the line (Enter at the beginning of the line leaves the
      // cursor on the line of code, at the end - translates to a new one)
      std::size_t ie = i, je = j;
      uint32_t cie = ci, cje = cj, lines = 0, col = 0; // to the cursor: translations of the line and column after the last
      while(ie < from.size() && space(from[ie])) {
        if(cie < at) {
          col = from[ie] == '\n' ? 0 : col + 1;
          lines += from[ie] == '\n';
        }
        ++ie, ++cie;
      }
      while(je < to.size() && space(to[je])) ++je, ++cje;
      if(at < cie) {
        for(uint32_t seen = 0; j < je && seen < lines; ++j, ++cj) seen += to[j] == '\n';
        for(; col > 0 && j < je && to[j] != '\n'; --col) ++j, ++cj;
        return cj;
      }
      i = ie, ci = cie, j = je, cj = cje;
    } else if(nj && from.substr(i, ni) == to.substr(j, nj)) {
      i += ni, j += nj, ++ci, ++cj;
    } else if(space(from[i])) {
      i += ni, ++ci;
    } else if(nj && space(to[j])) {
      j += nj, ++cj;
    } else { // Divergence (literal written differently) - go together
      i += ni, ++ci;
      if(nj) j += nj, ++cj;
    }
  }
  return cj;
}

uint32_t code_points(std::string_view s) {
  uint32_t n = 0;
  for(char c: s)
    if((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
  return n;
}

struct editor_server {
  enum op : uint64_t { op_accept = 1, op_recv, op_send };

  struct peer {
    int fd = -1;
    bool open = true;
    std::string in;
    std::deque<std::string> out;
    std::size_t sent = 0;
    bool sending = false;
    bool recv_armed = false;
    uint32_t handled = 0; // cast
    bool debugging = false; // Launch track open: step keys interpret VM
    uint32_t trace_pos = 0;
    std::deque<editor_view> views;
  };

  explicit editor_server(vm_session & session) : session(session) {
    if(!ring.ok) throw diagnostic("io_uring недоступен", {});
  }

  void listen(std::string const & path) {
    listen_fd = os::listen_unix(path);
    if(listen_fd < 0) throw diagnostic(cat("сокет ", path, ": ", std::strerror(errno)), {});
    ring.accept_all(listen_fd, tag(op_accept, 0));
  }

  peer & add_peer(int fd) {
    peers.push_back({});
    peers.back().fd = fd;
    arm_recv(peers.size() - 1);
    return peers.back();
  }

  // Wait for the core events and process them.
  void step() {
    for(auto const & c: ring.wait()) complete(c);
  }

  // Everything the customer sent was processed and sent.
  bool settled(peer const & p) const { return !p.sending && p.out.empty(); }

  static uint64_t tag(op o, std::size_t i) { return (uint64_t(o) << 56) | i; }

  void arm_recv(std::size_t i) {
    peers[i].recv_armed = true;
    ring.recv_all(peers[i].fd, tag(op_recv, i));
  }

  void complete(os::uring::completion const & c) {
    if(!c.tag) return;
    op o = op(c.tag >> 56);
    std::size_t i = c.tag & 0xFFFFFFFFFFFFFF;
    bool more = c.flags & IORING_CQE_F_MORE;
    switch(o) {
      case op_accept:
        if(c.res >= 0) add_peer(c.res);
        if(!more) ring.accept_all(listen_fd, tag(op_accept, 0));
        break;
      case op_recv: {
        peer & p = peers[i];
        if(!more) p.recv_armed = false;
        if(c.res > 0) {
          unsigned bid = c.flags >> IORING_CQE_BUFFER_SHIFT;
          p.in.append(ring.buffer(bid), std::size_t(c.res));
          ring.give_back(bid);
          proto::take_frames(p.in, [&](std::string_view body) { on_frame(p, body); });
          if(!more && p.open) arm_recv(i);
        } else if(c.res == -ENOBUFS) {
          if(!more && p.open) arm_recv(i);
        } else if(!more) close_peer(p);
        break;
      }
      case op_send: {
        peer & p = peers[i];
        p.sending = false;
        if(c.res < 0) {
          close_peer(p);
          break;
        }
        p.sent += std::size_t(c.res);
        if(p.sent >= p.out.front().size()) {
          p.out.pop_front();
          p.sent = 0;
        }
        send_next(i);
        break;
      }
    }
  }

  void close_peer(peer & p) {
    if(!p.open) return;
    p.open = false;
    p.out.clear();
    os::close_fd(p.fd);
  }

  void send(peer & p, proto::server_msg const & m) {
    if(!p.open) return;
    p.out.push_back(proto::frame(m));
    send_next(index_of(p));
  }
  std::size_t index_of(peer const & p) const {
    for(std::size_t i = 0; i < peers.size(); ++i)
      if(&peers[i] == &p) return i;
    return 0;
  }
  void send_next(std::size_t i) {
    peer & p = peers[i];
    if(p.sending || p.out.empty() || !p.open) return;
    p.sending = true;
    ring.send(p.fd, p.out.front().data() + p.sent, p.out.front().size() - p.sent, tag(op_send, i));
  }

  void on_frame(peer & p, std::string_view body) {
    ++p.handled;
    proto::client_msg m;
    try {
      proto::decode(body, m);
      std::visit([&](auto const & x) { handle(p, x); }, m);
    } catch(diagnostic const & d) {
      send(p, proto::s_error{d.what()});
    }
  }

  // --- message ---

  void handle(peer & p, proto::c_hello const & m) {
    if(m.version != proto::version) {
      send(p, proto::s_error{cat("версия протокола ", m.version, " не поддерживается (VM: ", proto::version, ")")});
      return;
    }
    send(p, proto::s_hello{proto::version, session.name, session.db.revision});
  }

  void handle(peer & p, proto::c_open_view const & m) {
    if(m.kind != "module") {
      send(p, proto::s_error{cat("неизвестный вид представления: ", m.kind)});
      return;
    }
    editor_view & v = p.views.emplace_back();
    v.id = uint32_t(p.views.size());
    v.root = session.root;
    show_view(p, v);
  }

  void show_view(peer & p, editor_view & v, bool relayout = true) {
    if(relayout) layout(v);
    proto::s_view out{v.id, v.root, session.db.revision, {}, v.kind};
    for(auto const & l: v.layouts) out.nodes.push_back(wire_node(l));
    send(p, out);
    send_cursor(p, v);
    recheck();
    send_marks(p, v);
  }

  void handle(peer & p, proto::c_close_view const & m) {
    if(editor_view * v = find(p, m.view)) v->root = {};
  }

  // The keys interpret the view: with an open addition - a list (arrows, Enter -
  // accept, Escape - close), otherwise the movement of the cursor and input.
  void handle(peer & p, proto::c_key const & m) {
    editor_view * v = find(p, m.view);
    if(!v) return send(p, proto::s_error{cat("нет представления ", m.view)});
    if(p.debugging && debug_key(p, *v, m)) return;
    if(v->completing && !v->items.empty()) {
      if(m.key == "ArrowDown" || m.key == "ArrowUp") {
        uint32_t n = uint32_t(v->items.size());
        v->selected = (v->selected + (m.key == "ArrowDown" ? 1 : n - 1)) % n;
        return send_completion(p, *v);
      }
      if(m.key == "Enter" && !v->targets.empty()) { // Transition list: Open or add
        auto t = v->targets[v->selected];
        close_completion(p, *v);
        return t.fn ? open_fn(p, t.fn) : add_overload(p, t.add);
      }
      if(m.key == "Enter") { // Accept: the typed word is replaced by the selected name
        std::string name = v->items[v->selected].name;
        close_completion(p, *v);
        return edit(*v, v->word_begin, v->cursor, name);
      }
      if(m.key == "Escape") return close_completion(p, *v);
    }
    if(m.key == " " && (m.mods & mod_ctrl)) return complete(p, *v, true);
    if(m.key == "F12") return goto_call(p, *v);
    if(m.key == "ArrowRight") {
      if(v->cursor + 1 < v->places.size()) ++v->cursor;
    } else if(m.key == "ArrowLeft") {
      if(v->cursor > 0) --v->cursor;
    } else if(m.key == "Home" || m.key == "End") {
      v->cursor = line_edge(*v, m.key == "End");
    } else if(m.key == "Escape") {
    } else if(m.key == "Backspace" && v->cursor > 0) {
      // indentation (only spaces to the left) - erase the translation of the line as
      // a whole: the empty line disappears, the line joins the previous one
      uint32_t s = v->cursor;
      while(s > 0 && v->lead[s - 1] == ' ') --s;
      bool indent = s > 0 && v->lead[s - 1] == '\n';
      edit(*v, indent ? s - 1 : v->cursor - 1, v->cursor, "");
      return complete(p, *v, false);
    } else if(m.key == "Delete" && v->cursor + 1 < v->places.size()) {
      close_completion(p, *v);
      return edit(*v, v->cursor, v->cursor + 1, "");
    } else if(m.key == "Backspace" || m.key == "Delete") { // edge of the text - nothing
    } else if(m.key == "Enter") {
      // new line, as in all editors (author's decision): empty line - layout in AST
      // (blank); in an empty block `{}` - two translations
      bool empty_block = v->cursor > 0 && v->lead[v->cursor - 1] == '{' && v->lead[v->cursor] == '}';
      close_completion(p, *v);
      return edit(*v, v->cursor, v->cursor, empty_block ? "\n\n" : "\n", 1);
    } else if(code_points(m.key) == 1 && !(m.mods & (mod_ctrl | mod_alt | mod_meta))) {
      edit(*v, v->cursor, v->cursor, m.key);
      return complete(p, *v, false);
    } else return send(p, proto::s_error{cat("клавиша не обработана: ", m.key)});
    close_completion(p, *v);
    send_cursor(p, *v);
  }

  // --- complement ---

  static bool ident_char(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

  // Live addition to the word in front of the cursor; forced (Ctrl + Space) - and without a word.
  void complete(peer & p, editor_view & v, bool forced) {
    uint32_t b = v.cursor;
    while(b > 0 && ident_char(v.lead[b - 1])) --b;
    std::string word;
    for(uint32_t k = b; k < v.cursor; ++k) word += v.lead[k];
    bool number = !word.empty() && word[0] >= '0' && word[0] <= '9';
    uint32_t place = b < v.cursor ? b : (v.cursor > 0 ? v.cursor - 1 : v.cursor);
    node_id at = v.places.empty() ? node_id{} : v.places[place].node;
    if(number || session.db.is<n_str>(at)) return close_completion(p, v);
    // after `.` - members of the value on the left (by its types in all instance);
    // the list opens immediately at the point
    bool dot = b > 0 && v.lead[b - 1] == '.';
    if(!dot && !forced && word.empty()) return close_completion(p, v);
    std::vector<completion_item> all;
    if(dot) {
      auto types = object_types(v, b - 1);
      if(types.empty()) return close_completion(p, v);
      all = member_completions(session.db, session.root, types);
      add_dot_functions(session.db, session.root, types, all, [&](node_id f) {
        auto const & fn = session.db.as<n_fn>(f);
        return types_of_name(f, session.db.as<n_param>(fn.params[0]).name);
      });
    } else all = context_identifiers(session.db, at);
    auto items = filter_completions(std::move(all), word);
    // The only option that matches the typed one is to show nothing.
    if(items.empty() || (items.size() == 1 && items[0].name == word)) return close_completion(p, v);
    v.items = std::move(items);
    v.selected = 0;
    v.word_begin = b;
    v.completing = true;
    send_completion(p, v);
  }

  // Types of value to the left of the point at the place `dot`: node `a.b` is its object;
  // otherwise (text still hole: `xs.`) is the name before the point, its types in the region.
  std::vector<std::string> object_types(editor_view const & v, uint32_t dot) {
    auto const & db = session.db;
    for(node_id n = v.places[dot].node; n; n = db[n].parent)
      if(db.is_member(n)) {
        auto it = session.node_types.find(db.member_of(n).object.value);
        return it == session.node_types.end() ? std::vector<std::string>{} : it->second;
      }
    uint32_t k = dot;
    while(k > 0 && ident_char(v.lead[k - 1])) --k;
    std::string name;
    for(uint32_t i = k; i < dot; ++i) name += v.lead[i];
    auto it = db.strings.index.find(name);
    if(name.empty() || it == db.strings.index.end()) return {};
    return types_of_name(scope_of(v.places[dot].node), it->second);
  }

  void close_completion(peer & p, editor_view & v) {
    if(!v.completing) return;
    v.completing = false;
    v.items.clear();
    v.targets.clear();
    send_completion(p, v);
  }

  void send_completion(peer & p, editor_view const & v) {
    caret c = v.word_begin < v.places.size() ? v.places[v.word_begin] : caret{};
    proto::s_complete out{v.id, c.node, c.piece, c.offset, v.selected, {}};
    for(auto const & x: v.items) out.items.push_back({x.name, x.kind, x.detail});
    send(p, out);
  }

  static constexpr uint32_t mod_ctrl = 2, mod_alt = 4, mod_meta = 8;

  // --- Introduction: editing the text projection → over parsing brackets ---
  //
  // Edit places [a, b) on `ins`. The unit above parsing is the smallest brackets around
  // the edit: block, then module element, then module.
// 
//  
// If the brackets of the unit itself are broken, external ones are taken. Anyone.
  // The text is disassembled: unwritten - holes
// ibid. The unit node retains
  // projection is immediately canonical (author's decision, 2026-09-29).
  struct unit {
    node_id node;
    int kind; // 0 - block, 1 - module element, 2 - module
    std::pair<uint32_t, uint32_t> range;
  };

  std::vector<unit> units_around(editor_view const & v, uint32_t a, uint32_t b) const {
    std::vector<unit> out;
    for(uint32_t k = 0; k < v.layouts.size(); ++k) {
      node_id n = v.layouts[k].node;
      auto r = v.range[k];
      if(session.db.is<n_block>(n) && r.second - r.first >= 2 && a >= r.first + 1 && b <= r.second - 1) out.push_back({n, 0, r});
    }
    std::ranges::sort(out, {}, [](unit const & u) { return u.range.second - u.range.first; });
    for(node_id item: session.db.as<n_module>(session.root).items) {
      uint32_t k = v.layout_of(item);
      if(k != ~uint32_t{0} && a >= v.range[k].first && b <= v.range[k].second) out.push_back({item, 1, v.range[k]});
    }
    if(v.kind == "module") out.push_back({session.root, 2, v.range[0]});
    return out;
  }

  // caret - where the cursor of the author will stand inside the inserted (in code
  // points; by default - after it).
  void edit(editor_view & v, uint32_t a, uint32_t b, std::string const & ins, uint32_t caret = ~uint32_t{0}) {
    ++edits;
    caret = std::min(caret, code_points(ins));
    if(edit_line(v, a, b, ins, caret)) return;
    for(auto const & u: units_around(v, a, b)) {
      std::string_view old = v.text_of(u.range);
      uint32_t base = v.byte_at[u.range.first];
      std::string edited = cat(old.substr(0, v.byte_at[a] - base), ins, old.substr(v.byte_at[b] - base));
      parser pr(session.db, edited, cat("<ввод #", edits, ">"));
      node_id fresh = u.kind == 2 ? pr.parse_module() : pr.parse_unit(u.kind == 0);
      if(!fresh || (u.kind == 0 && !session.db.is<n_block>(fresh))) continue;
      if(u.kind != 2) { // f(…) is a reference to the module function
        node_list items = session.db.as<n_module>(session.root).items;
        pr.resolve_refs(items);
      }
      node_id target = u.node;
      auto range = [&](editor_view const & w) { return w.range[w.layout_of(target)]; };
      auto mutate = [&] {
        node_kind k = session.db[fresh].kind;
        if(target == session.root) { // Module from projection - internal elements are saved
          auto & nm = std::get<n_module>(k);
          for(node_id x: inner_items()) nm.items.push_back(x);
          nm.inner = session.db.as<n_module>(session.root).inner;
        }
        session.db.replace(target, std::move(k));
      };
      apply(v, target, mutate, range, range, edited, {a - u.range.first, b - u.range.first, code_points(ins)}, caret);
      return;
    }
    // representation of function: text ceases to be a function-hole (the same node) until
    // it becomes again; the module from this projection is not redefined
    if(v.kind == "fn") {
      auto r = v.range[0];
      std::string_view old = v.text_of(r);
      std::string edited = cat(old.substr(0, v.byte_at[a] - v.byte_at[r.first]), ins, old.substr(v.byte_at[b] - v.byte_at[r.first]));
      node_id target = v.root;
      auto range = [&](editor_view const & w) { return w.range[w.layout_of(target)]; };
      apply(v, target, [&] { session.db.replace(target, n_hole{edited, "не функция"}); }, range, range, edited,
          {a - r.first, b - r.first, code_points(ins)}, caret);
    }
  }

  // --- line of set: instruction - its unit ---
  //
  // Author's decision (2026-09-29): a string is like a new block, not yet
  // disassembled. Editing within a block instruction (or module element) only
  // re-examines its text; typing on an empty line or after `;` creates a new
  // element in that place. The unwritten is only the hole of this line. Broken
  // line brackets - to outer brackets.

  // Place container: the smallest block, inside the brackets of which is edited, otherwise the module.
  node_id container_of(editor_view const & v, uint32_t a, uint32_t b) const {
    for(auto const & u: units_around(v, a, b))
      if(u.kind == 0) return u.node;
    return session.root;
  }

  // Container elements in projection: the module has no internal (they are at the end of items).
  node_list items_of(ast_db const & db, node_id c) const {
    if(db.is<n_block>(c)) return db.as<n_block>(c).items;
    node_list out;
    for(node_id n: db.as<n_module>(c).items)
      if(!is_inner(n)) out.push_back(n);
    return out;
  }
  node_list inner_items() const {
    node_list out;
    for(uint32_t n: session.db.as<n_module>(session.root).inner) out.push_back(node_id(n));
    return out;
  }

  // Places of container elements [from, from + count]: their text; count = = 0 is the
  // insert point `at`.
  std::pair<uint32_t, uint32_t> items_range(editor_view const & w, node_id c, uint32_t from, uint32_t count, uint32_t at) const {
    auto const items = items_of(session.db, c);
    if(count == 0) return {at, at};
    return {w.range[w.layout_of(items[from])].first, w.range[w.layout_of(items[from + count - 1])].second};
  }

  bool edit_line(editor_view & v, uint32_t a, uint32_t b, std::string const & ins, uint32_t caret) {
    auto & db = session.db;
    node_id c = container_of(v, a, b);
    bool module_level = c == session.root;
    if(module_level && v.kind != "module") return false; // header of a function in its representation - through a unit-function
    node_list const items = items_of(db, c);
    // element whose text is edited (end inclusive: set before `;`)
    uint32_t k = 0, count = 0;
    for(; k < items.size(); ++k) {
      uint32_t l = v.layout_of(items[k]);
      if(l == ~uint32_t{0}) continue;
      if(a >= v.range[l].first && b <= v.range[l].second) {
        count = 1;
        break;
      }
      if(v.range[l].first >= b) break; // In the interval before the element k
    }
    bool blank_text = std::ranges::all_of(ins, [](char ch) { return ch == ' ' || ch == '\n' || ch == '\t'; });
    if(count == 0 && (a != b || blank_text)) return false; // erasure and gaps in the gap - through brackets
    auto old = items_range(v, c, k, count, a);
    std::string_view was = v.text_of(old);
    uint32_t base = v.byte_at[old.first];
    std::string edited = count ? cat(was.substr(0, v.byte_at[a] - base), ins, was.substr(v.byte_at[b] - base)) : ins;
    parser pr(db, edited, cat("<ввод #", edits, ">"));
    bool value = false;
    auto fresh = pr.parse_line(module_level, value);
    if(!fresh) return false;
    if(module_level) { // & f(…) - a reference to the module function (and the one just typed)
      node_list items = db.as<n_module>(session.root).items;
      items.insert(items.end(), fresh->begin(), fresh->end());
      pr.resolve_refs(items);
    }
    // the line has broken up into elements not separated by `;` or `}` (`f() => n n`
    // after erasing `*`) - it is not yet written: the hole is whole, one line
    for(std::size_t i = 0; i + 1 < fresh->size(); ++i) {
      span w = db[(*fresh)[i]].where;
      std::size_t e = std::min<std::size_t>(w.end, edited.size()), after = e;
      while(e > 0 && (edited[e - 1] == ' ' || edited[e - 1] == '\n')) --e;
      while(after < edited.size() && (edited[after] == ' ' || edited[after] == '\n')) ++after;
      bool closed = (e > 0 && (edited[e - 1] == ';' || edited[e - 1] == '}')) || (after < edited.size() && edited[after] == ';');
      if(!closed) {
        fresh = node_list{db.add(n_hole{edited, "строка не дописана"}, {0, uint32_t(edited.size())})};
        value = false;
        break;
      }
    }
    // Empty space lines when inserted: above and below the new element
    std::vector<uint32_t> blank = db.is<n_block>(c) ? db.as<n_block>(c).blank : db.as<n_module>(c).blank;
    blank.resize(items.size() + 1, 0);
    uint32_t canon = module_level ? 2 : 1;
    if(!count && !fresh->empty()) {
      uint32_t prev_end = k > 0 ? v.range[v.layout_of(items[k - 1])].second : (module_level ? 0 : v.range[v.layout_of(c)].first + 1);
      uint32_t next_start = k < items.size() ? v.range[v.layout_of(items[k])].first : (module_level ? uint32_t(v.places.size() - 1) : v.range[v.layout_of(c)].second - 1);
      uint32_t above = 0, below = 0;
      for(uint32_t x = prev_end; x < a; ++x) above += v.lead[x] == '\n';
      for(uint32_t x = a; x < next_start; ++x) below += v.lead[x] == '\n';
      blank[k] = above > canon ? above - canon : 0;
      blank.insert(blank.begin() + k + 1, below > canon ? below - canon : 0);
      for(uint32_t n = 1; n < fresh->size(); ++n) blank.insert(blank.begin() + k + 1, 0);
    } else if(count) {
      for(uint32_t n = 1; n < fresh->size(); ++n) blank.insert(blank.begin() + k + 1, 0);
      if(fresh->empty()) blank.erase(blank.begin() + k + 1);
    }
    node_list list(items.begin(), items.begin() + k);
    node_list added = *fresh;
    if(count && !added.empty()) added[0] = items[k]; // id of the control element is retained
    list.insert(list.end(), added.begin(), added.end());
    list.insert(list.end(), items.begin() + k + count, items.end());
    bool tail = k + count == items.size();
    // unwritten (hole) - not yet decided whether the meaning: the flag of the block does not change
    bool now_hole = fresh->size() == 1 && db.is<n_hole>((*fresh)[0]);
    node_id first_fresh = fresh->empty() ? node_id{} : (*fresh)[0];
    uint32_t n_new = uint32_t(added.size());
    auto mutate = [&] {
      if(count && first_fresh) db.replace(items[k], db[first_fresh].kind);
      if(db.is<n_block>(c)) {
        n_block nb = db.as<n_block>(c);
        nb.items = list;
        nb.blank = blank;
        parser::trim_blank(nb.blank);
        // element text - without `;` (it is printed by the block): editing the element
        // does not make the instruction a value; the value is removed by typed `;`
        if(tail && !now_hole) nb.has_value = n_new > 0 && value && (count == 0 || nb.has_value);
        db.replace(c, nb);
      } else {
        n_module nm = db.as<n_module>(c);
        nm.items = list;
        for(node_id x: inner_items()) nm.items.push_back(x); // internal, at the end, as they were
        nm.blank = blank;
        parser::trim_blank(nm.blank);
        db.replace(c, nm);
      }
    };
    auto before = [&](editor_view const & w) { return items_range(w, c, k, count, a); };
    auto after = [&](editor_view const & w) {
      if(n_new) return items_range(w, c, k, n_new, 0);
      // Element removed: place - after the previous or immediately after `{` block
      uint32_t at = k > 0 ? w.range[w.layout_of(items_of(db, c)[k - 1])].second : w.range[w.layout_of(c)].first + (db.is<n_block>(c) ? 1 : 0);
      return std::pair{at, at};
    };
    apply(v, c, mutate, before, after, edited, {count ? a - old.first : 0, count ? b - old.first : 0, code_points(ins)}, caret);
    return true;
  }

  // Editing within the unit (in places from its beginning): [from, to] replaced by
  // inserted code points.
  struct change {
    uint32_t from, to, inserted;
    // place before editing → place in the text after editing (up to parsing)
    uint32_t map(uint32_t c) const { return c >= to ? c - (to - from) + inserted : c > from ? from : c; }
  };

  // Change the base (mutate) and send the new projection node target to all
  // submissions of all clients; each cursor is transferred through the alignment
  // of the texts of the changed place (range_before / range_after).
  void apply(editor_view & av, node_id target, auto && mutate, auto && range_before, auto && range_after,
      std::string const & edited, change ch, uint32_t caret) {
    struct before {
      editor_view * v;
      peer * p;
      std::pair<uint32_t, uint32_t> r;
      std::string tail; // text after the changed place - alignment goes through it
    };
    std::vector<before> views;
    for(auto & p: peers)
      for(auto & v: p.views)
        if(v.root && v.layout_of(target) != ~uint32_t{0}) {
          auto r = range_before(v);
          views.push_back({&v, &p, r, v.text.substr(v.byte_at[r.second])});
        }
    mutate();
    session.db.link_parents(session.root);
    // Edit - the track describes the previous version: close it all
    for(auto & q: peers)
      if(q.debugging) {
        q.debugging = false;
        send(q, proto::s_trace{false, 0, 0, {}, {}, {}, {}});
      }
    for(auto & b: views) {
      editor_view & v = *b.v;
      uint32_t c = v.cursor;
      layout(v);
      auto r = range_after(v);
      // was: correct place text + everything after it; became: projection from the beginning of the place
      std::string was = edited + b.tail;
      std::string_view now = std::string_view(v.text).substr(v.byte_at[r.first]);
      // cursor of the author - immediately after entering; someone else - is moved by
      // editing; then alignment with the canonical projection (she puts gaps)
      if(&v == &av) c = blank_line_end(v, r.first + align(was, ch.from + caret, now));
      else if(c >= b.r.first) c = r.first + align(was, ch.map(c - b.r.first), now);
      v.cursor = std::min<uint32_t>(c, uint32_t(v.places.size() - 1));
      send(*b.p, proto::s_update{v.id, session.db.revision, target, subtree(v, target)});
      // f(…) { … } prints the name and signature of the target: editing the target changes its projection
      for(node_id e: fn_extensions())
        if(v.layout_of(e) != ~uint32_t{0} && within(node_id(session.db.as<n_extend>(e).target), target) && !within(e, target))
          send(*b.p, proto::s_update{v.id, session.db.revision, e, subtree(v, e)});
      if(&v != &av) close_completion(*b.p, v); // crossword
      send_cursor(*b.p, v);
    }
    recheck();
    for(auto & b: views) send_marks(*b.p, *b.v);
  }

  // Extensions of functions by reference in the module.
  std::vector<node_id> fn_extensions() const {
    std::vector<node_id> out;
    for(node_id x: session.db.as<n_module>(session.root).items)
      if(session.db.is<n_extend>(x) && session.db.as<n_extend>(x).target) out.push_back(x);
    return out;
  }
  bool within(node_id x, node_id top) const {
    for(; x; x = session.db[x].parent)
      if(x == top) return true;
    return false;
  }

  // The cursor on a line of one space (empty line with indentation) - at the end of
  // the indentation: the set will begin in the code column.
  static uint32_t blank_line_end(editor_view const & v, uint32_t c) {
    if(c >= v.lead.size()) return c;
    uint32_t s = c, e = c;
    while(s > 0 && v.lead[s - 1] != '\n') --s;
    while(e + 1 < v.lead.size() && v.lead[e] != '\n') ++e;
    for(uint32_t k = s; k < e; ++k)
      if(v.lead[k] != ' ') return c;
    return e;
  }

  std::vector<proto::s_node> subtree(editor_view const & v, node_id n) const {
    std::vector<proto::s_node> out;
    auto rec = [&](auto && self, node_id x) -> void {
      uint32_t k = v.layout_of(x);
      if(k == ~uint32_t{0}) return;
      out.push_back(wire_node(v.layouts[k]));
      for(auto const & pc: v.layouts[k].pieces)
        if(auto const * c = std::get_if<node_id>(&pc)) self(self, *c);
    };
    rec(rec, n);
    return out;
  }

  proto::s_node wire_node(node_layout const & l) const {
    std::string note;
    if(session.db.is<n_hole>(l.node)) note = session.db.as<n_hole>(l.node).reason;
    return {l.node, std::string(kind_name(session.db[l.node])), l.pieces, note};
  }

  void handle(peer & p, proto::c_pointer const & m) {
    editor_view * v = find(p, m.view);
    if(!v) return send(p, proto::s_error{cat("нет представления ", m.view)});
    // The end of the piece is the place after its last symbol.
    for(uint32_t k = 0; k < v->places.size(); ++k) {
      caret const & c = v->places[k];
      if(c.node != m.node || c.piece != m.piece) continue;
      if(c.offset == m.offset || (m.offset && c.offset + 1 == m.offset && (k + 1 == v->places.size() || v->places[k + 1].node != c.node || v->places[k + 1].piece != c.piece))) {
        v->cursor = c.offset == m.offset ? k : k + 1;
        close_completion(p, *v);
        return send_cursor(p, *v);
      }
    }
    send(p, proto::s_error{cat("нет места #", m.node.value, "/", m.piece, "/", m.offset)});
  }

  // --- Checking tags: Types and errors in place ---
  //
  // After each revision, a reactive check session (only affected instance, §10.1 are
  // counted); node types are counted across all instance contexts. As in KDevelop; an
  // error is counted at a place of use with a instantiation (§13.4) track.
  //

  uint64_t checked_revision = ~uint64_t{0};

  void recheck() {
    if(checked_revision == session.db.revision) return;
    session.check();
    checked_revision = session.db.revision;
  }

  // Name types in the area: all its mentions in the subtree (without nested
  // functions) and initializers `name = e`.
  std::vector<std::string> types_of_name(node_id scope, symbol name) const {
    std::vector<std::string> out;
    auto take = [&](node_id n) {
      if(auto it = session.node_types.find(n.value); it != session.node_types.end())
        for(auto const & t: it->second)
          if(!std::ranges::contains(out, t)) out.push_back(t);
    };
    auto rec = [&](auto && self, node_id n) -> void {
      auto const & db = session.db;
      if(db.is<n_ident>(n) && db.as<n_ident>(n).name == name) take(n);
      if(db.is<n_op>(n) && db.strings.text(db.as<n_op>(n).op) == "=" && db.as<n_op>(n).args.size() == 2 && db.is<n_ident>(db.as<n_op>(n).args[0]) && db.as<n_ident>(db.as<n_op>(n).args[0]).name == name)
        take(db.as<n_op>(n).args[1]);
      if(db.is<n_var>(n) && db.as<n_var>(n).name == name && db.as<n_var>(n).init) take(db.as<n_var>(n).init);
      db.for_each_child(n, [&](node_id c) {
        if(!db.is<n_fn>(c)) self(self, c);
      });
    };
    rec(rec, scope);
    return out;
  }

  static std::string join_types(std::vector<std::string> ts) {
    std::ranges::sort(ts); // order of contexts - hashes; show by name
    std::string s;
    for(auto const & t: ts) s += (s.empty() ? "" : " | ") + t;
    return s;
  }

  // Name area: Nearest function or block of structure (lambda) up.
  node_id scope_of(node_id n) const {
    for(node_id x = session.db[n].parent; x; x = session.db[x].parent)
      if(session.db.is<n_fn>(x) || session.db.is<n_lambda>(x)) return x;
    return session.root;
  }

  // Node projection, in the text of which lies the place of diagnosis (the smallest).
  node_id node_at(editor_view const & v, span w) const {
    node_id best;
    uint32_t size = ~uint32_t{0};
    for(auto const & l: v.layouts) {
      span const & s = session.db[l.node].where;
      if(s.source == w.source && s.begin <= w.begin && w.end <= s.end && s.end - s.begin < size) {
        best = l.node;
        size = s.end - s.begin;
      }
    }
    return best;
  }

  // Presentation layer: Observations of a function in a selected context (no, all).
  interpreter::layer_info const * layer_of(editor_view const & v) const {
    if(v.kind != "fn" || v.layer.empty()) return nullptr;
    auto fl = session.layers.find(v.root.value);
    if(fl == session.layers.end()) return nullptr;
    auto it = fl->second.find(v.layer);
    return it == fl->second.end() ? nullptr : &it->second;
  }

  std::vector<proto::s_mark> marks(editor_view const & v) const {
    auto const & db = session.db;
    std::vector<proto::s_mark> out;
    if(auto const * l = layer_of(v)) return layer_marks(v, *l);
    for(auto const & l: v.layouts) {
      node_id n = l.node;
      std::vector<std::string> ts;
      node_id at = n;
      if(db.is<n_param>(n)) ts = types_of_name(scope_of(n), db.as<n_param>(n).name);
      else if(db.is<n_var>(n)) ts = types_of_name(scope_of(n), db.as<n_var>(n).name);
      else if(db.is<n_op>(n) && db.strings.text(db.as<n_op>(n).op) == "=" && db.as<n_op>(n).args.size() == 2 && db.is<n_ident>(db.as<n_op>(n).args[0])) {
        // `x = e` announces if the name is not yet available (author's decision: no auto)
        at = db.as<n_op>(n).args[0];
        symbol name = db.as<n_ident>(at).name;
        bool known = false;
        for(auto const & c: context_identifiers(db, n))
          if(c.name == db.strings.text(name) && (c.kind == "переменная" || c.kind == "параметр")) known = true;
        if(known) continue;
        ts = types_of_name(scope_of(n), name);
      } else continue;
      if(!ts.empty()) out.push_back({at, "type", join_types(ts)});
    }
    for(auto const & m: marks_errors(v)) out.push_back(m); // place in prelude - the first place of the program from Trace
    return out;
  }

  // Layer tags: types of ads in this context, known meanings of names that are not executed
  // in it - muffle (the top such node); errors - as everywhere.
  std::vector<proto::s_mark> layer_marks(editor_view const & v, interpreter::layer_info const & l) const {
    auto const & db = session.db;
    std::vector<proto::s_mark> out;
    auto types_in = [&](node_id scope, symbol name) {
      std::vector<std::string> ts;
      auto rec = [&](auto && self, node_id n) -> void {
        if(db.is<n_ident>(n) && db.as<n_ident>(n).name == name)
          if(auto it = l.types.find(n.value); it != l.types.end())
            for(auto const & t: it->second)
              if(!std::ranges::contains(ts, t)) ts.push_back(t);
        db.for_each_child(n, [&](node_id c) {
          if(!db.is<n_fn>(c)) self(self, c);
        });
      };
      rec(rec, scope);
      return ts;
    };
    node_id f = v.root;
    for(auto const & lay: v.layouts) {
      node_id n = lay.node;
      if(db.is<n_param>(n))
        if(auto ts = types_in(f, db.as<n_param>(n).name); !ts.empty()) out.push_back({n, "type", join_types(ts)});
      // meanings: names and conditions of forks (known condition - which branch lives)
      bool cond = db[n].parent && db.is<n_if>(db[n].parent) && db.as<n_if>(db[n].parent).cond == n;
      if((db.is<n_ident>(n) && db[n].parent && !db.is<n_call>(db[n].parent)) || cond)
        if(auto it = l.literal.find(n.value); it != l.literal.end()) out.push_back({n, "value", it->second});
      // not performed in this context: the node of the body without observation, the parent with observation n
      bool body = n != f && enclosing(n) == f && !db.is<n_param>(n) && !(db[n].parent && db.is<n_param>(db[n].parent));
      if(body && !l.types.contains(n.value) && (db[n].parent == f || l.types.contains(db[n].parent.value))) out.push_back({n, "dead", "не исполняется в этом контексте"});
    }
    for(auto const & m: marks_errors(v)) out.push_back(m);
    return out;
  }

  node_id enclosing(node_id n) const {
    for(node_id x = session.db[n].parent; x; x = session.db[x].parent)
      if(session.db.is<n_fn>(x)) return x;
    return {};
  }

  std::vector<proto::s_mark> marks_errors(editor_view const & v) const {
    std::vector<proto::s_mark> out;
    for(auto const & d: session.errors) {
      node_id n = node_at(v, d.where);
      for(auto const & f: d.trace)
        if(!n) n = node_at(v, f.where);
      std::string text = d.what();
      for(auto const & f: d.trace) text += cat("\n  ", f.what);
      if(n) out.push_back({n, "error", text});
    }
    return out;
  }

  void send_marks(peer & p, editor_view const & v) {
    send(p, proto::s_decor{v.id, session.db.revision, marks(v)});
    if(v.kind == "fn") send_layers(p, v);
  }

  // Tabs of the layers of the function: "all" and contexts of its use.
  void send_layers(peer & p, editor_view const & v) {
    proto::s_layers out{v.id, {}, v.layer};
    if(auto fl = session.layers.find(v.root.value); fl != session.layers.end())
      for(auto const & [key, l]: fl->second) out.layers.push_back({key, l.ct ? "кт" : "рт"});
    send(p, out);
  }

  void handle(peer & p, proto::c_layer const & m) {
    editor_view * v = find(p, m.view);
    if(!v || v->kind != "fn") return send(p, proto::s_error{cat("слои — у представления функции")});
    v->layer = m.key;
    send_marks(p, *v);
  }

  // --- transition and "add in place" ---
  //
  // 
// 
// 
// 
// F12 on call - overload selected in
  // instance ("tracing"), all others and "add" for argument types without overload;
  // Ctrl+P is the same for the typed signature. The internal element of the module
  // (inner layout), opens in the second panel.

  // Name overload: Program module (with internal) and prelude.
  std::vector<node_id> overloads(std::string_view name) const {
    std::vector<node_id> out;
    for(node_id m: {session.root, session.db.prelude}) {
      if(!m || !session.db.is<n_module>(m)) continue;
      for(node_id f: session.db.as<n_module>(m).items)
        if(session.db.is<n_fn>(f) && session.db.strings.text(session.db.as<n_fn>(f).name) == name) out.push_back(f);
    }
    return out;
  }

  std::string signature(node_id f) const {
    auto const & fn = session.db.as<n_fn>(f);
    std::string s = cat(session.db.strings.text(fn.name), "(");
    for(uint32_t i = 0; i < fn.params.size(); ++i) {
      auto const & p = session.db.as<n_param>(fn.params[i]);
      if(i) s += ", ";
      if(p.type && session.db.is<n_ident>(p.type)) s += cat(session.db.strings.text(session.db.as<n_ident>(p.type).name), " ");
      s += session.db.strings.text(p.name);
    }
    return s + ")";
  }

  // overload is suitable for types (parameter without type - any).
  bool accepts(node_id f, std::vector<std::string> const & types) const {
    auto const & fn = session.db.as<n_fn>(f);
    if(fn.params.size() != types.size()) return false;
    for(uint32_t i = 0; i < types.size(); ++i) {
      auto const & p = session.db.as<n_param>(fn.params[i]);
      if(p.type && session.db.is<n_ident>(p.type) && session.db.strings.text(session.db.as<n_ident>(p.type).name) != types[i].substr(0, types[i].find('<'))) return false;
    }
    return true;
  }
  // Is it precisely typed for these types (all parameters with the type).
  bool exact(node_id f, std::vector<std::string> const & types) const {
    auto const & fn = session.db.as<n_fn>(f);
    if(!accepts(f, types)) return false;
    return std::ranges::all_of(fn.params, [&](node_id p) { return bool(session.db.as<n_param>(p).type); });
  }

  // Transition list: in the cursor location, items - functions and "add".
  void show_nav(peer & p, editor_view & v, std::vector<std::pair<completion_item, editor_view::nav_target>> list) {
    if(list.empty()) return send(p, proto::s_error{"переход: ничего не найдено"});
    v.items.clear();
    v.targets.clear();
    for(auto & [item, t]: list) {
      v.items.push_back(item);
      v.targets.push_back(t);
    }
    v.selected = 0;
    v.word_begin = v.cursor;
    v.completing = true;
    send_completion(p, v);
  }

  static std::string add_text(std::string_view name, std::vector<std::string> const & types) {
    std::string s = cat(name, "(");
    for(uint32_t i = 0; i < types.size(); ++i) s += cat(i ? ", " : "", types[i]);
    return s + ")";
  }

  // F12: Call in the place of the cursor - its overload.
  void goto_call(peer & p, editor_view & v) {
    auto const & db = session.db;
    uint32_t at = v.cursor < v.places.size() ? v.cursor : 0;
    node_id call;
    for(uint32_t k: {at, at > 0 ? at - 1 : at}) {
      for(node_id n = v.places[k].node; n && !call; n = db[n].parent)
        if(db.is<n_call>(n) && (db.is<n_ident>(db.as<n_call>(n).callee) || db.is_member(db.as<n_call>(n).callee))) call = n;
      if(call) break;
    }
    if(!call) return send(p, proto::s_error{"F12: курсор не на вызове функции"});
    auto const & c = db.as<n_call>(call);
    node_list args = c.args;
    symbol name;
    if(db.is<n_ident>(c.callee)) name = db.as<n_ident>(c.callee).name;
    else { // a.f(b) ≡ f(a, b) (§4.5)
      name = db.member_of(c.callee).name;
      args.insert(args.begin(), db.member_of(c.callee).object);
    }
    std::string n(db.strings.text(name));
    auto chosen = session.call_targets.contains(call.value) ? session.call_targets.at(call.value) : std::vector<uint32_t>{};
    std::vector<std::pair<completion_item, editor_view::nav_target>> list;
    auto fns = overloads(n);
    std::ranges::stable_sort(fns, {}, [&](node_id f) { return !std::ranges::contains(chosen, f.value); });
    for(node_id f: fns) {
      std::string detail = std::ranges::contains(chosen, f.value) ? "выбрана здесь" : "";
      if(is_inner(f)) detail += detail.empty() ? "внутренняя" : ", внутренняя";
      if(db[f].where.source == db[db.prelude].where.source) detail += detail.empty() ? "прелюдия" : ", прелюдия";
      list.push_back({{signature(f), "перегрузка", detail, 0}, {f, {}}});
    }
    // Argument types for all instances: combinations without suitable overload - "add"
    std::vector<std::vector<std::string>> combos{{}};
    for(node_id a: args) {
      auto it = session.node_types.find(a.value);
      std::vector<std::string> ts = it == session.node_types.end() ? std::vector<std::string>{} : it->second;
      if(ts.empty()) return show_nav(p, v, std::move(list)); // Type of argument unknown - only transition
      std::vector<std::vector<std::string>> next;
      for(auto const & cmb: combos)
        for(auto const & t: ts)
          if(next.size() < 8) {
            next.push_back(cmb);
            next.back().push_back(t);
          }
      combos = std::move(next);
    }
    for(auto const & cmb: combos)
      if(std::ranges::none_of(fns, [&](node_id f) { return accepts(f, cmb); }))
        list.insert(list.begin(), {{cat("добавить ", add_text(n, cmb)), "добавить", "внутренняя перегрузка", 0}, {{}, add_text(n, cmb)}});
    show_nav(p, v, std::move(list));
  }

  // Ctrl+P: "foo(i64, str)" or "foo" - overload and "add".
  void handle(peer & p, proto::c_goto const & m) {
    editor_view * v = find(p, m.view);
    if(!v) return send(p, proto::s_error{cat("нет представления ", m.view)});
    std::string_view t = m.text;
    while(!t.empty() && t.front() == ' ') t.remove_prefix(1);
    while(!t.empty() && t.back() == ' ') t.remove_suffix(1);
    auto open = t.find('(');
    std::string name(t.substr(0, open));
    while(!name.empty() && name.back() == ' ') name.pop_back();
    if(name.empty()) return send(p, proto::s_error{"переход: нужна сигнатура вида foo(i64)"});
    std::optional<std::vector<std::string>> types;
    if(open != std::string_view::npos) {
      types.emplace();
      std::string_view inside = t.substr(open + 1, t.rfind(')') == std::string_view::npos ? std::string_view::npos : t.rfind(')') - open - 1);
      for(auto part: std::views::split(inside, ',')) {
        std::string s(part.begin(), part.end());
        while(!s.empty() && s.front() == ' ') s.erase(0, 1);
        while(!s.empty() && s.back() == ' ') s.pop_back();
        if(!s.empty()) types->push_back(s);
      }
    }
    std::vector<std::pair<completion_item, editor_view::nav_target>> list;
    auto fns = overloads(name);
    if(types) std::ranges::stable_sort(fns, {}, [&](node_id f) { return exact(f, *types) ? 0 : accepts(f, *types) ? 1 : 2; });
    for(node_id f: fns) {
      std::string detail = types && exact(f, *types) ? "точно" : types && accepts(f, *types) ? "подходит" : "";
      if(is_inner(f)) detail += detail.empty() ? "внутренняя" : ", внутренняя";
      list.push_back({{signature(f), "перегрузка", detail, 0}, {f, {}}});
    }
    if(types && std::ranges::none_of(fns, [&](node_id f) { return exact(f, *types); }))
      list.insert(list.begin(), {{cat("добавить ", add_text(name, *types)), "добавить", "внутренняя перегрузка", 0}, {{}, add_text(name, *types)}});
    show_nav(p, *v, std::move(list));
  }

  // Open the function in the second panel: The client has one representation of the function.
  void open_fn(peer & p, node_id f) {
    editor_view * fv = nullptr;
    for(auto & w: p.views)
      if(w.root && w.kind == "fn") fv = &w;
    if(!fv) {
      fv = &p.views.emplace_back();
      fv->id = uint32_t(p.views.size());
      fv->kind = "fn";
    }
    fv->root = f;
    fv->layer.clear();
    fv->cursor = 0;
    fv->completing = false;
    layout(*fv);
    // cursor to the body: on an empty line, if any, otherwise after `{`
    for(uint32_t k = 0; k + 1 < fv->lead.size(); ++k)
      if(fv->lead[k] == '{') {
        fv->cursor = blank_line_end(*fv, k + 2 < fv->lead.size() ? k + 2 : k + 1);
        break;
      }
    show_view(p, *fv, false);
  }

  // Add the internal overload "foo(i64, str)": parameter names are like overload's of the
  // same aricity, otherwise a, b, c...; the body is an empty string to type.
  void add_overload(peer & p, std::string const & sig) {
    auto & db = session.db;
    auto open = sig.find('(');
    std::string name = sig.substr(0, open);
    std::vector<std::string> types;
    for(auto part: std::views::split(std::string_view(sig).substr(open + 1, sig.size() - open - 2), ',')) {
      std::string s(part.begin(), part.end());
      while(!s.empty() && s.front() == ' ') s.erase(0, 1);
      if(!s.empty()) types.push_back(s);
    }
    std::vector<std::string> names;
    for(node_id f: overloads(name))
      if(db.as<n_fn>(f).params.size() == types.size()) {
        for(node_id pn: db.as<n_fn>(f).params) names.emplace_back(db.strings.text(db.as<n_param>(pn).name));
        break;
      }
    std::string text = cat(name, "(");
    for(uint32_t i = 0; i < types.size(); ++i)
      text += cat(i ? ", " : "", types[i].substr(0, types[i].find('<')), " ", i < names.size() ? names[i] : std::string(1, char('a' + i)));
    text += ") {\n\n}\n";
    ++edits;
    parser pr(db, text, cat("<добавить ", sig, ">"));
    node_id mod = pr.parse_module();
    auto const & items = db.as<n_module>(mod).items;
    if(items.size() != 1 || !db.is<n_fn>(items[0])) return send(p, proto::s_error{cat("не удалось добавить ", sig)});
    node_id f = items[0];
    n_module nm = db.as<n_module>(session.root);
    nm.items.push_back(f);
    nm.inner.push_back(f.value);
    db.replace(session.root, nm);
    db.link_parents(session.root);
    // Module representations: no internal element visible - only labels change
    recheck();
    for(auto & q: peers)
      for(auto & w: q.views)
        if(w.root && w.kind == "module") send_marks(q, w);
    open_fn(p, f);
  }

  // --- start-up ---

  void handle(peer & p, proto::c_run const & m) {
    session.stdin_lines.clear();
    for(auto l: lines_of(m.stdin_text)) session.stdin_lines.emplace_back(l);
    std::vector<std::string> args;
    for(auto w: words_of(m.args)) args.emplace_back(w);
    bool ok = true;
    std::string out = session.run(args, ok);
    send(p, proto::s_output{out, ok});
  }

  // .tzdb next to the program: stats.tz → stats.tzdb §3.1)
  void handle(peer & p, proto::c_save const &) {
    std::string path = session.name;
    if(path.ends_with(".tz")) path += "db";
    else if(!path.ends_with(".tzdb")) path += ".tzdb";
    save_db(session.db, session.root, path);
    send(p, proto::s_saved{path});
  }

  // --- debugger: launch track ---
  //
  // Author's decision: the execution path (§13.2) is recorded at launch, the
  // steps are forward and backward; VM does not stop.
// 
// built-in calls.
  // usual steps
//).

  std::string trace_output, trace_error;

  void handle(peer & p, proto::c_trace const & m) {
    session.stdin_lines.clear();
    for(auto l: lines_of(m.stdin_text)) session.stdin_lines.emplace_back(l);
    std::vector<std::string> args;
    for(auto w: words_of(m.args)) args.emplace_back(w);
    bool ok = true;
    trace_output = session.trace_run(args, ok, trace_error);
    p.debugging = true;
    p.trace_pos = 0;
    step_to(p, next_stmt(0, false, ~uint32_t{0}));
  }

  // Instruction: block element or expression-body function.
  bool is_stmt(uint32_t i) const {
    node_id n(session.trace[i].node);
    node_id up = session.db[n].parent;
    if(session.db.is<n_block>(n)) return false; // body block is not a step, steps are his instructions
    return up && (session.db.is<n_block>(up) || (session.db.is<n_fn>(up) && session.db.as<n_fn>(up).body == n));
  }

  // The following (or previous) instruction is no deeper than max_depth; no, stay.
  uint32_t next_stmt(uint32_t from, bool back, uint32_t max_depth) const {
    auto const & t = session.trace;
    if(t.empty()) return 0;
    if(!back) {
      for(uint32_t i = from; i < t.size(); ++i)
        if(is_stmt(i) && t[i].depth <= max_depth) return i;
    } else {
      for(uint32_t i = from + 1; i-- > 0;)
        if(is_stmt(i) && t[i].depth <= max_depth) return i;
    }
    return std::min<uint32_t>(from, uint32_t(t.size() - 1));
  }

  bool debug_key(peer & p, editor_view & v, proto::c_key const & m) {
    auto const & t = session.trace;
    uint32_t pos = p.trace_pos, depth = t.empty() ? 0 : t[pos].depth;
    bool shift = m.mods & 1;
    if(m.key == "Escape") {
      p.debugging = false;
      send(p, proto::s_trace{false, 0, 0, {}, {}, {}, {}});
      for(auto & w: p.views)
        if(w.root) send_marks(p, w);
      return true;
    }
    if(m.key == "F10") step_to(p, shift ? next_stmt(pos > 0 ? pos - 1 : 0, true, depth) : next_stmt(pos + 1, false, depth));
    else if(m.key == "F11") step_to(p, shift ? next_stmt(pos + 1, false, depth > 0 ? depth - 1 : 0) : next_stmt(pos + 1, false, ~uint32_t{0}));
    else if(m.key == "F4") { // cursor
      node_id at = v.places.empty() ? node_id{} : v.places[v.cursor].node;
      for(; at && session.db[at].parent && !session.db.is<n_block>(session.db[at].parent); at = session.db[at].parent) {}
      for(uint32_t i = pos + 1; i < t.size(); ++i)
        if(t[i].node == at.value) return step_to(p, i), true;
      send(p, proto::s_error{"F4: эта инструкция дальше не исполняется"});
    } else return false;
    return true;
  }

  // extra value icon: literal (value and so in the text), block, name of the called function
  bool quiet_value(node_id n) const {
    auto const & db = session.db;
    if(db.is<n_int>(n) || db.is<n_float>(n) || db.is<n_str>(n) || db.is<n_block>(n)) return true;
    node_id up = db[n].parent;
    return up && db.is<n_call>(up) && db.as<n_call>(up).callee == n;
  }

  void step_to(peer & p, uint32_t pos) {
    auto const & t = session.trace;
    p.trace_pos = pos;
    proto::s_trace out{true, pos, uint32_t(t.size()), {}, {}, {}, trace_error};
    if(t.empty()) {
      out.output = trace_output;
      return send(p, out);
    }
    auto const & e = t[pos];
    out.node = node_id(e.node);
    out.output = trace_output.substr(0, std::min<std::size_t>(e.out, trace_output.size()));
    // Stack: For each depth, a function of the last step of that depth
    for(uint32_t d = e.depth; d >= 1; --d)
      for(uint32_t i = pos + 1; i-- > 0;)
        if(t[i].depth == d) {
          node_id f = enclosing(node_id(t[i].node));
          out.stack.push_back(f ? std::string(session.db.strings.text(session.db.as<n_fn>(f).name)) : std::string("?"));
          break;
        }
    send(p, out);
    // values of subexpressions of the current instruction (same depth) - icons
    uint32_t from = pos;
    while(from > 0 && !(is_stmt(from - 1) && t[from - 1].depth == e.depth)) --from;
    std::map<uint32_t, std::string> values;
    for(uint32_t i = from; i <= pos; ++i)
      if(t[i].depth == e.depth && !t[i].value.empty() && t[i].value != "()") values[t[i].node] = t[i].value;
    for(auto & w: p.views) {
      if(!w.root) continue;
      std::vector<proto::s_mark> marks;
      for(auto const & [n, val]: values)
        if(node_id x(n); w.layout_of(x) != ~uint32_t{0} && !quiet_value(x)) marks.push_back({x, "value", val});
      if(w.layout_of(node_id(e.node)) != ~uint32_t{0}) marks.push_back({node_id(e.node), "current", ""});
      send(p, proto::s_decor{w.id, session.db.revision, marks});
    }
  }

  // --- Version: Merge with another copy of the database (§9.4) ---

  merge_result pending; // conflicts of the last merger before the decision of the person

  void refresh_all() {
    recheck();
    for(auto & q: peers)
      for(auto & w: q.views)
        if(w.root) show_view(q, w);
  }

  void send_merge(peer & p, std::vector<std::string> const & taken) {
    proto::s_merge out{taken, {}};
    for(auto const & c: pending.conflicts) out.conflicts.push_back({c.key, c.base, c.mine, c.theirs});
    send(p, out);
  }

  void handle(peer & p, proto::c_merge const & m) {
    std::string path = m.path;
    if(!path.starts_with('/')) // programme
      if(auto slash = session.name.rfind('/'); slash != std::string::npos) path = session.name.substr(0, slash + 1) + path;
    node_id theirs_root;
    std::vector<journal_entry> theirs;
    try {
      theirs = read_journal(path, theirs_root);
    } catch(diagnostic const & d) {
      return send(p, proto::s_error{cat("слияние: ", d.what())});
    }
    pending = merge_into(session.db, session.root, session.db.journal(), theirs, theirs_root);
    refresh_all();
    send_merge(p, pending.taken);
  }

  void handle(peer & p, proto::c_take const & m) {
    auto it = std::ranges::find(pending.conflicts, m.key, &merge_conflict::key);
    if(it == pending.conflicts.end()) return send(p, proto::s_error{cat("нет конфликта ", m.key)});
    if(m.theirs) {
      take_theirs(session.db, session.root, pending.base_items, it->id, it->theirs_exists ? std::optional<std::string>(it->theirs) : std::nullopt);
      refresh_all();
    }
    pending.conflicts.erase(it);
    send_merge(p, {});
  }

  editor_view * find(peer & p, uint32_t id) {
    for(auto & v: p.views)
      if(v.id == id && v.root) return &v;
    return nullptr;
  }

  void send_cursor(peer & p, editor_view const & v) {
    caret c = v.places.empty() ? caret{} : v.places[v.cursor];
    send(p, proto::s_cursor{v.id, c.node, c.piece, c.offset});
  }

  // --- layout ---

  bool is_inner(node_id n) const { return std::ranges::contains(session.db.as<n_module>(session.root).inner, n.value); }

  void layout(editor_view & v) {
    v.layouts.clear();
    // The presentation of the module does not show the internal elements - "not littering"
    printer pr(session.db, v.kind == "module" ? printer::filter([&](node_id n) { return !is_inner(n); }) : printer::filter{});
    pr.layouts = &v.layouts;
    pr.print(v.root);
    v.at.assign(session.db.size(), ~uint32_t{0});
    for(uint32_t k = 0; k < v.layouts.size(); ++k) v.at[v.layouts[k].node.value] = k;
    v.places.clear();
    v.lead.clear();
    v.text.clear();
    v.byte_at.clear();
    v.range.assign(v.layouts.size(), {});
    caret last{};
    walk(v, 0, last);
    v.places.push_back(last); // texture
    v.lead += '\n';
    v.byte_at.push_back(uint32_t(v.text.size()));
    v.cursor = 0;
  }

  // Places - before each code point; at the junction of pieces there is one place
  // (before the next symbol), the end of the piece is not considered a separate place.
  void walk(editor_view & v, uint32_t k, caret & last) {
    node_layout const & l = v.layouts[k];
    v.range[k].first = uint32_t(v.places.size());
    for(uint32_t i = 0; i < l.pieces.size(); ++i) {
      if(auto const * t = std::get_if<std::string>(&l.pieces[i])) {
        uint32_t n = code_points(*t), o = 0;
        for(std::size_t j = 0; j < t->size(); j += cp_len(*t, j)) {
          v.places.push_back({l.node, i, o++});
          v.lead += (*t)[j];
          v.byte_at.push_back(uint32_t(v.text.size() + j));
        }
        v.text += *t;
        if(n) last = {l.node, i, n};
      } else walk(v, v.at[std::get<node_id>(l.pieces[i]).value], last);
    }
    v.range[k].second = uint32_t(v.places.size());
  }

  uint32_t line_edge(editor_view const & v, bool end) const {
    uint32_t k = v.cursor;
    if(end) {
      while(k + 1 < v.places.size() && v.lead[k] != '\n') ++k;
    } else {
      while(k > 0 && v.lead[k - 1] != '\n') --k;
    }
    return k;
  }

  vm_session & session;
  uint32_t edits = 0;
  os::uring ring;
  int listen_fd = -1;
  std::deque<peer> peers; // Deque: Customer links are stable
};

} // namespace tz
