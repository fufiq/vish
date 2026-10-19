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
#include "editor_lib.hpp"
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

struct editor_server;
// server whose message is served: the primitives of the editor module (native "cpp")
thread_local editor_server * editor_host = nullptr;

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

  explicit editor_server(vm_session & session) : session(session), editor_module(load_editor(session.db)) {
    if(!ring.ok) throw diagnostic("io_uring недоступен", {});
    intern_operator_entities(session.db);
  }

  // --- solutions - in language (editor module, lib/editor.tz) ---
  //
  //  
// What makes the client signal solves the function in the language; its function with [[editor]] in
  // projection presentation or in the program is the first (closest announcement as operand_parens).
  // Presentation state - here (author's decision 2026-10-19): the language sees it as the primitives of
  // the native "cpp" editor module - via editor_host and now_peer.
  static bool marked_editor(ast_db const & db, node_id f) {
    for(node_id a: db.as<n_fn>(f).attrs)
      if(db.is<n_ident>(a) && db.strings.text(db.as<n_ident>(a).name) == "editor") return true;
    return false;
  }
  node_id editor_fn(editor_view const * v, std::string_view name) const {
    auto const & db = session.db;
    for(node_id m: {v ? v->projection : node_id{}, session.root, editor_module}) {
      if(!m || !db.is<n_module>(m)) continue;
      for(node_id x: db.as<n_module>(m).items)
        if(db.is<n_fn>(x) && db.strings.text(db.as<n_fn>(x).name) == name && (m == editor_module || marked_editor(db, x))) return x;
    }
    return {};
  }
  // Module function interpreter: Editor module - one per server, programs and projection - to edit the base
  interpreter & lang_of(node_id m) {
    for(auto & l: lang_vms)
      if(l.module == m && (m == editor_module || l.revision == session.db.revision)) return *l.vm;
    std::erase_if(lang_vms, [&](lang_vm const & l) { return l.module == m; });
    // elaboration of the program - to its own: when operating, its own does not roll back the mutations of the ct program
    recheck(false);
    lang_vms.push_back({m, session.db.revision, std::make_unique<interpreter>(session.db, m, std::vector<std::string>{}, mode::run)});
    lang_vms.back().vm->detached = m == editor_module; // Editor code does not touch the program
    return *lang_vms.back().vm;
  }
  // Editor’s solution: name function in language (in representation v - its projection; without
  // representation - client commands: launch, base)
  void decide(peer & p, editor_view * v, std::string_view name, std::vector<value> args) {
    node_id f = editor_fn(v, name);
    if(!f) return send(p, proto::s_error{cat("нет функции редактора ", name)});
    node_id m = f;
    while(m && !session.db.is<n_module>(m)) m = session.db[m].parent;
    struct host_hold {
      editor_server * host;
      peer * was_peer;
      editor_server * was_host = std::exchange(editor_host, host);
      ~host_hold() {
        editor_host = was_host;
        host->now_peer = was_peer;
      }
    } hold{this, std::exchange(now_peer, &p)};
    try {
      not_an_edit quiet{*this};
      lang_of(m).analyze_instance(f, std::move(args));
    } catch(diagnostic const & d) { // Error of solution to the client language, with a place in editor.tz
      send(p, proto::s_error{cat("редактор: ", render(d, "editor.tz", session.db))});
    }
  }
  // The editor code in the program language does not rule, but parsing and execution give rise to derivatives
  // of node (base revision is growing): no editing - program verification is not repeated (retakes instance
  // from the cache and would lose layers of nested calls). Program editing - db.edits.
  struct not_an_edit {
    editor_server & s;
    uint64_t edits = s.session.db.edits;
    bool checked = s.checked_revision == s.session.db.revision;
    ~not_an_edit() {
      if(checked && s.session.db.edits == edits) s.checked_revision = s.session.db.revision;
    }
  };
  // command without representation (launch, base): projection - at the first representation of the client, if there is
  editor_view * any_view(peer & p) {
    for(auto & w: p.views)
      if(w.root) return &w;
    return nullptr;
  }
  // representation of the customer whose message is served
  editor_view & view_now(int64_t id) {
    if(now_peer)
      if(editor_view * w = find(*now_peer, uint32_t(id))) return *w;
    throw diagnostic(cat("нет представления ", id), {});
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
    not_an_edit quiet{*this};
    lang_of(editor_module).elaborate(); // Editor’s decisions - ready for the first signal (after parsing)
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

  // The key is the solution in language (on_key of the editor module); the steps of the track are still here.
  void handle(peer & p, proto::c_key const & m) {
    editor_view * v = find(p, m.view);
    if(!v) return send(p, proto::s_error{cat("нет представления ", m.view)});
    decide(p, v, "on_key", {value(int64_t(v->id)), value(m.key), value(int64_t(m.mods))});
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


  // The cursor is moved (the solution is in the language, move_to of the editor module): the word set is
  // over, the addition is closed; outside the draft - the canon; the cursor and the hint - to the client.
  void cursor_moved(peer & p, editor_view & v) {
    v.typing = false;
    close_completion(p, v);
    if(auto r = draft_region(v); r && !(r->contains(v.cursor) && (v.anchor == none || r->contains(v.anchor)))) finalize(p, v);
    else send_cursor(p, v);
  }
  // code node; code() is empty
  static node_id node_or_none(interpreter & vm, value const & c, span where) {
    if(c.is<agg_v>() && !c.as<agg_v>().fields.empty() && (!c.as<agg_v>().fields[0].second.is<int64_t>() || !node_id(uint32_t(c.as<agg_v>().fields[0].second.as<int64_t>())))) return {};
    return vm.code_node(c, where);
  }
  // Instruction lines in the language (line) - in the protocol
  std::vector<proto::s_line> wire_lines(interpreter & vm, value const & xs, span where) {
    auto const & db = session.db;
    auto field = [&](value const & x, std::string_view f) -> value const & {
      for(auto const & [k, fv]: x.as<agg_v>().fields)
        if(db.strings.text(k) == f) return fv;
      throw diagnostic(cat("line: нет поля ", f), where);
    };
    std::vector<proto::s_line> out;
    for(uint32_t k = 0; k < xs.as<array_v>().size(); ++k) {
      value x = xs.as<array_v>().get(k);
      out.push_back({std::string(field(x, "kind").as<str_v>().view()), std::string(field(x, "text").as<str_v>().view()), node_or_none(vm, field(x, "node"), where), uint32_t(field(x, "from").as<int64_t>()), uint32_t(field(x, "to").as<int64_t>())});
    }
    return out;
  }
  void hide_hint(peer & p, editor_view & v) {
    if(!v.hint_shown) return;
    v.hint_shown = false;
    send(p, proto::s_hint{v.id, {}, 0, 0, {}});
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

  // Text input, click, pointer - solutions in language (on_input, on_select, on_pointer)
  void handle(peer & p, proto::c_input const & m) {
    editor_view * v = find(p, m.view);
    if(!v) return send(p, proto::s_error{cat("нет представления ", m.view)});
    decide(p, v, "on_input", {value(int64_t(v->id)), value(m.text)});
  }

  void handle(peer & p, proto::c_select const & m) {
    editor_view * v = find(p, m.view);
    if(!v) return send(p, proto::s_error{cat("нет представления ", m.view)});
    auto t = place_of(*v, m.node, m.piece, m.offset);
    if(!t) {
      finalize_peer(p, v);
      return send(p, proto::s_error{cat("нет места #", m.node.value, "/", m.piece, "/", m.offset)});
    }
    decide(p, v, "on_select", {value(int64_t(v->id)), value(int64_t(*t)), value(int64_t(m.clicks))});
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


  // Construction: clauses, expression parameters or blocks, syntax recording - it has a form.
  bool construction(node_id f) const {
    auto const & fn = session.db.as<n_fn>(f);
    if(!fn.form.empty()) return true;
    return std::ranges::any_of(fn.params, [&](node_id p) {
      auto const & x = session.db.as<n_param>(p);
      return x.clause || x.mode == param_mode::expr;
    });
  }

  void handle(peer & p, proto::c_hover const & m) {
    editor_view * v = find(p, m.view);
    if(!v) return send(p, proto::s_error{cat("нет представления ", m.view)});
    decide(p, v, "on_hover", {value(int64_t(v->id)), m.node && m.node.value < session.db.size() ? lang_of(editor_module).code_value(m.node) : lang_of(editor_module).code_value(node_id{})});
  }

  void handle(peer & p, proto::c_open const & m) {
    editor_view * v = find(p, m.view);
    if(!v) return send(p, proto::s_error{cat("нет представления ", m.view)});
    if(!m.node || m.node.value >= session.db.size()) return send(p, proto::s_error{"нет такого узла"});
    decide(p, v, "on_open", {value(int64_t(v->id)), lang_of(editor_module).code_value(m.node)});
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
        std::string name(lang_field(std::meta::identifier_of(m))); // field-name
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

  // Inspector - solution in language (on_inspect editor module)
  void handle(peer & p, proto::c_inspect const & m) {
    editor_view * v = find(p, m.view);
    if(!v) return send(p, proto::s_error{cat("нет представления ", m.view)});
    interpreter & lang = lang_of(editor_module);
    decide(p, v, "on_inspect", {value(int64_t(v->id)), lang.code_value(m.node && m.node.value < session.db.size() ? m.node : node_id{})});
  }

  // --- complement ---

  static bool ident_char(char c) { return lexer::is_ident(c); }

  // Supplement - in language (complete editor module)
  void complete(peer & p, editor_view & v, bool forced) { decide(p, &v, "complete", {value(int64_t(v.id)), value(forced)}); }

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
    auto t = place_of(*v, m.node, m.piece, m.offset);
    if(!t) {
      finalize_peer(p, v);
      return send(p, proto::s_error{cat("нет места #", m.node.value, "/", m.piece, "/", m.offset)});
    }
    decide(p, v, "on_pointer", {value(int64_t(v->id)), value(int64_t(*t))});
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
        // one update is a common ancestor of the changed (the client does not see an intermediate mixture)
        node_id top;
        for(node_id x: shown) {
          if(v.layout_of(x) == ~uint32_t{0}) continue;
          if(!top) top = x;
          while(top && !within(x, top)) top = db[top].parent;
        }
        while(top && v.layout_of(top) == ~uint32_t{0}) top = db[top].parent;
        if(!top) top = v.root;
        send(p, proto::s_update{v.id, db.revision, top, subtree(v, top)});
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

  // Tags and layers - solution in language (send_marks editor module)
  void send_marks(peer & p, editor_view const & v) { decide(p, const_cast<editor_view *>(&v), "send_marks", {value(int64_t(v.id))}); }

  // projection presentation (§4.4, variant C - author's decision 2026-10-05): a group of
  // records syntax - module; the presentation prints from his records and disassembles edits
  // according to his rules (print on the record R is disassembled by the same R). The module is
  // searched by key next to the program, like use k.
// *My:* projection is the module, the choice is in the presentation.
  void handle(peer & p, proto::c_projection const & m) {
    editor_view * v = find(p, m.view);
    if(!v) return send(p, proto::s_error{cat("нет представления ", m.view)});
    decide(p, v, "on_projection", {value(int64_t(v->id)), value(m.key)});
  }
  // projection key module (like use k next to the program)
  std::string set_projection(editor_view & v, std::string const & key) {
    node_id mod;
    if(!key.empty()) {
      auto dir = session.dir.empty() ? std::filesystem::path(session.name).parent_path() : std::filesystem::path(session.dir);
      auto errors = preload_uses(session.db, cat("use ", key, ";"), dir);
      if(auto it = session.db.strings.index.find(key); it != session.db.strings.index.end()) mod = find_module(session.db, it->second);
      if(!mod) return cat("нет модуля проекции ", key);
      if(!errors.empty()) return cat("проекция ", key, ": ", errors.front().what());
    }
    v.projection = mod;
    return {};
  }
  // Rules parsing projection representation - on top of the rules of the place (the last - the first).
  void projection_rules(parser & pr, editor_view const & v) {
    if(!v.projection) return;
    for(node_id x: session.db.as<n_module>(v.projection).items) pr.learn_item(x);
  }

  void handle(peer & p, proto::c_layer const & m) {
    editor_view * v = find(p, m.view);
    if(!v) return send(p, proto::s_error{cat("слои — у представления функции")});
    decide(p, v, "on_layer", {value(int64_t(v->id)), value(m.key)});
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

  // Ctrl+P - solution in language (on_goto editor module)
  void handle(peer & p, proto::c_goto const & m) {
    editor_view * v = find(p, m.view);
    if(!v) return send(p, proto::s_error{cat("нет представления ", m.view)});
    decide(p, v, "on_goto", {value(int64_t(v->id)), value(m.text)});
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

  // Add the internal element of the module (text - from the solution in the language, add_overload):
  // parsing, the element - at the end of the module, internal; tags of the module, open in the second panel.
  void add_inner(peer & p, std::string const & text, std::string const & sig) {
    auto & db = session.db;
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

  // start, save - solutions in language (on_run, on_save editor module)
  void handle(peer & p, proto::c_run const & m) {
    decide(p, any_view(p), "on_run", {value(m.args), value(m.stdin_text)});
  }
  void handle(peer & p, proto::c_save const &) {
    decide(p, any_view(p), "on_save", {});
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

  // Launch track (F6) - solution in language (on_trace_run)
  void handle(peer & p, proto::c_trace const & m) {
    editor_view * v = any_view(p);
    decide(p, v, "on_trace_run", {value(int64_t(v ? int64_t(v->id) : -1)), value(m.args), value(m.stdin_text)});
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

  // merger:
// conflicts - before a human decision; "" - merged, otherwise a mistake
  std::string merge_from(std::string const & path) {
    node_id theirs_root;
    std::vector<journal_entry> theirs;
    try {
      theirs = read_journal(path, theirs_root);
    } catch(diagnostic const & d) {
      return d.what();
    }
    pending = merge_into(session.db, session.root, session.db.journal(), theirs, theirs_root);
    refresh_all();
    send_merge(*now_peer, pending.taken);
    return {};
  }
  bool take_version(std::string const & key, bool theirs) {
    auto it = std::ranges::find(pending.conflicts, key, &merge_conflict::key);
    if(it == pending.conflicts.end()) return false;
    if(theirs) {
      take_theirs(session.db, session.root, pending.base_items, it->id, it->theirs_exists ? std::optional<std::string>(it->theirs) : std::nullopt);
      refresh_all();
    }
    pending.conflicts.erase(it);
    send_merge(*now_peer, {});
    return true;
  }
  // Function editing, merging, conflict - language solutions (on_apply, on_merge, on_take)
  void handle(peer & p, proto::c_apply const & m) {
    decide(p, any_view(p), "on_apply", {value(m.fn)});
  }
  void handle(peer & p, proto::c_merge const & m) {
    decide(p, any_view(p), "on_merge", {value(m.path)});
  }
  void handle(peer & p, proto::c_take const & m) {
    decide(p, any_view(p), "on_take", {value(m.key), value(m.theirs)});
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
  node_id editor_module; // Editor's decisions - in the language (lib/editor.tz)
  struct lang_vm {
    node_id module;
    uint64_t revision;
    std::unique_ptr<interpreter> vm;
  };
  std::vector<lang_vm> lang_vms; // Interpreters of Editor Functions - Module by Module
  peer * now_peer = nullptr; // Customer whose message is being served
  std::map<std::string, value> memo; // Memorized by the Editor's Decisions (remember)
  uint32_t edits = 0;
  os::uring ring;
  int listen_fd = -1;
  std::deque<peer> peers; // Deque: Customer links are stable
};

} // namespace tz

// out/native.hpp (tz native, build.sh): the native “cpp” prelude body and the editor module after
// the server, its primitives to editor_host
#include "native.hpp"
