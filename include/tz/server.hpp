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
#include "modules.hpp"
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
  node_id projection; // projection (§4.4): Module of print records and rules parsing edits; empty - default
  // transition list (F12, Ctrl+P): by item - function or signature "add"
  struct nav_target {
    node_id fn;
    std::string add;
  };
  std::vector<nav_target> targets; // not empty - open list - transition, not addition

  // Draft (author's decision 2026-10-12): the elements of the module that rule - the
  // text exactly as typed; in the representation of the function - it itself.
  struct draft_state {
    bool open = false;
    node_list items; // elements of the draft module (representation of the function is its root)
    node_id next; // module element after draft (empty - at the end)
    uint32_t source = 0; // Import of typed text: places node - in it
    std::vector<uint32_t> blank; // Empty parsing lines for the canon
    std::vector<std::string> rest; // comments after the last element - the following
    uint64_t since = 0; // revision of the base at the opening: journal after it - one edit
    std::string before; // text of the unit before the draft (canon) - for rollback of the accepted
    std::vector<uint32_t> sources; // importation
  } draft;
  struct snapshot {
    std::string text;
    uint32_t caret; // byte
  };
  std::vector<snapshot> undo, redo; // drawback
  bool typing = false; // It's a set of words: rollback in a word, not a letter.
  // Accepted edits (closed drafts): elements and their text before and after - rollback and
  // repetition of accepted - editing back (*my*: history - edits, journal is not rewritten)
  struct closed {
    node_list items;
    node_id next;
    std::string before, after;
  };
  std::vector<closed> done, undone;
  // layout of the module (printer): bytes of text by element number, end of elements, draft
  std::vector<uint32_t> chunk_at, content_at;
  uint32_t items_end = 0, draft_begin = 0, draft_end = 0;
  static constexpr uint32_t none = ~uint32_t{0};
  uint32_t anchor = none; // second end of allocation (place)
  uint32_t goal = none; // column
  mutable bool selection_sent = false;
  bool hint_shown = false; // The tip of the set is shown - to hide when there is nothing

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
    } else if(nj && to[j] == ';') { // `;` delivered projection (after a hole in the block)
      j += nj, ++cj;
    } else if(from[i] == ';') { // Excess `;` set (`y = 2;;`) - projection removed it
      i += ni, ++ci;
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
  enum op : uint64_t { op_accept = 1, op_recv, op_send, op_timer };

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
      case op_timer:
        on_timer();
        break;
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
    recheck(); // elaboration - to layout: ct-code mutations are visible immediately
    if(relayout) layout(v);
    proto::s_view out{v.id, v.root, session.db.revision, {}, v.kind};
    for(auto const & l: v.layouts) out.nodes.push_back(wire_node(l));
    send(p, out);
    send_cursor(p, v);
    recheck();
    send_marks(p, v);
  }

  void handle(peer & p, proto::c_close_view const & m) {
    if(editor_view * v = find(p, m.view)) {
      finalize(p, *v);
      v->root = {};
    }
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
        return replace(p, *v, v->word_begin, v->cursor, name);
      }
      if(m.key == "Escape") return close_completion(p, *v);
    }
    finalize_peer(p, v); // the key to another representation - drafts of others to the canon
    bool shift = m.mods & 1, ctrl = m.mods & mod_ctrl;
    if(m.key == " " && ctrl) return complete(p, *v, true);
    if(m.key == "F12") {
      finalize(p, *v);
      return goto_call(p, *v);
    }
    bool vert = false;
    if(auto t = motion(*v, m, vert)) return move_to(p, *v, *t, shift, vert);
    auto [s, e] = selection(*v);
    if(m.key == "Escape") { // release; draft - to the canon
      close_completion(p, *v);
      v->anchor = none;
      if(v->draft.open) finalize(p, *v);
      else send_cursor(p, *v);
      if(v->hint_shown) { // tip-off
        v->hint_shown = false;
        send(p, proto::s_hint{v->id, {}, 0, 0, {}});
      }
      return;
    }
    if(ctrl && (m.key == "a" || m.key == "A" || m.key == "ф")) {
      v->anchor = 0;
      return move_to(p, *v, uint32_t(v->places.size() - 1), true);
    }
    if(ctrl && (m.key == "c" || m.key == "C" || m.key == "с")) return copy(p, *v, false);
    if(ctrl && (m.key == "x" || m.key == "X" || m.key == "ч")) return copy(p, *v, true);
    if(ctrl && (m.key == "z" || m.key == "Z" || m.key == "я")) return undo(p, *v, shift);
    if(ctrl && (m.key == "y" || m.key == "Y" || m.key == "н")) return undo(p, *v, true);
    close_completion(p, *v);
    if(m.key == "Backspace" || m.key == "Delete") {
      if(s != e) return replace(p, *v, s, e, "");
      bool back = m.key == "Backspace";
      if(back && s == 0) return send_cursor(p, *v);
      if(!back && s + 1 >= v->places.size()) return send_cursor(p, *v);
      uint32_t a = back ? s - 1 : s, b = back ? s : s + 1;
      if(back && !ctrl) { // indent, up to the previous indentation column
        uint32_t ls = line_start(*v, s);
        if(s > ls && std::ranges::all_of(std::string_view(v->lead).substr(ls, s - ls), [](char c) { return c == ' '; })) a = s - ((s - ls) % 2 ? 1 : 2);
      }
      if(ctrl) back ? a = word_left(*v, s) : b = word_right(*v, s);
      replace(p, *v, a, b, "");
      if(back) complete(p, *v, false);
      return send_hint(p, *v);
    }
    if(m.key == "Enter") return newline(p, *v);
    if(m.key == "Tab") return indent(p, *v, shift);
    if(code_points(m.key) == 1 && !(m.mods & (mod_ctrl | mod_alt | mod_meta))) return type_text(p, *v, m.key);
    send(p, proto::s_error{cat("клавиша не обработана: ", m.key)});
  }

  // --- draft: set as in a regular editor ---
  //
  // User Solutions (2026-10-12): Keys go to VM
// the text of the module element, which
  // `;`, spaces and empty lines are set by a person, not projection. Each edit disassembles the
  // draft as a whole (written - node, unwritten - holes in their lines, unpaired bracket -
  // indentation, §4.1); layout - in places node in the typed text, the DOM remains the AST
  // tree. Checking and tags - on a pause of 1 sec; canon - when leaving the module element
  // (cursor outside it, Escape, save, launch, other presentation).
  //
//.

  static constexpr uint32_t pause_ms = 1000;
  bool timers = true; // Tests: pause with idle team, no clock
  uint64_t edited_at = 0; // last-draft
  bool check_pending = false, timer_armed = false;
  static constexpr uint32_t none = editor_view::none;

  // Number of the first draft element in the items of the module (empty - insertion place).
  uint32_t group_at(editor_view const & v) const {
    auto const & items = session.db.as<n_module>(session.root).items;
    node_id first = v.draft.items.empty() ? v.draft.next : v.draft.items[0];
    if(first)
      if(auto it = std::ranges::find(items, first); it != items.end()) return uint32_t(it - items.begin());
    uint32_t k = 0; // In the end, before the internal elements.
    while(k < items.size() && !is_inner(items[k])) ++k;
    return k;
  }
  std::optional<printer::verbatim_group> draft_group(editor_view const & v) const {
    if(!v.draft.open || v.kind != "module") return std::nullopt;
    return printer::verbatim_group{group_at(v), uint32_t(v.draft.items.size()), v.draft.source};
  }

  uint32_t place_of_byte(editor_view const & v, uint32_t b) const {
    return uint32_t(std::ranges::lower_bound(v.byte_at, b) - v.byte_at.begin());
  }
  // The beginning of the lines of the first deduced element with the number j; no - the end of the elements.
  uint32_t content_from(editor_view const & v, uint32_t j) const {
    for(; j < v.content_at.size(); ++j)
      if(v.content_at[j] != none) return v.content_at[j];
    return v.items_end;
  }

  // The unit of the draft around the place: the place [from, to), the elements of the module
  // [k0, k1); the last (the end of the text of the elements is its place).
  struct region {
    uint32_t from, to, k0, k1;
    bool last;
    bool contains(uint32_t p) const { return p >= from && (p < to || (p == to && last)); }
  };
  std::optional<region> unit_of(editor_view const & v, uint32_t place) const {
    uint32_t end = uint32_t(v.places.size() - 1);
    if(v.kind != "module") return region{0, end, 0, 0, true};
    auto const & items = session.db.as<n_module>(session.root).items;
    uint32_t pb = v.byte_at[place];
    if(v.draft.open) {
      uint32_t at = group_at(v), c = uint32_t(v.draft.items.size());
      region r{place_of_byte(v, v.draft_begin), place_of_byte(v, v.draft_end), at, at + c, v.draft_end == v.items_end};
      if(r.contains(place)) return r;
    }
    std::optional<uint32_t> k; // element: the last one whose lines start no further than the place
    for(uint32_t j = 0; j < v.content_at.size(); ++j)
      if(v.content_at[j] != none && v.content_at[j] <= pb) k = j;
    if(!k) {
      bool any = std::ranges::any_of(v.content_at, [](uint32_t c) { return c != none; });
      if(any || (v.draft.open && v.draft_begin < v.draft_end)) return std::nullopt; // module-cap
      uint32_t at = 0;
      while(at < items.size() && !is_inner(items[at])) ++at;
      uint32_t e = place_of_byte(v, v.items_end);
      return region{e, e, at, at, true};
    }
    uint32_t to = content_from(v, *k + 1);
    if(v.draft.open && v.draft_begin > v.chunk_at[*k] && v.draft_begin < to) to = v.draft_begin; // draft
    region r{place_of_byte(v, v.chunk_at[*k]), place_of_byte(v, to), *k, *k + 1, to == v.items_end};
    if(!r.contains(place)) return std::nullopt; // end-of-module comments
    return r;
  }
  std::optional<region> draft_region(editor_view const & v) const {
    if(!v.draft.open) return std::nullopt;
    if(v.kind != "module") return region{0, uint32_t(v.places.size() - 1), 0, 0, true};
    uint32_t at = group_at(v);
    return region{place_of_byte(v, v.draft_begin), place_of_byte(v, v.draft_end), at, at + uint32_t(v.draft.items.size()), v.draft_end == v.items_end};
  }

  // Draft covering places [a, b]: open or expand
//.
  // An element in another client's draft - the edit is rejected (refused).
  std::optional<region> open_draft(peer & p, editor_view & v, uint32_t a, uint32_t b, bool & refused) {
    auto ra = unit_of(v, a), rb = unit_of(v, b);
    if(!ra || !rb) return std::nullopt;
    region r{std::min(ra->from, rb->from), std::max(ra->to, rb->to), std::min(ra->k0, rb->k0), std::max(ra->k1, rb->k1), ra->last || rb->last};
    node_list group;
    if(v.kind == "module") {
      auto const & items = session.db.as<n_module>(session.root).items;
      group.assign(items.begin() + r.k0, items.begin() + r.k1);
    } else group = {v.root};
    for(auto & q: peers)
      for(auto & w: q.views)
        if(&w != &v && w.root && w.draft.open)
          for(node_id x: w.draft.items)
            if(std::ranges::contains(group, x)) {
              if(&q != &p) {
                refused = true;
                return std::nullopt;
              }
              finalize(q, w); // His other vision is to the canon.
            }
    if(v.draft.open && v.draft.items == group) return r;
    uint32_t gb = v.byte_at[r.from], ge = v.byte_at[r.to];
    if(!v.draft.open) {
      v.draft.since = session.db.revision;
      v.draft.sources.clear();
      v.draft.before = v.text.substr(gb, ge - gb);
    } else if(auto was = draft_region(v)) { // extension: neighbors - their text to the draft
      uint32_t wb = v.byte_at[was->from], we = v.byte_at[was->to];
      v.draft.before = cat(std::string_view(v.text).substr(gb, wb - gb), v.draft.before, std::string_view(v.text).substr(we, ge - we));
    }
    v.draft.open = true; // new draft or extension of the open
    v.draft.items = group;
    if(v.kind == "module") {
      auto const & items = session.db.as<n_module>(session.root).items;
      v.draft.next = r.k1 < items.size() && !is_inner(items[r.k1]) ? items[r.k1] : node_id{};
    }
    return r;
  }

  // Edit places [a, b) on ins in the draft; caret - cursor after editing (code points inside
  // ins). 0 - place not in the module element (cap, end comments): normal editing; -1 -
  // rejected.
  int draft_edit(peer & p, editor_view & v, uint32_t a, uint32_t b, std::string const & ins, uint32_t caret) {
    bool refused = false;
    auto r = open_draft(p, v, a, b, refused);
    if(refused) {
      send(p, proto::s_error{"элемент правит другой клиент"});
      return -1;
    }
    if(!r) return 0;
    uint32_t base = v.byte_at[r->from];
    std::string text = v.text.substr(base, v.byte_at[r->to] - base);
    uint32_t ia = v.byte_at[a] - base, ib = v.byte_at[b] - base;
    std::string edited = cat(std::string_view(text).substr(0, ia), ins, std::string_view(text).substr(ib));
    std::size_t cb = 0; // cursor-byte
    for(uint32_t n = 0; n < caret && cb < ins.size(); ++n) cb += cp_len(ins, cb);
    bool word = a == b && code_points(ins) == 1 && ident_char(ins[0]);
    if(!(word && v.typing)) {
      v.undo.push_back({text, v.byte_at[v.cursor] - base});
      if(v.undo.size() > 500) v.undo.erase(v.undo.begin());
    }
    v.typing = word;
    v.redo.clear();
    hint = {v.places[a], b - a, code_points(ins), true};
    commit_draft(p, v, edited, uint32_t(ia + cb), r->from, r->to);
    return 1;
  }

  // Disassemble the text of the draft and put its elements in the module (id elements of the same
  // type are saved); send out the layout. From is the beginning of the draft before editing.
  void commit_draft(peer & p, editor_view & v, std::string const & text, uint32_t caret_at, uint32_t from, uint32_t to) {
    auto & db = session.db;
    ++edits;
    parser pr(db, text, cat("<черновик #", edits, ">"));
    node_list old = v.draft.items;
    reparsed.clear();
    parser::draft_items d;
    if(v.kind != "module") {
      pr.place(db[v.root].parent, v.root);
      projection_rules(pr, v);
      d = pr.parse_draft(true);
      node_list all = db.as<n_module>(session.root).items;
      all.insert(all.end(), d.items.begin(), d.items.end());
      pr.resolve_refs(all);
      if(d.items.size() == 1 && db.is<n_fn>(d.items[0])) {
        if(!db.is<n_fn>(v.root)) { // It was a hole, a function again.
          db.replace(v.root, db[d.items[0]].kind);
          db.place(v.root, db[d.items[0]].where);
          db.set_note(v.root, db.note_of(d.items[0]) ? *db.note_of(d.items[0]) : node_note{});
        } else graft(v.root, d.items[0]);
      } else { // ceased to be a function, a hole (the same node), until it became again.
        db.replace(v.root, n_hole{text, "не функция"});
        db.place(v.root, span{0, uint32_t(text.size()), pr.source});
      }
      v.draft.items = {v.root};
    } else {
      uint32_t at = group_at(v), count = uint32_t(old.size());
      pr.place(session.root, count ? old[0] : v.draft.next); // Rules of Place (§4.4)
      projection_rules(pr, v);
      d = pr.parse_draft(true);
      n_module m = db.as<n_module>(session.root);
      {
        node_list all = m.items; // f(…) is a reference to the module function
        all.insert(all.end(), d.items.begin(), d.items.end());
        pr.resolve_refs(all);
      }
      // The draft declared other parsing rules - the following elements are re-examined:
      // 
      std::string was_rules, now_rules;
      for(node_id x: old) was_rules += parser::rule_trace(db, x);
      for(node_id x: d.items) now_rules += parser::rule_trace(db, x);
      node_list ids;
      for(uint32_t i = 0; i < d.items.size(); ++i) {
        node_id f = d.items[i];
        if(i >= count) ids.push_back(f);
        else if(db[old[i]].kind.index() == db[f].kind.index()) ids.push_back(graft(old[i], f)); // node of the same kind - the same id
        else { // module element saves id and when changing the view: editing the source of the ct code rolls back its changes (§7.4 )
          db.replace(old[i], db[f].kind);
          db.place(old[i], db[f].where);
          db.set_note(old[i], db.note_of(f) ? *db.note_of(f) : node_note{});
          ids.push_back(old[i]);
        }
      }
      m.items.erase(m.items.begin() + at, m.items.begin() + at + count);
      m.items.insert(m.items.begin() + at, ids.begin(), ids.end());
      if(!m.blank.empty()) {
        m.blank.resize(std::max<std::size_t>(m.blank.size(), at + count + 1), 0);
        m.blank.erase(m.blank.begin() + at, m.blank.begin() + at + count);
        m.blank.insert(m.blank.begin() + at, ids.size(), 0);
        parser::trim_blank(m.blank);
      }
      // The module changes only if its elements are changed: editing inside the element does not
      // touch the rest (and does not roll back their changes with the ct code, §7.4).
      if(m.items != db.as<n_module>(session.root).items || m.blank != db.as<n_module>(session.root).blank) db.replace(session.root, std::move(m));
      v.draft.items = ids;
      db.link_parents(session.root);
      if(was_rules != now_rules) {
        reparse_after(session.root, at + ids.size());
        reparse_users(session.root);
      }
    }
    v.draft.source = pr.source;
    v.draft.sources.push_back(pr.source);
    v.draft.blank = std::move(d.blank);
    v.draft.rest = std::move(d.rest);
    db.link_parents(session.root);
    close_traces();
    broadcast(v, from, to, caret_at);
    hint.valid = false;
    schedule_check();
  }

  // node of the new subtree, which coincided in place and appearance with the old ones, get the old id:
  // 
// Labels, cursors of other clients and layers
  // going through the set. Children's lists are matched from the beginning and end -
  // inserting or deleting in the middle doesn't shift neighbors.
  node_id graft(node_id o, node_id n) {
    auto & db = session.db;
    if(!o || !n || o == n || db[o].kind.index() != db[n].kind.index()) return n;
    node_kind k = db[n].kind;
    node_kind const was = db[o].kind;
    auto same = [&](node_id a, node_id b) { return a && b && db[a].kind.index() == db[b].kind.index(); };
    std::visit([&](auto & x) {
      using T = std::remove_cvref_t<decltype(x)>;
      T const & y = std::get<T>(was);
      template for(constexpr auto m: fields_of<T>()) {
        using field_t = [:std::meta::type_of(m):];
        if constexpr(std::is_same_v<field_t, node_id>) x.[:m:] = graft(y.[:m:], x.[:m:]);
        else if constexpr(std::is_same_v<field_t, node_list>) {
          auto & xs = x.[:m:];
          auto const & ys = y.[:m:];
          std::size_t a = 0, b = 0;
          for(; a < xs.size() && a < ys.size() && same(ys[a], xs[a]); ++a) xs[a] = graft(ys[a], xs[a]);
          for(; b < xs.size() - a && b < ys.size() - a && same(ys[ys.size() - 1 - b], xs[xs.size() - 1 - b]); ++b)
            xs[xs.size() - 1 - b] = graft(ys[ys.size() - 1 - b], xs[xs.size() - 1 - b]);
        }
      }
    }, k);
    db.replace(o, std::move(k));
    db.place(o, db[n].where);
    db.set_note(o, db.note_of(n) ? *db.note_of(n) : node_note{});
    return o;
  }

  // Edit the author in coordinates node (up to parsing): cursors of other clients in the same piece
  // are shifted by it, as in a conventional editor.
  struct edit_hint {
    caret at;
    uint32_t removed = 0, inserted = 0;
    bool valid = false;
  } hint;

  void close_traces() {
    for(auto & q: peers)
      if(q.debugging) {
        q.debugging = false;
        send(q, proto::s_trace{false, 0, 0, {}, {}, {}, {}});
      }
  }

  // New layout for all performances. The author (av, draft) - cursor on caret bytes from the
  // beginning of the draft; changed only inside the draft - updating its elements, otherwise -
  // the module as a whole. The rest is the cursor in the same place node.
  void broadcast(editor_view & av, uint32_t old_from, uint32_t old_to, uint32_t caret_at) {
    for(auto & q: peers)
      for(auto & w: q.views) {
        if(!w.root) continue;
        std::string was = w.text;
        auto was_root = w.layouts.empty() ? std::vector<piece>{} : w.layouts[0].pieces;
        bool mine = &w == &av;
        uint32_t ob = mine && !w.byte_at.empty() ? w.byte_at[std::min<uint32_t>(old_from, uint32_t(w.byte_at.size() - 1))] : 0;
        uint32_t oe = mine && !w.byte_at.empty() ? w.byte_at[std::min<uint32_t>(old_to, uint32_t(w.byte_at.size() - 1))] : 0;
        caret keep = w.places.empty() ? caret{} : w.places[w.cursor];
        uint32_t keep_byte = w.byte_at.empty() ? 0 : w.byte_at[w.cursor];
        node_list tops = av.draft.items; // corrected elements and reconstructed - their layouts before editing
        for(node_id x: reparsed)
          if(!std::ranges::any_of(tops, [&](node_id y) { return within(x, y); })) tops.push_back(x);
        std::vector<std::string> before;
        for(node_id x: tops) before.push_back(signature(w, x));
        std::map<uint32_t, std::string> own_before; // own chunks of node - for the narrowest update
        for(node_id x: tops)
          if(w.layout_of(x) != ~uint32_t{0})
            for(auto const & n: subtree(w, x)) own_before[n.id.value] = own_signature(n);
        layout(w);
        if(mine) {
          uint32_t start = w.kind == "module" ? w.draft_begin : 0;
          w.cursor = std::min<uint32_t>(place_of_byte(w, start + caret_at), uint32_t(w.places.size() - 1));
          w.anchor = w.goal = none;
        } else {
          if(hint.valid && keep.node == hint.at.node && keep.piece == hint.at.piece && keep.offset >= hint.at.offset)
            keep.offset = keep.offset >= hint.at.offset + hint.removed ? keep.offset - hint.removed + hint.inserted : hint.at.offset + hint.inserted;
          w.cursor = std::min<uint32_t>(place_of_byte(w, keep_byte), uint32_t(w.places.size() - 1));
          for(uint32_t k = 0; k < w.places.size(); ++k)
            if(w.places[k].node == keep.node && w.places[k].piece == keep.piece && w.places[k].offset == keep.offset) w.cursor = k;
          if(w.anchor != none) w.anchor = std::min<uint32_t>(w.anchor, uint32_t(w.places.size() - 1));
        }
        if(!mine && w.text == was) continue;
        // the root is the same (its pieces are the same texts and elements): updates only
        // the draft elements and redisassembled according to the new rules n
        bool inside = !w.layouts.empty() && w.layouts[0].pieces == was_root;
        if(mine && w.kind == "module")
          inside = inside && std::string_view(was).substr(0, ob) == std::string_view(w.text).substr(0, w.draft_begin) &&
                   std::string_view(was).substr(oe) == std::string_view(w.text).substr(w.draft_end);
        // one update: one element has changed - it, otherwise - the root (the client does not see
        // the intermediate mixture)
        node_list changed;
        for(uint32_t i = 0; i < tops.size(); ++i)
          if(w.layout_of(tops[i]) != ~uint32_t{0} && signature(w, tops[i]) != before[i] &&
             !std::ranges::any_of(tops, [&](node_id y) { return y != tops[i] && w.layout_of(y) != ~uint32_t{0} && within(tops[i], y); }))
            changed.push_back(tops[i]);
        if(inside && changed.size() == 1) {
          node_id at = narrowest(w, changed[0], own_before);
          send(q, proto::s_update{w.id, session.db.revision, at, subtree(w, at)});
        }
        else if(!inside || !changed.empty()) send(q, proto::s_update{w.id, session.db.revision, w.root, subtree(w, w.root)});
        if(!mine) close_completion(q, w);
        send_cursor(q, w);
      }
  }

  // The layout of the node subtree in the view is for comparison before and after editing.
  std::string signature(editor_view const & w, node_id x) const {
    std::string out;
    if(w.layout_of(x) == ~uint32_t{0}) return out;
    for(auto const & n: subtree(w, x)) {
      out += cat("#", n.id.value, n.kind, "[");
      for(auto const & pc: n.pieces) {
        if(auto const * t = std::get_if<std::string>(&pc)) out += cat("\"", *t, "\"");
        else out += cat("#", std::get<node_id>(pc).value);
      }
      out += cat("]", n.note, ";");
    }
    return out;
  }

  static std::string own_signature(proto::s_node const & n) {
    std::string out = cat(n.kind, "[");
    for(auto const & pc: n.pieces) {
      if(auto const * t = std::get_if<std::string>(&pc)) out += cat("\"", *t, "\"");
      else out += cat("#", std::get<node_id>(pc).value);
    }
    return out + cat("]", n.note);
  }
  // The deepest node, outside of which the layout has not changed: their pieces are the same,
  // and the subtree of exactly one child has changed - the descent into it (editing in a damp
  // block - updating only the block).
  node_id narrowest(editor_view const & w, node_id x, std::map<uint32_t, std::string> const & own_before) const {
    std::map<uint32_t, bool> memo;
    auto changed = [&](auto && self, node_id n) -> bool {
      if(auto it = memo.find(n.value); it != memo.end()) return it->second;
      uint32_t k = w.layout_of(n);
      bool c = k == ~uint32_t{0};
      if(!c) {
        auto it = own_before.find(n.value);
        c = it == own_before.end() || it->second != own_signature(wire_node(w.layouts[k]));
        if(!c)
          for(auto const & pc: w.layouts[k].pieces)
            if(auto const * ch = std::get_if<node_id>(&pc); ch && self(self, *ch)) c = true;
      }
      return memo[n.value] = c;
    };
    for(;;) {
      uint32_t k = w.layout_of(x);
      auto it = own_before.find(x.value);
      if(k == ~uint32_t{0} || it == own_before.end() || it->second != own_signature(wire_node(w.layouts[k]))) return x;
      node_id only;
      uint32_t n = 0;
      for(auto const & pc: w.layouts[k].pieces)
        if(auto const * ch = std::get_if<node_id>(&pc); ch && changed(changed, *ch)) {
          only = *ch;
          ++n;
        }
      if(n != 1) return x;
      x = only;
    }
  }

  void schedule_check() {
    check_pending = true;
    edited_at = os::uring::now_ms();
    if(timers && !timer_armed) {
      ring.timeout(pause_ms, tag(op_timer, 0));
      timer_armed = true;
    }
  }
  void on_timer() {
    timer_armed = false;
    if(!check_pending) return;
    uint64_t now = os::uring::now_ms();
    if(now - edited_at >= pause_ms) return idle();
    ring.timeout(uint32_t(pause_ms - (now - edited_at)), tag(op_timer, 0));
    timer_armed = true;
  }
  // Set pause: check (reactive check) and tags for all submissions.
  void idle() {
    if(!check_pending) return;
    check_pending = false;
    recheck();
    for(auto & q: peers)
      for(auto & w: q.views)
        if(w.root) send_marks(q, w);
  }

  // Canon: the draft leaves, the presentation is printed projection. The course and
  // highlighting - until the draft in the same places, inside - aligning the typed text with
  // the canonical, then with a shift.
  enum class history { edit, undo, redo };
  void finalize(peer & p, editor_view & v, history mode = history::edit) {
    if(!v.draft.open) return;
    auto & db = session.db;
    bool module = v.kind == "module";
    uint32_t ob = module ? v.draft_begin : 0, oe = module ? v.draft_end : uint32_t(v.text.size());
    uint32_t at = module ? group_at(v) : 0, count = uint32_t(v.draft.items.size());
    std::string was = v.text;
    uint32_t cur = v.byte_at[v.cursor], anc = v.anchor != none ? v.byte_at[v.anchor] : none;
    if(module) {
      n_module m = db.as<n_module>(session.root);
      bool next = v.draft.next && at + count < m.items.size() && m.items[at + count] == v.draft.next;
      m.blank.resize(std::max<std::size_t>(m.blank.size(), m.items.size() + 1), 0);
      for(uint32_t i = 0; i < count && i < v.draft.blank.size(); ++i) m.blank[at + i] = at + i == 0 ? 0 : v.draft.blank[i];
      if(next && !v.draft.blank.empty()) m.blank[at + count] = v.draft.blank.back();
      parser::trim_blank(m.blank);
      if(m.blank != db.as<n_module>(session.root).blank) db.replace(session.root, std::move(m));
      if(!v.draft.rest.empty()) { // comments after the draft with their lines - the next element
        node_id to = next ? v.draft.next : session.root;
        node_note nt = db.note_of(to) ? *db.note_of(to) : node_note{};
        auto & lines = next ? nt.lead : nt.end;
        lines.insert(lines.begin(), v.draft.rest.begin(), v.draft.rest.end());
        db.set_note(to, std::move(nt));
      }
      db.link_parents(session.root);
    }
    db.squash_log(v.draft.since, v.draft.sources, v.draft.source); // Draft keys - one edit in journal
    editor_view::closed entry{v.draft.items, v.draft.next, std::move(v.draft.before), {}};
    v.draft = {};
    v.undo.clear();
    v.redo.clear();
    v.typing = false;
    recheck(false); // elaboration - to layout: the canon shows the changed ct-code
    layout(v);
    uint32_t ne = module ? content_from(v, at + count) : uint32_t(v.text.size());
    if(module && ne < ob) ne = ob;
    if(module) { // History of the accepted: the text of the unit after - canon
      entry.after = v.text.substr(ob, ne - ob);
      if(entry.before != entry.after) {
        (mode == history::undo ? v.undone : v.done).push_back(std::move(entry));
        if(mode == history::edit) v.undone.clear();
      }
    }
    auto map = [&](uint32_t x) -> uint32_t {
      if(x < ob) return place_of_byte(v, x);
      if(x >= oe && !(x == oe && oe == uint32_t(was.size()))) return place_of_byte(v, x - oe + ne);
      uint32_t cp = code_points(std::string_view(was).substr(ob, x - ob));
      uint32_t got = align(std::string_view(was).substr(ob), cp, std::string_view(v.text).substr(ob));
      return std::min<uint32_t>(place_of_byte(v, ob) + got, uint32_t(v.places.size() - 1));
    };
    v.cursor = map(cur);
    v.anchor = anc != none ? map(anc) : none;
    for(auto & q: peers)
      for(auto & w: q.views) {
        if(!w.root) continue;
        if(&w != &v) {
          std::string t = w.text;
          caret keep = w.places.empty() ? caret{} : w.places[w.cursor];
          layout(w);
          for(uint32_t k = 0; k < w.places.size(); ++k)
            if(w.places[k].node == keep.node && w.places[k].piece == keep.piece && w.places[k].offset == keep.offset) w.cursor = k;
          if(w.text == t) continue;
        }
        send(q, proto::s_update{w.id, session.db.revision, w.root, subtree(w, w.root)});
        send_cursor(q, w);
      }
    close_completion(p, v);
    check_pending = true;
    idle();
  }
  void finalize_peer(peer & p, editor_view const * except = nullptr) {
    for(auto & w: p.views)
      if(w.root && &w != except) finalize(p, w);
  }
  void finalize_all() {
    for(auto & q: peers) finalize_peer(q);
  }

  // Edit [a, b) on ins: in the draft, otherwise (module cap) - in parentheses parsing.
  void replace(peer & p, editor_view & v, uint32_t a, uint32_t b, std::string const & ins, uint32_t caret = ~uint32_t{0}) {
    caret = std::min(caret, code_points(ins));
    v.anchor = v.goal = none;
    int r = draft_edit(p, v, a, b, ins, caret);
    if(r == 0) {
      v.typing = false;
      edit(v, a, b, ins, caret);
    }
  }

  // --- cursor ---

  uint32_t line_start(editor_view const & v, uint32_t k) const {
    while(k > 0 && v.lead[k - 1] != '\n') --k;
    return k;
  }
  uint32_t line_end(editor_view const & v, uint32_t k) const {
    while(k + 1 < v.places.size() && v.lead[k] != '\n') ++k;
    return k;
  }
  static int char_class(char c) { return c == ' ' || c == '\n' || c == '\t' ? 0 : ident_char(c) || (static_cast<unsigned char>(c) & 0x80) ? 1 : 2; }
  uint32_t word_left(editor_view const & v, uint32_t k) const {
    while(k > 0 && char_class(v.lead[k - 1]) == 0) --k;
    if(k == 0) return 0;
    int c = char_class(v.lead[k - 1]);
    while(k > 0 && char_class(v.lead[k - 1]) == c) --k;
    return k;
  }
  uint32_t word_right(editor_view const & v, uint32_t k) const {
    uint32_t end = uint32_t(v.places.size() - 1);
    while(k < end && char_class(v.lead[k]) == 0) ++k;
    if(k == end) return end;
    int c = char_class(v.lead[k]);
    while(k < end && char_class(v.lead[k]) == c) ++k;
    return k;
  }
  // Line up (dir < 0) or down on lines - in the goal column.
  uint32_t vertical(editor_view & v, int dir, uint32_t lines) const {
    uint32_t ls = line_start(v, v.cursor);
    uint32_t col = v.goal != none ? v.goal : v.cursor - ls;
    v.goal = col;
    for(uint32_t n = 0; n < lines; ++n) {
      if(dir < 0) {
        if(ls == 0) return n == 0 ? 0 : std::min(ls + col, line_end(v, ls));
        ls = line_start(v, ls - 1);
      } else {
        uint32_t le = line_end(v, ls);
        if(le + 1 >= v.places.size()) return n == 0 ? le : std::min(ls + col, le);
        ls = le + 1;
      }
    }
    return std::min(ls + col, line_end(v, ls));
  }
  std::pair<uint32_t, uint32_t> selection(editor_view const & v) const {
    if(v.anchor == none) return {v.cursor, v.cursor};
    return {std::min(v.anchor, v.cursor), std::max(v.anchor, v.cursor)};
  }

  // The cursor to place t (shift - allocation stretches); outside the draft - the canon.
  void move_to(peer & p, editor_view & v, uint32_t t, bool shift, bool keep_goal = false) {
    if(shift) {
      if(v.anchor == none) v.anchor = v.cursor;
    } else v.anchor = none;
    v.cursor = std::min<uint32_t>(t, uint32_t(v.places.size() - 1));
    if(!keep_goal) v.goal = none;
    v.typing = false;
    close_completion(p, v);
    if(auto r = draft_region(v); r && !(r->contains(v.cursor) && (v.anchor == none || r->contains(v.anchor)))) finalize(p, v);
    else send_cursor(p, v);
    send_hint(p, v);
  }

  // Motion key: new cursor location; vertical - column remembered
  std::optional<uint32_t> motion(editor_view & v, proto::c_key const & m, bool & vert) const {
    bool ctrl = m.mods & mod_ctrl;
    uint32_t end = uint32_t(v.places.size() - 1);
    auto [s, e] = selection(v);
    bool collapse = v.anchor != none && !(m.mods & 1);
    vert = false;
    if(m.key == "ArrowLeft") return collapse && !ctrl ? s : ctrl ? word_left(v, v.cursor) : v.cursor > 0 ? v.cursor - 1 : 0;
    if(m.key == "ArrowRight") return collapse && !ctrl ? e : ctrl ? word_right(v, v.cursor) : std::min(v.cursor + 1, end);
    if(m.key == "Home") {
      if(ctrl) return 0u;
      uint32_t ls = line_start(v, v.cursor), fs = ls;
      while(fs < end && v.lead[fs] == ' ') ++fs;
      return v.cursor == fs ? ls : fs;
    }
    if(m.key == "End") return ctrl ? end : line_end(v, v.cursor);
    vert = true;
    if(m.key == "ArrowUp") return vertical(v, -1, 1);
    if(m.key == "ArrowDown") return vertical(v, 1, 1);
    if(m.key == "PageUp") return vertical(v, -1, 30);
    if(m.key == "PageDown") return vertical(v, 1, 30);
    return std::nullopt;
  }

  // Type of text (letter, insert): replaces the selection; `}` in indentation - a level less.
  void type_text(peer & p, editor_view & v, std::string const & t) {
    auto [s, e] = selection(v);
    if(t == "}" && s == e) {
      uint32_t ls = line_start(v, s);
      bool indent = s > ls && std::ranges::all_of(std::string_view(v.lead).substr(ls, s - ls), [](char c) { return c == ' '; });
      if(indent) return replace(p, v, ls, s, cat(std::string((s - ls) >= 2 ? s - ls - 2 : 0, ' '), "}"));
    }
    replace(p, v, s, e, t);
    if(code_points(t) == 1) complete(p, v, false);
    send_hint(p, v);
  }

  // Enter: line translation with line indentation; after `{` - deeper, between `{` and `}` -
  // two lines, cursor - on the middle.
  void newline(peer & p, editor_view & v) {
    auto [s, e] = selection(v);
    uint32_t ls = line_start(v, s), ind = 0;
    while(ls + ind < s && v.lead[ls + ind] == ' ') ++ind;
    uint32_t b = s;
    while(b > ls && v.lead[b - 1] == ' ') --b;
    bool open = b > ls && v.lead[b - 1] == '{';
    uint32_t f = e;
    while(f + 1 < v.places.size() && v.lead[f] == ' ') ++f;
    bool close = open && v.lead[f] == '}';
    std::string pad(ind + (open ? 2 : 0), ' ');
    if(close) return replace(p, v, s, f, cat("\n", pad, "\n", std::string(ind, ' ')), 1 + uint32_t(pad.size()));
    replace(p, v, s, e, cat("\n", pad));
  }

  // Tab / Shift + Tab: indentation of the selection lines by 2 (without highlighting Tab -
  // spaces to the next column of indentation).
  void indent(peer & p, editor_view & v, bool out) {
    auto [s, e] = selection(v);
    if(!out && s == e) {
      uint32_t col = s - line_start(v, s);
      return replace(p, v, s, s, std::string(2 - col % 2, ' '));
    }
    uint32_t a = line_start(v, s), b = line_end(v, e > s && v.lead[e - 1] == '\n' ? e - 1 : e);
    std::string text = v.text.substr(v.byte_at[a], v.byte_at[b] - v.byte_at[a]), done;
    for(auto l: std::views::split(std::string_view(text), '\n')) {
      std::string_view line(l.begin(), l.end());
      if(!done.empty() || &*l.begin() != text.data()) done += '\n';
      if(out) {
        uint32_t n = 0;
        while(n < 2 && n < line.size() && line[n] == ' ') ++n;
        done += line.substr(n);
      } else done += line.empty() ? std::string(line) : cat("  ", line);
    }
    replace(p, v, a, b, done);
  }

  void copy(peer & p, editor_view & v, bool cut) {
    auto [s, e] = selection(v);
    if(s == e) { // single-line
      s = line_start(v, s);
      e = std::min<uint32_t>(line_end(v, e) + 1, uint32_t(v.places.size() - 1));
    }
    send(p, proto::s_clipboard{v.text.substr(v.byte_at[s], v.byte_at[e] - v.byte_at[s])});
    if(cut) replace(p, v, s, e, "");
  }

  // Recoil and repeat - inside the draft (rollback of the adopted - on the journal base, later).
  void undo(peer & p, editor_view & v, bool again) {
    auto & from = again ? v.redo : v.undo;
    auto & to = again ? v.undo : v.redo;
    auto r = draft_region(v);
    if(!r || from.empty()) return undo_closed(p, v, again);
    uint32_t base = v.byte_at[r->from];
    to.push_back({v.text.substr(base, v.byte_at[r->to] - base), v.byte_at[v.cursor] - base});
    auto snap = std::move(from.back());
    from.pop_back();
    v.typing = false;
    v.anchor = v.goal = none;
    commit_draft(p, v, snap.text, snap.caret, r->from, r->to);
  }

  // Retracement (repeat) of the adopted editing: the unit - the same elements, its text - as
  // it was before the editing (after it); editing back - with a draft, immediately to the
  // canon. The text of the unit has changed since (another client) is a waiver.
  void undo_closed(peer & p, editor_view & v, bool again) {
    finalize(p, v);
    auto & from = again ? v.undone : v.done;
    if(from.empty()) return send(p, proto::s_error{again ? "повторять нечего" : "откатывать нечего"});
    if(v.kind != "module") return send(p, proto::s_error{"откат принятого — в представлении модуля"});
    editor_view::closed e = std::move(from.back());
    from.pop_back();
    auto const & items = session.db.as<n_module>(session.root).items;
    uint32_t at = 0;
    if(!e.items.empty()) {
      auto it = std::ranges::find(items, e.items[0]);
      bool here = it != items.end() && std::size_t(items.end() - it) >= e.items.size() && std::ranges::equal(e.items, std::span(it, e.items.size()));
      if(!here) return send(p, proto::s_error{"откат: элементов правки больше нет"});
      at = uint32_t(it - items.begin());
    } else {
      auto it = e.next ? std::ranges::find(items, e.next) : items.end();
      at = uint32_t(it - items.begin());
      while(!e.next && at > 0 && is_inner(items[at - 1])) --at;
    }
    uint32_t count = uint32_t(e.items.size());
    uint32_t ob = at < v.chunk_at.size() && count ? v.chunk_at[at] : content_from(v, at), oe = content_from(v, at + count);
    if(oe < ob || v.text.substr(ob, oe - ob) != e.after) return send(p, proto::s_error{"откат: элемент изменился после правки"});
    for(auto & q: peers)
      for(auto & w: q.views)
        if(&w != &v && w.root && w.draft.open)
          for(node_id x: w.draft.items)
            if(std::ranges::contains(e.items, x)) return send(p, proto::s_error{"элемент правит другой клиент"});
    uint32_t from_place = place_of_byte(v, ob), to_place = place_of_byte(v, oe);
    v.draft = {};
    v.draft.open = true;
    v.draft.items = e.items;
    v.draft.next = e.next;
    v.draft.since = session.db.revision;
    v.draft.before = e.after;
    v.anchor = v.goal = none;
    v.typing = false;
    // cursor - in place of editing: the first difference of texts
    uint32_t same = 0;
    while(same < e.before.size() && same < e.after.size() && e.before[same] == e.after[same]) ++same;
    while(same > 0 && (static_cast<unsigned char>(e.before[same]) & 0xC0) == 0x80) --same;
    commit_draft(p, v, e.before, same, from_place, to_place);
    finalize(p, v, again ? history::redo : history::undo);
  }

  std::optional<uint32_t> place_of(editor_view const & v, node_id node, uint32_t piece, uint32_t offset) const {
    for(uint32_t k = 0; k < v.places.size(); ++k) {
      caret const & c = v.places[k];
      if(c.node != node || c.piece != piece) continue;
      // The end of the piece is the place after its last symbol.
      if(c.offset == offset || (offset && c.offset + 1 == offset && (k + 1 == v.places.size() || v.places[k + 1].node != c.node || v.places[k + 1].piece != c.piece)))
        return c.offset == offset ? k : k + 1;
    }
    return std::nullopt;
  }

  void handle(peer & p, proto::c_input const & m) {
    editor_view * v = find(p, m.view);
    if(!v) return send(p, proto::s_error{cat("нет представления ", m.view)});
    finalize_peer(p, v);
    close_completion(p, *v);
    v->typing = false;
    type_text(p, *v, m.text);
  }

  void handle(peer & p, proto::c_select const & m) {
    editor_view * v = find(p, m.view);
    if(!v) return send(p, proto::s_error{cat("нет представления ", m.view)});
    finalize_peer(p, v);
    auto t = place_of(*v, m.node, m.piece, m.offset);
    if(!t) return send(p, proto::s_error{cat("нет места #", m.node.value, "/", m.piece, "/", m.offset)});
    if(m.clicks == 2) { // word
      uint32_t a = *t, b = *t;
      int c = char_class(v->lead[a]);
      while(a > 0 && char_class(v->lead[a - 1]) == c) --a;
      while(b + 1 < v->places.size() && char_class(v->lead[b]) == c) ++b;
      v->cursor = a;
      return move_to(p, *v, b, true);
    }
    if(m.clicks >= 3) { // line
      v->cursor = line_start(*v, *t);
      v->anchor = none;
      return move_to(p, *v, std::min<uint32_t>(line_end(*v, *t) + 1, uint32_t(v->places.size() - 1)), true);
    }
    move_to(p, *v, *t, true);
  }

  // --- hints: guidance, design shape, signature ---
  //
  // 
// The following lines are from AST and
  // parsing (type over all instance, value in layer, error with a trace, ads with links), not
  // text. *Mine: * line composition.

  // The function declaration head is projection without a body: `when(c, expr yes) otherwise(expr
  // no)`, `syntax if (c) {then_} else {else_}`.
  std::string head_text(node_id f) const {
    auto const & db = session.db;
    std::vector<node_layout> ls;
    printer pr(db);
    pr.layouts = &ls;
    pr.print(f);
    std::map<uint32_t, uint32_t> at;
    for(uint32_t k = 0; k < ls.size(); ++k) at[ls[k].node.value] = k;
    node_id body = db.as<n_fn>(f).body;
    std::string out;
    bool stop = false;
    auto rec = [&](auto && self, node_id n) -> void {
      auto it = at.find(n.value);
      if(it == at.end()) return;
      for(auto const & pc: ls[it->second].pieces) {
        if(stop) return;
        if(auto const * t = std::get_if<std::string>(&pc)) out += *t;
        else if(std::get<node_id>(pc) == body) stop = true;
        else self(self, std::get<node_id>(pc));
      }
    };
    rec(rec, f);
    while(!out.empty() && (out.back() == ' ' || out.back() == '=' || out.back() == '>')) out.pop_back();
    return out;
  }

  // Name announcements: module functions (with internal) and prelude, syntax records.
  std::vector<node_id> declarations(std::string_view name) const {
    std::vector<node_id> out;
    for(node_id m: {session.root, session.db.prelude}) {
      if(!m || !session.db.is<n_module>(m)) continue;
      for(node_id f: session.db.as<n_module>(m).items)
        if(session.db.is<n_fn>(f) && session.db.strings.text(session.db.as<n_fn>(f).name) == name) out.push_back(f);
    }
    return out;
  }

  // Construction: clauses, expression parameters or blocks, syntax recording - it has a form.
  bool construction(node_id f) const {
    auto const & fn = session.db.as<n_fn>(f);
    if(!fn.form.empty()) return true;
    return std::ranges::any_of(fn.params, [&](node_id p) {
      auto const & x = session.db.as<n_param>(p);
      return x.clause || x.mode == param_mode::expr;
    });
  }

  void declaration_lines(std::vector<proto::s_line> & out, std::string_view name, std::vector<uint32_t> const & chosen) {
    auto fns = declarations(name);
    std::ranges::stable_sort(fns, {}, [&](node_id f) { return !std::ranges::contains(chosen, f.value); });
    uint32_t n = 0;
    for(node_id f: fns) {
      if(++n > 8) break;
      bool here = std::ranges::contains(chosen, f.value);
      bool prelude = session.db[f].where.source == session.db[session.db.prelude].where.source;
      std::string where = here ? "выбрана здесь" : prelude ? "прелюдия" : is_inner(f) ? "внутренняя" : "";
      out.push_back({construction(f) ? "форма" : "объявление", cat(head_text(f), where.empty() ? "" : cat("  — ", where)), f});
      if(auto const * nt = session.db.note_of(f))
        for(auto const & l: nt->lead)
          if(!l.empty()) out.push_back({"комментарий", l, {}});
    }
  }

  std::vector<proto::s_line> hover_lines(editor_view const & v, node_id n) {
    auto const & db = session.db;
    std::vector<proto::s_line> out;
    if(!n || n.value >= db.size()) return out;
    if(db.is<n_hole>(n)) out.push_back({"дыра", db.as<n_hole>(n).reason, {}});
    // type: the name-advertisement - according to all its mentions, otherwise - node
    std::string ts;
    node_id up = db[n].parent;
    if(db.is<n_param>(n)) ts = shown_name(scope_of(n), db.as<n_param>(n).name);
    else if(db.is<n_var>(n)) ts = shown_name(scope_of(n), db.as<n_var>(n).name);
    else if(session.node_types.contains(n.value)) ts = shown_node(n);
    else if(db.is<n_ident>(n)) ts = shown_name(scope_of(n), db.as<n_ident>(n).name);
    bool callee = up && db.is<n_call>(up) && db.as<n_call>(up).callee == n;
    if(!ts.empty() && !callee) out.push_back({"тип", ts, {}});
    if(auto const * l = layer_of(v))
      if(auto it = l->literal.find(n.value); it != l->literal.end()) out.push_back({"значение", it->second, {}});
    for(auto const & m: marks_errors(v))
      if(m.node == n) out.push_back({"ошибка", m.text, {}});
    // function or design name - ads with links (selected in instance - first)
    symbol name;
    if(db.is<n_fn>(n)) name = db.as<n_fn>(n).name; // announcement - his overload
    else if(db.is<n_ident>(n)) name = db.as<n_ident>(n).name;
    else if(db.is_member(n) && up && db.is<n_call>(up)) name = db.member_of(n).name;
    else if(db.is<n_call>(n) && db.is<n_ident>(db.as<n_call>(n).callee)) name = db.as<n_ident>(db.as<n_call>(n).callee).name;
    if(name) {
      node_id call = db.is<n_call>(n) ? n : callee ? up : node_id{};
      auto chosen = call && session.call_targets.contains(call.value) ? session.call_targets.at(call.value) : std::vector<uint32_t>{};
      declaration_lines(out, db.strings.text(name), chosen);
    }
    return out;
  }

  void handle(peer & p, proto::c_hover const & m) {
    editor_view * v = find(p, m.view);
    if(!v) return send(p, proto::s_error{cat("нет представления ", m.view)});
    send(p, proto::s_hover{v->id, m.node, hover_lines(*v, m.node)});
  }

  void handle(peer & p, proto::c_open const & m) {
    editor_view * v = find(p, m.view);
    if(!v) return send(p, proto::s_error{cat("нет представления ", m.view)});
    auto const & db = session.db;
    if(!m.node || m.node.value >= db.size()) return send(p, proto::s_error{"нет такого узла"});
    finalize_peer(p);
    if(db.is<n_fn>(m.node)) return open_fn(p, m.node);
    uint32_t k = v->layout_of(m.node);
    if(k == ~uint32_t{0}) return send(p, proto::s_error{"узла нет в этом представлении"});
    move_to(p, *v, v->range[k].first, false);
  }

  // Hint set: cursor inside the brackets of the call - the signature with the current parameter;
  // the word in front of the cursor - the name of the design - its shape. Otherwise, hide it.
  void send_hint(peer & p, editor_view & v) {
    auto const & db = session.db;
    std::vector<proto::s_line> lines;
    caret anchor{};
    uint32_t c = v.cursor;
    // cursor
    uint32_t b = c;
    while(b > 0 && ident_char(v.lead[b - 1])) --b;
    std::string word;
    for(uint32_t k = b; k < c; ++k) word += v.lead[k];
    if(!word.empty() && !(c < v.lead.size() && ident_char(v.lead[c]))) {
      for(node_id f: declarations(word))
        if(construction(f) && lines.size() < 6) lines.push_back({"форма", head_text(f), f});
      if(!lines.empty()) anchor = v.places[b];
    }
    // Call around the cursor: brackets of his arguments
    if(lines.empty() && c > 0)
      for(node_id n = v.places[c - 1].node; n; n = db[n].parent) {
        if(!db.is<n_call>(n) || !db.is<n_ident>(db.as<n_call>(n).callee)) continue;
        auto const & call = db.as<n_call>(n);
        uint32_t k = v.layout_of(n), ck = v.layout_of(call.callee);
        if(k == ~uint32_t{0} || ck == ~uint32_t{0}) break;
        uint32_t open = v.range[ck].second; // `(` immediately after the name
        if(open >= v.lead.size() || v.lead[open] != '(' || c <= open || c >= v.range[k].second) continue; // after `)` - no longer in brackets
        uint32_t arg = 0; // current parameter - on arguments that ended before the cursor
        for(node_id a: call.args) {
          uint32_t ak = v.layout_of(a);
          if(ak != ~uint32_t{0} && v.range[ak].second < c && v.lead[std::min<uint32_t>(v.range[ak].second, uint32_t(v.lead.size() - 1))] == ',') ++arg;
          else if(ak != ~uint32_t{0} && v.range[ak].second < c) {
            uint32_t q = v.range[ak].second;
            while(q < c && v.lead[q] == ' ') ++q;
            if(q < c && v.lead[q] == ',') ++arg;
          }
        }
        std::string name(db.strings.text(db.as<n_ident>(call.callee).name));
        auto chosen = session.call_targets.contains(n.value) ? session.call_targets.at(n.value) : std::vector<uint32_t>{};
        auto fns = declarations(name);
        std::ranges::stable_sort(fns, {}, [&](node_id f) { return !std::ranges::contains(chosen, f.value); });
        for(node_id f: fns) {
          if(lines.size() >= 6) break;
          auto const & fn = db.as<n_fn>(f);
          std::string s = cat(name, "(");
          uint32_t from = 0, to = 0, head = 0;
          for(uint32_t i = 0; i < fn.params.size(); ++i) {
            auto const & pa = db.as<n_param>(fn.params[i]);
            if(pa.clause) break; // Clauses - after the head
            if(head) s += ", ";
            std::string one;
            if(pa.type && db.is<n_ident>(pa.type)) one = cat(db.strings.text(db.as<n_ident>(pa.type).name), " ");
            if(pa.mode == param_mode::expr) one = cat("expr ", one);
            one += db.strings.text(pa.name);
            if(head == arg || (pa.pack && arg >= head)) {
              from = code_points(s);
              to = from + code_points(one);
            }
            s += one;
            ++head;
          }
          lines.push_back({"сигнатура", s + ")", f, from, to});
        }
        if(!lines.empty()) anchor = v.places[v.range[ck].first];
        break;
      }
    if(lines.empty() && !v.hint_shown) return;
    v.hint_shown = !lines.empty();
    send(p, proto::s_hint{v.id, anchor.node, anchor.piece, anchor.offset, std::move(lines)});
  }

  // --- AST inspector: what is and who is disassembled ---
  //
  //  
// 
//.
  // *My:* Module element tree with seats in the parent (node field and recording or
  // construction parameter), who disassembled the node and focus disclosure.

  // node children with field names (reflection of node): `cond`, `args[1]`.
  std::vector<std::pair<node_id, std::string>> child_roles(node_id n) const {
    std::vector<std::pair<node_id, std::string>> out;
    std::visit([&](auto const & x) {
      template for(constexpr auto m: fields_of<std::remove_cvref_t<decltype(x)>>()) {
        using field_t = [:std::meta::type_of(m):];
        std::string name(std::meta::identifier_of(m));
        if constexpr(std::is_same_v<field_t, node_id>) {
          if(x.[:m:]) out.push_back({x.[:m:], name});
        } else if constexpr(std::is_same_v<field_t, node_list>) {
          for(std::size_t i = 0; i < x.[:m:].size(); ++i)
            if(x.[:m:][i]) out.push_back({x.[:m:][i], cat(name, "[", i, "]")});
        }
      }
    }, session.db[n].kind);
    return out;
  }

  // node: name, literal, operator, cause of the hole.
  std::string own_text(node_id n) const {
    auto const & db = session.db;
    auto clip = [](std::string t) {
      for(auto & c: t)
        if(c == '\n') c = ' ';
      if(code_points(t) > 40) {
        std::size_t b = 0;
        for(uint32_t k = 0; k < 40 && b < t.size(); ++k) b += cp_len(t, b);
        t = t.substr(0, b) + "…";
      }
      return t;
    };
    if(db.is<n_ident>(n)) return std::string(db.strings.text(db.as<n_ident>(n).name));
    if(db.is<n_fn>(n)) return std::string(db.strings.text(db.as<n_fn>(n).name));
    if(db.is<n_param>(n)) return std::string(db.strings.text(db.as<n_param>(n).name));
    if(db.is<n_var>(n)) return std::string(db.strings.text(db.as<n_var>(n).name));
    if(db.is<n_op>(n)) return std::string(db.strings.text(db.as<n_op>(n).op));
    if(db.is<n_int>(n) || db.is<n_float>(n) || db.is<n_str>(n)) return clip(printer(db).print(n));
    if(db.is<n_hole>(n)) return clip(db.as<n_hole>(n).text);
    if(db.is<n_raw>(n)) return clip(db.as<n_raw>(n).text);
    return {};
  }

  // Rule node: Syntax recording (printing and parsing on it) or the construction it causes;
  // children's places are its parameters.
  struct rule_of {
    node_id decl;
    std::string what; // "record" or "construction"
    std::map<uint32_t, std::string> params; // child → parameter of the rule
  };
  std::optional<rule_of> rule_for(node_id n, printer & pr) {
    auto const & db = session.db;
    if(auto const * r = pr.record_for(n)) {
      auto const & [d, info] = *r;
      rule_of out{d, "запись", {}};
      auto const & fn = db.as<n_fn>(d);
      for(uint32_t k = 0; k < fn.params.size() && k < info.slot.size(); ++k) {
        node_id at = info.kind >= 0 ? node_place(db[n].kind, info.slot[k]) : db.as<n_call>(n).args[info.slot[k]];
        if(at) out.params[at.value] = std::string(db.strings.text(db.as<n_param>(fn.params[k]).name));
      }
      return out;
    }
    if(!db.is<n_call>(n) || !db.is<n_ident>(db.as<n_call>(n).callee)) return std::nullopt;
    auto const & c = db.as<n_call>(n);
    auto chosen = session.call_targets.contains(n.value) ? session.call_targets.at(n.value) : std::vector<uint32_t>{};
    auto fns = declarations(db.strings.text(db.as<n_ident>(c.callee).name));
    std::ranges::stable_sort(fns, {}, [&](node_id f) { return !std::ranges::contains(chosen, f.value); });
    for(node_id f: fns) {
      if(!construction(f)) continue;
      rule_of out{f, "конструкция", {}};
      auto const & fn = db.as<n_fn>(f);
      // arguments - parameters in order inside the head and each clause
      std::map<uint32_t, uint32_t> used; // Clause → taken parameters
      for(uint32_t i = 0; i < c.args.size(); ++i) {
        symbol w = i < c.words.size() ? c.words[i] : symbol{};
        uint32_t seen = 0;
        for(node_id pa: fn.params) {
          auto const & x = db.as<n_param>(pa);
          if(x.clause != w) continue;
          if(seen++ < used[w.value] && !x.pack) continue;
          std::string name(db.strings.text(x.name));
          out.params[c.args[i].value] = w ? cat(db.strings.text(w), ": ", name) : name;
          if(!x.pack) ++used[w.value];
          break;
        }
      }
      return out;
    }
    return std::nullopt;
  }

  void handle(peer & p, proto::c_inspect const & m) {
    editor_view * v = find(p, m.view);
    if(!v) return send(p, proto::s_error{cat("нет представления ", m.view)});
    auto const & db = session.db;
    if(!m.node || m.node.value >= db.size()) return send(p, proto::s_inspect{v->id, {}, {}, {}, {}});
    // root - an element of the module (in the representation of the function - it)
    node_id root = m.node;
    while(db[root].parent && root != v->root && !db.is<n_module>(db[root].parent)) root = db[root].parent;
    printer pr(db);
    proto::s_inspect out{v->id, root, m.node, {}, {}};
    std::map<uint32_t, std::string> role;
    auto rec = [&](auto && self, node_id n, uint32_t depth) -> void {
      if(out.nodes.size() >= 2000) return;
      proto::s_inode x{n, depth, role.contains(n.value) ? role[n.value] : std::string{}, std::string(kind_name(db[n])), own_text(n), {}, {}, {}};
      auto r = rule_for(n, pr);
      if(r) {
        x.rule = cat(r->what, " ", head_text(r->decl));
        x.rule_at = r->decl;
      } else if(db.is<n_call>(n)) // function call - selected in instance overload
        if(auto it = session.call_targets.find(n.value); it != session.call_targets.end() && !it->second.empty()) {
          x.rule = cat("вызов ", head_text(node_id(it->second[0])));
          x.rule_at = node_id(it->second[0]);
        }
      x.types = shown_node(n);
      out.nodes.push_back(std::move(x));
      for(auto const & [c, name]: child_roles(n)) {
        bool param = r && r->params.contains(c.value);
        role[c.value] = !param ? name : r->params[c.value] == name ? name : cat(name, " → ", r->params[c.value]);
        self(self, c, depth + 1);
      }
    };
    rec(rec, root, 0);
    // Disclosure: focus or the nearest node above with the rule
    for(node_id n = m.node; n; n = db[n].parent) {
      auto r = rule_for(n, pr);
      if(!r) {
        if(n == root) break;
        continue;
      }
      auto const & fn = db.as<n_fn>(r->decl);
      out.expand.push_back({r->what, head_text(r->decl), r->decl});
      std::map<std::string, node_id> args;
      for(auto const & [c, name]: child_roles(n))
        if(r->params.contains(c.value)) {
          out.expand.push_back({"параметр", cat(r->params[c.value], " = ", printer(db).print(c)), c});
          auto bare = r->params[c.value];
          if(auto colon = bare.find(": "); colon != std::string::npos) bare = bare.substr(colon + 2);
          args[bare] = c;
        }
      if(r->what == "запись") { // summation
        std::string fields;
        for(auto const & [c, name]: child_roles(n)) fields += cat(fields.empty() ? "" : ", ", name, ": ", printer(db).print(c));
        out.expand.push_back({"итог", cat(kind_name(db[n]), "{", fields, "}"), {}});
      } else if(fn.body) { // frame-up
        printer::substitution sub;
        sub.text = [&](node_id x) -> std::optional<std::string> {
          if(!db.is<n_ident>(x)) return std::nullopt;
          auto it = args.find(std::string(db.strings.text(db.as<n_ident>(x).name)));
          if(it == args.end()) return std::nullopt;
          std::string t = printer(db).print(it->second);
          node_id up = db[x].parent; // in parentheses (condition, argument) - without their
          bool bracketed = up && (db.is<n_if>(up) || db.is<n_call>(up) || db.is<n_loop>(up));
          return db.is<n_op>(it->second) && !bracketed ? cat("(", t, ")") : t;
        };
        out.expand.push_back({"раскрытие", printer(db, {}, sub).print(fn.body), {}});
      }
      break;
    }
    send(p, out);
  }

  // --- complement ---

  static bool ident_char(char c) { return lexer::is_ident(c); }

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
    if(edit_raw(v, a, b, ins, caret)) return;
    if(edit_line(v, a, b, ins, caret)) return;
    for(auto const & u: units_around(v, a, b)) {
      std::string_view old = v.text_of(u.range);
      uint32_t base = v.byte_at[u.range.first];
      std::string edited = cat(old.substr(0, v.byte_at[a] - base), ins, old.substr(v.byte_at[b] - base));
      parser pr(session.db, edited, cat("<ввод #", edits, ">"));
      // parsing Rules: Contexts around the unit declared before it (§4.4)
      if(u.kind != 2) pr.place(session.db[u.node].parent, u.node);
      else pr.place({});
      projection_rules(pr, v);
      node_id fresh = u.kind == 2 ? pr.parse_module() : pr.parse_unit(u.kind == 0);
      if(!fresh || (u.kind == 0 && !session.db.is<n_block>(fresh))) continue;
      if(u.kind != 2) { // f(…) is a reference to the module function
        node_list items = session.db.as<n_module>(session.root).items;
        pr.resolve_refs(items);
      }
      node_id target = u.node;
      // The module element declares other parsing rules - the following are redefined
      bool rules_changed = u.kind == 1 && parser::rule_trace(session.db, target) != parser::rule_trace(session.db, fresh);
      // block - the body of the structure, and the rules in it are different - reexamination of its calls
      bool body_rules = u.kind == 0 && items_trace(target) != items_trace(fresh);
      auto range = [&](editor_view const & w) { return w.range[w.layout_of(target)]; };
      auto mutate = [&] {
        node_kind k = session.db[fresh].kind;
        if(target == session.root) { // Module from projection - internal elements are saved
          auto & nm = std::get<n_module>(k);
          for(node_id x: inner_items()) nm.items.push_back(x);
          nm.inner = session.db.as<n_module>(session.root).inner;
        }
        session.db.replace(target, std::move(k));
        session.db.adopt_notes(fresh, target); // Commentary on the new text - the preserved id
        if(rules_changed) {
          auto const & items = session.db.as<n_module>(session.root).items;
          reparse_after(session.root, std::size_t(std::ranges::find(items, target) - items.begin()) + 1);
        }
        if(body_rules) reparse_users(target);
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

  // --- raw block: unit of editing - the block itself ---
  //
  // Editing inside the brackets of the raw block (§4.4) reassembles only its phase 1 - atoms and
  // sub-blocks; the node of the block retains id, the result of parsing is a derivative, editing
  // it invalidates (block revision); instruction with a call and neighbors do not touch, only
  // the block leaves customers.
// 
//  
// 
// My unit is the call block.
  // whole (subblock - no: its text is part of the text of the block); the brackets of the block
  // are broken - to the external unit (line), as in the blocks.
  bool edit_raw(editor_view & v, uint32_t a, uint32_t b, std::string const & ins, uint32_t caret) {
    auto & db = session.db;
    for(uint32_t k = 0; k < v.layouts.size(); ++k) {
      node_id n = v.layouts[k].node;
      auto r = v.range[k];
      if(!db.is<n_raw>(n) || !db.as<n_raw>(n).rule || r.second - r.first < 2 || a < r.first + 1 || b > r.second - 1) continue;
      std::string_view old = v.text_of(r);
      uint32_t base = v.byte_at[r.first];
      std::string edited = cat(old.substr(0, v.byte_at[a] - base), ins, old.substr(v.byte_at[b] - base));
      parser pr(db, edited, cat("<ввод #", edits, ">"));
      node_id fresh = pr.parse_raw(db.as<n_raw>(n).rule);
      if(!fresh) return false;
      auto mutate = [&] {
        db.replace(n, db[fresh].kind);
        db.place(n, db[fresh].where); // atoms - in the text of the edit, the place of the block - there
      };
      auto range = [&](editor_view const & w) { return w.range[w.layout_of(n)]; };
      apply(v, n, mutate, range, range, edited, {a - r.first, b - r.first, code_points(ins)}, caret);
      return true;
    }
    return false;
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
    if(count == 0 && in_comment(v, c, k, a)) return false; // Quotation in comments (§4.0) - also in brackets
    auto old = items_range(v, c, k, count, a);
    std::string_view was = v.text_of(old);
    uint32_t base = v.byte_at[old.first];
    std::string edited = count ? cat(was.substr(0, v.byte_at[a] - base), ins, was.substr(v.byte_at[b] - base)) : ins;
    parser pr(db, edited, cat("<ввод #", edits, ">"));
    pr.place(c, k < items.size() ? items[k] : node_id{}); // Rules of Place (§4.4)
    projection_rules(pr, v);
    bool value = false;
    auto fresh = pr.parse_line(module_level, value);
    if(!fresh) return false;
    if(fresh->empty() && !pr.comments.empty()) return false; // The line became a comment - it is at the neighboring element: through brackets
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
    // The line declared other parsing rules (syntax, operator, construction, use) - the
    // context elements are then redefined:
//  
    std::string was_rules, now_rules;
    for(uint32_t i = k; i < k + count; ++i) was_rules += parser::rule_trace(db, items[i]);
    for(node_id x: *fresh) now_rules += parser::rule_trace(db, x);
    bool rules_changed = was_rules != now_rules;
    auto mutate = [&] {
      if(count && first_fresh) {
        db.replace(items[k], db[first_fresh].kind);
        db.adopt_notes(first_fresh, items[k]);
      }
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
      if(rules_changed) {
        reparse_after(c, k + n_new);
        reparse_users(c);
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

  // Place a in the gap before the element k of the container c - inside the comment: in
  // the interval only spaces and comments (§4.0), `//` - to the end of the line.
  bool in_comment(editor_view const & v, node_id c, uint32_t k, uint32_t a) const {
    node_list const items = items_of(session.db, c);
    uint32_t from = k > 0 ? v.range[v.layout_of(items[k - 1])].second : (c == session.root ? 0 : v.range[v.layout_of(c)].first + 1);
    std::string_view t = std::string_view(v.text).substr(v.byte_at[from], v.byte_at[a] - v.byte_at[from]);
    bool line = false, block = false;
    for(std::size_t i = 0; i < t.size(); ++i) {
      if(line) line = t[i] != '\n';
      else if(block) block = !(t[i] == '*' && i + 1 < t.size() && t[i + 1] == '/' && ++i);
      else if(t.substr(i).starts_with("//")) line = true;
      else if(t.substr(i).starts_with("/*")) block = true;
    }
    return line || block;
  }

  std::string items_trace(node_id block) const {
    std::string out;
    for(node_id x: session.db.as<n_block>(block).items) out += parser::rule_trace(session.db, x);
    return out;
  }
  // The rules of the body of the design are the rules of its blocks (§4.4): editing in the
  // body re-examines the elements of the context of the ad after it - there its calls (the
  // call above the ad is not understood by the design).
  void reparse_users(node_id body) {
    auto & db = session.db;
    node_id f = db[body].parent;
    if(!f || !db.is<n_fn>(f) || db.as<n_fn>(f).body != body) return;
    node_id c = db[f].parent;
    if(!c || !db.is_context(c)) return;
    auto const & items = db.context_items(c);
    auto at = std::ranges::find(items, f);
    if(at != items.end()) reparse_after(c, std::size_t(at - items.begin()) + 1);
  }

  // The parsing rule has changed - elements of the context c from the number from from from are
  // rediscovered from their text according to the rules of their place, as in full parsing (§4.4):
  // the element retains id; broke up into several - they stand in its place (c is updated).
  node_list reparsed; // Redisassembled outside the editing unit - their projection is also sent out
  void reparse_after(node_id c, std::size_t from) {
    auto & db = session.db;
    bool module_level = db.is<n_module>(c);
    for(std::size_t k = from; k < db.context_items(c).size(); ++k) {
      node_id x = db.context_items(c)[k];
      bool last = k + 1 == db.context_items(c).size();
      bool was_value = !module_level && last && db.as<n_block>(c).has_value;
      printer pp(db);
      std::string text = pp.print(x);
      if(!was_value) {
        pp.out.clear();
        pp.terminate(x);
        text += pp.out;
      }
      parser pr(db, text, cat("<переразбор #", edits, ">"));
      pr.place(c, x);
      bool value = false;
      auto fresh = pr.parse_line(module_level, value);
      if(!fresh || fresh->empty()) continue;
      if(module_level) { // f(…) is a reference to the module function
        node_list all = db.as<n_module>(c).items;
        all.insert(all.end(), fresh->begin(), fresh->end());
        pr.resolve_refs(all);
      }
      db.replace(x, db[(*fresh)[0]].kind);
      db.adopt_notes((*fresh)[0], x);
      reparsed.push_back(x);
      if(fresh->size() == 1) continue;
      // disintegrated: the rest - in context after (the value of the block - as it was)
      node_list items = db.context_items(c);
      items.insert(items.begin() + std::ptrdiff_t(k) + 1, fresh->begin() + 1, fresh->end());
      if(module_level) {
        n_module m = db.as<n_module>(c);
        m.items = items;
        if(!m.blank.empty()) m.blank.insert(m.blank.begin() + std::ptrdiff_t(k) + 1, fresh->size() - 1, 0);
        db.replace(c, std::move(m));
      } else {
        n_block b = db.as<n_block>(c);
        b.items = items;
        if(!b.blank.empty()) b.blank.insert(b.blank.begin() + std::ptrdiff_t(k) + 1, fresh->size() - 1, 0);
        db.replace(c, std::move(b));
      }
      if(!std::ranges::contains(reparsed, c)) reparsed.push_back(c);
      k += fresh->size() - 1;
    }
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
    reparsed.clear();
    mutate();
    session.db.link_parents(session.root);
    // elaboration - to layout: node, modified by the ct code (and rollback of its mutations), -
    // in the same projection as editing
    recheck(false);
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
      // Disassembled out of place change their text (the hole loses the value of the block): one
      // update of the common ancestor - the client does not see the intermediate mixture
      node_id top = target;
      for(node_id x: reparsed)
        while(top && !within(x, top) && session.db[top].parent) top = session.db[top].parent;
      for(uint32_t x: session.db.changed) // modified by elaboration in the same update
        if(v.layout_of(node_id(x)) != ~uint32_t{0})
          while(top && !within(node_id(x), top) && session.db[top].parent) top = session.db[top].parent;
      if(top != target && v.layout_of(top) != ~uint32_t{0}) {
        send(*b.p, proto::s_update{v.id, session.db.revision, top, subtree(v, top)});
        if(&v != &av) close_completion(*b.p, v);
        send_cursor(*b.p, v);
        continue;
      }
      send(*b.p, proto::s_update{v.id, session.db.revision, target, subtree(v, target)});
      // f(…) { … } prints the name and signature of the target: editing the target changes its projection
      for(node_id e: fn_extensions())
        if(v.layout_of(e) != ~uint32_t{0} && within(node_id(session.db.as<n_extend>(e).target), target) && !within(e, target))
          send(*b.p, proto::s_update{v.id, session.db.revision, e, subtree(v, e)});
      // Redisassembled under the new parsing rules (after corrected announcement)
      for(node_id x: reparsed)
        if(v.layout_of(x) != ~uint32_t{0} && !within(x, target) && !std::ranges::any_of(reparsed, [&](node_id y) { return y != x && within(x, y) && !within(y, target); }))
          send(*b.p, proto::s_update{v.id, session.db.revision, x, subtree(v, x)});
      if(&v != &av) close_completion(*b.p, v); // crossword
      send_cursor(*b.p, v);
    }
    refresh_changed(); // representations which the amendment did not concern
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
    finalize_peer(p, v);
    auto t = place_of(*v, m.node, m.piece, m.offset);
    if(!t) return send(p, proto::s_error{cat("нет места #", m.node.value, "/", m.piece, "/", m.offset)});
    move_to(p, *v, *t, false);
  }

  // --- Checking tags: Types and errors in place ---
  //
  // After each revision, a reactive check session (only affected instance, §10.1 are
  // counted); node types are counted across all instance contexts. As in KDevelop; an
  // error is counted at a place of use with a instantiation (§13.4) track.
  //

  uint64_t checked_revision = ~uint64_t{0};

  void recheck(bool refresh = true) {
    if(checked_revision != session.db.revision) {
      session.check();
      checked_revision = session.db.revision;
    }
    if(refresh) refresh_changed();
  }

  // node modified by ct code mutations and their rollback (check elaboration, source
  // editing, §7.4): representations where they are visible and whose text has changed are
  // updated - the editor shows the result of elaboration
//.
  void refresh_changed() {
    auto & db = session.db;
    if(db.changed.empty()) return;
    std::vector<uint32_t> xs = std::move(db.changed);
    db.changed.clear();
    for(auto & p: peers)
      for(auto & v: p.views) {
        if(!v.root) continue;
        std::vector<node_id> shown;
        for(uint32_t x: xs)
          if(v.layout_of(node_id(x)) != ~uint32_t{0} && !std::ranges::contains(shown, node_id(x))) shown.push_back(node_id(x));
        if(shown.empty()) continue;
        std::string was = v.text;
        uint32_t cur = v.cursor;
        caret at = v.places[cur];
        layout(v); // cursor - in the same place node
        v.cursor = std::min<uint32_t>(cur, uint32_t(v.places.size() - 1));
        for(uint32_t k = 0; k < v.places.size(); ++k)
          if(v.places[k].node == at.node && v.places[k].piece == at.piece && v.places[k].offset == at.offset) v.cursor = k;
        if(v.text == was) continue;
        for(node_id x: shown)
          if(v.layout_of(x) != ~uint32_t{0} && !std::ranges::any_of(shown, [&](node_id y) { return y != x && within(x, y); }))
            send(p, proto::s_update{v.id, db.revision, x, subtree(v, x)});
        send_cursor(p, v);
      }
  }

  // Name types in the domain: all its mentions in the subtree (without nested functions)
  // and initializers `name = e` . what are node types (node_types ) or their compile
  // values (node_values )
  std::vector<std::string> types_of_name(node_id scope, symbol name, std::map<uint32_t, std::vector<std::string>> const * what = nullptr) const {
    if(!what) what = &session.node_types;
    std::vector<std::string> out;
    auto take = [&](node_id n) {
      if(auto it = what->find(n.value); it != what->end())
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

  // Type - two parts: runtime and compiltime: "i64 = 3", "f64 = 1.5 | i64 = 2", "i64 = 1 | 4".
  // The value of runtime in at least one instance of this type (or values greater than four) is
  // only type. vs is "type\literal" (node_values).
  static std::string shown(std::vector<std::string> ts, std::vector<std::string> const & vs) {
    if(vs.empty()) return join_types(std::move(ts));
    std::map<std::string, std::vector<std::string>> by; // type → values (" - runtime )
    for(auto const & x: vs) {
      auto tab = x.find('\t');
      by[x.substr(0, tab)].push_back(tab == std::string::npos ? std::string() : x.substr(tab + 1));
    }
    std::string out;
    for(auto & [t, values]: by) {
      std::string one = t;
      if(!std::ranges::contains(values, std::string()) && values.size() <= 4) {
        std::ranges::sort(values);
        std::string v;
        for(auto const & x: values) v += (v.empty() ? "" : " | ") + x;
        one = cat(t, " = ", v);
      }
      out += (out.empty() ? "" : " | ") + one;
    }
    return out;
  }
  std::string shown_name(node_id scope, symbol name) const {
    return shown(types_of_name(scope, name), types_of_name(scope, name, &session.node_values));
  }
  std::string shown_node(node_id n) const {
    auto t = session.node_types.find(n.value);
    auto v = session.node_values.find(n.value);
    return shown(t == session.node_types.end() ? std::vector<std::string>{} : t->second, v == session.node_values.end() ? std::vector<std::string>{} : v->second);
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
      std::string ts;
      node_id at = n;
      if(db.is<n_param>(n)) ts = shown_name(scope_of(n), db.as<n_param>(n).name);
      else if(db.is<n_var>(n)) ts = shown_name(scope_of(n), db.as<n_var>(n).name);
      else if(db.is<n_op>(n) && db.strings.text(db.as<n_op>(n).op) == "=" && db.as<n_op>(n).args.size() == 2 && db.is<n_ident>(db.as<n_op>(n).args[0])) {
        // `x = e` announces if the name is not yet available (author's decision: no auto)
        at = db.as<n_op>(n).args[0];
        symbol name = db.as<n_ident>(at).name;
        bool known = false;
        for(auto const & c: context_identifiers(db, n))
          if(c.name == db.strings.text(name) && (c.kind == "переменная" || c.kind == "параметр")) known = true;
        if(known) continue;
        ts = shown_name(scope_of(n), name);
      } else continue;
      if(!ts.empty()) out.push_back({at, "type", ts});
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
      if(!n) continue;
      auto [from, to] = raw_range(n, d.where);
      out.push_back({n, "error", text, from, to});
    }
    return out;
  }

  // The place of error inside the raw block (§4.4): its projection is the text as written, the
  // positions of the atoms are in this text, so the label indicates the block - the atom and the
  // displacement in it *My:* tag node - the block (unit projection and edits), the place - the
  // code points [from, to] its text; {0, 0} - the entire node.
  //
  std::pair<uint32_t, uint32_t> raw_range(node_id n, span w) const {
    auto const & db = session.db;
    if(!db.is<n_raw>(n)) return {0, 0};
    span b = db[n].where;
    std::string_view t = db.as<n_raw>(n).text;
    if(w.source != b.source || w.begin < b.begin || w.end < w.begin || w.end - b.begin > t.size()) return {0, 0};
    if(w.begin == b.begin && w.end - b.begin == t.size()) return {0, 0};
    uint32_t from = code_points(t.substr(0, w.begin - b.begin)), to = code_points(t.substr(0, w.end - b.begin));
    return {from, std::max(to, from + 1)};
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

  // projection presentation (§4.4, variant C - author's decision 2026-10-05): a group of
  // records syntax - module; the presentation prints from his records and disassembles edits
  // according to his rules (print on the record R is disassembled by the same R). The module is
  // searched by key next to the program, like use k.
// *My:* projection is the module, the choice is in the presentation.
  void handle(peer & p, proto::c_projection const & m) {
    editor_view * v = find(p, m.view);
    if(!v) return send(p, proto::s_error{cat("нет представления ", m.view)});
    finalize_peer(p);
    node_id mod;
    if(!m.key.empty()) {
      auto dir = session.dir.empty() ? std::filesystem::path(session.name).parent_path() : std::filesystem::path(session.dir);
      auto errors = preload_uses(session.db, cat("use ", m.key, ";"), dir);
      if(auto it = session.db.strings.index.find(m.key); it != session.db.strings.index.end()) mod = find_module(session.db, it->second);
      if(!mod) return send(p, proto::s_error{cat("нет модуля проекции ", m.key)});
      if(!errors.empty()) return send(p, proto::s_error{cat("проекция ", m.key, ": ", errors.front().what())});
    }
    v->projection = mod;
    v->cursor = 0;
    show_view(p, *v);
  }
  // Rules parsing projection representation - on top of the rules of the place (the last - the first).
  void projection_rules(parser & pr, editor_view const & v) {
    if(!v.projection) return;
    for(node_id x: session.db.as<n_module>(v.projection).items) pr.learn_item(x);
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
        if(session.db.is<n_fn>(f) && session.db.strings.text(session.db.as<n_fn>(f).name) == name && !record_of(session.db, f)) out.push_back(f);
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
    finalize_peer(p);
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
    pr.place(session.root); // internal element at the end of the module
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
    finalize_peer(p); // Launch and Preservation - Accepted Text
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
    finalize_peer(p); // Launch and Preservation - Accepted Text
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
    finalize_peer(p); // Launch and Preservation - Accepted Text
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
    finalize_all();
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
    finalize_all();
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
    bool sel = v.anchor != none && v.anchor != v.cursor && v.anchor < v.places.size();
    if(!sel && !v.selection_sent) return;
    caret a = sel ? v.places[v.anchor] : caret{};
    send(p, proto::s_select{v.id, a.node, a.piece, a.offset});
    v.selection_sent = sel;
  }

  // --- layout ---

  bool is_inner(node_id n) const { return std::ranges::contains(session.db.as<n_module>(session.root).inner, n.value); }

  void layout(editor_view & v) {
    v.layouts.clear();
    // The presentation of the module does not show the internal elements - "not littering"
    printer pr(session.db, v.kind == "module" ? printer::filter([&](node_id n) { return !is_inner(n); }) : printer::filter{});
    pr.layouts = &v.layouts;
    pr.projection = v.projection;
    auto group = draft_group(v);
    if(group) pr.draft = &*group;
    if(v.kind == "fn" && v.draft.open && session.db[v.root].where.source == v.draft.source) pr.verbatim(v.root, session.db.source(v.draft.source), true);
    else pr.print(v.root);
    v.chunk_at = std::move(pr.chunk_at);
    v.content_at = std::move(pr.content_at);
    v.items_end = pr.items_end;
    v.draft_begin = pr.draft_begin;
    v.draft_end = pr.draft_end;
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
