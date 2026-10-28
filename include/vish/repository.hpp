// Semantic repository data. Version and branch politics live on language.
#pragma once

#include "store.hpp"

#include <filesystem>
#include <set>

namespace vish {

struct repository_module {
  node_id node;
  std::string name;
};

struct repository_snapshot {
  uint32_t id;
  std::vector<uint32_t> parents;
  std::string name;
  std::vector<repository_module> modules;
  uint64_t end; // end of the image in a single, growing journal repository
};

constexpr unsigned char repository_build_bytes[] = {
#embed "../../lib/prelude.vish" suffix(,)
#embed "../../lib/tools.vish" suffix(,)
#embed "../../lib/versions.vish" suffix(,)
#embed "../../lib/editor.vish" suffix(,)
#embed "../../lib/cli.vish" suffix(,)
#embed "../../lib/tests.vish"
};
uint64_t repository_hash(std::string_view bytes) {
  uint64_t h = 14695981039346656037ull;
  for(unsigned char c: bytes) h = (h ^ c) * 1099511628211ull;
  return h;
}
uint64_t repository_build() {
  return repository_hash({reinterpret_cast<char const *>(repository_build_bytes), sizeof repository_build_bytes});
}

struct repository_branch {
  std::string name;
  uint32_t head;
};

struct repository_state {
  std::string path;
  std::string branch = "main";
  std::vector<repository_module> modules;
  std::vector<repository_snapshot> snapshots;
  std::vector<repository_branch> branches{{"main", 0}};
  uint32_t next_id = 1;
};

struct repository_file {
  node_id root;
  repository_state state;
  std::vector<journal_entry> journal;
};

// The reserved names belong to host and are not included in the user manifest.
bool repository_builtin(std::string_view name) {
  return name == "prelude" || name == "tools" || name == "versions" || name == "tests" || name == "cli" || name == "editor";
}

repository_state repository_create(ast_db const & db, node_id root, std::string const & path) {
  if(std::filesystem::exists(path)) throw diagnostic(cat(path, ": файл уже существует"), {});
  if(!root || root.value >= db.size() || !db.is<n_module>(root)) throw diagnostic("репозиторий: неверный корень", {});
  repository_state state;
  state.path = path;
  for(uint32_t i = 0; i < db.size(); ++i) {
    node_id n(i);
    if(!db.is<n_module>(n) || db[n].parent) continue;
    auto key = db.as<n_module>(n).name;
    std::string name = key ? std::string(db.strings.text(key)) : std::string();
    if(n == root && name.empty()) name = "main";
    else if(name.empty()) continue;
    if(repository_builtin(name)) {
      if(n == root) throw diagnostic("репозиторий: корень — встроенный модуль", {});
      continue;
    }
    for(auto const & m: state.modules)
      if(m.name == name) throw diagnostic(cat("репозиторий: повтор имени ", name), {});
    state.modules.push_back({n, std::move(name)});
  }
  return state;
}

// The container contains journal once. The image is the prefix boundary of this journal,
// so the recovery does not lose future node numbers and the history of other branches.
constexpr std::string_view repository_magic = "VISHREPO01";
constexpr uint64_t repository_limit = 512ull * 1024 * 1024;

// Check links before the playback operation: regular replay trusts its journal. The checksum detects
// damage to bytes, this check is damage to the structure.
void repository_replay(ast_db & db, std::vector<journal_entry> const & entries) {
  auto bad = [] { throw diagnostic("репозиторий: повреждён журнал", {}); };
  std::vector<uint32_t> entered, finished;
  uint32_t visit = 0;
  auto tree = [&](node_id root) {
    entered.resize(db.size());
    finished.resize(db.size());
    ++visit;
    std::vector<std::pair<node_id, bool>> stack{{root, false}};
    while(!stack.empty()) {
      auto [n, leave] = stack.back();
      stack.pop_back();
      if(!n || n.value >= db.size()) bad();
      if(leave) { finished[n.value] = visit; continue; }
      if(entered[n.value] == visit) bad(); // cycle
      entered[n.value] = visit;
      stack.push_back({n, true});
      db.for_each_child(n, [&](node_id c) { stack.push_back({c, false}); });
      if(stack.size() > 1000000) bad();
    }
  };
  auto kind_ok = [&](node_kind const & kind, uint32_t count) {
    auto field = [&](auto const & self, auto const & v) -> void {
      using T = std::remove_cvref_t<decltype(v)>;
      if constexpr(std::is_same_v<T, node_id>) { if(v && v.value >= count) bad(); }
      else if constexpr(std::is_same_v<T, symbol>) { if(v && v.value >= db.strings.size()) bad(); }
      else if constexpr(is_vector<T>::value) for(auto const & x: v) self(self, x);
    };
    std::visit([&](auto const & k) {
      template for(constexpr auto m: fields_of<std::remove_cvref_t<decltype(k)>>()) field(field, k.[:m:]);
      if constexpr(std::is_same_v<std::remove_cvref_t<decltype(k)>, n_ref>)
        if(k.target >= count) bad();
    }, kind);
  };
  auto place_ok = [&](span w) {
    if(w.source >= db.sources.size()) { if(!db.sources.empty() || w.source) bad(); }
    else if(w.begin > w.end || w.end > db.source(w.source).size()) bad();
  };
  for(auto const & e: entries) {
    std::visit([&](auto const & op) {
      using T = std::remove_cvref_t<decltype(op)>;
      if constexpr(std::is_same_v<T, j_intern>) {
        if(op.id != db.strings.size() || db.strings.index.contains(op.text)) bad();
      } else if constexpr(std::is_same_v<T, j_add>) {
        if(op.id != db.size() || db.size() >= 1000000) bad();
        kind_ok(op.kind, db.size());
        place_ok(op.where);
      } else if constexpr(std::is_same_v<T, j_replace> || std::is_same_v<T, j_mutate>) {
        if(op.id >= db.size()) bad();
        kind_ok(op.kind, db.size());
        if constexpr(std::is_same_v<T, j_mutate>) if(op.source && op.source - 1 >= db.size()) bad();
      } else if constexpr(std::is_same_v<T, j_link>) {
        tree(node_id(op.root));
      } else if constexpr(std::is_same_v<T, j_place>) {
        if(op.id >= db.size()) bad();
        place_ok(op.where);
      } else if constexpr(std::is_same_v<T, j_note> || std::is_same_v<T, j_derived>) {
        if(op.id >= db.size()) bad();
      } else if constexpr(std::is_same_v<T, j_gap>) {
        if(op.nodes > 1000000 - db.size() || op.imports > 100000) bad();
      } else if constexpr(std::is_same_v<T, j_rollback>) {
        if(op.source && op.source - 1 >= db.size()) bad();
      }
    }, e.op);
    db.replay({e});
  }
}

void repository_validate(repository_file const & f, ast_db const & db) {
  auto bad = [] { throw diagnostic("репозиторий: повреждён каталог или история", {}); };
  if(f.root.value >= db.size() || !db.is<n_module>(f.root)) bad();
  auto modules = [&](auto const & xs) {
    bool main = false;
    std::set<uint32_t> ids;
    std::set<std::string> names;
    for(auto const & m: xs) {
      if(m.node.value >= db.size() || !db.is<n_module>(m.node) || m.name.empty() || repository_builtin(m.name) || !ids.insert(m.node.value).second || !names.insert(m.name).second) bad();
      main = main || m.node == f.root;
    }
    if(!main) bad();
  };
  modules(f.state.modules);
  std::set<uint32_t> ids{0};
  uint64_t previous_end = 0;
  for(auto const & s: f.state.snapshots) {
    if(!s.id || s.id >= f.state.next_id || !s.end || s.end > f.journal.size() || s.end < previous_end || s.name.empty()) bad();
    for(auto p: s.parents) if(!p || !ids.contains(p)) bad();
    if(!ids.insert(s.id).second) bad();
    previous_end = s.end;
    modules(s.modules);
  }
  bool active = false;
  std::set<std::string> names;
  for(auto const & b: f.state.branches) {
    if(b.name.empty() || !names.insert(b.name).second || !ids.contains(b.head)) bad();
    active = active || b.name == f.state.branch;
  }
  if(!active) bad();
}

void repository_save(repository_state const & state, ast_db const & db, node_id root) {
  repository_file f{root, state, db.journal()};
  repository_validate(f, db);
  writer body;
  body.put(repository_build());
  body.put(std::string(vishdb_magic));
  body.put(f);
  if(body.bytes.size() > repository_limit) throw diagnostic("репозиторий больше 512 МиБ", {});
  writer out;
  out.raw(repository_magic.data(), repository_magic.size());
  out.put(uint64_t(body.bytes.size()));
  out.put(repository_hash(body.bytes));
  out.raw(body.bytes.data(), body.bytes.size());
  os::write_file_atomic(state.path, out.bytes);
}

repository_file repository_load(std::string const & path) {
  os::mapped_file file(path);
  auto bytes = file.text();
  if(!bytes.starts_with(repository_magic) || bytes.size() > repository_limit + 26) throw diagnostic("не файл репозитория Vish", {});
  reader header(bytes.substr(repository_magic.size()));
  auto length = header.get<uint64_t>();
  auto checksum = header.get<uint64_t>();
  auto payload = header.in.substr(header.pos);
  if(length != payload.size() || checksum != repository_hash(payload)) throw diagnostic("репозиторий повреждён: длина или контрольная сумма", {});
  reader r(payload);
  if(r.get<uint64_t>() != repository_build() || r.get<std::string>() != vishdb_magic) throw diagnostic("репозиторий создан другой сборкой Vish; требуется совместимый экспорт", {});
  auto f = r.get<repository_file>();
  if(!r.at_end()) throw diagnostic("лишние данные репозитория", {});
  f.state.path = path;
  ast_db checked;
  repository_replay(checked, f.journal);
  repository_validate(f, checked);
  return f;
}

} // namespace vish
