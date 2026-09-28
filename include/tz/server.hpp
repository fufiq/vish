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
#include "parser.hpp"
#include "session.hpp"

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
    if(nj && from.substr(i, ni) == to.substr(j, nj)) {
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
    layout(v);
    proto::s_view out{v.id, v.root, session.db.revision, {}};
    for(auto const & l: v.layouts) out.nodes.push_back(wire_node(l));
    send(p, out);
    send_cursor(p, v);
  }

  void handle(peer & p, proto::c_close_view const & m) {
    if(editor_view * v = find(p, m.view)) v->root = {};
  }

  // The keys interpret the view: with an open addition - a list (arrows, Enter -
  // accept, Escape - close), otherwise the movement of the cursor and input.
  void handle(peer & p, proto::c_key const & m) {
    editor_view * v = find(p, m.view);
    if(!v) return send(p, proto::s_error{cat("нет представления ", m.view)});
    if(v->completing && !v->items.empty()) {
      if(m.key == "ArrowDown" || m.key == "ArrowUp") {
        uint32_t n = uint32_t(v->items.size());
        v->selected = (v->selected + (m.key == "ArrowDown" ? 1 : n - 1)) % n;
        return send_completion(p, *v);
      }
      if(m.key == "Enter") { // Accept: the typed word is replaced by the selected name
        std::string name = v->items[v->selected].name;
        close_completion(p, *v);
        return edit(*v, v->word_begin, v->cursor, name);
      }
      if(m.key == "Escape") return close_completion(p, *v);
    }
    if(m.key == " " && (m.mods & mod_ctrl)) return complete(p, *v, true);
    if(m.key == "ArrowRight") {
      if(v->cursor + 1 < v->places.size()) ++v->cursor;
    } else if(m.key == "ArrowLeft") {
      if(v->cursor > 0) --v->cursor;
    } else if(m.key == "Home" || m.key == "End") {
      v->cursor = line_edge(*v, m.key == "End");
    } else if(m.key == "Escape") {
    } else if(m.key == "Backspace" && v->cursor > 0) {
      edit(*v, v->cursor - 1, v->cursor, "");
      return complete(p, *v, false);
    } else if(m.key == "Delete" && v->cursor + 1 < v->places.size()) {
      close_completion(p, *v);
      return edit(*v, v->cursor, v->cursor + 1, "");
    } else if(m.key == "Backspace" || m.key == "Delete") { // edge of the text - nothing
    } else if(m.key == "Enter") {
      return edit(*v, v->cursor, v->cursor, "\n");
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
    if(number || (!forced && word.empty()) || session.db.is<n_str>(at)) return close_completion(p, v);
    auto items = filter_completions(context_identifiers(session.db, at), word);
    // The only option that matches the typed one is to show nothing.
    if(items.empty() || (items.size() == 1 && items[0].name == word)) return close_completion(p, v);
    v.items = std::move(items);
    v.selected = 0;
    v.word_begin = b;
    v.completing = true;
    send_completion(p, v);
  }

  void close_completion(peer & p, editor_view & v) {
    if(!v.completing) return;
    v.completing = false;
    v.items.clear();
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
    out.push_back({session.root, 2, v.range[0]});
    return out;
  }

  void edit(editor_view & v, uint32_t a, uint32_t b, std::string const & ins) {
    ++edits;
    for(auto const & u: units_around(v, a, b)) {
      std::string_view old = v.text_of(u.range);
      uint32_t base = v.byte_at[u.range.first];
      std::string edited = cat(old.substr(0, v.byte_at[a] - base), ins, old.substr(v.byte_at[b] - base));
      parser pr(session.db, edited, cat("<ввод #", edits, ">"));
      node_id fresh = u.kind == 2 ? pr.parse_module() : pr.parse_unit(u.kind == 0);
      if(!fresh || (u.kind == 0 && !session.db.is<n_block>(fresh))) continue;
      apply(v, u.node, fresh, edited, {a - u.range.first, b - u.range.first, code_points(ins)});
      return;
    }
  }

  // Replace the contents of the node unit and send the new projection to all
  // representations of all clients; each cursor is carried over through unit
  // alignment (before and after parsing). Editing within the unit (in places from
  // its beginning): [from, to] replaced by inserted code points.
  //
  struct change {
    uint32_t from, to, inserted;
    // place before editing → place in the text after editing (up to parsing)
    uint32_t map(uint32_t c) const { return c >= to ? c - (to - from) + inserted : c > from ? from : c; }
  };

  void apply(editor_view & av, node_id target, node_id fresh, std::string const & edited, change ch) {
    struct before {
      editor_view * v;
      peer * p;
      std::pair<uint32_t, uint32_t> r;
    };
    std::vector<before> views;
    for(auto & p: peers)
      for(auto & v: p.views)
        if(v.root && v.root == session.root) {
          uint32_t k = v.layout_of(target);
          if(k == ~uint32_t{0}) continue;
          views.push_back({&v, &p, v.range[k]});
        }
    session.db.replace(target, session.db[fresh].kind);
    session.db.link_parents(session.root);
    for(auto & b: views) {
      editor_view & v = *b.v;
      uint32_t c = v.cursor;
      layout(v);
      auto r = v.range[v.layout_of(target)];
      std::string_view now = v.text_of(r);
      // cursor of the author - immediately after entering; someone else - is moved by
      // editing; then alignment with the canonical projection (she puts gaps)
      if(&v == &av) c = r.first + align(edited, ch.from + ch.inserted, now);
      else if(c >= b.r.second) c = c - b.r.second + r.second;
      else if(c >= b.r.first) c = r.first + align(edited, ch.map(c - b.r.first), now);
      v.cursor = std::min<uint32_t>(c, uint32_t(v.places.size() - 1));
      send(*b.p, proto::s_update{v.id, session.db.revision, target, subtree(v, target)});
      if(&v != &av) close_completion(*b.p, v); // crossword
      send_cursor(*b.p, v);
    }
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

  void layout(editor_view & v) {
    v.layouts.clear();
    printer pr(session.db);
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
