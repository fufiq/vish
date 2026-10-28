// Tools Module (lib/tools.vish): Tuling logic outside the editor is code in a language embedded in the
// assembly as an editor module (author's solution 2026-10-20:).
// 
// 
// Here is the base of the module: versions of the base (copies by journal), elements of the version
// module with data, editing the parsing element in place; tools module programs do not see (do not use).
#pragma once

#include "interp.hpp"
#include "parser.hpp"
#include "prelude.hpp"
#include "printer.hpp"
#include "store.hpp"

#include <deque>

namespace vish {

struct vm_session;

// Tuling modules (lib/*.vish) are texts in the assembly; they are loaded by the command that is needed.
constexpr unsigned char tools_bytes[] = {
#embed "../../lib/tools.vish"
};
constexpr unsigned char versions_bytes[] = {
#embed "../../lib/versions.vish"
};
constexpr unsigned char tests_bytes[] = {
#embed "../../lib/tests.vish"
};
constexpr unsigned char cli_bytes[] = {
#embed "../../lib/cli.vish"
};
std::string_view builtin_text(std::string_view key) {
  auto view = [](auto const & bytes) { return std::string_view(reinterpret_cast<char const *>(bytes), sizeof bytes); };
  if(key == "tools") return view(tools_bytes);
  if(key == "versions") return view(versions_bytes);
  if(key == "tests") return view(tests_bytes);
  if(key == "cli") return view(cli_bytes);
  return {};
}

// Built-in name module (editor, tools...): it is already in the database (the database is saved with
// it) - it, otherwise parsing text; parsing error - with space in the file.
node_id load_builtin_module(ast_db & db, std::string_view name, std::string_view text, std::string_view file) {
  for(uint32_t i = 0; i < db.size(); ++i)
    if(db.is<n_module>(node_id(i)) && db.as<n_module>(node_id(i)).name && db.strings.text(db.as<n_module>(node_id(i)).name) == name && !db[node_id(i)].parent)
      return node_id(i);
  parser p(db, text, std::string(file));
  node_id m = p.parse_module();
  if(!p.errors().empty()) {
    diagnostic d = p.errors().front();
    auto lc = locate(text, d.where.begin);
    throw diagnostic(cat(file, ":", lc.line, ":", lc.col, ": ", d.what()), d.where);
  }
  return m;
}
// Tuling module by key; tools - before the rest (they use tools)
node_id load_builtin(ast_db & db, std::string_view key) {
  if(key != "tools") load_builtin_module(db, "tools", builtin_text("tools"), "tools.vish");
  if(builtin_text(key).empty()) throw diagnostic(cat("нет встроенного модуля ", key), {});
  return load_builtin_module(db, key, builtin_text(key), cat(key, ".vish"));
}
node_id load_tools(ast_db & db) { return load_builtin(db, "tools"); }

// the file name of the embedded module, where the error text (import source); not built-in - "tools.vish"
std::string builtin_file(ast_db const & db, uint32_t source) {
  for(std::string_view key: {"tools", "versions", "tests", "cli"})
    for(uint32_t i = 0; i < db.size(); ++i)
      if(db.is<n_module>(node_id(i)) && !db[node_id(i)].parent && db.as<n_module>(node_id(i)).name && db.strings.text(db.as<n_module>(node_id(i)).name) == key && db[node_id(i)].where.source == source)
        return cat(key, ".vish");
  return "tools.vish";
}

// Versions of the base (§9.4): Version is a copy of the base according to journal; number 0 is the base from
// which they work (the editor session program; the vish team has empty). The common beginning of journal of
// the two versions is their common base: journal is determined, its node numbers are the same for copies.
// Merger solutions (whose, conflict, what to take) are in language (lib/tools.vish); here are copies, data
// module elements, and editing the parsing element in place.
//
struct version {
  std::optional<ast_db> own; // copy (from someone else's base - no)
  ast_db * db = nullptr;
  node_id root; // module
  std::vector<journal_entry> journal; // journal copy; from someone else's base - taken from it
};

struct tools_context {
  std::deque<version> versions; // Deque: Links stable; language number
  std::string error; // opening error
  ast_db * db = nullptr; // base of the tool module (command vish): its functions - by name
  node_id module;
  std::vector<vm_session *> sessions; // VM sessions (vish vm, scripts .session, vish apply): language number

  int64_t session_of(vm_session & s) {
    for(std::size_t k = 0; k < sessions.size(); ++k)
      if(sessions[k] == &s) return int64_t(k);
    sessions.push_back(&s);
    return int64_t(sessions.size() - 1);
  }
  vm_session & session(int64_t k) {
    if(k < 0 || std::size_t(k) >= sessions.size()) throw diagnostic("tools: нет сессии", {});
    return *sessions[std::size_t(k)];
  }

  tools_context() { versions.emplace_back(); }
  tools_context(ast_db & db, node_id root) : tools_context() {
    versions[0].db = &db;
    versions[0].root = root;
  }

  node_id fn(std::string_view name) const {
    if(!db || !module) return {};
    for(node_id x: db->as<n_module>(module).items)
      if(db->is<n_fn>(x) && db->strings.text(db->as<n_fn>(x).name) == name) return x;
    return {};
  }
  version * at(int64_t v) {
    return v >= 0 && std::size_t(v) < versions.size() && versions[std::size_t(v)].db ? &versions[std::size_t(v)] : nullptr;
  }
  std::vector<journal_entry> journal_of(version const & x) const { return x.own ? x.journal : x.db->journal(); }

  // copy on file .vishdb; -1 - not opened (error)
  int64_t open(std::string const & path) {
    node_id root;
    std::vector<journal_entry> entries;
    try {
      entries = read_journal(path, root);
    } catch(diagnostic const & d) {
      error = d.what();
      return -1;
    } catch(std::exception const & e) {
      error = cat(path, ": ", e.what());
      return -1;
    }
    return add_copy(std::move(entries), root);
  }
  int64_t add_copy(std::vector<journal_entry> entries, node_id root) {
    // Closed copies are reused: AI queries do not hoard arenas and empty version slots.
    std::size_t slot = 1;
    while(slot < versions.size() && versions[slot].db) ++slot;
    if(slot == versions.size()) versions.emplace_back();
    version & x = versions[slot];
    x.db = &x.own.emplace();
    x.db->replay(entries);
    x.journal = std::move(entries);
    x.root = root;
    return int64_t(slot);
  }
  // Alien base (live F5 launch process) - version, no copy; same - same number.
  int64_t adopt(ast_db & db, node_id root) {
    for(std::size_t k = 1; k < versions.size(); ++k)
      if(versions[k].db == &db) {
        versions[k].root = root;
        return int64_t(k);
      }
    version & x = versions.emplace_back();
    x.db = &db;
    x.root = root;
    return int64_t(versions.size() - 1);
  }
  // The common base of versions a and b is the common beginning of their journal; the root is the root a, if it is already there.
  int64_t base(int64_t a, int64_t b) {
    version * x = at(a), * y = at(b);
    if(!x || !y) return -1;
    auto ea = journal_of(*x), eb = journal_of(*y);
    std::size_t k = 0;
    for(; k < ea.size() && k < eb.size(); ++k) {
      writer wa, wb;
      wa.put(ea[k]);
      wb.put(eb[k]);
      if(wa.bytes != wb.bytes) break;
    }
    node_id root = x->root;
    ea.resize(k);
    return add_copy(std::move(ea), root);
  }
  void close(int64_t v) {
    if(v <= 0 || std::size_t(v) >= versions.size()) return;
    version & x = versions[std::size_t(v)];
    x.own.reset();
    x.journal.clear();
    x.db = nullptr;
  }
  // version module: root ≥ 0 - this (live process has several modules), otherwise - programs
  node_id module_of(version const & x, int64_t root) const {
    node_id m = root >= 0 ? node_id(uint32_t(root)) : x.root;
    return m.value < x.db->size() && x.db->is<n_module>(m) ? m : node_id{};
  }
};

// the context of tuling, whose work goes on: primitives of the tool module (native "cpp") - to it
thread_local tools_context * tools_host = nullptr;

// language arrays; language arrays
std::vector<std::string> strings_of(value const & a) {
  std::vector<std::string> out;
  auto const & xs = a.as<array_v>();
  for(uint32_t k = 0; k < xs.size(); ++k) out.emplace_back(xs.get(k).as<str_v>().view());
  return out;
}
value strings_value(std::vector<std::string> const & xs) {
  std::vector<value> out;
  for(auto const & x: xs) out.push_back(value(x));
  return value(array_v::of(std::move(out)));
}

// The element of the module m of the db base - data: number, text projection, internal (§13.3), empty lines
// in front of it, comments with it (the module prints them, not the element): - the element is transferred
// with everything that it has in the database.
std::vector<value> module_item_facts(ast_db const & db, node_id m) {
  std::vector<value> out;
  if(!m) return out;
  auto const & mod = db.as<n_module>(m);
  for(std::size_t k = 0; k < mod.items.size(); ++k) {
    node_id n = mod.items[k];
    std::vector<value> lead;
    std::string tail;
    if(node_note const * nt = db.note_of(n)) {
      for(auto const & l: nt->lead) lead.push_back(value(l));
      tail = nt->tail;
    }
    out.push_back(value(array_v::of({value(int64_t(n.value)), value(printer(db).print(n)), value(std::ranges::contains(mod.inner, n.value)), value(int64_t(k < mod.blank.size() ? mod.blank[k] : 0)), value(array_v::of(std::move(lead))), value(std::move(tail))})));
  }
  return out;
}

// What is the element (for the key in the language): array {вид, имя, части, цель}; view - fn (parts -
// parameter types, " " - without type), syntax (parts - form words), struct, extend, extend_ref (goal -
// expandable function number), var, other.
value item_signature(ast_db const & db, node_id n) {
  auto str = [&](symbol s) { return value(std::string(s ? db.strings.text(s) : std::string_view())); };
  auto sig = [&](std::string_view kind, value name, std::vector<value> parts, int64_t target) {
    return value(array_v::of({value(std::string(kind)), std::move(name), value(array_v::of(std::move(parts))), value(target)}));
  };
  if(db.is<n_fn>(n)) {
    auto const & f = db.as<n_fn>(n);
    std::vector<value> parts;
    if(!f.form.empty()) {
      for(symbol p: f.form) parts.push_back(str(p));
      return sig("syntax", value(std::string()), std::move(parts), -1);
    }
    for(node_id pn: f.params) {
      auto const & p = db.as<n_param>(pn);
      parts.push_back(p.type && db.is<n_ident>(p.type) ? str(db.as<n_ident>(p.type).name) : value(std::string("_")));
    }
    return sig("fn", str(f.name), std::move(parts), -1);
  }
  if(db.is_struct(n)) return sig("struct", str(db.as<n_extend>(n).name), {}, -1);
  if(db.is<n_extend>(n) && db.as<n_extend>(n).target) return sig("extend_ref", value(std::string()), {}, int64_t(db.as<n_extend>(n).target));
  if(db.is<n_extend>(n)) return sig("extend", str(db.as<n_extend>(n).name), {}, -1);
  if(db.is<n_var>(n)) return sig("var", str(db.as<n_var>(n).name), {}, -1);
  return sig("other", value(std::string()), {}, -1);
}

// The element of the module m - text, parsing according to the rules of its place (§4.4): mine -
// replace it (node saves id), otherwise new - after the element after (-1 - at the end); layout -
// blank, inner, comments. The result is a node of the element; not understood - empty.
node_id put_item(ast_db & db, node_id m, node_id mine, std::string const & text, std::string const & name, node_id after, uint32_t blank, bool inner, node_note note) {
  n_module mod = db.as<n_module>(m);
  auto at_of = [&](node_id n) { return std::size_t(std::ranges::find(mod.items, n) - mod.items.begin()); };
  if(mod.blank.size() < mod.items.size() + 1) mod.blank.resize(mod.items.size() + 1, 0);
  parser pr(db, text, cat("<слияние ", name, ">"));
  pr.outer_items = mod.items; // f(…) - on the function of this module
  pr.place(m, mine); // Rules parsing of the Item Place (§4.4)
  node_id snippet = pr.parse_module();
  auto const & got = db.as<n_module>(snippet).items;
  if(got.empty()) return {};
  node_id item = got[0];
  if(mine) {
    db.replace(mine, db[item].kind);
    item = mine;
  } else {
    std::size_t k = after ? at_of(after) + 1 : mod.items.size();
    k = std::min(k, mod.items.size());
    mod.items.insert(mod.items.begin() + std::ptrdiff_t(k), item);
    mod.blank.insert(mod.blank.begin() + std::ptrdiff_t(k), 0);
  }
  mod.blank[at_of(item)] = blank;
  std::erase(mod.inner, item.value);
  if(inner) mod.inner.push_back(item.value);
  db.replace(m, mod);
  db.set_note(item, std::move(note));
  db.link_parents(m);
  return item;
}

// Remove element n of module m (with its layout).
void remove_item(ast_db & db, node_id m, node_id n) {
  n_module mod = db.as<n_module>(m);
  auto it = std::ranges::find(mod.items, n);
  if(it == mod.items.end()) return;
  std::size_t k = std::size_t(it - mod.items.begin());
  mod.items.erase(it);
  if(k < mod.blank.size()) mod.blank.erase(mod.blank.begin() + std::ptrdiff_t(k));
  std::erase(mod.inner, n.value);
  db.replace(m, mod);
  db.link_parents(m);
}

} // namespace vish
