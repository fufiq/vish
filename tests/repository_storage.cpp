// Internal container invariants; triggers editor/src/protocol/repository.test.ts.
#include "vish/repository.hpp"
#include <cassert>
#include <cstdio>
using namespace vish;
int main(int argc, char ** argv) {
  if(argc == 3 && std::string_view(argv[2]) == "race") {
    auto file = repository_load(argv[1]);
    ast_db db; db.replay(file.journal);
    std::puts("ready"); std::fflush(stdout);
    std::string line; if(!os::host_stdin().next(line)) return 3;
    ++file.state.generation;
    try { repository_save(file.state, db, file.root); return 0; }
    catch(diagnostic const &) { return 2; }
  }
  assert(argc == 2);
  std::string path = argv[1];
  ast_db db;
  auto literal = db.add(n_int{7}, {});
  n_fn fn{};
  fn.name = db.strings.intern("f"); fn.body = literal;
  auto f = db.add(fn, {});
  auto ref = db.add(n_ref{f.value}, {});
  n_fn caller{};
  caller.name = db.strings.intern("caller"); caller.body = ref;
  auto g = db.add(caller, {});
  n_module mod{};
  mod.items = {f, g}; mod.blank = {0, 1, 0};
  auto root = db.add(mod, {});
  db.link_parents(root);
  db.set_note(f, {{"// сохранённый комментарий"}, "// хвост", {}});
  auto state = repository_create(db, root, path);
  state.modules = {{root, "main"}};
  state.branch = "main"; state.branches = {{"main", 1}};
  state.snapshots.push_back({1, {}, "Начало", state.modules, db.journal().size()});
  state.next_id = 2; state.branches[0].head = 1;
  repository_save(state, db, root);
  auto initial = repository_load(path);
  ast_db original;
  original.replay(initial.journal);
  fn.name = db.strings.intern("renamed");
  db.replace(f, fn); db.link_parents(root);
  auto removed = mod; removed.items = {f}; removed.blank = {0, 0};
  db.replace(root, removed); db.link_parents(root);
  assert(!db[g].parent);
  repository_save(state, db, root);
  auto loaded = repository_load(path);
  ast_db live;
  live.replay(loaded.journal);
  assert(live.strings.text(live.as<n_fn>(f).name) == "renamed");
  assert(live.as<n_ref>(ref).target == f.value);
  restore_nodes(live, original, {root});
  assert(live.strings.text(live.as<n_fn>(f).name) == "f");
  assert(live.as<n_ref>(ref).target == f.value);
  assert(live.note_of(f)->tail == "// хвост");
  assert(live.as<n_module>(root).blank[1] == 1);
  ast_db replayed;
  replayed.replay(live.journal());
  assert(replayed[ref].parent == g && replayed[f].parent == root);
  auto reject = [](auto && action) {
    bool caught = false;
    try { action(); } catch(diagnostic const &) { caught = true; }
    assert(caught);
  };
  ast_db unrelated;
  unrelated.add(n_int{99}, {});
  auto before = live.journal().size();
  reject([&] { restore_nodes(live, unrelated, {root}); });
  assert(live.journal().size() == before);
  auto forged = [&](repository_file const & file, bool structural = true) {
    writer body, out;
    body.put(repository_build()); body.put(std::string(vishdb_magic)); body.put(file);
    out.raw(repository_magic.data(), repository_magic.size());
    out.put(uint64_t(body.bytes.size())); out.put(repository_hash(body.bytes));
    out.raw(body.bytes.data(), body.bytes.size());
    std::string destination = path + (structural ? ".bad" : ".badmeta.vishrepo");
    os::write_file_atomic(destination, out.bytes);
    if(structural) reject([&] { repository_load(destination); });
    else repository_load(destination); // Branch rules check Vish, not C + +
  };
  auto bad = loaded;
  bad.journal.push_back({999, j_replace{999999999, n_int{0}}});
  forged(bad); // correct amount, incorrect link before replay
  bad = loaded;
  n_module cycle{}; cycle.items = {root};
  bad.journal.push_back({999, j_replace{root.value, cycle}});
  bad.journal.push_back({1000, j_link{root.value}});
  forged(bad);
  bad = loaded; bad.state.branches[0].head = 12345; forged(bad, false);
  bad = loaded;
  bad.journal.push_back({999, j_replace{ref.value, n_ref{999999999}}});
  forged(bad);
  // The mutation removed the child, the late link made him the parent of the old root. The possessing
  // graph is no longer cycled, but the old parent forms a cycle: the next touch would freeze.
  repository_file stale{}; stale.root = node_id(0);
  n_module outer{}; outer.items = {node_id(1)};
  n_block nested{}; nested.items = {node_id(0)};
  stale.journal = {
    {1, j_add{0, n_module{}, {}}}, {2, j_add{1, n_block{}, {}}},
    {3, j_replace{0, outer}}, {4, j_link{0}},
    {5, j_mutate{0, n_module{}, 0}}, {6, j_mutate{1, nested, 0}},
    {7, j_link{1}}, {8, j_replace{0, n_module{}}}
  };
  forged(stale);
  auto bytes = std::string(os::mapped_file(path).text());
  os::write_file_atomic(path + ".bad", bytes.substr(0, bytes.size() / 2));
  reject([&] { repository_load(path + ".bad"); });
  bytes.back() ^= 1;
  os::write_file_atomic(path + ".bad", bytes);
  reject([&] { repository_load(path + ".bad"); });
  reject([&] { repository_create(db, root, path); });
  // Two process opens one file: the second process does not affect saving the first.
  auto writer1 = repository_load(path), writer2 = repository_load(path);
  repository_save(writer1.state, live, root);
  auto accepted = std::string(os::mapped_file(path).text());
  reject([&] { repository_save(writer2.state, db, root); });
  assert(os::mapped_file(path).text() == accepted);
  auto failed = writer1.state;
  failed.path = path + "/missing/repo"; failed.disk_checksum = 0;
  reject([&] { repository_save(failed, live, root); });
  assert(os::mapped_file(path).text() == accepted);
  std::puts("repository storage: restore, references, replay, corruption, concurrent save: OK");
}
