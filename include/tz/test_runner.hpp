// Reference test runner: `tz test <каталог>`. No external dependencies.
//
// Comments at the beginning of the .tz file:
// // args: a b c arguments of the program
// // stdin: 1 2 3 line stdin (may be several)
// // out: text expected line stdout (in order)
// // error: adjustment expected error (diagnosis adjustment)
// // instances: dot(...) expected instance
// // check: substrate expected line of `tz check` report
// Check-error: Predicted static parsing error
// // residual: ab construct residual program for arguments
// // residual-has: text... it has a line
// // residual-lacks: text ... there is no line in it
// // slice: function variable
// // slice-has: text / slice-lacks: text
// // fmt-stable check fmtffmt = fmt
// // fmt-ct-has: text is in projection after elaboration (tz fmt-ct)
// // fmt-ct-lacks: text not in it
// // fmt-has: text in projection (with holes) has a string
// // store-roundtrip save to .tzdb, download: same projection and same output
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

#include <filesystem>
#include <thread>

namespace tz {

struct test_runner {
  bool a64 = false; // --a64: each instance JIT is also coded AArch64 in qemu
  uint32_t a64_checked = 0, a64_skipped = 0;
  std::set<std::string> a64_notes;

  int run(std::string const & dir) {
    std::vector<std::filesystem::path> files;
    for(auto & e: std::filesystem::directory_iterator(dir))
      if(e.path().extension() == ".tz" || e.path().extension() == ".session" || e.path().extension() == ".wire") files.push_back(e.path());
    std::ranges::sort(files);
    uint32_t passed = 0;
    auto show = [&](std::filesystem::path const & f, std::string const & why) {
      if(why.empty()) {
        ++passed;
        put(stdout, cat("  ok   ", f.filename().string(), "\n"));
      } else {
        put(stdout, cat("  FAIL ", f.filename().string(), "\n", why, "\n"));
      }
    };
    if(uint32_t jobs = test_jobs(); jobs > 1) run_forked(files, jobs, show);
    else for(auto & f: files) show(f, run_one(f));
    if(a64) {
      put(stdout, cat("AArch64: проверено вызовов ", a64_checked, ", не проверено ", a64_skipped, "\n"));
      for(auto const & n: a64_notes) put(stdout, cat("  не проверено  ", n, "\n"));
    }
    put(stdout, cat(passed, "/", files.size(), " тестов прошло\n"));
    return passed == files.size() ? 0 : 1;
  }

  // Simultaneous tests: TZ_TEST_JOBS, otherwise - on the cores; 1 - in a row in this process, without fork.
  static uint32_t test_jobs() {
    if(char const * s = std::getenv("TZ_TEST_JOBS")) {
      uint32_t n = 0;
      std::from_chars(s, s + std::strlen(s), n);
      return std::max(n, 1u);
    }
    return std::max(std::thread::hardware_concurrency(), 1u);
  }

  // Each test is in its child (fork), no more jobs at once; the results are in the order of the
  // files, as when running in a row. The child sends to the pipe: counters and notes AArch64
  // (they are saved by the runner between tests), then the text parsing (empty - passed).
  void run_forked(std::vector<std::filesystem::path> const & files, uint32_t jobs, auto && show) {
    struct running {
      os::child c;
      std::string got;
      bool done = false;
      int code = 0;
    };
    std::vector<running> kids(files.size());
    std::size_t next = 0, shown = 0;
    uint32_t busy = 0;
    while(shown < files.size()) {
      for(; busy < jobs && next < files.size(); ++next, ++busy)
        kids[next].c = os::fork_child([&] {
          a64_checked = a64_skipped = 0;
          a64_notes.clear();
          std::string why;
          try {
            why = run_one(files[next]);
          } catch(diagnostic const & d) {
            why = cat("    ", d.what(), "\n");
          }
          std::string out;
          put_u32(out, a64_checked);
          put_u32(out, a64_skipped);
          put_u32(out, uint32_t(a64_notes.size()));
          for(auto const & n: a64_notes) {
            put_u32(out, uint32_t(n.size()));
            out += n;
          }
          return out + why;
        });
      std::vector<int> fds;
      std::vector<std::size_t> who;
      for(std::size_t i = shown; i < next; ++i)
        if(!kids[i].done) {
          fds.push_back(kids[i].c.fd);
          who.push_back(i);
        }
      if(!fds.empty()) {
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
      for(; shown < next && kids[shown].done; ++shown) {
        auto & r = kids[shown];
        if(r.code != 0) { // The fallen child did not complete the result
          show(files[shown], cat("    тест не закончился: ", r.code < 0 ? cat("сигнал ", -r.code) : cat("код ", r.code), "\n"));
          continue;
        }
        std::string_view in = r.got;
        a64_checked += take_u32(in);
        a64_skipped += take_u32(in);
        for(uint32_t k = 0, notes = take_u32(in); k < notes; ++k) {
          uint32_t n = std::min<uint32_t>(take_u32(in), uint32_t(in.size()));
          a64_notes.emplace(in.substr(0, n));
          in.remove_prefix(n);
        }
        show(files[shown], std::string(in));
        r.got = {};
      }
    }
  }

  static void put_u32(std::string & to, uint32_t v) { to.append(reinterpret_cast<char const *>(&v), 4); }
  static uint32_t take_u32(std::string_view & from) {
    uint32_t v = 0;
    if(from.size() < 4) return 0;
    std::memcpy(&v, from.data(), 4);
    from.remove_prefix(4);
    return v;
  }
  struct expect {
    std::vector<std::string> args, stdin_lines, out, out2, errors, instances, check, check_lacks, check_errors;
    std::vector<std::string> residual, residual_has, residual_lacks, slice, slice_has, slice_lacks, jit_compiled, jit_stats, fmt_has, fmt_ct_has, fmt_ct_lacks, unknown;
    bool fmt_stable = false;
    bool machine = false; // The same program is a process machine: the same output
    std::vector<std::string> machine_dump; // dump step: dump, restore to new base, count
    bool store_roundtrip = false;
  };

  static expect read_header(std::string const & src) {
    expect e;
    for(std::string_view line: lines_of(src)) {
      if(!line.starts_with("//")) {
        if(line.empty()) continue;
        break;
      }
      auto body = line.substr(2);
      while(body.starts_with(' ')) body.remove_prefix(1);
      auto take = [&](std::string_view key, std::vector<std::string> & to) {
        if(!body.starts_with(key)) return false;
        auto rest = body.substr(key.size());
        while(rest.starts_with(' ')) rest.remove_prefix(1);
        to.emplace_back(rest);
        return true;
      };
      std::vector<std::string> args_line;
      if(take("args:", args_line)) {
        for(auto a: words_of(args_line[0])) e.args.emplace_back(a);
      } else if(take("stdin:", e.stdin_lines) || take("out2:", e.out2) || take("out:", e.out) || take("error:", e.errors) || take("instances:", e.instances) || take("check-error:", e.check_errors) || take("check-lacks:", e.check_lacks) || take("check:", e.check) || take("residual-has:", e.residual_has) || take("residual-lacks:", e.residual_lacks) || take("residual:", e.residual) || take("slice-has:", e.slice_has) || take("slice-lacks:", e.slice_lacks) || take("slice:", e.slice) || take("jit-compiled:", e.jit_compiled) || take("jit-stats:", e.jit_stats) || take("fmt-has:", e.fmt_has) || take("fmt-ct-has:", e.fmt_ct_has) || take("fmt-ct-lacks:", e.fmt_ct_lacks)) {
      } else if(take("machine-dump:", e.machine_dump)) {
        e.machine = true;
      } else if(body == "machine") {
        e.machine = true;
      } else if(body.starts_with("fmt-stable")) {
        e.fmt_stable = true;
      } else if(body.starts_with("store-roundtrip")) {
        e.store_roundtrip = true;
      } else if(auto c = body.find(':'); c != std::string::npos && c > 0 && std::ranges::all_of(body.substr(0, c), [](char ch) { return (ch >= 'a' && ch <= 'z') || ch == '-'; })) {
        e.unknown.emplace_back(body.substr(0, c)); // a typo in the directive shall not silently turn off the check
      }
    }
    return e;
  }

  // Scenario of the VM session: commands and after each waiting for its conclusion.
  // // program: file.tz program (relative to script directory)
  // // expect: substring // expect-not: substring (ой - translation of a line) Scenario .wire - the same through
  // the protocol of the editor (@ substring[:N] - node): the line is the message of the client in text form, the
  // output is the answers of VM, passed through this socket.
  std::string run_session(std::filesystem::path const & path) {
    os::mapped_file text(path.string());
    std::vector<std::string> lines;
    for(auto l: lines_of(text.text())) lines.emplace_back(l);
    ast_db db;
    node_id root;
    std::optional<vm_session> session;
    std::optional<editor_server> server;
    wire_client client, second; // [2] is the second client of the same VM.
    bool wire = path.extension() == ".wire";
    std::string last, why;
    std::map<std::string, std::string> names; // `// let:` - memorized node
    for(std::size_t i = 0; i < lines.size(); ++i) {
      auto const & l = lines[i];
      if(l.starts_with("// program:")) {
        auto file = path.parent_path() / std::string_view(l).substr(11 + (l.size() > 11 && l[11] == ' '));
        os::mapped_file f(file.string());
        std::vector<diagnostic> ignored;
        root = parse_program(db, f.text(), file.parent_path(), ignored); // use k; modules next to the program
        session.emplace(db, root, file.filename().string());
        if(wire) {
          int fds[2];
          if(!os::socket_pair(fds)) return "socketpair не удался";
          server.emplace(*session);
          client.peer = &server->add_peer(fds[0]);
          client.fd = fds[1];
          client.db = &db;
          int more[2];
          if(!os::socket_pair(more)) return "socketpair не удался";
          second.peer = &server->add_peer(more[0]);
          second.fd = more[1];
          second.db = &db;
        }
        continue;
      }
      // `// let: имя @подстрока` is the node number now; `$имя` in expectation is the same
      // (node saves id after editing)
      if(wire && l.starts_with("// let:")) {
        auto ws = words_of(std::string_view(l).substr(7));
        if(ws.size() == 2) names[std::string(ws[0])] = client.resolve(cat("x ", ws[1])).substr(2);
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
      if(wire) {
        bool two = l.starts_with("[2] ");
        wire_client & who = two ? second : client;
        try {
          last = who.exchange(*server, two ? l.substr(4) : l);
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
        auto rec = [&](auto && self, node_id n) -> void {
          auto const * x = node(n);
          if(!x || found != "#?") return;
          for(auto const & pc: x->pieces)
            if(auto const * t = std::get_if<std::string>(&pc); t && t->contains(want)) {
              if(--nth == 0) found = cat("#", n.value);
              break;
            }
          for(auto const & pc: x->pieces)
            if(auto const * c = std::get_if<node_id>(&pc)) self(self, *c);
        };
        rec(rec, root);
        out += found;
      }
      return out;
    }

    std::string exchange(editor_server & server, std::string const & line) {
      os::write_all(fd, proto::frame(proto::parse_client(resolve(line))));
      ++sent;
      while(peer->handled < sent || !server.settled(*peer)) server.step();
      for(auto & p: server.peers) // Sending to other customers should also go away.
        while(!server.settled(p)) server.step();
      return collect();
    }

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
      std::vector<node_layout> scratch;
      // The presentation of the module does not show the internal elements
      std::vector<uint32_t> inner;
      if(db->is<n_module>(root)) inner = db->as<n_module>(root).inner;
      printer pr(*db, views[view].kind == "module" ? printer::filter([&](node_id n) { return !std::ranges::contains(inner, n.value); }) : printer::filter{});
      pr.layouts = &scratch;
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

  std::string run_one(std::filesystem::path const & path) {
    if(path.extension() == ".session" || path.extension() == ".wire") return run_session(path);
    os::mapped_file file(path.string());
    std::string src(file.text());
    expect e = read_header(src);

    ast_db db;
    std::vector<diagnostic> parse_errors;
    node_id root = parse_program(db, src, path.parent_path(), parse_errors); // use k; modules next to the program

    std::string errors;
    for(auto const & d: parse_errors) errors += render(d, path.filename().string(), db);

    if(e.fmt_stable) {
      std::string once = printer(db).print(root);
      ast_db db2;
      std::vector<diagnostic> ignored;
      std::string twice = printer(db2).print(parse_program(db2, once, path.parent_path(), ignored));
      if(once != twice) return "    проекция нестабильна: fmt∘fmt ≠ fmt\n";
    }
    if(!e.fmt_has.empty()) {
      std::string text = printer(db).print(root);
      for(auto & want: e.fmt_has)
        if(!text.contains(want)) return cat("    нет в проекции: ", in_quotes(want), "\n", text);
    }
    if((!e.fmt_ct_has.empty() || !e.fmt_ct_lacks.empty()) && errors.empty()) { // projection after elaboration (tz fmt --ct)
      std::string text;
      try {
        text = elaborated_text(db, root, path.filename().string());
      } catch(diagnostic const & d) {
        return cat("    ошибка элаборации: ", render(d, path.filename().string(), db));
      }
      for(auto & want: e.fmt_ct_has)
        if(!text.contains(want)) return cat("    нет в проекции после элаборации: ", in_quotes(want), "\n", text);
      for(auto & bad: e.fmt_ct_lacks)
        if(text.contains(bad)) return cat("    лишнее в проекции после элаборации: ", in_quotes(bad), "\n", text);
    }

    // stdin - queue of interpreter lines (as in a session), output - in a row
    auto input = [&] { return interpreter::known_inputs{e.stdin_lines}; };

    // Program state (§12.2) in its temporary directory for each execution
    // out2: — вывод второго запуска с тем же состоянием
    state_dirs states;
    states.fresh();
    std::string out;
    std::string instances;
    if(errors.empty()) {
      std::vector<std::string> args{path.filename().string()};
      args.insert(args.end(), e.args.begin(), e.args.end());
      interpreter vm(db, root, args, mode::run, input());
      vm.capture_output(&out);
      try {
        vm.run_main();
      } catch(diagnostic const & d) {
        errors += render(d, path.filename().string(), db);
      }
      instances = vm.instances();
    }
    std::string why_state;
    if(!e.out2.empty() && errors.empty()) { // second launch - state from the first
      std::string again;
      std::vector<std::string> args{path.filename().string()};
      interpreter vm(db, root, args, mode::run, input());
      vm.capture_output(&again);
      try {
        vm.run_main();
      } catch(diagnostic const & d) {
        why_state += render(d, path.filename().string(), db);
      }
      uint32_t k = 0;
      for(std::string_view line: lines_of(again)) {
        if(k >= e.out2.size() || line != e.out2[k]) why_state += cat("    второй запуск, строка ", k + 1, ": ", in_quotes(line), "\n");
        ++k;
      }
      if(k < e.out2.size()) why_state += "    второй запуск: не всё выведено\n";
    }
    states.fresh();

    std::string why;
    for(auto & d: e.unknown) why += cat("    неизвестная директива теста: ", d, "\n");
    // Differential (§10.5): With JIT, the output and error locations coincide with the interpreter.
    if(parse_errors.empty()) {
      std::string out3, err3;
      std::vector<std::string> args{path.filename().string()};
      args.insert(args.end(), e.args.begin(), e.args.end());
      interpreter vm3(db, root, args, mode::run, input());
      jit_engine jit(db, root);
      vm3.use_jit(&jit);
      a64_check check(db, root);
      if(a64) vm3.a64 = &check;
      vm3.capture_output(&out3);
      try {
        vm3.run_main();
      } catch(diagnostic const & d) {
        err3 = render(d, path.filename().string(), db);
      }
      auto first_loc = [](std::string const & t) { return t.substr(0, t.find(": ошибка")); };
      if(a64) {
        a64_checked += check.checked;
        a64_skipped += check.skipped;
        a64_notes.insert(check.notes.begin(), check.notes.end());
        for(auto const & m: check.mismatches) why += cat("    AArch64: ", m, "\n");
      }
      if(out3 != out) why += cat("    JIT: вывод отличается: ", in_quotes(out3), " против ", in_quotes(out), "\n", jit.stats());
      for(auto & want: e.jit_stats)
        if(!jit.stats().contains(want)) why += cat("    нет в статистике JIT: ", in_quotes(want), "\n", jit.stats());
      for(auto & fn: e.jit_compiled)
        if(!jit.stats().contains(cat("jit  ", fn, "("))) why += cat("    JIT не скомпилировал ", fn, ":\n", jit.stats());
      if(first_loc(err3) != first_loc(errors) && !(errors.empty() && err3.empty()))
        why += cat("    JIT: ошибка отличается:\n", err3, "против\n", errors);
    }
    if(!e.check.empty() || !e.check_lacks.empty() || !e.check_errors.empty()) {
      std::string check_errors, report;
      if(parse_errors.empty()) {
        interpreter chk(db, root, {path.filename().string()}, mode::check);
        try {
          chk.run_main();
        } catch(diagnostic const & d) {
          check_errors = render(d, path.filename().string(), db);
        }
        report = chk.report(path.filename().string(), src);
      }
      for(auto & want: e.check)
        if(!report.contains(want)) why += cat("    нет в отчёте check: ", in_quotes(want), "\n", report);
      for(auto & bad: e.check_lacks)
        if(report.contains(bad)) why += cat("    лишнее в отчёте check: ", in_quotes(bad), "\n", report);
      for(auto & want: e.check_errors)
        if(!check_errors.contains(want)) why += cat("    нет ошибки check: ", in_quotes(want), "; есть: ", check_errors, "\n");
      if(e.check_errors.empty() && !check_errors.empty()) why += "    неожиданная ошибка check:\n" + check_errors;
    }
    if(!e.residual.empty() && parse_errors.empty()) {
      std::vector<std::string> args{path.filename().string()};
      for(auto a: words_of(e.residual[0])) args.emplace_back(a);
      interpreter vm(db, root, args, mode::check, interpreter::known_inputs{});
      try {
        vm.run_main();
      } catch(diagnostic const &) {
      }
      std::string text = residual_view(db, vm).print(root);
      for(auto & want: e.residual_has)
        if(!text.contains(want)) why += cat("    нет в остаточной программе: ", in_quotes(want), "\n", text);
      for(auto & bad: e.residual_lacks)
        if(text.contains(bad)) why += cat("    лишнее в остаточной программе: ", in_quotes(bad), "\n", text);
    }
    if(!e.slice.empty() && parse_errors.empty()) {
      auto w = words_of(e.slice[0]);
      std::string fn(w.size() > 0 ? w[0] : ""), var(w.size() > 1 ? w[1] : "");
      std::string text = slice_view(db).print(root, fn, var);
      for(auto & want: e.slice_has)
        if(!text.contains(want)) why += cat("    нет в срезе: ", in_quotes(want), "\n", text);
      for(auto & bad: e.slice_lacks)
        if(text.contains(bad)) why += cat("    лишнее в срезе: ", in_quotes(bad), "\n", text);
    }
    if(e.store_roundtrip) {
      auto tmp = std::filesystem::temp_directory_path() / cat("tz-test-", path.stem().string(), "-", getpid(), ".tzdb");
      save_db(db, root, tmp.string());
      ast_db db2;
      node_id root2 = load_db(db2, tmp.string());
      std::filesystem::remove(tmp);
      if(printer(db).print(root) != printer(db2).print(root2)) why += "    проекция после загрузки базы отличается\n";
      std::string out2;
      std::vector<std::string> args{path.filename().string()};
      args.insert(args.end(), e.args.begin(), e.args.end());
      states.fresh();
      interpreter vm2(db2, root2, args, mode::run, input());
      vm2.capture_output(&out2);
      try {
        vm2.run_main();
      } catch(diagnostic const &) {
      }
      if(out2 != out) why += cat("    вывод из базы отличается: ", in_quotes(out2), " против ", in_quotes(out), "\n");
    }
    why += why_state;
    states.fresh();
    if(e.machine && parse_errors.empty()) why += machine_check(db, root, path, e, out, errors);
    uint32_t i = 0;
    for(std::string_view line: lines_of(out)) {
      if(i >= e.out.size()) {
        why += cat("    лишний вывод: ", line, "\n");
        continue;
      }
      if(line != e.out[i]) why += cat("    строка ", i + 1, ": ожидалось ", in_quotes(e.out[i]), ", получено ", in_quotes(line), "\n");
      ++i;
    }
    for(; i < e.out.size(); ++i) why += cat("    не выведено: ", in_quotes(e.out[i]), "\n");
    for(auto & want: e.errors)
      if(!errors.contains(want)) why += cat("    нет ожидаемой ошибки: ", in_quotes(want), "\n");
    if(e.errors.empty() && !errors.empty()) why += "    неожиданная ошибка:\n" + errors;
    for(auto & want: e.instances)
      if(!instances.contains(want)) why += cat("    нет экземпляра ", in_quotes(want), "; есть:\n", instances);
    return why;
  }

  // Process machine (M5): the same conclusion as the interpreter; with dump - dump after
  // N steps, recovery in a new database from journal dump ("in another VM") and counting:
  // output to dump + after recovery = complete. Error text without space: "...: Error:
  // <text>" of the first line.
  static std::string error_text(std::string const & rendered) {
    auto at = rendered.find("ошибка: ");
    if(at == std::string::npos) return {};
    auto end = rendered.find('\n', at);
    return rendered.substr(at + 14, end == std::string::npos ? std::string::npos : end - at - 14);
  }

  // Temporary state directories of the program (§12.2): its own for each execution
  struct state_dirs {
    std::vector<std::filesystem::path> made;
    void fresh() {
      std::string t = (std::filesystem::temp_directory_path() / "tz-state-XXXXXX").string();
      if(!mkdtemp(t.data())) throw diagnostic("раннер: нет временного каталога состояния", {});
      made.emplace_back(t);
      state_dir = t;
    }
    ~state_dirs() {
      for(auto const & d: made) std::filesystem::remove_all(d);
      state_dir.clear();
    }
  };

  static std::string machine_check(ast_db & db, node_id root, std::filesystem::path const & path, expect const & e, std::string const & out,
      std::string const & errors) {
    std::string why, mout, merr;
    state_dirs states;
    states.fresh();
    try {
      process_machine m(db, root, path.filename().string());
      m.captured = &mout;
      m.start(e.args, e.stdin_lines);
      m.run();
    } catch(diagnostic const & d) {
      merr = render(d, path.filename().string(), db);
    }
    if(mout != out) why += cat("    машина процессов: вывод ", in_quotes(mout), " против ", in_quotes(out), "\n");
    if(error_text(merr) != error_text(errors)) why += cat("    машина процессов: ошибка ", in_quotes(error_text(merr)), " против ", in_quotes(error_text(errors)), "\n");
    for(auto const & at: e.machine_dump) { // each directive - its launch with a dump at this step
      uint64_t n = 0;
      std::from_chars(at.data(), at.data() + at.size(), n);
      std::string dout;
      states.fresh();
      try {
        std::string bytes;
        { // The original process goes away: the restored one has the same addresses (pages as is)
          process_machine m(db, root, path.filename().string());
          m.captured = &dout;
          m.start(e.args, e.stdin_lines);
          m.run(n);
          if(m.st.done) why += cat("    машина: процесс закончился раньше шага дампа ", n, "\n");
          bytes = m.dump();
        }
        restored_process r(bytes); // New base from journal dump - "another VM"
        r.m->captured = &dout;
        r.m->run();
      } catch(diagnostic const & d) {
      }
      if(dout != out) why += cat("    дамп на шаге ", n, ": вывод ", in_quotes(dout), " против ", in_quotes(out), "\n");
    }
    return why;
  }
};

} // namespace tz
