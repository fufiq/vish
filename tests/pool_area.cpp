// Base area in the repository (S5.1-S5.2): close/open - the base is completely the same WITHOUT journal (node, children,
// text, intern, import texts, journal, comments, mutations, tables on node); old and foreign version, cropped file,
// unwritten area occupied by another execution, name-path; copy for samples and fork-child does not spoil the original;
// compaction saves the content. Runs check.sh. Argument is a directory for domain files.
#include "vish/repository.hpp"
#include <sys/stat.h>
#include <sys/wait.h>
#include <cassert>
#include <cstdio>
#include <sstream>
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

// Fingerprint of the entire database: node, intern, imports, journal (how it goes to the disk), comments, mutations, tables
static std::string fingerprint(ast_db & db) {
  std::string out = dump(db);
  for(uint32_t k = 0; k < db.strings.size(); ++k) out += cat("S", k, "=", db.strings.text(symbol(k)), "\n");
  for(uint32_t k = 0; k < db.source_count(); ++k) out += cat("I", k, "=", db.source(k), "|", db.source_label(k), "\n");
  writer w;
  for(auto const & e: db.journal()) w.put(e);
  out += cat("J", w.bytes.size(), ":", std::hash<std::string>{}(w.bytes), "\n");
  if(std::getenv("FP_DEBUG")) { uint32_t k = 0; for(auto const & e: db.journal()) { writer one; one.put(e); out += cat("j", k++, ":", e.op.index(), ":", one.bytes.size(), ":", std::hash<std::string>{}(one.bytes), "\n"); } }
  for(uint32_t k = 0; k < db.size(); ++k)
    if(auto const * n = db.note_of(node_id(k))) {
      out += cat("N", k, "[");
      for(auto const & l: n->lead) out += l + "|";
      out += cat(n->tail, "|");
      for(auto const & l: n->end) out += l + "|";
      out += "]\n";
    }
  for(auto const & m: db.mutations) out += cat("M", m.target, ",", m.source, ",", m.at, "\n");
  out += cat("R", db.revision, ",", db.births, ",", db.edits, ",", db.prelude.value, "\n");
  for(uint32_t k = 0; k < db.size(); ++k) out += cat(db.node_revision(node_id(k)), ",");
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
  std::string before;
  node_id root;
  uint32_t nodes_before = 0, used_before = 0, strings_before = 0;
  {
    db_area area;
    assert(area.open(dir, "база").empty() && area.fresh);
    ast_db db(&area);
    root = build(db, 600);
    assert(db.size() > 3000);
    db.record_import("// черновик\nf0();", "метка");
    node_note nt;
    nt.lead = {"// первая", "// вторая"};
    nt.tail = "// хвост";
    nt.end = {"// конец"};
    db.set_note(node_id(40), nt);
    db.set_note(node_id(41), node_note{{}, "// только хвост", {}});
    n_module m = db.copy_of<n_module>(root);
    m.items.pop_back();
    db.replace(root, m);
    n_int forty{40};
    db.mutate(node_id(3), forty, 7); // ct code mutation: journal recording and mutations
    db.mutate(node_id(5), n_int{50}, 8);
    assert(!db.mutations.empty());
    assert(db.note_of(node_id(40)));
    before = fingerprint(db);
    nodes_before = db.size();
    strings_before = db.strings.size();
    assert(db.commit_area().empty());
    used_before = db.pool.used; // fixation
    // The second opener is a refusal (one execution), a name-path is a refusal.
    db_area other;
    assert(other.open(dir, "база").find("занято") != std::string::npos);
    assert(!other.open(dir, "../база").empty());
  }
  { // Open: the base is the same without journal and without reading .vishdb
    db_area area;
    assert(area.open(dir, "база", false).empty() && !area.fresh);
    ast_db db(&area);
    assert(db.size() == nodes_before && db.pool.used == used_before && db.strings.size() == strings_before);
    std::string after = fingerprint(db);
    if(after != before) { // divergent print lines (first 6)
      std::istringstream a(after), b(before);
      std::string la, lb;
      int shown = 0;
      while(std::getline(a, la) && std::getline(b, lb) && shown < 6)
        if(la != lb) {
          std::fprintf(stderr, "расходится:\n  после: %.100s\n  до:    %.100s\n", la.c_str(), lb.c_str());
          ++shown;
        }
    }
    assert(after == before);
    // journal from the region is played into normal memory: the same node
    ast_db ref;
    ref.replay(db.journal());
    assert(same_nodes(db, ref) && dump(ref) == dump(db));
    // sample copy does not spoil the original
    ast_db probe;
    probe.replay(db.journal());
    n_module pm = probe.copy_of<n_module>(root);
    pm.items.push_back(pm.items[0]);
    probe.replace(root, pm);
    probe.set_note(node_id(40), node_note{});
    assert(fingerprint(db) == before && dump(probe) != dump(db));
    // editing an already open area: intern, comments, node - and fixing again
    symbol fresh_name = db.strings.intern("новый символ после открытия");
    assert(fresh_name.value == strings_before);
    db.set_note(node_id(42), node_note{{"// после открытия"}, {}, {}});
    n_module mm = db.copy_of<n_module>(root);
    mm.items.pop_back();
    db.replace(root, mm);
    before = fingerprint(db);
    assert(before.find("новый символ после открытия") != std::string::npos);
    assert(db.commit_area().empty());
  }
  {
    db_area area;
    assert(area.open(dir, "база", false).empty());
    ast_db db(&area);
    assert(fingerprint(db) == before);
  }
  { // unwritten area (no fixation after editing) - refusal with cause
    {
      db_area area;
      assert(area.open(dir, "база", false).empty());
      ast_db db(&area);
      n_module mm = db.copy_of<n_module>(root);
      mm.items.push_back(mm.items[0]);
      db.replace(root, mm); // dirty: no commit
    }
    db_area area;
    std::string why = area.open(dir, "база", false);
    assert(why.find("не была зафиксирована") != std::string::npos);
  }
  { // old (S5.1), another version and someone else's layout: understandable reason, not falling
    ::mkdir((dir + "/версия.storage").c_str(), 0700);
    std::string d2 = dir + "/версия.storage";
    for(auto [name, version] : {std::pair{"старая", 1u}, std::pair{"чужая", 99u}}) {
      {
        db_area a;
        assert(a.open(d2, name).empty());
        a.h->version = version;
        a.h->checksum = db_area::sum(*a.h);
      }
      db_area b;
      std::string why = b.open(d2, name, false);
      assert(why.find(cat("версия формата ", version)) != std::string::npos);
    }
    {
      db_area a;
      assert(a.open(d2, "иная").empty());
      a.h->layout ^= 1;
      a.h->checksum = db_area::sum(*a.h);
    }
    db_area c;
    std::string why = c.open(d2, "иная", false);
    assert(why.find("устройством узлов") != std::string::npos);
    { // cropped file: shorter data and shorter header
      db_area a;
      assert(a.open(d2, "обрезка").empty());
      ast_db db(&a);
      build(db, 50);
      assert(db.commit_area().empty());
    }
    std::string file = d2 + "/обрезка";
    struct stat st{};
    ::stat(file.c_str(), &st);
    ::truncate(file.c_str(), off_t(st.st_size - st.st_size / 2));
    db_area d;
    why = d.open(d2, "обрезка", false);
    assert(why.find("обрезана") != std::string::npos);
    ::truncate(file.c_str(), 10);
    db_area e;
    why = e.open(d2, "обрезка", false);
    assert(why.find("короче заголовка") != std::string::npos);
  }
  { // compaction: abandoned chunks of rewrites go away, content, journal and area are the same
    db_area area;
    assert(area.open(dir, "компактация").empty());
    ast_db db(&area);
    node_id r = build(db, 300);
    db.set_note(node_id(7), node_note{{"// заметка"}, "// хвост", {}});
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
    std::string text = fingerprint(db);
    uint32_t used = db.pool.used;
    uint64_t estimate = db.reclaimable();
    assert(estimate > 0);
    uint32_t stat_live = db.pool_stats().live;
    assert(db.commit_area().empty()); // The image is up to compact: it updates and
    uint32_t freed = db.compact_pool();
    assert(freed > 0 && db.pool.used < used && db.reclaimable() == 0 && db.pool.used >= stat_live);
    assert(fingerprint(db) == text); // node, children, text, journal, comments - same
    ast_db ref;
    ref.replay(db.journal()); // journal is played back into normal memory after compaction.
    assert(dump(ref) == dump(db));
    assert(db.commit_area().empty());
    uint32_t pool_used = db.pool.used;
    std::printf("компактация: пул %u → %u слов (оценка %llu), узлов %u\n", used, pool_used, (unsigned long long)estimate, db.size());
    db.area = nullptr;
  }
  {
    db_area area;
    assert(area.open(dir, "компактация", false).empty());
    ast_db db(&area);
    ast_db ref;
    ref.replay(db.journal());
    assert(dump(db) == dump(ref));
  }
  { // fork: the descendant changes the base in the area - the original is intact, the fixation of the parent works (MAP_SHARED → the descendant is private)
    db_area area;
    assert(area.open(dir, "форк").empty());
    ast_db db(&area);
    node_id r = build(db, 200);
    std::string text = dump(db);
    pid_t pid = ::fork();
    if(pid == 0) { // descendant: rules and writes a lot (including new pool pages), he fixes
      for(int i = 0; i < 30; ++i) {
        n_module m = db.copy_of<n_module>(r);
        m.items.erase(m.items.begin() + 2);
        db.replace(r, m);
      }
      _exit(db.size() > 0 && dump(db) != text ? 0 : 1);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(dump(db) == text); // original untouched
    assert(db.commit_area().empty());
    db_area again;
    assert(!again.open(dir, "форк", false).empty()); // parent still holds
  }
  {
    db_area area;
    assert(area.open(dir, "форк", false).empty());
    ast_db db(&area);
    assert(db.size() > 0 && db.size() == db.nodes.size());
  }
  std::puts("area ok");
}
