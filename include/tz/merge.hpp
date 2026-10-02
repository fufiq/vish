// Base versions: semantic diff and fusion (spec §9.4, M5).
//
//  
// Live editing through VM does not give conflicts. Copies of the base diverge only when
// work was going on in two places.
//
//
//
// The common base of the two copies is the common beginning of their journal (journal
// is deterministic). Diff and fusion are by module elements; the function is
// identified by name and signature, not by lines. Changed only in one copy takes
// itself, changed in both in different ways - conflict: both options to man.
#pragma once

#include "parser.hpp"
#include "printer.hpp"
#include "store.hpp"

#include <optional>

namespace tz {

// Element of the module in the version: who is it (id - node number, if the element was
// in the common database: the numbers of the general beginning of journal for copies
// coincide, and the change of signature is an edit of the same element; otherwise - name
// and signature), as it is called (key) and what it is (text projection).
struct item_version {
  std::string id;
  std::string key;
  std::string text;
  node_id node;
};

std::string item_key(ast_db const & db, node_id n) {
  auto str = [&](symbol s) { return std::string(db.strings.text(s)); };
  if(db.is<n_fn>(n)) {
    auto const & f = db.as<n_fn>(n);
    if(!f.form.empty()) { // syntax - by writing the form
      std::string k = "syntax";
      for(symbol p: f.form) k += " " + str(p);
      return k;
    }
    std::string k = str(f.name) + "(";
    for(uint32_t i = 0; i < f.params.size(); ++i) {
      auto const & p = db.as<n_param>(f.params[i]);
      k += cat(i ? ", " : "", p.type && db.is<n_ident>(p.type) ? str(db.as<n_ident>(p.type).name) : std::string("_"));
    }
    return k + ")";
  }
  if(db.is_struct(n)) return "struct " + str(db.as<n_extend>(n).name);
  if(db.is<n_extend>(n) && db.as<n_extend>(n).target) return "&" + item_key(db, node_id(db.as<n_extend>(n).target)) + " { … }";
  if(db.is<n_extend>(n)) return str(db.as<n_extend>(n).name) + " { … }";
  if(db.is<n_var>(n)) return str(db.as<n_var>(n).name);
  return printer(db).print(n); // more
}

// base_items is the number of elements of the common base module (recognition by number).
std::vector<item_version> module_items(ast_db const & db, node_id root, std::vector<uint32_t> const & base_items = {}) {
  std::vector<item_version> out;
  for(node_id n: db.as<n_module>(root).items) {
    std::string key = item_key(db, n);
    for(uint32_t k = 2; std::ranges::any_of(out, [&](item_version const & x) { return x.key == key; }); ++k) key = cat(item_key(db, n), " #", k);
    std::string id = std::ranges::contains(base_items, n.value) ? cat("#", n.value) : key;
    out.push_back({id, key, printer(db).print(n), n});
  }
  return out;
}

std::optional<item_version> find_item(std::vector<item_version> const & xs, std::string const & id) {
  for(auto const & x: xs)
    if(x.id == id) return x;
  return std::nullopt;
}

// A copy of the base in memory: journal in its entirety or its beginning.
struct db_copy {
  ast_db db;
  node_id root;
};

void load_journal(db_copy & c, std::vector<journal_entry> const & entries, node_id root) {
  c.db.replay(entries);
  c.root = root;
}

// The common beginning of the two journals is the base from which the copies were dispersed.
std::size_t common_prefix(std::vector<journal_entry> const & a, std::vector<journal_entry> const & b) {
  std::size_t k = 0;
  for(; k < a.size() && k < b.size(); ++k) {
    writer wa, wb;
    wa.put(a[k]);
    wb.put(b[k]);
    if(wa.bytes != wb.bytes) break;
  }
  return k;
}

struct merge_conflict {
  std::string id, key, base, mine, theirs; // There is no element in this version.
  bool theirs_exists = true;
};

struct merge_result {
  std::vector<std::string> taken; // from their version
  std::vector<merge_conflict> conflicts;
  std::vector<uint32_t> base_items; // for take_theirs on conflict
};

// Replace (or add or delete) the id element in db with text from their version.
void take_theirs(ast_db & db, node_id root, std::vector<uint32_t> const & base_items, std::string const & key,
    std::optional<std::string> const & text) {
  auto mine = find_item(module_items(db, root, base_items), key);
  n_module m = db.as<n_module>(root);
  if(!text) { // remotely
    if(mine) std::erase(m.items, mine->node);
    db.replace(root, m);
    db.link_parents(root);
    return;
  }
  parser pr(db, *text, cat("<слияние ", key, ">"));
  pr.outer_items = m.items; // f(…) - on the function of this module
  pr.place(root, mine ? mine->node : node_id{}); // Rules parsing of the Item Place (§4.4)
  node_id snippet = pr.parse_module();
  auto const & items = db.as<n_module>(snippet).items;
  if(items.empty()) return;
  if(mine) db.replace(mine->node, db[items[0]].kind);
  else {
    m.items.push_back(items[0]);
    db.replace(root, m);
  }
  db.link_parents(root);
}

// Tripartite merging of their version into mine (db is mine, changes in place).
merge_result merge_into(ast_db & mine, node_id mine_root, std::vector<journal_entry> const & mine_journal,
    std::vector<journal_entry> const & theirs_journal, node_id theirs_root) {
  std::size_t k = common_prefix(mine_journal, theirs_journal);
  db_copy base, theirs;
  // node numbers are the same: the root of the base is my root, if it is already there.
  load_journal(base, std::vector<journal_entry>(mine_journal.begin(), mine_journal.begin() + k), mine_root);
  load_journal(theirs, theirs_journal, theirs_root);
  bool base_ok = base.root.value < base.db.size() && base.db.is<n_module>(base.root);
  std::vector<uint32_t> base_items;
  if(base_ok)
    for(node_id n: base.db.as<n_module>(base.root).items) base_items.push_back(n.value);
  auto b = base_ok ? module_items(base.db, base.root, base_items) : std::vector<item_version>{};
  auto m = module_items(mine, mine_root, base_items);
  auto t = module_items(theirs.db, theirs.root, base_items);
  std::vector<std::string> ids;
  for(auto const * xs: {&b, &m, &t})
    for(auto const & x: *xs)
      if(!std::ranges::contains(ids, x.id)) ids.push_back(x.id);
  merge_result out;
  for(auto const & id: ids) {
    auto bi = find_item(b, id), mi = find_item(m, id), ti = find_item(t, id);
    auto text = [](std::optional<item_version> const & x) { return x ? std::optional<std::string>(x->text) : std::nullopt; };
    std::string name = mi ? mi->key : ti ? ti->key : bi->key;
    if(text(mi) == text(ti) || text(ti) == text(bi)) continue; // matched or unchanged.
    if(text(mi) == text(bi)) { // They are only changed by themselves.
      take_theirs(mine, mine_root, base_items, id, text(ti));
      out.taken.push_back(name);
      continue;
    }
    out.conflicts.push_back({id, name, bi ? bi->text : "", mi ? mi->text : "", ti ? ti->text : "", ti.has_value()});
  }
  out.base_items = base_items;
  return out;
}

// Bilateral diff: What is B different than A (by module elements).
std::string describe_diff(ast_db const & a, node_id ra, ast_db const & b, node_id rb) {
  auto xa = module_items(a, ra), xb = module_items(b, rb);
  std::string out;
  for(auto const & x: xa)
    if(auto y = find_item(xb, x.key); !y) out += cat("- ", x.key, "\n");
    else if(y->text != x.text) out += cat("~ ", x.key, "\n  было:  ", x.text, "\n  стало: ", y->text, "\n");
  for(auto const & y: xb)
    if(!find_item(xa, y.key)) out += cat("+ ", y.key, "\n  ", y.text, "\n");
  return out.empty() ? "без различий\n" : out;
}

} // namespace tz
