// Editor server: VM is a single state, editors are clients (§13.1, M5).
//
// 
// 
// Connections - Unix socket, events - stream from io_uring (multishot
// accept/recv, like the network in M3):
//
//
// Solutions and state of views - in language (editor module, lib/editor.tz - VM service, user
// solutions 2026-10-19/20:). Here are the socket, the frames, the set pause timer and the primitives
// of the language base that the solutions use: printing projection layout-value, parsing editing the
// unit in the database, sending to the client. .
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

// The length of the UTF-8 code point per byte k.
std::size_t cp_len(std::string_view s, std::size_t k) {
  std::size_t n = 1;
  while(k + n < s.size() && (static_cast<unsigned char>(s[k + n]) & 0xC0) == 0x80) ++n;
  return n;
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
  };

  explicit editor_server(vm_session & session) : session(session), editor_module(load_editor(session.db)) {
    if(!ring.ok) throw diagnostic("io_uring недоступен", {});
    intern_operator_entities(session.db);
  }

  // --- solutions - in language (editor module, lib/editor.tz) ---
  //
  //  
// What makes the client signal solves the function in the language; its function with [[editor]] in
  // projection presentation or in the program is the first (closest announcement as operand_parens). The
  // state of views is the variables of the editor module (VM service, author's solution 2026-10-20); the
  // primitives of the native "cpp" module are via editor_host and now_peer.
  static bool marked_editor(ast_db const & db, node_id f) {
    for(node_id a: db.as<n_fn>(f).attrs)
      if(db.is<n_ident>(a) && db.strings.text(db.as<n_ident>(a).name) == "editor") return true;
    return false;
  }
  node_id fn_in(node_id m, std::string_view name, bool marked) const {
    auto const & db = session.db;
    if(!m || !db.is<n_module>(m)) return {};
    for(node_id x: db.as<n_module>(m).items)
      if(db.is<n_fn>(x) && db.strings.text(db.as<n_fn>(x).name) == name && (!marked || marked_editor(db, x))) return x;
    return {};
  }
  // name solution function: its own in projection view (its module knows the language), in the program,
  // otherwise the editor module
  node_id editor_fn(int64_t view, std::string_view name) {
    for(node_id m: projections)
      if(fn_in(m, name, true)) {
        value pm = ask("view_projection", {value(view)});
        if(node_id x = node_or_none(lang_of(editor_module), pm, {}); x && x == m) return fn_in(m, name, true);
        break;
      }
    if(node_id f = fn_in(session.root, name, true)) return f;
    return fn_in(editor_module, name, false);
  }
  // Module function interpreter: Editor module - one per server, programs and projection - to edit the base
  interpreter & lang_of(node_id m) {
    for(auto & l: lang_vms)
      if(l.module == m && (m == editor_module || l.revision == session.db.revision)) return *l.vm;
    std::erase_if(lang_vms, [&](lang_vm const & l) { return l.module == m; });
    // elaboration of the program - to its own: when operating, its own does not roll back the mutations of the ct program
    recheck();
    lang_vms.push_back({m, session.db.revision, std::make_unique<interpreter>(session.db, m, std::vector<std::string>{}, mode::run)});
    lang_vms.back().vm->detached = m == editor_module; // Editor code does not touch the program
    return *lang_vms.back().vm;
  }
  // the customer whose message is being served - at the time of the decision
  struct host_hold {
    editor_server * host;
    peer * was_peer;
    editor_server * was_host = std::exchange(editor_host, host);
    ~host_hold() {
      editor_host = was_host;
      host->now_peer = was_peer;
    }
  };
  // Editor's solution: name function in language; view - representation of the client (protocol number; -1
  // - first) whose projection can override the solution
  void decide(peer * p, int64_t view, std::string_view name, std::vector<value> args) {
    host_hold hold{this, std::exchange(now_peer, p)};
    node_id f = editor_fn(view, name);
    if(!f) {
      if(p) send(*p, proto::s_error{cat("нет функции редактора ", name)});
      return;
    }
    node_id m = f;
    while(m && !session.db.is<n_module>(m)) m = session.db[m].parent;
    try {
      not_an_edit quiet{*this};
      lang_of(m).analyze_instance(f, std::move(args));
    } catch(diagnostic const & d) { // Error of solution to the client language, with a place in editor.tz
      if(p) send(*p, proto::s_error{cat("редактор: ", render(d, "editor.tz", session.db))});
      else put(stderr, cat("редактор: ", render(d, "editor.tz", session.db), "\n"));
    }
  }
  // Question Module Editor (function name) - value
  value ask(std::string_view name, std::vector<value> args) {
    host_hold hold{this, now_peer};
    not_an_edit quiet{*this};
    return lang_of(editor_module).analyze_instance(fn_in(editor_module, name, false), std::move(args));
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
  // Draft client representation (correlate scripts .wire: text as typed - language): no - empty.
  std::optional<std::string> draft_text(peer & p, uint32_t view) {
    host_hold hold{this, std::exchange(now_peer, &p)};
    value r = ask("view_draft", {value(int64_t(view))});
    auto const & xs = r.as<array_v>();
    if(!xs.get(0).as<bool>()) return std::nullopt;
    return std::string(xs.get(1).as<str_v>().view());
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
  // client number k (language number: view remembers its client)
  void send_to(int64_t k, proto::server_msg const & m) {
    if(k >= 0 && std::size_t(k) < peers.size()) send(peers[std::size_t(k)], m);
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

  // --- Messages: each - solution in language ---

  void handle(peer & p, proto::c_hello const & m) {
    if(m.version != proto::version) {
      send(p, proto::s_error{cat("версия протокола ", m.version, " не поддерживается (VM: ", proto::version, ")")});
      return;
    }
    send(p, proto::s_hello{proto::version, session.name, session.db.revision});
  }
  void handle(peer & p, proto::c_open_view const & m) {
    if(m.kind != "module") return send(p, proto::s_error{cat("неизвестный вид представления: ", m.kind)});
    decide(&p, -1, "on_open_view", {});
  }
  void handle(peer & p, proto::c_close_view const & m) { decide(&p, m.view, "on_close_view", {value(int64_t(m.view))}); }
  void handle(peer & p, proto::c_key const & m) {
    decide(&p, m.view, "on_key", {value(int64_t(m.view)), value(m.key), value(int64_t(m.mods))});
  }
  void handle(peer & p, proto::c_input const & m) { decide(&p, m.view, "on_input", {value(int64_t(m.view)), value(m.text)}); }
  void handle(peer & p, proto::c_select const & m) {
    decide(&p, m.view, "on_select", {value(int64_t(m.view)), value(int64_t(m.node.value)), value(int64_t(m.piece)), value(int64_t(m.offset)), value(int64_t(m.clicks))});
  }
  void handle(peer & p, proto::c_pointer const & m) {
    decide(&p, m.view, "on_pointer", {value(int64_t(m.view)), value(int64_t(m.node.value)), value(int64_t(m.piece)), value(int64_t(m.offset))});
  }
  value node_arg(node_id n) { return lang_of(editor_module).code_value(n && n.value < session.db.size() ? n : node_id{}); }
  void handle(peer & p, proto::c_hover const & m) { decide(&p, m.view, "on_hover", {value(int64_t(m.view)), node_arg(m.node)}); }
  void handle(peer & p, proto::c_open const & m) {
    if(!m.node || m.node.value >= session.db.size()) return send(p, proto::s_error{"нет такого узла"});
    decide(&p, m.view, "on_open", {value(int64_t(m.view)), node_arg(m.node)});
  }
  void handle(peer & p, proto::c_inspect const & m) { decide(&p, m.view, "on_inspect", {value(int64_t(m.view)), node_arg(m.node)}); }
  void handle(peer & p, proto::c_goto const & m) { decide(&p, m.view, "on_goto", {value(int64_t(m.view)), value(m.text)}); }
  void handle(peer & p, proto::c_layer const & m) { decide(&p, m.view, "on_layer", {value(int64_t(m.view)), value(m.key)}); }
  void handle(peer & p, proto::c_projection const & m) { decide(&p, m.view, "on_projection", {value(int64_t(m.view)), value(m.key)}); }
  void handle(peer & p, proto::c_run const & m) { decide(&p, -1, "on_run", {value(m.args), value(m.stdin_text)}); }
  void handle(peer & p, proto::c_save const &) { decide(&p, -1, "on_save", {}); }
  void handle(peer & p, proto::c_trace const & m) { decide(&p, -1, "on_trace_run", {value(int64_t(-1)), value(m.args), value(m.stdin_text)}); }
  void handle(peer & p, proto::c_apply const & m) { decide(&p, -1, "on_apply", {value(m.fn)}); }
  void handle(peer & p, proto::c_merge const & m) { decide(&p, -1, "on_merge", {value(m.path)}); }
  void handle(peer & p, proto::c_take const & m) { decide(&p, -1, "on_take", {value(m.key), value(m.theirs)}); }

  // --- set pause: OS timer ---
  //
  // Verification and tags - on a pause of 1 s (author's decision 2026-10-12): editing the language asks for verification
  // (schedule_check), the io_uring timer calls the solution on_idle. In tests, the pause is the idle team.

  static constexpr uint32_t pause_ms = 1000;
  bool timers = true; // Tests: pause with idle team, no clock
  uint64_t edited_at = 0; // last-draft
  bool check_pending = false, timer_armed = false;

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
  // Set pause: Check (reactive check) and tag all views - on_idle solution.
  void idle() {
    if(!check_pending) return;
    check_pending = false;
    decide(nullptr, -1, "on_idle", {});
  }

  // --- check-up ---
  //
  // After each audit, reactive check sessions (only affected instance, §10.1 are counted); node
  // types are counted across all instance contexts.

  uint64_t checked_revision = ~uint64_t{0};

  void recheck() {
    if(checked_revision != session.db.revision) {
      session.check();
      checked_revision = session.db.revision;
    }
  }

  // --- Print of presentation: layout of the value (base of language) ---
  //
  // projection is printed by the printer (the syntax is described in one place - syntax, §4.5 entries);
  // the language receives the layout of the whole - pieces of node, text and cursor places - and decides
  // the rest itself (cursor, draft, mailing). Places - in front of each code point; at the junction of
  // pieces, one place (before the next symbol), the end of the text - the last place. *My:* node
  // signatures (my chunks) and subtree (with children) are hashes so that the language compares layouts
  // before and after editing without bypassing the text.

  bool is_inner(node_id n) const { return std::ranges::contains(session.db.as<n_module>(session.root).inner, n.value); }

  static uint64_t mix(uint64_t h, std::string_view s) {
    for(char c: s) h = (h ^ static_cast<unsigned char>(c)) * 0x100000001b3ull;
    return (h ^ 0xFF) * 0x100000001b3ull;
  }
  static uint64_t mix(uint64_t h, uint64_t x) {
    for(int i = 0; i < 8; ++i) h = (h ^ ((x >> (8 * i)) & 0xFF)) * 0x100000001b3ull;
    return h;
  }
  std::string_view note_of_hole(node_id n) const { return session.db.is<n_hole>(n) ? std::string_view(session.db.as<n_hole>(n).reason) : std::string_view(); }

  // Printed: node in order of printing (root first) with chunks and places of module elements; with which
  // module draft printed (elements [group_at, group_at + group_count]; -1 - without).
  struct printed {
    std::vector<node_layout> ls;
    std::vector<uint32_t> chunk_at, content_at;
    uint32_t items_end = 0, draft_begin = 0, draft_end = 0;
    int64_t group_at = 0, group_count = -1;
  };

  // The layout (struct layout of the editor module): the root, the module (otherwise one function),
  // projection, the draft module - elements [g_at, g_at + g_count ) text as typed (g_count -1 - no), the
  // draft function - its text as typed (fn_source -1 - no).
  value print_view(interpreter & vm, node_id root, bool module, node_id projection, int64_t g_at, int64_t g_count, int64_t g_source, int64_t fn_source) {
    auto const & db = session.db;
    printed p;
    // The presentation of the module does not show the internal elements - "not littering"
    printer pr(db, module ? printer::filter([&](node_id n) { return !is_inner(n); }) : printer::filter{});
    pr.layouts = &p.ls;
    pr.projection = projection;
    printer::verbatim_group group{uint32_t(g_at), uint32_t(std::max<int64_t>(g_count, 0)), uint32_t(g_source)};
    if(module && g_count >= 0) pr.draft = &group;
    if(!module && fn_source >= 0 && db[root].where.source == uint32_t(fn_source)) pr.verbatim(root, db.source(uint32_t(fn_source)), true);
    else pr.print(root);
    p.chunk_at = std::move(pr.chunk_at);
    p.content_at = std::move(pr.content_at);
    p.items_end = pr.items_end;
    p.draft_begin = pr.draft_begin;
    p.draft_end = pr.draft_end;
    if(module && g_count >= 0) {
      p.group_at = g_at;
      p.group_count = g_count;
    }
    return layout_value(vm, p);
  }

  // Draft module after editing - print only its area and paste in the former layout lay (printed with a
  // draft [at, at + old_count ), its elements - old):
// 
// The rest of the module is not printed: its pieces are the same, places
  // They're moving. Pieces of the root are glued together like a printer (the text between children is one piece).
  value print_splice(interpreter & vm, value const & lay, node_id root, int64_t at, node_list const & old, int64_t count, int64_t source, span where) {
    auto const & db = session.db;
    auto ints = [&](std::string_view f) -> array_v const & { return field(lay, f, where).as<array_v>(); };
    auto const & nodes = ints("nodes");
    auto const & first = ints("first");
    auto const & pcount = ints("count");
    auto const & pchild = ints("pchild");
    auto const & pbyte = ints("pbyte");
    auto const & from = ints("from");
    auto const & to = ints("to");
    auto const & byte = ints("byte");
    std::string_view ptext = field(lay, "ptext", where).as<str_v>().view();
    uint32_t m = nodes.size();
    std::vector<node_layout> was(m);
    std::unordered_map<uint32_t, uint32_t> idx;
    idx.reserve(m * 2);
    for(uint32_t k = 0; k < m; ++k) {
      was[k].node = node_id(uint32_t(nodes.get(k).as<int64_t>()));
      idx[was[k].node.value] = k;
      int64_t f = first.get(k).as<int64_t>(), c = pcount.get(k).as<int64_t>();
      for(int64_t j = f; j < f + c; ++j) {
        int64_t ch = pchild.get(uint32_t(j)).as<int64_t>();
        if(ch >= 0) was[k].pieces.push_back(node_id(uint32_t(ch)));
        else {
          int64_t b = pbyte.get(uint32_t(j)).as<int64_t>(), e = pbyte.get(uint32_t(j + 1)).as<int64_t>();
          was[k].pieces.push_back(std::string(ptext.substr(std::size_t(b), std::size_t(e - b))));
        }
      }
    }
    // the draft area again: only the text of the set
    printed p;
    std::vector<node_layout> part;
    printer pr(db);
    pr.layouts = &part;
    printer::verbatim_group group{uint32_t(at), uint32_t(count), uint32_t(source)};
    pr.draft = &group;
    std::string region = pr.print_draft_group(root);
    uint32_t ob = uint32_t(field(lay, "draft_begin", where).as<int64_t>()), oe = uint32_t(field(lay, "draft_end", where).as<int64_t>());
    // snippets of the root: to the area, (area), after - bytes of the former text; children - places of their text
    std::vector<piece> pieces;
    auto add = [&](piece pc) {
      if(auto const * t = std::get_if<std::string>(&pc)) {
        if(t->empty()) return;
        if(!pieces.empty())
          if(auto * prev = std::get_if<std::string>(&pieces.back())) {
            *prev += *t;
            return;
          }
      }
      pieces.push_back(std::move(pc));
    };
    std::vector<piece> after;
    uint32_t pos = 0, b0 = 0, b1 = 0;
    bool mid_seen = false, post_seen = false;
    for(auto const & pc: was.empty() ? std::vector<piece>{} : was[0].pieces) {
      if(auto const * t = std::get_if<std::string>(&pc)) {
        uint32_t b = pos, e = pos + uint32_t(t->size());
        if(b < ob) add(t->substr(0, std::min(e, ob) - b));
        if(e > oe) after.push_back(t->substr(std::max(b, oe) - b));
        pos = e;
        continue;
      }
      node_id c = std::get<node_id>(pc);
      uint32_t k = idx.at(c.value);
      uint32_t len = uint32_t(byte.get(uint32_t(to.get(k).as<int64_t>())).as<int64_t>() - byte.get(uint32_t(from.get(k).as<int64_t>())).as<int64_t>());
      if(std::ranges::contains(old, c)) {
        if(!mid_seen) b0 = k;
        mid_seen = true;
      } else if(pos < ob || (pos == ob && !mid_seen && len == 0)) add(c);
      else {
        if(!post_seen) b1 = k;
        post_seen = true;
        after.push_back(c);
      }
      pos += len;
    }
    if(!post_seen) b1 = m;
    if(!mid_seen) b0 = b1;
    for(auto const & pc: part.empty() ? std::vector<piece>{} : part[0].pieces) add(pc);
    for(auto & pc: after) add(std::move(pc));
    // node: root, before the region, after
    p.ls.reserve(m + part.size());
    p.ls.push_back({root, std::move(pieces)});
    for(uint32_t k = 1; k < b0; ++k) p.ls.push_back(std::move(was[k]));
    for(std::size_t k = 1; k < part.size(); ++k) p.ls.push_back(std::move(part[k]));
    for(uint32_t k = b1; k < m; ++k) p.ls.push_back(std::move(was[k]));
    // places of module elements: draft elements - without their place, after the area - with a shift
    int64_t shift = int64_t(region.size()) - int64_t(oe - ob);
    auto moved = [&](array_v const & xs) {
      std::vector<uint32_t> out;
      for(uint32_t k = 0; k < xs.size(); ++k) {
        if(k == uint32_t(at)) out.insert(out.end(), std::size_t(count), ~uint32_t{0});
        if(k >= uint32_t(at) && k < uint32_t(at) + uint32_t(old.size())) continue;
        int64_t x = xs.get(k).as<int64_t>();
        out.push_back(x < 0 ? ~uint32_t{0} : uint32_t(x >= int64_t(oe) ? x + shift : x));
      }
      if(xs.size() <= uint32_t(at)) out.insert(out.end(), std::size_t(count), ~uint32_t{0});
      return out;
    };
    p.chunk_at = moved(ints("chunk_at"));
    p.content_at = moved(ints("content_at"));
    int64_t items_end = field(lay, "items_end", where).as<int64_t>();
    p.items_end = uint32_t(items_end >= int64_t(oe) ? items_end + shift : items_end);
    p.draft_begin = ob;
    p.draft_end = uint32_t(ob + region.size());
    p.group_at = at;
    p.group_count = count;
    return layout_value(vm, p);
  }
  // Reconciliation of the paste with full seal (TZ_SPLICE_CHECK = 1): discrepancy - field and output.
  void splice_check(interpreter & vm, value const & spliced, value const & full) {
    auto const & a = spliced.as<agg_v>().fields;
    auto const & b = full.as<agg_v>().fields;
    for(uint32_t k = 0; k < a.size() && k < b.size(); ++k)
      if(vm.show_v(a[k].second) != vm.show_v(b[k].second)) {
        put(stderr, cat("вклейка черновика расходится с печатью: поле ", session.db.strings.text(a[k].first), "\n  вклейка: ", vm.show_v(a[k].second).substr(0, 400), "\n  печать:  ", vm.show_v(b[k].second).substr(0, 400), "\n"));
        std::abort();
      }
  }

  // Structure layout of the editor module (fields - in order of announcement).
  value layout_value(interpreter & vm, printed const & p) {
    auto const & db = session.db;
    auto const & ls = p.ls;
    uint32_t m = uint32_t(ls.size());
    std::unordered_map<uint32_t, uint32_t> at;
    at.reserve(m * 2);
    for(uint32_t k = 0; k < m; ++k) at[ls[k].node.value] = k;
    std::string text, lead, ptext;
    array_v node, ppiece, poffset, byte, nodes, up, from, to, own, tree, first, count, pchild, pbyte, plen, pplace, sorted, order, chunk_at, content_at;
    std::vector<int64_t> v_up(m, -1), v_from(m, 0), v_to(m, 0);
    std::vector<uint64_t> v_own(m, 0), v_tree(m, 0);
    for(uint32_t k = 0; k < m; ++k) { // their pieces: signature (view, texts, children, note)
      uint64_t h = mix(0xcbf29ce484222325ull, kind_name(db[ls[k].node]));
      first.push(value(int64_t(pchild.size())));
      for(auto const & pc: ls[k].pieces) {
        pbyte.push(value(int64_t(ptext.size())));
        if(auto const * t = std::get_if<std::string>(&pc)) {
          h = mix(mix(h, "t"), *t);
          ptext += *t;
          pchild.push(value(int64_t(-1)));
          plen.push(value(int64_t(code_points(*t))));
        } else {
          h = mix(mix(h, "c"), uint64_t(std::get<node_id>(pc).value));
          pchild.push(value(int64_t(std::get<node_id>(pc).value)));
          plen.push(value(int64_t(0)));
        }
      }
      count.push(value(int64_t(ls[k].pieces.size())));
      v_own[k] = mix(h, note_of_hole(ls[k].node));
      nodes.push(value(int64_t(ls[k].node.value)));
    }
    pbyte.push(value(int64_t(ptext.size())));
    std::vector<int64_t> v_pplace(pchild.size(), 0);
    int64_t last = -1, last_piece = 0, last_len = 0;
    auto walk = [&](auto && self, uint32_t k) -> void {
      node_layout const & l = ls[k];
      v_from[k] = int64_t(node.size());
      uint64_t h = mix(mix(0xcbf29ce484222325ull, uint64_t(l.node.value)), v_own[k]);
      int64_t piece = 0;
      for(auto const & pc: l.pieces) {
        v_pplace[std::size_t(first.get(k).as<int64_t>() + piece)] = int64_t(node.size());
        if(auto const * t = std::get_if<std::string>(&pc)) {
          int64_t o = 0;
          for(std::size_t j = 0; j < t->size(); j += cp_len(*t, j)) {
            node.push(value(int64_t(l.node.value)));
            ppiece.push(value(piece));
            poffset.push(value(o++));
            lead += (*t)[j];
            byte.push(value(int64_t(text.size() + j)));
          }
          text += *t;
          if(o) {
            last = int64_t(l.node.value);
            last_piece = piece;
            last_len = o;
          }
        } else if(auto it = at.find(std::get<node_id>(pc).value); it != at.end()) {
          v_up[it->second] = int64_t(k);
          self(self, it->second);
          h = mix(h, v_tree[it->second]);
        }
        ++piece;
      }
      v_to[k] = int64_t(node.size());
      v_tree[k] = h;
    };
    if(m) walk(walk, 0);
    node.push(value(last)); // end of text after the last symbol of the last empty piece
    ppiece.push(value(last_piece));
    poffset.push(value(last_len));
    lead += '\n';
    byte.push(value(int64_t(text.size())));
    for(int64_t x: v_pplace) pplace.push(value(x));
    for(uint32_t k = 0; k < m; ++k) {
      up.push(value(v_up[k]));
      from.push(value(v_from[k]));
      to.push(value(v_to[k]));
      own.push(value(int64_t(v_own[k])));
      tree.push(value(int64_t(v_tree[k])));
    }
    std::vector<uint32_t> ord(m);
    for(uint32_t k = 0; k < m; ++k) ord[k] = k;
    std::ranges::sort(ord, {}, [&](uint32_t k) { return ls[k].node.value; });
    for(uint32_t k: ord) {
      sorted.push(value(int64_t(ls[k].node.value)));
      order.push(value(int64_t(k)));
    }
    auto place = [](uint32_t x) { return value(x == ~uint32_t{0} ? int64_t(-1) : int64_t(x)); };
    for(uint32_t x: p.chunk_at) chunk_at.push(place(x));
    for(uint32_t x: p.content_at) content_at.push(place(x));
    // fields - in order of struct layout announcement (lib/editor.tz)
    agg_v out{vm.types.named("layout"), {}};
    auto put = [&](std::string_view name, value x) { out.fields.push_back({vm.sym(name), std::move(x)}); };
    put("text", value(std::move(text)));
    put("lead", value(std::move(lead)));
    put("node", value(std::move(node)));
    put("piece", value(std::move(ppiece)));
    put("offset", value(std::move(poffset)));
    put("byte", value(std::move(byte)));
    put("nodes", value(std::move(nodes)));
    put("up", value(std::move(up)));
    put("from", value(std::move(from)));
    put("to", value(std::move(to)));
    put("sig", value(std::move(own)));
    put("sub", value(std::move(tree)));
    put("first", value(std::move(first)));
    put("count", value(std::move(count)));
    put("pchild", value(std::move(pchild)));
    put("pbyte", value(std::move(pbyte)));
    put("plen", value(std::move(plen)));
    put("pplace", value(std::move(pplace)));
    put("ptext", value(std::move(ptext)));
    put("sorted", value(std::move(sorted)));
    put("order", value(std::move(order)));
    put("chunk_at", value(std::move(chunk_at)));
    put("content_at", value(std::move(content_at)));
    put("items_end", value(int64_t(p.items_end)));
    put("draft_begin", value(int64_t(p.draft_begin)));
    put("draft_end", value(int64_t(p.draft_end)));
    put("group_at", value(p.group_at));
    put("group_count", value(p.group_count));
    return value(std::move(out));
  }

  // Struct value field (layout, prompt line...) by name
  value const & field(value const & x, std::string_view f, span where) const {
    for(auto const & [k, fv]: x.as<agg_v>().fields)
      if(session.db.strings.text(k) == f) return fv;
    throw diagnostic(cat("нет поля ", f), where);
  }
  // node lay layout for the protocol: subtree at (empty - all in order of layout).
  std::vector<proto::s_node> wire_nodes(value const & lay, node_id at, span where) const {
    auto const & nodes = field(lay, "nodes", where).as<array_v>();
    auto const & first = field(lay, "first", where).as<array_v>();
    auto const & count = field(lay, "count", where).as<array_v>();
    auto const & pchild = field(lay, "pchild", where).as<array_v>();
    auto const & pbyte = field(lay, "pbyte", where).as<array_v>();
    std::string_view ptext = field(lay, "ptext", where).as<str_v>().view();
    auto one = [&](uint32_t k) {
      node_id n(uint32_t(nodes.get(k).as<int64_t>()));
      proto::s_node out{n, std::string(kind_name(session.db[n])), {}, std::string(note_of_hole(n))};
      int64_t f = first.get(k).as<int64_t>(), c = count.get(k).as<int64_t>();
      for(int64_t j = f; j < f + c; ++j) {
        int64_t ch = pchild.get(uint32_t(j)).as<int64_t>();
        if(ch >= 0) out.pieces.push_back(node_id(uint32_t(ch)));
        else {
          int64_t b = pbyte.get(uint32_t(j)).as<int64_t>(), e = pbyte.get(uint32_t(j + 1)).as<int64_t>();
          out.pieces.push_back(std::string(ptext.substr(std::size_t(b), std::size_t(e - b))));
        }
      }
      return out;
    };
    std::vector<proto::s_node> out;
    if(!at) {
      for(uint32_t k = 0; k < nodes.size(); ++k) out.push_back(one(k));
      return out;
    }
    // node is a binary node increase search
    auto const & sorted = field(lay, "sorted", where).as<array_v>();
    auto const & order = field(lay, "order", where).as<array_v>();
    auto index = [&](node_id x) -> int64_t {
      uint32_t lo = 0, hi = sorted.size();
      while(lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if(sorted.get(mid).as<int64_t>() < int64_t(x.value)) lo = mid + 1;
        else hi = mid;
      }
      return lo < sorted.size() && sorted.get(lo).as<int64_t>() == int64_t(x.value) ? order.get(lo).as<int64_t>() : -1;
    };
    auto rec = [&](auto && self, node_id x) -> void {
      int64_t k = index(x);
      if(k < 0) return;
      out.push_back(one(uint32_t(k)));
      for(auto const & pc: out.back().pieces)
        if(auto const * c = std::get_if<node_id>(&pc)) self(self, *c);
    };
    rec(rec, at);
    return out;
  }

  // --- parsing edits to the database (language base) ---
  //
  // Which unit to redefine, where the draft and where the cursors will stand, the language decides; here is
  // the parsing text of the unit according to the rules of its place and the replacement of the node base.
// 
// 
//.

  // Rules parsing projection representation - on top of the rules of the place (the last - the first).
  void projection_rules(parser & pr, node_id projection) {
    if(!projection) return;
    for(node_id x: session.db.as<n_module>(projection).items) pr.learn_item(x);
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
  value codes(interpreter & vm, node_list const & xs) {
    std::vector<value> out;
    for(node_id x: xs) out.push_back(vm.code_value(x));
    return value(array_v::of(std::move(out)));
  }

  // Draft (author's decision 2026-10-12): the text of the module elements that rule is exactly as typed;
  // parsing is entirely according to the rules of place, elements of the same kind retain id (graft). The
  // result is draft elements, import of text, empty lines and comments of the end, disassembled.
  value draft_parse(interpreter & vm, bool module, node_id fn_root, std::string const & text, node_list const & old, node_id next, uint32_t at, node_id projection) {
    auto & db = session.db;
    ++edits;
    parser pr(db, text, cat("<черновик #", edits, ">"));
    reparsed.clear();
    parser::draft_items d;
    node_list ids;
    if(!module) {
      pr.place(db[fn_root].parent, fn_root);
      projection_rules(pr, projection);
      d = pr.parse_draft(true);
      node_list all = db.as<n_module>(session.root).items;
      all.insert(all.end(), d.items.begin(), d.items.end());
      pr.resolve_refs(all);
      if(d.items.size() == 1 && db.is<n_fn>(d.items[0])) {
        if(!db.is<n_fn>(fn_root)) { // It was a hole, a function again.
          db.replace(fn_root, db[d.items[0]].kind);
          db.place(fn_root, db[d.items[0]].where);
          db.set_note(fn_root, db.note_of(d.items[0]) ? *db.note_of(d.items[0]) : node_note{});
        } else graft(fn_root, d.items[0]);
      } else { // ceased to be a function, a hole (the same node), until it became again.
        db.replace(fn_root, n_hole{text, "не функция"});
        db.place(fn_root, span{0, uint32_t(text.size()), pr.source});
      }
      ids = {fn_root};
    } else {
      uint32_t count = uint32_t(old.size());
      pr.place(session.root, count ? old[0] : next); // Rules of Place (§4.4)
      projection_rules(pr, projection);
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
      db.link_parents(session.root);
      if(was_rules != now_rules) {
        reparse_after(session.root, at + ids.size());
        reparse_users(session.root);
      }
    }
    db.link_parents(session.root);
    array_v blank, rest;
    for(uint32_t x: d.blank) blank.push(value(int64_t(x)));
    for(auto & x: d.rest) rest.push(value(x));
    return value(array_v::of({codes(vm, ids), value(int64_t(pr.source)), value(std::move(blank)), value(std::move(rest)), codes(vm, reparsed)}));
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

  // The canon of the draft: empty lines of the set - layout of the module, comments after the last
  // element - the next (or end of the module), the draft keys - one edit journal.
  void draft_close(bool module, uint32_t at, uint32_t count, node_id next, std::vector<uint32_t> const & blank, std::vector<std::string> const & rest, uint64_t since, std::vector<uint32_t> const & sources, uint32_t source) {
    auto & db = session.db;
    if(module) {
      n_module m = db.as<n_module>(session.root);
      bool has_next = next && at + count < m.items.size() && m.items[at + count] == next;
      m.blank.resize(std::max<std::size_t>(m.blank.size(), m.items.size() + 1), 0);
      for(uint32_t i = 0; i < count && i < blank.size(); ++i) m.blank[at + i] = at + i == 0 ? 0 : blank[i];
      if(has_next && !blank.empty()) m.blank[at + count] = blank.back();
      parser::trim_blank(m.blank);
      if(m.blank != db.as<n_module>(session.root).blank) db.replace(session.root, std::move(m));
      if(!rest.empty()) { // comments after the draft with their lines - the next element
        node_id to = has_next ? next : session.root;
        node_note nt = db.note_of(to) ? *db.note_of(to) : node_note{};
        auto & lines = has_next ? nt.lead : nt.end;
        lines.insert(lines.begin(), rest.begin(), rest.end());
        db.set_note(to, std::move(nt));
      }
      db.link_parents(session.root);
    }
    db.squash_log(since, sources, source); // Draft keys - one edit in journal
  }

  // Unit over parsing (kind: 0 - block, 1 - module element, 2 - module) - edit text; unit node retains
  // id. He did not understand the false language.
  value unit_edit(interpreter & vm, node_id target, int64_t kind, std::string const & edited, node_id projection) {
    auto & db = session.db;
    ++edits;
    parser pr(db, edited, cat("<ввод #", edits, ">"));
    // parsing Rules: Contexts around the unit declared before it (§4.4)
    if(kind != 2) pr.place(db[target].parent, target);
    else pr.place({});
    projection_rules(pr, projection);
    node_id fresh = kind == 2 ? pr.parse_module() : pr.parse_unit(kind == 0);
    if(!fresh || (kind == 0 && !db.is<n_block>(fresh))) return value(array_v::of({value(false), codes(vm, {})}));
    if(kind != 2) { // f(…) is a reference to the module function
      node_list items = db.as<n_module>(session.root).items;
      pr.resolve_refs(items);
    }
    // The module element declares other parsing rules - the following are redefined
    bool rules_changed = kind == 1 && parser::rule_trace(db, target) != parser::rule_trace(db, fresh);
    // block - the body of the structure, and the rules in it are different - reexamination of its calls
    bool body_rules = kind == 0 && items_trace(target) != items_trace(fresh);
    reparsed.clear();
    node_kind k = db[fresh].kind;
    if(target == session.root) { // Module from projection - internal elements are saved
      auto & nm = std::get<n_module>(k);
      for(node_id x: inner_items()) nm.items.push_back(x);
      nm.inner = db.as<n_module>(session.root).inner;
    }
    db.replace(target, std::move(k));
    db.adopt_notes(fresh, target); // Commentary on the new text - the preserved id
    if(rules_changed) {
      auto const & items = db.as<n_module>(session.root).items;
      reparse_after(session.root, std::size_t(std::ranges::find(items, target) - items.begin()) + 1);
    }
    if(body_rules) reparse_users(target);
    db.link_parents(session.root);
    return value(array_v::of({value(true), codes(vm, reparsed)}));
  }

  // Raw block (§4.4): editing inside its parentheses rediscovers only its phase 1 - atoms and
  // sub-blocks; the block node retains id, the result parsing rule - derivative, editing it
  // invalidates.
//.
  bool raw_edit(node_id n, std::string const & edited) {
    auto & db = session.db;
    ++edits;
    parser pr(db, edited, cat("<ввод #", edits, ">"));
    node_id fresh = pr.parse_raw(db.as<n_raw>(n).rule);
    if(!fresh) return false;
    db.replace(n, db[fresh].kind);
    db.place(n, db[fresh].where); // atoms - in the text of the edit, the place of the block - there
    db.link_parents(session.root);
    return true;
  }

  // Representation of a function: Text ceases to be a function - a hole (the same node) - until it becomes again.
  void fn_hole(node_id root, std::string const & edited) {
    session.db.replace(root, n_hole{edited, "не функция"});
    session.db.link_parents(session.root);
  }

  // Line of set (author's decision 2026-09-29):
// ): elements [k, k + count] of the container c - edit text (count 0 - new element
  // in place k); above/below - translations of the line of the interval before and after the insertion. The
  // result is whether it is disassembled how many elements have been redisassembled.
  value line_edit(interpreter & vm, node_id c, uint32_t k, uint32_t count, std::string const & edited, bool module_level, node_id projection, uint32_t above, uint32_t below) {
    auto & db = session.db;
    ++edits;
    node_list const items = items_of(db, c);
    auto fail = [&] { return value(array_v::of({value(false), value(int64_t(0)), codes(vm, {})})); };
    parser pr(db, edited, cat("<ввод #", edits, ">"));
    pr.place(c, k < items.size() ? items[k] : node_id{}); // Rules of Place (§4.4)
    projection_rules(pr, projection);
    bool value_now = false;
    auto fresh = pr.parse_line(module_level, value_now);
    if(!fresh) return fail();
    if(fresh->empty() && !pr.comments.empty()) return fail(); // The line became a comment - it is at the neighboring element: through brackets
    if(module_level) { // & f(…) - a reference to the module function (and the one just typed)
      node_list all = db.as<n_module>(session.root).items;
      all.insert(all.end(), fresh->begin(), fresh->end());
      pr.resolve_refs(all);
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
        value_now = false;
        break;
      }
    }
    // Empty space lines when inserted: above and below the new element
    std::vector<uint32_t> blank = db.is<n_block>(c) ? db.as<n_block>(c).blank : db.as<n_module>(c).blank;
    blank.resize(items.size() + 1, 0);
    uint32_t canon = module_level ? 2 : 1;
    if(!count && !fresh->empty()) {
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
    reparsed.clear();
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
      if(tail && !now_hole) nb.has_value = n_new > 0 && value_now && (count == 0 || nb.has_value);
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
    db.link_parents(session.root);
    return value(array_v::of({value(true), value(int64_t(n_new)), codes(vm, reparsed)}));
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

  // Add the internal element of the module (text - from the solution in the language, add_overload):
  // parsing, the element - at the end of the module, internal. No function is empty.
  node_id add_inner(std::string const & text, std::string const & sig) {
    auto & db = session.db;
    ++edits;
    parser pr(db, text, cat("<добавить ", sig, ">"));
    pr.place(session.root); // internal element at the end of the module
    node_id mod = pr.parse_module();
    auto const & items = db.as<n_module>(mod).items;
    if(items.size() != 1 || !db.is<n_fn>(items[0])) return {};
    node_id f = items[0];
    n_module nm = db.as<n_module>(session.root);
    nm.items.push_back(f);
    nm.inner.push_back(f.value);
    db.replace(session.root, nm);
    db.link_parents(session.root);
    return f;
  }

  // projection presentation (§4.4, variant C - author's decision 2026-10-05): a group of syntax
  // records - module; the presentation prints from its records and analyzes edits according to
  // its rules. The module is searched by key next to the program, like use k.
// Outcome - module (empty - by
  // default) and error (“no”).
  std::pair<node_id, std::string> projection_of(std::string const & key) {
    if(key.empty()) return {};
    node_id mod;
    auto dir = session.dir.empty() ? std::filesystem::path(session.name).parent_path() : std::filesystem::path(session.dir);
    auto errors = preload_uses(session.db, cat("use ", key, ";"), dir);
    if(auto it = session.db.strings.index.find(key); it != session.db.strings.index.end()) mod = find_module(session.db, it->second);
    if(!mod) return {{}, cat("нет модуля проекции ", key)};
    if(!errors.empty()) return {{}, cat("проекция ", key, ": ", errors.front().what())};
    if(!std::ranges::contains(projections, mod)) projections.push_back(mod);
    return {mod, {}};
  }

  // --- parsing facts for clues ---
  //
  // 
//.

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

  // code node; code() is empty
  static node_id node_or_none(interpreter & vm, value const & c, span where) {
    if(c.is<agg_v>() && !c.as<agg_v>().fields.empty() && (!c.as<agg_v>().fields[0].second.is<int64_t>() || !node_id(uint32_t(c.as<agg_v>().fields[0].second.as<int64_t>())))) return {};
    return vm.code_node(c, where);
  }
  // Instruction lines in the language (line) - in the protocol
  std::vector<proto::s_line> wire_lines(interpreter & vm, value const & xs, span where) {
    std::vector<proto::s_line> out;
    for(uint32_t k = 0; k < xs.as<array_v>().size(); ++k) {
      value x = xs.as<array_v>().get(k);
      out.push_back({std::string(field(x, "kind", where).as<str_v>().view()), std::string(field(x, "text", where).as<str_v>().view()), node_or_none(vm, field(x, "node", where), where), uint32_t(field(x, "from", where).as<int64_t>()), uint32_t(field(x, "to", where).as<int64_t>())});
    }
    return out;
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

  // Node from nodes (presentation layout), in the text of which lies the place of diagnosis (the smallest).
  node_id node_at(std::vector<node_id> const & nodes, span w) const {
    node_id best;
    uint32_t size = ~uint32_t{0};
    for(node_id n: nodes) {
      span const & s = session.db[n].where;
      if(s.source == w.source && s.begin <= w.begin && w.end <= s.end && s.end - s.begin < size) {
        best = n;
        size = s.end - s.begin;
      }
    }
    return best;
  }
  // parsing errors on node presentation (place in prelude is the first place of the program from the track).
  std::vector<proto::s_mark> marks_errors(std::vector<node_id> const & nodes) const {
    std::vector<proto::s_mark> out;
    for(auto const & d: session.errors) {
      node_id n = node_at(nodes, d.where);
      for(auto const & f: d.trace)
        if(!n) n = node_at(nodes, f.where);
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

  // Function Layer (instance key): Observations of a function in this context; no, empty.
  interpreter::layer_info const * layer_of(node_id fn, std::string const & key) const {
    if(key.empty()) return nullptr;
    auto fl = session.layers.find(fn.value);
    if(fl == session.layers.end()) return nullptr;
    auto it = fl->second.find(key);
    return it == fl->second.end() ? nullptr : &it->second;
  }

  // --- launch, debugger, versions ---

  std::string trace_output, trace_error;
  merge_result pending; // conflicts of the last merger before the decision of the person

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
    return {};
  }
  // The conflict is resolved: 0 is not such, 1 is mine (the same base), 2 is their (the base has changed)
  int take_version(std::string const & key, bool theirs) {
    auto it = std::ranges::find(pending.conflicts, key, &merge_conflict::key);
    if(it == pending.conflicts.end()) return 0;
    if(theirs) take_theirs(session.db, session.root, pending.base_items, it->id, it->theirs_exists ? std::optional<std::string>(it->theirs) : std::nullopt);
    pending.conflicts.erase(it);
    return theirs ? 2 : 1;
  }
  void send_merge(bool with_taken) {
    proto::s_merge out{with_taken ? pending.taken : std::vector<std::string>{}, {}};
    for(auto const & c: pending.conflicts) out.conflicts.push_back({c.key, c.base, c.mine, c.theirs});
    if(now_peer) send(*now_peer, out);
  }

  vm_session & session;
  node_id editor_module; // Editor's decisions - in the language (lib/editor.tz)
  struct lang_vm {
    node_id module;
    uint64_t revision;
    std::unique_ptr<interpreter> vm;
  };
  std::vector<lang_vm> lang_vms; // Interpreters of Editor Functions - Module by Module
  std::vector<node_id> projections; // projection modules loaded with views ([[editor]] in them)
  peer * now_peer = nullptr; // Customer whose message is being served
  uint32_t edits = 0;
  os::uring ring;
  int listen_fd = -1;
  std::deque<peer> peers; // Deque: Client links are stable; language number
};

} // namespace tz

// out/native.hpp (tz native, build.sh): the native “cpp” prelude body and the editor module after
// the server, its primitives to editor_host
#include "native.hpp"
