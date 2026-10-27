// Run reference tests (`vish test <каталог>`) - in language (lib/tools.vish, step 3 transfer tuling): directives
// cap, reconciliations and texts of reasons, order and parallel - there.
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

namespace vish {

// Scenario of a VM session (.session) or editor protocol (.wire): strings, expectations, text form of messages
// and the text of the client's presentation - in language (lib/tools.vish). Here is a VM session, a server and
// two clients in the same process (pairs of sockets): the footage goes away and comes for real.
struct scenario {
  std::deque<ast_db> bases; // reopen - new base from .vishdb (the former live until the end of the script)
  ast_db * dbp = nullptr;
  node_id root;
  std::optional<vm_session> session;
  std::optional<editor_server> server;
  struct client {
    int fd = -1;
    editor_server::peer * peer = nullptr;
    uint32_t sent = 0;
    std::string in;
  };
  client clients[2]; // [2] is the second client of the same VM.
  bool wire = false;
  std::filesystem::path dir; // script
  std::string program_name;
  std::filesystem::path program_dir;
  std::pair<std::size_t, std::size_t> journal_seen{0, 0};

  scenario(std::string const & path, bool wire) : wire(wire), dir(std::filesystem::path(path).parent_path()) { dbp = &bases.emplace_back(); }
  ~scenario() { detach(); }
  void detach() {
    for(auto & c: clients)
      if(c.fd >= 0) {
        os::close_fd(c.fd);
        c.fd = -1;
      }
  }
  // server and two clients per session (program or reopen)
  std::string attach() {
    server.emplace(*session);
    server->timers = false; // set pause - idle team
    server->quantum_steps = 100'000; // квант запуска — шагами машины: вывод детерминирован
    for(auto & c: clients) {
      int fds[2];
      if(!os::socket_pair(fds)) return "socketpair не удался";
      c = client{};
      c.peer = &server->add_peer(fds[0]);
      c.fd = fds[1];
    }
    return "";
  }
  // `// program: файл` (relative to the script directory); .wire - editor module before the program
  std::string program(std::string const & file) {
    auto f = dir / file;
    os::mapped_file text(f.string());
    std::vector<diagnostic> ignored;
    if(wire) {
      load_prelude(*dbp);
      load_editor(*dbp);
    }
    root = parse_program(*dbp, text.text(), f.parent_path(), ignored); // use k; modules next to the program
    program_name = f.filename().string();
    program_dir = f.parent_path();
    session.emplace(*dbp, root, program_name);
    session->dir = program_dir.string();
    return wire ? attach() : "";
  }
  // `reopen файл` - session again from .vishdb (copy of merging base, §9.4)
  std::string reopen(std::string const & file) {
    detach();
    server.reset();
    session.reset();
    dbp = &bases.emplace_back();
    root = load_db(*dbp, file);
    session.emplace(*dbp, root, program_name);
    session->dir = program_dir.string();
    return attach();
  }
  // journal base growth from last time: records, by type, bytes of import text
  std::tuple<int64_t, std::vector<std::string>, std::vector<int64_t>, int64_t> journal() {
    auto entries = dbp->journal();
    std::size_t text = 0;
    for(auto const & e: entries)
      if(auto const * x = std::get_if<j_import>(&e.op)) text += x->text.size();
    std::map<std::string, int64_t> kinds;
    for(std::size_t i = journal_seen.first; i < entries.size(); ++i)
      std::visit([&](auto const & x) { ++kinds[std::string(std::meta::identifier_of(std::meta::remove_cvref(^^decltype(x))))]; }, entries[i].op);
    std::vector<std::string> names;
    std::vector<int64_t> counts;
    for(auto const & [k, n]: kinds) {
      names.push_back(k);
      counts.push_back(n);
    }
    auto r = std::tuple{int64_t(entries.size() - journal_seen.first), names, counts, int64_t(text - journal_seen.second)};
    journal_seen = {entries.size(), text};
    return r;
  }
  // All customers got theirs (sending to others should go too)
  void settle() {
    for(auto & p: server->peers)
      while(!server->settled(p)) server->step();
  }
  // client message c: frame - at the end of a pair of sockets, the server spins until it responds
  void send(int c, proto::client_msg const & m) {
    client & x = clients[c];
    os::write_all(x.fd, proto::frame(m));
    ++x.sent;
    while(x.peer->handled < x.sent || !server->settled(*x.peer)) server->step();
    settle();
  }
  // start: quanta to the end (n < 0) or n quanta until the process waits for host
  void wait(int64_t n) {
    while(server->running() && !server->run_waits_host() && (n < 0 || n-- > 0)) server->step();
    settle();
  }
  void finish() {
    while(server->running()) server->step();
    settle();
  }
  // Receiving customer c VM messages
  std::vector<proto::server_msg> collect(int c) {
    client & x = clients[c];
    x.in += os::read_ready(x.fd);
    std::vector<proto::server_msg> out;
    proto::take_frames(x.in, [&](std::string_view body) {
      proto::server_msg m;
      proto::decode(body, m);
      out.push_back(std::move(m));
    });
    return out;
  }
  // projection root for reconciliation: Module representation - no internal elements, projection - key
  // recording module (no - default)
  std::string print(node_id r, bool module_view, std::string const & key) {
    std::vector<uint32_t> inner;
    if(dbp->is<n_module>(r)) inner = dbp->as<n_module>(r).inner;
    // as in the presentation of the module: without internal and without folded overloads (they are set by the language of the editor)
    auto hidden = [&](node_id n) { return std::ranges::contains(inner, n.value) || (server && std::ranges::contains(server->folded, n)); };
    printer pr(*dbp, module_view ? printer::filter([&](node_id n) { return !hidden(n); }) : printer::filter{});
    std::vector<node_layout> scratch;
    pr.layouts = &scratch;
    if(!key.empty())
      if(auto it = dbp->strings.index.find(key); it != dbp->strings.index.end()) pr.projection = find_module(*dbp, it->second);
    return pr.print(r);
  }
  bool has_module(std::string const & key) const {
    auto it = dbp->strings.index.find(key);
    return it != dbp->strings.index.end() && find_module(*dbp, it->second);
  }
};

// VM message - flat tree for tongue: array {виды, имена, тексты, числа, детей, после поддерева}
value flat_value(proto::server_msg const & m) {
  proto::flat_tree t = proto::flat(m);
  std::vector<value> kind, name, text, num, count, next;
  for(auto k: t.kind) kind.push_back(value(std::string(k)));
  for(auto & x: t.name) name.push_back(value(std::move(x)));
  for(auto & x: t.text) text.push_back(value(std::move(x)));
  for(int64_t x: t.num) num.push_back(value(x));
  for(int64_t x: t.count) count.push_back(value(x));
  for(int64_t x: t.next) next.push_back(value(x));
  return value(array_v::of({value(array_v::of(std::move(kind))), value(array_v::of(std::move(name))), value(array_v::of(std::move(text))), value(array_v::of(std::move(num))), value(array_v::of(std::move(count))), value(array_v::of(std::move(next)))}));
}

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
    std::string t = (std::filesystem::temp_directory_path() / "vish-state-XXXXXX").string();
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
  std::deque<std::optional<scenario>> scenarios;
  scenario & scene(int64_t h) {
    if(h < 0 || std::size_t(h) >= scenarios.size() || !scenarios[std::size_t(h)]) throw diagnostic("раннер: нет сценария", {});
    return *scenarios[std::size_t(h)];
  }
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
  // save to .vishdb and download: is the same projection; output from downloaded
  std::pair<bool, std::string> store_roundtrip(int64_t p, std::vector<std::string> const & args, std::vector<std::string> const & input) {
    test_program & x = at(p);
    auto tmp = std::filesystem::temp_directory_path() / cat("vish-test-", std::filesystem::path(x.name).stem().string(), "-", getpid(), ".vishdb");
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

} // namespace vish
