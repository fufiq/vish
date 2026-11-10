// Editor server: VM is a single state, editors are clients (§13.1, M5).
//
// 
// 
// Connections - Unix socket, events - stream from io_uring (multishot
// accept/recv, like the network in M3):
//
//
// Solutions and state of views - in language (editor module, lib/editor.vish - VM service, user
// solutions 2026-10-19/20:). Here are the socket, the frames, the set pause timer and the primitives
// of the language base that the solutions use: printing projection layout-value, parsing editing the
// unit in the database, sending to the client. .
//
#pragma once

#include "proto.hpp"
#include "complete.hpp"
#include "editor_lib.hpp"
#include "tools.hpp"
#include "modules.hpp"
#include "parser.hpp"
#include "session.hpp"

#include <ranges>

namespace vish {

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
  enum op : uint64_t { op_accept = 1, op_recv, op_send, op_timer, op_run };

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

  // --- solutions - in language (editor module, lib/editor.vish) ---
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
  // [[editor]] is a function of the program: in its module, then in modules connected to use
//;
  // The next announcement is the first - the plugin covers its function. *Mine: * Use in-depth.
  node_id program_fn(std::string_view name) const {
    auto const & db = session.db;
    std::vector<node_id> todo{session.root};
    std::set<uint32_t> seen;
    for(size_t i = 0; i < todo.size(); ++i) {
      node_id m = todo[i];
      if(!m || !db.is<n_module>(m) || !seen.insert(m.value).second) continue;
      if(node_id f = fn_in(m, name, true)) return f;
      for(node_id x: db.as<n_module>(m).items)
        if(db.is<n_use>(x))
          if(node_id u = find_module(db, db.as<n_use>(x).key)) todo.push_back(u);
    }
    return {};
  }
  // all [[editor]] functions of the program and its plugins in search order (module, function)
  std::vector<std::pair<node_id, node_id>> program_points() const {
    auto const & db = session.db;
    std::vector<std::pair<node_id, node_id>> out;
    std::vector<node_id> todo{session.root};
    std::set<uint32_t> seen;
    for(size_t i = 0; i < todo.size(); ++i) {
      node_id m = todo[i];
      if(!m || !db.is<n_module>(m) || !seen.insert(m.value).second) continue;
      for(node_id x: db.as<n_module>(m).items) {
        if(db.is<n_fn>(x) && marked_editor(db, x)) out.push_back({m, x});
        if(db.is<n_use>(x))
          if(node_id u = find_module(db, db.as<n_use>(x).key)) todo.push_back(u);
      }
    }
    return out;
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
    if(node_id f = program_fn(name)) return f;
    return fn_in(editor_module, name, false);
  }
  // Extension of the editor in the program (card visualizers, visual <name>): its name function with
  // [[editor]] from node c is an array of lines; no function or error is empty (error is in stderr).
  value extension(std::string const & name, node_id c) {
    node_id f = program_fn(name);
    if(!f) return value(array_v::of({}));
    try {
      not_an_edit quiet{*this};
      node_id m = f;
      while(m && !session.db.is<n_module>(m)) m = session.db[m].parent;
      interpreter & l = lang_of(m);
      value r = l.analyze_instance(f, {l.code_value(c)});
      return r.is<array_v>() ? r : value(array_v::of({}));
    } catch(diagnostic const & d) {
      put(stderr, cat("редактор: ", render(d, "editor.vish", session.db), "\n"));
      return value(array_v::of({}));
    }
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
    if(m == editor_module) tools_jit(editor_jit, *lang_vms.back().vm);
    return *lang_vms.back().vm;
  }
  // the customer whose message is being served - at the time of the decision
  struct host_hold {
    editor_server * host;
    peer * was_peer;
    editor_server * was_host = std::exchange(editor_host, host);
    tools_context * was_tools = std::exchange(tools_host, &host->versions); // Tools module: version 0 - session base
    ~host_hold() {
      editor_host = was_host;
      tools_host = was_tools;
      host->now_peer = was_peer;
    }
  };
  // Editor's solution: name function in language; view - representation of the client (protocol number; -1
  // - first) whose projection can override the solution
  void decide(peer * p, int64_t view, std::string_view name, std::vector<value> args) {
    uint64_t before = session.db.edits;
    {
      ++deciding;
      struct depth {
        uint32_t & d;
        ~depth() { --d; }
      } guard{deciding};
      decide_now(p, view, name, std::move(args));
    }
    if(!deciding) {
      if(session.repository && name != "on_idle" && session.db.edits != before) schedule_check();
      flush_run_notes();
      flush_gone();
      flush_peers();
    }
  }
  void decide_now(peer * p, int64_t view, std::string_view name, std::vector<value> args) {
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
      auto & language = lang_of(m);
      tool_authority permitted{versions, language};
      language.analyze_instance(f, std::move(args));
    } catch(diagnostic const & d) { // solution error in the client language, with a place in editor.vish
      if(p) send(*p, proto::s_error{cat("редактор: ", render(d, "editor.vish", session.db))});
      else put(stderr, cat("редактор: ", render(d, "editor.vish", session.db), "\n"));
    }
  }
  // cursors of other clients - after the decision
  void flush_peers() {
    try {
      ask("share_cursors", {});
    } catch(diagnostic const & d) {
      put(stderr, cat("редактор: ", render(d, "editor.vish", session.db), "\n"));
    }
  }
  // Question Module Editor (function name) - value
  value ask(std::string_view name, std::vector<value> args) {
    host_hold hold{this, now_peer};
    not_an_edit quiet{*this};
    auto & language = lang_of(editor_module);
    tool_authority permitted{versions, language};
    return language.analyze_instance(fn_in(editor_module, name, false), std::move(args));
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

  // Wait for the core events and process them. It's running -- the quantum of the process, then the finished
  // events without waiting; the process is waiting for host -- its ring is waiting for the server ring.
  void step() {
    if(live && !live->m->host_waiting()) {
      run_quantum();
      for(auto const & c: ring.peek()) complete(c);
      return;
    }
    if(live && !live->polling) {
      ring.poll(live->m->host_fd(), POLLIN, tag(op_run, live->gen));
      live->polling = true;
    }
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
      case op_run: // the process ring is ready: the completion of the process, the steps - the next quantum
        if(live && live->gen == i) {
          live->polling = false;
          live->m->pump();
        }
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
    gone.push_back(int64_t(index_of(p))); // Language - after the current decision
    if(!deciding) flush_gone();
  }
  std::vector<int64_t> gone; // Disconnected clients: their views close (solution on_peer_gone)
  void flush_gone() {
    while(!gone.empty()) {
      int64_t k = gone.front();
      gone.erase(gone.begin());
      decide(nullptr, -1, "on_peer_gone", {value(k)});
    }
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
    if(!deciding) {
      session.db.sweep_retired(); // Message served - no rounds
      session.db.maybe_compact(); // pool: abandoned chunks of rewriting lists (S5.1)
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
  void handle(peer & p, proto::c_stop const &) { decide(&p, -1, "on_stop", {}); }
  void handle(peer & p, proto::c_save const &) { decide(&p, -1, "on_save", {}); }
  void handle(peer & p, proto::c_trace const & m) { decide(&p, -1, "on_trace_run", {value(int64_t(-1)), value(m.args), value(m.stdin_text)}); }
  void handle(peer & p, proto::c_apply const & m) { decide(&p, -1, "on_apply", {value(m.fn)}); }
  void handle(peer & p, proto::c_trace_step const & m) { decide(&p, -1, "on_trace_step", {value(int64_t(m.step))}); }
  void handle(peer & p, proto::c_name const & m) { decide(&p, -1, "on_name", {value(m.name)}); }
  void handle(peer & p, proto::c_items const &) { decide(&p, -1, "on_items", {}); }
  void handle(peer & p, proto::c_extensions const &) { decide(&p, -1, "on_extensions", {}); }
  void handle(peer & p, proto::c_propose const & m) { decide(&p, -1, "on_propose", {value(m.key), value(m.text), value(m.why)}); }
  void handle(peer & p, proto::c_decide const & m) { decide(&p, -1, "on_decide", {value(int64_t(m.id)), value(m.accept)}); }
  void handle(peer & p, proto::c_context const & m) { decide(&p, m.view, "on_context", {value(int64_t(m.request)), value(int64_t(m.view)), value(int64_t(m.node.value)), value(int64_t(m.revision))}); }
  void handle(peer & p, proto::c_check const & m) { decide(&p, -1, "on_agent_check", {value(int64_t(m.request)), value(int64_t(m.revision))}); }
  void handle(peer & p, proto::c_workspace const & m) { decide(&p, -1, "on_workspace", {value(int64_t(m.request)), value(int64_t(m.workspace)), value(m.action), value(int64_t(m.revision)), value(m.key), value(m.text), value(m.args), value(m.input), value(m.expected)}); }
  void handle(peer & p, proto::c_repository const & m) { decide(&p, -1, "on_repository", {value(int64_t(m.request)), value(m.action), value(m.key), value(m.text), value(int64_t(m.revision))}); }
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
    session.db.sweep_retired();
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
  // overload, folded in the module representation
//:
  // Printing and container elements do not see them as internal. The set defines the language (set_folded).
  std::vector<node_id> folded;
  bool unseen(node_id n) const { return is_inner(n) || std::ranges::contains(folded, n); }

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
    printer pr(db, module ? printer::filter([&](node_id n) { return !unseen(n); }) : printer::filter{});
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

  // node printed with projection entries (frame "projection": one node - one color in both texts): pairs
  // [node, record signature] - type node (ast.loop), the call - the name of the record.
  value projection_records(interpreter & vm, node_id root, bool module, node_id projection) {
    auto const & db = session.db;
    array_v out;
    if(!projection) return value(std::move(out));
    std::vector<node_layout> ls;
    printer pr(db, module ? printer::filter([&](node_id n) { return !unseen(n); }) : printer::filter{});
    pr.layouts = &ls;
    pr.projection = projection;
    pr.print(root);
    for(auto && l: ls) {
      auto const * r = pr.record_for(l.node);
      if(!r) continue;
      node_id at = r->first;
      while(at && at != projection) at = db[at].parent;
      if(!at) continue;
      // Signature - node (ast.function), the call - the name of the record
      std::string label = db.is<n_call>(l.node) ? std::string(db.strings.text(db.as<n_fn>(r->first).name)) : std::string(lang_kind_of(db[l.node]));
      out.push(value(array_v::of({vm.code_value(l.node), value(std::move(label))})));
    }
    return value(std::move(out));
  }

  // Draft module after editing - print only its area and paste in the former layout lay (printed with a
  // draft [at, at + old_count ), its elements - old):
// 
// The rest of the module is not printed: its pieces are the same, places
  // They're moving. Pieces of the root are glued together like a printer (the text between children is one piece).
  value print_splice(interpreter & vm, value const & lay, node_id root, int64_t at, node_span old, int64_t count, int64_t source, span where) {
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
  // Reconciliation of the paste with full seal (VISH_SPLICE_CHECK = 1): discrepancy - field and output.
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
    // fields in order of struct layout (lib/editor.vish)
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
      if(!unseen(n)) out.push_back(n);
    return out;
  }
  node_list inner_items() const {
    node_list out;
    for(uint32_t n: session.db.as<n_module>(session.root).inner) out.push_back(node_id(n));
    return out;
  }
  value codes(interpreter & vm, node_span xs) {
    std::vector<value> out;
    for(node_id x: xs) out.push_back(vm.code_value(x));
    return value(array_v::of(std::move(out)));
  }

  // Draft (author's decision 2026-10-12): the text of the module elements that rule is exactly as typed;
  // parsing is entirely according to the rules of place, elements of the same kind retain id (graft). The
  // result is draft elements, import of text, empty lines and comments of the end, disassembled.
  value draft_parse(interpreter & vm, bool module, node_id fn_root, std::string const & text, node_span old, node_id next, uint32_t at, node_id projection) {
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
        else if constexpr(std::is_same_v<field_t, list<node_id>>) {
          auto & xs = x.[:m:];
          auto const & ys = y.[:m:];
          std::size_t a = 0, b = 0;
          for(; a < xs.size() && a < ys.size() && same(ys[a], xs[a]); ++a) xs[a] = graft(ys[a], xs[a]);
          for(; b < xs.size() - a && b < ys.size() - a && same(ys[ys.size() - 1 - b], xs[xs.size() - 1 - b]); ++b)
            xs[xs.size() - 1 - b] = graft(ys[ys.size() - 1 - b], xs[xs.size() - 1 - b]);
        }
      }
    }, k);
    // The same view (children - the same id) - the knot is not corrected: revision and caches behind it are
    // intact, editing neighboring brackets does not touch it; the place is new.
    if(!db.kind_equal(db[o].kind, k)) db.replace(o, std::move(k));
    db.place(o, db[n].where);
    db.set_note(o, db.note_of(n) ? *db.note_of(n) : node_note{});
    return o;
  }

  // The canon of the draft: empty lines of the set - layout of the module, comments after the last
  // element - the next (or end of the module), the draft keys - one edit journal.
  void draft_close(bool module, uint32_t at, uint32_t count, node_id next, std::span<uint32_t const> blank, std::vector<std::string> const & rest, uint64_t since, std::vector<uint32_t> const & sources, uint32_t source) {
    auto & db = session.db;
    if(module) {
      n_module m = db.copy_of<n_module>(session.root);
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
    std::vector<uint32_t> blank = (db.is<n_block>(c) ? db.as<n_block>(c).blank : db.as<n_module>(c).blank).vec();
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
      node_list items(db.context_items(c).begin(), db.context_items(c).end());
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
// What to show (type "i64 = 3")
  // Advertisements, child roles, errors in node, solve language (lib/editor.vish); here are the facts of the base.

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

  // Running (F5) is the process of the machine in the same VM:
// 
  // The server executes the process with quanta between socket events and does not stand up.
  // 
  // - Shift+F5. *My:* process executes the received code - its copy of the database (journal in whole: node numbers
  // are the same); the editing falls into it when the drafts are closed (on_idle solution in the language): module
  // elements changed from the last time - a key replacement, like proc/edit (§9.3), the started frames live in the
  // old body. What the machine does not know is the interpreter, as before (with waiting).
  struct live_run {
    ast_db db;
    node_id root;
    std::unique_ptr<process_machine> m;
    std::string out; // quantization
    std::vector<span> out_at; // print-place
    std::vector<std::string> args, input; // back-up
    peer * who = nullptr; // client running the program
    bool sent = false; // The conclusion has already been sent: the next - the continuation
    bool ok = true;
    uint64_t edits = 0; // base corrections included in the process
    uint64_t gen = 0; // generation of waiting for the ring of the machine (obsolete completion - past)
    bool polling = false;
  };
  std::unique_ptr<live_run> live;
  uint64_t live_gen = 0;
  uint64_t quantum_steps = 0; // Quantum - steps of the machine (without clocks); 0 - time
  static constexpr uint64_t quantum_ms = 10;
  bool running() const { return live != nullptr; }
  bool run_waits_host() { return live && live->m && live->m->host_waiting(); } // waiting for host

  void start_run(std::vector<std::string> args, std::vector<std::string> input) {
    live.reset();
    auto r = std::make_unique<live_run>();
    r->db.replay(session.db.journal());
    r->root = session.root;
    r->args = args;
    r->input = input;
    r->who = now_peer;
    r->edits = session.db.edits;
    r->gen = ++live_gen;
    live = std::move(r);
    try {
      live->m = std::make_unique<process_machine>(live->db, live->root, session.name);
      live->m->captured = &live->out;
      live->m->print_spans = &live->out_at;
      live->m->external_wait = true;
      live->m->start(args, input);
    } catch(diagnostic const & d) {
      run_failed(d);
      return;
    }
    run_quantum();
  }
  // process error: "not supported" by the machine - run by the interpreter, otherwise - program error
  void run_failed(diagnostic const & d) {
    live_run & r = *live;
    if(r.m && r.m->unsupported_hit) {
      session.stdin_lines = r.input;
      bool ok = true;
      std::string text = session.run(r.args, ok);
      r.out = std::move(text);
      r.sent = false;
      r.ok = ok;
    } else {
      r.out += render(d, session.name, r.db);
      r.ok = false;
    }
    finish_run("");
  }
  void run_quantum() {
    live_run & r = *live;
    uint64_t t0 = os::uring::now_ms();
    try {
      for(uint64_t k = 1;; ++k) {
        if(!r.m->step()) break;
        if(r.m->host_waiting()) break;
        if(quantum_steps ? k >= quantum_steps : (k % 256 == 0 && os::uring::now_ms() - t0 >= quantum_ms)) break;
      }
    } catch(diagnostic const & d) {
      return run_failed(d);
    }
    if(r.m->st.done) {
      r.m->save_state_vars(true);
      return finish_run("");
    }
    r.db.sweep_retired(); // There are no process base quanta
    if(!r.out.empty()) run_output("running");
  }
  // output to the client - on_run_output solution in the language, after the current solution (start and
  // stop the solution - no nested solution)
  struct run_note {
    peer * who;
    std::string text;
    bool ok, more;
    std::string state;
    std::vector<span> at;
    span now;
  };
  std::vector<run_note> run_notes;
  uint32_t deciding = 0;
  void run_output(std::string_view state) {
    live_run & r = *live;
    bool more = std::exchange(r.sent, true);
    span now = state == "running" && r.m ? r.m->running_span() : span{};
    run_notes.push_back({r.who, std::exchange(r.out, {}), r.ok, more, std::string(state), std::exchange(r.out_at, {}), now});
    if(!deciding) flush_run_notes();
  }
  void flush_run_notes() {
    while(!run_notes.empty()) {
      run_note n = std::move(run_notes.front());
      run_notes.erase(run_notes.begin());
      auto place = [](span w) { return value(array_v::of({value(int64_t(w.source)), value(int64_t(w.begin)), value(int64_t(w.end))})); };
      std::vector<value> at;
      for(span w: n.at) at.push_back(place(w));
      decide(n.who, -1, "on_run_output", {value(std::move(n.text)), value(n.ok), value(n.more), value(std::move(n.state)), value(array_v::of(std::move(at))), place(n.now)});
    }
  }
  void finish_run(std::string_view state) {
    run_output(state);
    live.reset();
  }
  void stop_run() {
    if(!live) return;
    finish_run("stopped");
  }
  // Editing - in a living process (drafts closed): what has changed and what to do with it, the language
  // decides (sync_program, lib/editor.vish); here - the process of the machine: its base - version of the tool
  // module, old values of struct, overload assistant, migrate, ct in place. The process is waiting for edits:
  // it comes, not finished, the base is corrected from the last time (noted)
  bool live_sync_due() {
    if(!live || !live->m || live->m->st.done || live->edits == session.db.edits) return false;
    live->edits = session.db.edits;
    return true;
  }
  int64_t live_version() { return live ? versions.adopt(live->db, live->root) : -1; }
  // Process program modules that are also in the session database
  std::vector<node_id> live_modules() const {
    std::vector<node_id> out;
    if(!live || !live->m) return out;
    std::vector<node_id> modules = live->m->helper.module_order;
    if(modules.empty()) modules.push_back(live->root);
    for(node_id m: modules)
      if(m.value < session.db.size() && session.db.is<n_module>(m)) out.push_back(m);
    return out;
  }
  // Struct n (session bases) of the old layout in a living process
  uint64_t live_stale(node_id n) {
    if(!live || !live->m || !session.db.is_struct(n)) return 0;
    process_machine & m = *live->m;
    return m.stale_values(m.helper.types.named(std::string(session.db.strings.text(session.db.as<n_extend>(n).name))), struct_fields(session.db, n));
  }
  // process base function n removed: new calls do not see it; added - see
  void live_forget(node_id n) {
    if(live && live->m && live->db.is<n_fn>(n)) std::erase(live->m->helper.functions[live->db.as<n_fn>(n).name], n);
  }
  void live_register(node_id n) {
    if(live && live->m) live->m->helper.register_item(n);
  }
  // overload name for the process assistant - again (they are set by the language: live_register).
  void live_clear(std::string const & name) {
    if(live && live->m) live->m->helper.functions[live->db.strings.intern(name)].clear();
  }
  // The struct item values of the old layout process base - via migrate fn (§9.3); which item and fn - decides
  // the language (sync_program).' - are transferred, otherwise why not.
  std::string live_migrate(node_id item, node_id fn) {
    if(!live || !live->m) return "";
    live_run & r = *live;
    process_machine & m = *r.m;
    try {
      auto t = m.helper.types.named(std::string(r.db.strings.text(r.db.as<n_extend>(item).name)));
      if(!fn) return m.auto_migrate(t, item, true); // migrate no - autotransport (O8)
      m.migrate_values(t, struct_fields(r.db, item), fn);
    } catch(diagnostic const & d) {
      return d.what();
    }
    return "";
  }
  void live_refresh() {
    if(live && live->m) live->m->refresh_ct(); // ct new code - in place in canonical order
  }

  // merger conflicts (and self-contained) to the client
  void send_merge(std::string path, std::vector<std::string> taken, std::vector<std::string> why, std::vector<proto::s_conflict> conflicts) {
    if(now_peer) send(*now_peer, proto::s_merge{std::move(path), std::move(taken), std::move(why), std::move(conflicts)});
  }

  vm_session & session;
  tools_context versions{session.db, session.root}; // Tools module version: 0 - session base
  node_id editor_module; // Editor's decisions - in language (lib/editor.vish)
  // Machine code for hot editor functions (search in lines, autocomplete) - tools_jit: one engine per editor
  // module interpreter, it lives the entire server; program and projection - interpreter
  jit_engine editor_jit{session.db, editor_module};
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

} // namespace vish

