// Base area in storage (S5.1): close/open - node, children and text are the same; another version, cropped file,
// unwritten area occupied by another execution, name-path; copy for samples does not spoil the original;
// compaction saves the contents. Runs check.sh. Argument is a directory for domain files.
#include "vish/repository.hpp"
#include <sys/stat.h>
#include <cassert>
#include <cstdio>
using namespace vish;

// Synthetic program: module of n functions, each has a block of calls with strings and numbers (lists, text, symbols)
static node_id build(ast_db & db, int n) {
  n_module mod{};
  mod.name = db.strings.intern("тест");
  for(int i = 0; i < n; ++i) {
    list<node_id> items;
    for(int k = 0; k < 1 + i % 5; ++k) {
      auto s = db.add(n_str{std::string("строка ") + std::to_string(i * 7 + k)}, {});
      n_call c{};
      c.callee = db.add(n_ident{db.strings.intern("print")}, {});
      c.args = {s, db.add(n_int{i + k}, {})};
      items.push_back(db.add(c, {}));
    }
    n_block b{};
    b.items = items;
    b.blank = {0, 1};
    n_fn f{};
    f.name = db.strings.intern("f" + std::to_string(i));
    f.body = db.add(b, {});
    mod.items.push_back(db.add(f, {}));
  }
  auto root = db.add(mod, {});
  db.link_parents(root);
  return root;
}

// node text: children, lines, names - one line to reconcile two bases
static std::string dump(ast_db & db) {
  std::string out;
  for(uint32_t k = 0; k < db.size(); ++k) {
    node_id n(k);
    out += cat(k, ":", db[n].kind.index(), "^", db[n].parent.value, "[");
    db.for_each_child(n, [&](node_id c) { out += cat(c.value, ","); });
    out += "]";
    if(db.is<n_str>(n)) out += db.as<n_str>(n).value.str();
    if(db.is<n_ident>(n)) out += std::string(db.strings.text(db.as<n_ident>(n).name));
    if(db.is<n_block>(n)) for(uint32_t b: db.as<n_block>(n).blank) out += cat("_", b);
    out += "\n";
  }
  return out;
}

static bool same_nodes(ast_db & a, ast_db & b) {
  if(a.size() != b.size()) return false;
  for(uint32_t k = 0; k < a.size(); ++k) {
    node_id n(k);
    if(a[n].kind.index() != b[n].kind.index() || a[n].parent != b[n].parent || a[n].where.begin != b[n].where.begin) {
      std::fprintf(stderr, "node %u: kind %zu/%zu parent %u/%u begin %u/%u\n", k, a[n].kind.index(), b[n].kind.index(), a[n].parent.value, b[n].parent.value, a[n].where.begin, b[n].where.begin);
      return false;
    }
    std::vector<uint32_t> ca, cb;
    a.for_each_child(n, [&](node_id c) { ca.push_back(c.value); });
    b.for_each_child(n, [&](node_id c) { cb.push_back(c.value); });
    if(ca != cb) return false;
  }
  return true;
}

int main(int argc, char ** argv) {
  assert(argc == 2);
  std::string dir = argv[1];
  std::string before, journal_path = dir + "/journal.vishdb";
  node_id root;
  uint32_t nodes_before = 0, used_before = 0;
  {
    db_area area;
    assert(area.open(dir, "база").empty() && area.fresh);
    ast_db db(&area);
    root = build(db, 600);
    assert(db.size() > 3000);
    before = dump(db);
    nodes_before = db.size();
    used_before = db.pool.used;
    save_db(db, root, journal_path);
    assert(db.commit_area().empty());
    // The second opener is a refusal (one execution), a name-path is a refusal.
    db_area other;
    assert(other.open(dir, "база").find("занято") != std::string::npos);
    assert(!other.open(dir, "../база").empty());
  }
  { // Open: Same text, same node
    db_area area;
    assert(area.open(dir, "база", false).empty() && !area.fresh);
    ast_db db(&area);
    assert(db.size() == nodes_before && db.pool.used == used_before);
    node_id r2;
    node_pool staging;
    auto entries = read_journal(journal_path, r2, staging);
    db.attach_meta(entries);
    assert(r2 == root);
    assert(dump(db) == before);
    // Reconciliation with the base played from journal in normal memory
    ast_db ref;
    ref.replay(entries);
    if(!same_nodes(db, ref)) std::fprintf(stderr, "nodes differ: %u vs %u\n", db.size(), ref.size());
    if(dump(ref) != before) { auto a = dump(ref); size_t i = 0; while(i < a.size() && i < before.size() && a[i] == before[i]) ++i; std::fprintf(stderr, "dump differs at %zu: [%s] vs [%s]\n", i, a.substr(i, 60).c_str(), before.substr(i, 60).c_str()); }
    assert(same_nodes(db, ref) && dump(ref) == before);
    // copy for samples (playing journal to another database) does not spoil the original: editing the copy is not here
    ast_db probe;
    probe.replay(entries);
    n_module m = probe.copy_of<n_module>(root);
    m.items.push_back(m.items[0]);
    probe.replace(root, m);
    assert(dump(db) == before && dump(probe) != before);
    // correction in the area, fixation, reopening
    n_module mm = db.copy_of<n_module>(root);
    mm.items.pop_back();
    db.replace(root, mm);
    assert(dump(db) != before);
    assert(db.commit_area().empty());
  }
  { // unwritten area (no fixation after editing) - refusal with cause
    {
      db_area area;
      assert(area.open(dir, "база", false).empty());
      ast_db db(&area);
      node_id r2;
      node_pool staging;
      db.attach_meta(read_journal(journal_path, r2, staging));
      n_module mm = db.copy_of<n_module>(root);
      mm.items.push_back(mm.items[0]);
      db.replace(root, mm); // dirty: no commit
    }
    db_area area;
    std::string why = area.open(dir, "база", false);
    assert(why.find("не была зафиксирована") != std::string::npos);
  }
  { // another version of the format and another’s layout: an understandable reason, not a fall
    std::string path = dir + "/версия.storage/чужая";
    ::mkdir((dir + "/версия.storage").c_str(), 0700);
    {
      db_area a;
      assert(a.open(dir + "/версия.storage", "чужая").empty());
      a.h->version = 99;
      a.h->checksum = db_area::sum(*a.h);
      a.h->clean = 1;
      a.h->checksum = db_area::sum(*a.h);
    }
    db_area b;
    std::string why = b.open(dir + "/версия.storage", "чужая", false);
    assert(why.find("версия формата 99") != std::string::npos);
    {
      db_area a;
      assert(a.open(dir + "/версия.storage", "иная").empty());
      a.h->layout ^= 1;
      a.h->checksum = db_area::sum(*a.h);
    }
    db_area c;
    why = c.open(dir + "/версия.storage", "иная", false);
    assert(why.find("устройством узлов") != std::string::npos);
    { // cropped file: shorter data and shorter header
      db_area a;
      assert(a.open(dir + "/версия.storage", "обрезка").empty());
      ast_db db(&a);
      build(db, 50);
      assert(db.commit_area().empty());
    }
    std::string file = dir + "/версия.storage/обрезка";
    assert(::truncate(file.c_str(), 64 << 20) == 0 || true);
    struct stat st{};
    ::stat(file.c_str(), &st);
    ::truncate(file.c_str(), off_t(st.st_size - st.st_size / 2));
    db_area d;
    why = d.open(dir + "/версия.storage", "обрезка", false);
    assert(why.find("обрезана") != std::string::npos);
    ::truncate(file.c_str(), 10);
    db_area e;
    why = e.open(dir + "/версия.storage", "обрезка", false);
    assert(why.find("короче заголовка") != std::string::npos);
  }
  { // compaction: abandoned chunks of rewrites go away, content, journal and area are the same
    db_area area;
    assert(area.open(dir, "компактация").empty());
    ast_db db(&area);
    node_id r = build(db, 300);
    for(int round = 0; round < 40; ++round) { // Module editing: inserting and erasing elements - each time a new piece
      n_module m = db.copy_of<n_module>(r);
      m.items.insert(m.items.begin() + 1, m.items[round % 7]);
      m.items.erase(m.items.begin() + 3);
      m.blank.push_back(uint32_t(round));
      db.replace(r, m);
      n_block b = db.copy_of<n_block>(db.as<n_fn>(m.items[5]).body);
      b.items.push_back(b.items[0]);
      db.replace(db.as<n_fn>(m.items[5]).body, b);
    }
    std::string text = dump(db);
    uint32_t used = db.pool.used;
    uint64_t estimate = db.reclaimable();
    assert(estimate > 0);
    uint32_t stat_live = db.pool_stats().live;
    uint32_t freed = db.compact_pool();
    assert(freed > 0 && db.pool.used < used && db.pool.used == stat_live && db.reclaimable() == 0);
    assert(dump(db) == text); // node, children, text - the same
    // journal after compaction is reproduced in normal memory: the same node (journal records carried pieces of pool)
    ast_db ref;
    ref.replay(db.journal());
    assert(dump(ref) == text);
    // and area: fixing, closing, opening - the same node
    node_id root2;
    std::string jp = dir + "/компактация.vishdb";
    save_db(db, r, jp);
    assert(db.commit_area().empty());
    uint32_t count = db.size(), pool_used = db.pool.used;
    db.area = nullptr;
    (void)root2;
    std::printf("компактация: пул %u → %u слов (оценка %llu), узлов %u\n", used, pool_used, (unsigned long long)estimate, count);
  }
  {
    db_area area;
    assert(area.open(dir, "компактация", false).empty());
    ast_db db(&area);
    node_id r2;
    node_pool staging;
    db.attach_meta(read_journal(dir + "/компактация.vishdb", r2, staging));
    ast_db ref;
    ref.replay(read_journal(dir + "/компактация.vishdb", r2, staging));
    assert(dump(db) == dump(ref));
  }
  std::puts("area ok");
}
