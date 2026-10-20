// Run reference tests (`tz test <каталог>`) - in the language (lib/tools.tz, step 3 transfer tuling): header
// directives, reconciliations and cause texts, order and parallelity - there.
// 
// - reconciliation of engines; Here is the base: engines over the test program (interpreter, JIT, AArch64,
// process machine, check, residual, slice, print), status directories, children (fork) and session/editor
// scripts.
#pragma once

#include "interp.hpp"
#include "os.hpp"
#include "modules.hpp"
#include "parser.hpp"
#include "printer.hpp"
#include "views.hpp"
#include "store.hpp"
#include "session.hpp"
#include "server.hpp"
#include "machine.hpp"
#include "jit/jit.hpp"
#include "dump.hpp"
#include "tools.hpp"

#include <filesystem>
#include <thread>

namespace tz {

// Scenarios of the VM session (.session) and the editor's protocol (.wire).
struct scenario_runner {
  // Scenario of the VM session: commands and after each waiting for its conclusion.
  // // program: file.tz program (relative to script directory)
  // // expect: substring // expect-not: substring (ой - translation of a line) Scenario .wire - the same through
  // the protocol of the editor (@ substring[:N] - node): the line is the message of the client in text form, the
  // output is the answers of VM, passed through this socket.
  std::string run_session(std::filesystem::path const & path) {
    ::unsetenv("TZ_GRANT"); // Resources in the `// grant:` scenario only
    os::mapped_file text(path.string());
    std::vector<std::string> lines;
    for(auto l: lines_of(text.text())) lines.emplace_back(l);
    std::deque<ast_db> bases(1); // reopen - new base from .tzdb (the former live until the end of the script)
    ast_db * dbp = &bases.back();
    node_id root;
    std::optional<vm_session> session;
    std::optional<editor_server> server;
    wire_client client, second; // [2] is the second client of the same VM.
    bool wire = path.extension() == ".wire";
    std::string program_name;
    std::filesystem::path program_dir;
    // server and two clients per session (program or reopen)
    auto attach = [&]() -> std::string {
      int fds[2];
      if(!os::socket_pair(fds)) return "socketpair не удался";
      server.emplace(*session);
      server->timers = false; // set pause - idle team
      server->quantum_steps = 100'000; // квант запуска — шагами машины: вывод детерминирован
      client = wire_client{};
      client.peer = &server->add_peer(fds[0]);
      client.server = &*server;
      client.fd = fds[1];
      client.db = dbp;
      int more[2];
      if(!os::socket_pair(more)) return "socketpair не удался";
      second = wire_client{};
      second.peer = &server->add_peer(more[0]);
      second.server = &*server;
      second.fd = more[1];
      second.db = dbp;
      return "";
    };
    std::string last, why;
    std::map<std::string, std::string> names; // `// let:` - memorized node
    bool live = false; // `// run: live` - Do not wait until the end
    std::pair<std::size_t, std::size_t> journal_seen{0, 0};
    for(std::size_t i = 0; i < lines.size(); ++i) {
      auto const & l = lines[i];
      if(l.starts_with("// program:")) {
        auto file = path.parent_path() / std::string_view(l).substr(11 + (l.size() > 11 && l[11] == ' '));
        os::mapped_file f(file.string());
        std::vector<diagnostic> ignored;
        if(wire) { // Editor module - before the program, like tz serve
          load_prelude(*dbp);
          load_editor(*dbp);
        }
        root = parse_program(*dbp, f.text(), file.parent_path(), ignored); // use k; modules next to the program
        program_name = file.filename().string();
        program_dir = file.parent_path();
        session.emplace(*dbp, root, program_name);
        session->dir = program_dir.string();
        if(wire)
          if(std::string e = attach(); !e.empty()) return e;
        continue;
      }
      // `save_to файл` - journal database to file (out/ directory); `reopen файл` - session again from it
      // (copies of the database for merging, §9.4 :)
      if(wire && l.starts_with("save_to ")) {
        save_db(*dbp, root, l.substr(8));
        last.clear();
        continue;
      }
      if(wire && l.starts_with("reopen ")) {
        if(client.fd >= 0) os::close_fd(client.fd);
        if(second.fd >= 0) os::close_fd(second.fd);
        server.reset();
        session.reset();
        dbp = &bases.emplace_back();
        root = load_db(*dbp, l.substr(7));
        session.emplace(*dbp, root, program_name);
        session->dir = program_dir.string();
        if(std::string e = attach(); !e.empty()) return e;
        last.clear();
        continue;
      }
      // `// let: имя @подстрока` is the node number now; `$имя` in expectation is the same
      // (node saves id after editing)
      if(wire && l.starts_with("// let:")) {
        auto ws = words_of(std::string_view(l).substr(7));
        if(ws.size() == 2) names[std::string(ws[0])] = client.resolve(cat("x ", ws[1])).substr(2);
        continue;
      }
      // `// run: live` - start not waiting (corrections on the move); `// quantum: N` - quantum in the steps
      // of the machine; `// grant: fs net` - resources to launch ("You give": here they give the script)
      if(wire && l.starts_with("// grant:")) {
        std::string g;
        for(auto w: words_of(std::string_view(l).substr(9))) g += (g.empty() ? "" : ",") + std::string(w);
        ::setenv("TZ_GRANT", g.c_str(), 1);
        continue;
      }
      if(wire && l == "// run: live") {
        live = true;
        continue;
      }
      if(wire && l.starts_with("// quantum:") && server) {
        server->quantum_steps = std::stoull(l.substr(11));
        continue;
      }
      if(l.starts_with("// expect:")) {
        auto want_line = lines_joined(std::string_view(l).substr(10));
        for(auto const & [name, id]: names | std::views::reverse) // blk2 - formerly blk
          for(std::size_t k; (k = want_line.find("$" + name)) != std::string::npos;) want_line.replace(k, name.size() + 1, id);
        std::string_view want = want_line;
        if(!last.contains(want)) why += cat("    строка ", i + 1, ": нет ", in_quotes(want), " в выводе:\n", last);
        continue;
      }
      if(l.starts_with("// expect-not:")) {
        auto bad_line = lines_joined(std::string_view(l).substr(14));
        std::string_view bad = bad_line;
        if(last.contains(bad)) why += cat("    строка ", i + 1, ": лишнее ", in_quotes(bad), " в выводе:\n", last);
        continue;
      }
      if(l.empty() || l.starts_with("//") || !session) continue;
      if(wire && l == "journal") { // journal base growth since last time: records and bytes of import text
        auto entries = dbp->journal();
        std::size_t text = 0;
        for(auto const & e: entries)
          if(auto const * x = std::get_if<j_import>(&e.op)) text += x->text.size();
        std::map<std::string, std::size_t> kinds; // species-based
        for(std::size_t i = journal_seen.first; i < entries.size(); ++i)
          std::visit([&](auto const & x) { ++kinds[std::string(std::meta::identifier_of(std::meta::remove_cvref(^^decltype(x))))]; }, entries[i].op);
        std::string by;
        for(auto const & [k, n]: kinds) by += cat(by.empty() ? "" : ", ", k, " ", n);
        last = cat("журнал: +", entries.size() - journal_seen.first, " записей (", by, "), текст импортов +", text - journal_seen.second, " байт\n");
        journal_seen = {entries.size(), text};
        continue;
      }
      if(wire && l == "roundtrip") { // journal → New Base: Same projection
        last = session->roundtrip();
        continue;
      }
      if(wire && (l == "wait" || l.starts_with("wait "))) { // launch: quanta to end (wait N - N quanta) or to wait for host
        uint64_t n = l == "wait" ? ~uint64_t{0} : std::stoull(l.substr(5));
        while(server->running() && !server->run_waits_host() && n-- > 0) server->step();
        for(auto & p: server->peers)
          while(!server->settled(p)) server->step();
        last = client.collect();
        continue;
      }
      if(wire && l == "idle") { // Set pause: check and tags (without hours)
        server->idle();
        for(auto & p: server->peers)
          while(!server->settled(p)) server->step();
        last = client.collect();
        if(std::string also = second.collect(); !also.empty()) last += cat("другому клиенту:\n", also);
        continue;
      }
      if(wire) {
        bool two = l.starts_with("[2] ");
        wire_client & who = two ? second : client;
        try {
          last = who.exchange(*server, two ? l.substr(4) : l);
          if(!live && server->running()) { // launch - to the end (quantas), unless the script requested otherwise
            while(server->running()) server->step();
            for(auto & p: server->peers)
              while(!server->settled(p)) server->step();
            last += who.collect();
          }
        } catch(diagnostic const & d) {
          why += cat("    строка ", i + 1, ": ", d.what(), "\n");
          continue;
        }
        // Answers sent to another client (one state for all)
        wire_client & other = two ? client : second;
        if(other.sent) {
          std::string also = other.collect();
          if(!also.empty()) last += cat("другому клиенту:\n", also);
        }
        for(auto * w: {&client, &second}) {
          if(!w->mismatch.empty()) why += cat("    строка ", i + 1, ": текст представления расходится с проекцией:\n", w->mismatch);
          w->mismatch.clear();
        }
        continue;
      }
      std::string body;
      if(l.starts_with("edit "))
        while(++i < lines.size() && lines[i] != ".") body += lines[i] + "\n";
      last = session->exec(l, body);
    }
    if(wire && client.fd >= 0) os::close_fd(client.fd);
    if(wire && second.fd >= 0) os::close_fd(second.fd);
    return why;
  }

  // Waiting for the script: without leading spaces; е - translation of the
  // output line (\n inside quotation marks - shielding in the output itself).
  static std::string lines_joined(std::string_view s) {
    while(s.starts_with(' ')) s.remove_prefix(1);
    std::string out(s);
    for(std::size_t k; (k = out.find("⏎")) != std::string::npos;) out.replace(k, std::string_view("⏎").size(), "\n");
    return out;
  }

  // The protocol client is in the same process: writes a frame at its end of a pair of
  // sockets, spins the server until it responds, and describes the answers in text. For
  // presentation, adds the text collected by the client, for the cursor - a line with .
  struct wire_client {
    int fd = -1;
    editor_server::peer * peer = nullptr;
    editor_server * server = nullptr;
    uint32_t sent = 0;
    std::string in;
    std::vector<proto::s_node> nodes; // node common to all representations (base node numbers)
    struct shown {
      node_id root;
      std::string kind;
    };
    std::map<uint32_t, shown> views; // Customer Representation: Root and View
    node_id root; // root
    ast_db const * db = nullptr;
    std::string mismatch; // pieced text ) projection (tz fmt)

    // text without comments (`//…` to the end of the line, `/*…*/`): no string context chunks
    static std::string without_comments(std::string_view t) {
      std::string out;
      for(std::size_t i = 0; i < t.size();) {
        if(t.substr(i).starts_with("//")) i = std::min(t.find('\n', i), t.size());
        else if(t.substr(i).starts_with("/*")) {
          std::size_t e = t.find("*/", i + 2);
          i = e == std::string_view::npos ? t.size() : e + 2;
        }
        else out += t[i++];
      }
      return out;
    }
    // @substring is the number of the first node in whose own text it is: scripts do
    // not depend on node numbers (they shift from prelude).
    std::string resolve(std::string const & line) {
      std::string out;
      auto ws = words_of(line); // `сообщение N …` - Links are searched in representation N
      if(ws.size() > 1) {
        uint32_t id = 0;
        std::from_chars(ws[1].data(), ws[1].data() + ws[1].size(), id);
        if(views.contains(id)) root = views[id].root;
      }
      for(auto w: ws) {
        if(!out.empty()) out += ' ';
        if(!w.starts_with('@')) {
          out += w;
          continue;
        }
        // only node current projection (from root, in order of text); @text:N
        // is the Nth such node
        auto want = w.substr(1);
        uint32_t nth = 1;
        if(auto colon = want.rfind(':'); colon != std::string_view::npos && colon + 1 < want.size() && want[colon + 1] >= '1' && want[colon + 1] <= '9') {
          std::from_chars(want.data() + colon + 1, want.data() + want.size(), nth);
          want = want.substr(0, colon);
        }
        std::string found = "#?";
        bool notes = false; // Search for comments, if not in the code
        uint32_t want_nth = nth;
        auto rec = [&](auto && self, node_id n) -> void {
          auto const * x = node(n);
          if(!x || found != "#?") return;
          bool ctx = !notes && (x->kind == "n_module" || x->kind == "n_block"); // text of the context - with comments: they are not node
          for(auto const & pc: x->pieces)
            if(auto const * t = std::get_if<std::string>(&pc); t && (ctx ? without_comments(*t) : *t).contains(want)) {
              if(--nth == 0) found = cat("#", n.value);
              break;
            }
          for(auto const & pc: x->pieces)
            if(auto const * c = std::get_if<node_id>(&pc)) self(self, *c);
        };
        rec(rec, root);
        if(found == "#?") {
          notes = true;
          nth = want_nth;
          rec(rec, root);
        }
        out += found;
      }
      return out;
    }

    std::string exchange(editor_server & server, std::string const & line) {
      proto::client_msg m = proto::parse_client(resolve(line));
      // projection representation (§4.4): text reconciliation - according to it, if the module is found
      auto * pj = std::get_if<proto::c_projection>(&m);
      std::string was;
      if(pj) {
        was = projection_key[pj->view];
        projection_key[pj->view] = pj->key;
      }
      os::write_all(fd, proto::frame(m));
      ++sent;
      while(peer->handled < sent || !server.settled(*peer)) server.step();
      for(auto & p: server.peers) // Sending to other customers should also go away.
        while(!server.settled(p)) server.step();
      std::string out = collect();
      if(pj && !pj->key.empty()) { // No Module - the previous projection
        auto it = db->strings.index.find(pj->key);
        if(it == db->strings.index.end() || !find_module(*db, it->second)) projection_key[pj->view] = was;
      }
      return out;
    }
    std::map<uint32_t, std::string> projection_key; // projection presentation (module key)

    std::string collect() {
      in += os::read_ready(fd);
      std::string out;
      proto::take_frames(in, [&](std::string_view body) {
        proto::server_msg m;
        proto::decode(body, m);
        out += proto::describe(m);
        auto take = [&](std::vector<proto::s_node> const & ns) {
          for(auto const & n: ns) {
            if(auto * old = const_cast<proto::s_node *>(node(n.id))) *old = n;
            else nodes.push_back(n);
          }
        };
        if(auto * v = std::get_if<proto::s_view>(&m)) {
          take(v->nodes);
          views[v->view] = {v->root, v->kind};
          root = v->root;
          out += checked_text(v->view);
        }
        if(auto * u = std::get_if<proto::s_update>(&m)) {
          take(u->nodes);
          root = views[u->view].root;
          out += checked_text(u->view);
        }
        if(auto * c = std::get_if<proto::s_cursor>(&m)) {
          root = views[c->view].root;
          out += cat("курсор: ", cursor_line(*c), "\n");
        }
      });
      return out;
    }

    // Collected from pieces of text and its reconciliation with projection for
    // the editor (layout: a hole - only your text).
    std::string checked_text(uint32_t view) {
      std::string t = text(root);
      // draft (typed text) - projection of the presentation itself (the language knows it, the editor module)
      if(auto typed = server->draft_text(*peer, view)) {
        if(t != *typed) mismatch = cat("собрано:\n", t, "\nпредставление VM:\n", *typed);
        return cat("текст:\n", t, "\n");
      }
      std::vector<node_layout> scratch;
      // The presentation of the module does not show the internal elements
      std::vector<uint32_t> inner;
      if(db->is<n_module>(root)) inner = db->as<n_module>(root).inner;
      printer pr(*db, views[view].kind == "module" ? printer::filter([&](node_id n) { return !std::ranges::contains(inner, n.value); }) : printer::filter{});
      pr.layouts = &scratch;
      if(auto k = projection_key.find(view); k != projection_key.end() && !k->second.empty())
        if(auto it = db->strings.index.find(k->second); it != db->strings.index.end()) pr.projection = find_module(*db, it->second);
      if(std::string want = pr.print(root); t != want) mismatch = cat("собрано:\n", t, "\nпроекция:\n", want);
      return cat("текст:\n", t, "\n");
    }

    proto::s_node const * node(node_id n) const {
      for(auto const & x: nodes)
        if(x.id == n) return &x;
      return nullptr;
    }

    // Text projection, collected from pieces - as it will collect the editor; with
    // a given cursor in its place put ном.
    std::string text(node_id n, proto::s_cursor const * c = nullptr) const {
      std::string out;
      auto const * x = node(n);
      if(!x) return out;
      for(uint32_t i = 0; i < x->pieces.size(); ++i) {
        if(auto const * t = std::get_if<std::string>(&x->pieces[i])) {
          if(c && c->node == n && c->piece == i) {
            uint32_t cp = 0;
            for(char ch: *t) {
              if((static_cast<unsigned char>(ch) & 0xC0) != 0x80 && cp++ == c->offset) out += "‸";
              out += ch;
            }
            if(cp == c->offset) out += "‸";
          } else out += *t;
        } else out += text(std::get<node_id>(x->pieces[i]), c);
      }
      return out;
    }

    std::string cursor_line(proto::s_cursor const & c) const {
      std::string all = text(root, &c);
      auto bar = all.find("‸");
      if(bar == std::string::npos) return "?";
      auto b = all.rfind('\n', bar), e = all.find('\n', bar);
      b = b == std::string::npos ? 0 : b + 1;
      return all.substr(b, e == std::string::npos ? std::string::npos : e - b);
    }
  };

};

// Test program: base, root, text, file name, directory, errors parsing text.
struct test_program {
  ast_db db;
  node_id root;
  std::string src, name;
  std::filesystem::path dir;
  std::string errors;
  bool parsed = true;
};

// Temporary state directories of the program (§12.2): its own for each execution
struct state_dirs {
  std::vector<std::filesystem::path> made;
  void fresh() {
    std::string t = (std::filesystem::temp_directory_path() / "tz-state-XXXXXX").string();
    if(!mkdtemp(t.data())) throw diagnostic("раннер: нет временного каталога состояния", {});
    made.emplace_back(t);
    state_dir = t;
  }
  void clear() {
    for(auto const & d: made) std::filesystem::remove_all(d);
    made.clear();
    state_dir.clear();
  }
  ~state_dirs() { clear(); }
};

struct test_host {
  std::deque<std::optional<test_program>> programs; // number
  state_dirs states;
  // AArch64 last JIT launch (-a64)
  uint32_t a64_checked = 0, a64_skipped = 0;
  std::vector<std::string> a64_notes, a64_mismatches;

  test_program & at(int64_t p) {
    if(p < 0 || std::size_t(p) >= programs.size() || !programs[std::size_t(p)]) throw diagnostic("раннер: нет программы теста", {});
    return *programs[std::size_t(p)];
  }
  // path file program: parsing with modules near (use k;)
  int64_t load(std::string const & path) {
    std::filesystem::path f(path);
    auto & x = programs.emplace_back().emplace();
    x.src = std::string(os::mapped_file(path).text());
    x.name = f.filename().string();
    x.dir = f.parent_path();
    std::vector<diagnostic> errors;
    x.root = parse_program(x.db, x.src, x.dir, errors);
    for(auto const & d: errors) x.errors += render(d, x.name, x.db);
    x.parsed = errors.empty();
    return int64_t(programs.size() - 1);
  }
  void close(int64_t p) {
    if(p >= 0 && std::size_t(p) < programs.size()) programs[std::size_t(p)].reset();
  }
  std::vector<std::string> run_args(test_program const & x, std::vector<std::string> const & args) {
    std::vector<std::string> out{x.name};
    out.insert(out.end(), args.begin(), args.end());
    return out;
  }
  // Interpreter startup: output, errors, instance
  std::array<std::string, 3> run(int64_t p, std::vector<std::string> const & args, std::vector<std::string> const & input) {
    test_program & x = at(p);
    std::array<std::string, 3> r;
    interpreter vm(x.db, x.root, run_args(x, args), mode::run, interpreter::known_inputs{input});
    vm.capture_output(&r[0]);
    try {
      vm.run_main();
    } catch(diagnostic const & d) {
      r[1] = render(d, x.name, x.db);
    }
    r[2] = vm.instances();
    return r;
  }
  // Launch with JIT (and AArch64 code in qemu): output, error, JIT statistics
  std::array<std::string, 3> run_jit(int64_t p, std::vector<std::string> const & args, std::vector<std::string> const & input, bool a64) {
    test_program & x = at(p);
    std::array<std::string, 3> r;
    interpreter vm(x.db, x.root, run_args(x, args), mode::run, interpreter::known_inputs{input});
    jit_engine jit(x.db, x.root);
    vm.use_jit(&jit);
    a64_check check(x.db, x.root);
    if(a64) vm.a64 = &check;
    vm.capture_output(&r[0]);
    try {
      vm.run_main();
    } catch(diagnostic const & d) {
      r[1] = render(d, x.name, x.db);
    }
    r[2] = jit.stats();
    a64_checked = check.checked;
    a64_skipped = check.skipped;
    a64_notes.assign(check.notes.begin(), check.notes.end());
    a64_mismatches = check.mismatches;
    return r;
  }
  // static parsing: error, report
  std::array<std::string, 2> check(int64_t p) {
    test_program & x = at(p);
    std::array<std::string, 2> r;
    interpreter chk(x.db, x.root, {x.name}, mode::check);
    try {
      chk.run_main();
    } catch(diagnostic const & d) {
      r[0] = render(d, x.name, x.db);
    }
    r[1] = chk.report(x.name, x.src);
    return r;
  }
  std::string residual(int64_t p, std::vector<std::string> const & args) {
    test_program & x = at(p);
    interpreter vm(x.db, x.root, run_args(x, args), mode::check, interpreter::known_inputs{});
    try {
      vm.run_main();
    } catch(diagnostic const &) {
    }
    return residual_view(x.db, vm).print(x.root);
  }
  // save to .tzdb and download: is the same projection; output from downloaded
  std::pair<bool, std::string> store_roundtrip(int64_t p, std::vector<std::string> const & args, std::vector<std::string> const & input) {
    test_program & x = at(p);
    auto tmp = std::filesystem::temp_directory_path() / cat("tz-test-", std::filesystem::path(x.name).stem().string(), "-", getpid(), ".tzdb");
    save_db(x.db, x.root, tmp.string());
    ast_db db2;
    node_id root2 = load_db(db2, tmp.string());
    std::filesystem::remove(tmp);
    bool same = printer(x.db).print(x.root) == printer(db2).print(root2);
    std::string out;
    interpreter vm(db2, root2, run_args(x, args), mode::run, interpreter::known_inputs{input});
    vm.capture_output(&out);
    try {
      vm.run_main();
    } catch(diagnostic const &) {
    }
    return {same, out};
  }
  // Process machine: output, error
  std::array<std::string, 2> machine(int64_t p, std::vector<std::string> const & args, std::vector<std::string> const & input) {
    test_program & x = at(p);
    std::array<std::string, 2> r;
    try {
      process_machine m(x.db, x.root, x.name);
      m.captured = &r[0];
      m.start(args, input);
      m.run();
    } catch(diagnostic const & d) {
      r[1] = render(d, x.name, x.db);
    }
    return r;
  }
  // Machine with dump after n steps, recovery in a new base from journal dump (“in another VM”) and
  // counting: output before the dump and after recovery; whether the process ended before the dump step.
  std::pair<std::string, bool> machine_dump(int64_t p, std::vector<std::string> const & args, std::vector<std::string> const & input, uint64_t n) {
    test_program & x = at(p);
    std::string out;
    bool early = false;
    try {
      std::string bytes;
      { // The original process goes away: the restored one has the same addresses (pages as is)
        process_machine m(x.db, x.root, x.name);
        m.captured = &out;
        m.start(args, input);
        m.run(n);
        early = m.st.done;
        bytes = m.dump();
      }
      restored_process r(bytes);
      r.m->captured = &out;
      r.m->run();
    } catch(diagnostic const &) {
    }
    return {out, early};
  }

  // --- tests in children: each - fork, no more jobs at once; results - in the order of files ---
  struct running {
    os::child c;
    std::string got;
    bool done = false;
    int code = 0;
  };
  std::vector<std::string> files;
  std::vector<running> kids;
  std::size_t next = 0, shown = 0;
  uint32_t busy = 0, jobs = 1;
  bool a64_mode = false; // Run mode: --a64, --projection k
  std::string projection;
  void start(std::vector<std::string> fs, uint32_t n) {
    files = std::move(fs);
    kids.assign(files.size(), {});
    next = shown = 0;
    busy = 0;
    jobs = std::max(n, 1u);
  }
  // The result of the next test in order: the text of the child, the exit code (signal - minus), whether the
  // language performed the test to the end. body(файл) is the performance of the test in a child.
  std::tuple<std::string, int, bool> take_next(auto && body) {
    while(shown < files.size()) {
      for(; busy < jobs && next < files.size(); ++next, ++busy)
        kids[next].c = os::fork_child([&] {
          try {
            return std::string(1, '\1') + body(files[next]); // tongue performed the test to the end
          } catch(diagnostic const & d) {
            return std::string(1, '\0') + cat("    ", d.what(), "\n");
          }
        });
      if(kids[shown].done) {
        auto & r = kids[shown++];
        std::string got = std::move(r.got);
        bool whole = !got.empty() && got[0] == '\1';
        return {got.empty() ? std::string() : got.substr(1), r.code, whole};
      }
      std::vector<int> fds;
      std::vector<std::size_t> who;
      for(std::size_t i = shown; i < next; ++i)
        if(!kids[i].done) {
          fds.push_back(kids[i].c.fd);
          who.push_back(i);
        }
      auto ready = os::wait_readable(fds);
      for(std::size_t k = 0; k < who.size(); ++k) {
        auto & r = kids[who[k]];
        if(!ready[k] || os::read_some(r.c.fd, r.got)) continue;
        os::close_fd(r.c.fd);
        r.code = os::wait_child(r.c.pid);
        r.done = true;
        --busy;
      }
    }
    return {"", 0, false};
  }
};

// Runner, whose work goes on: primitives run tests module tools (native "cpp")
thread_local test_host * tests_host = nullptr;

// Text program (not file) - parsing with modules next to the dir directory: print by default or in projection
// key (key recording module, like use k; header - the text itself tells how to disassemble it); wood dump and
// the first parsing error. Bottom line: no projection module is false.
std::pair<bool, std::string> print_text(std::string const & text, std::string const & dir, std::string const & key, bool header) {
  ast_db db;
  std::vector<diagnostic> ignored;
  node_id root = parse_program(db, text, dir, ignored);
  printer pr(db);
  if(!key.empty()) {
    ignored = preload_uses(db, cat("use ", key, ";"), dir);
    if(auto it = db.strings.index.find(key); it != db.strings.index.end()) pr.projection = find_module(db, it->second);
    if(!pr.projection) return {false, ""};
    pr.header = header;
  }
  return {true, pr.print(root)};
}
std::pair<std::string, std::string> parse_dump(std::string const & text, std::string const & dir, std::string const & name) {
  ast_db db;
  std::vector<diagnostic> errors;
  node_id root = parse_program(db, text, dir, errors);
  return {errors.empty() ? std::string() : render(errors.front(), name, db), dumper(db).dump(root)};
}

} // namespace tz
