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
  std::string branch;
  std::vector<repository_module> modules;
  std::vector<repository_snapshot> snapshots;
  std::vector<repository_branch> branches;
  uint32_t next_id = 1;
  uint64_t generation = 0; // changes to the directory, pictures and branches in addition to the edits AST
  mutable uint64_t disk_checksum = 0; // last read/written version of the file
};

struct repository_file {
  node_id root;
  repository_state state;
  std::vector<journal_entry> journal;
};

repository_state repository_create(ast_db const & db, node_id root, std::string const & path) {
  if(os::file_exists(path)) throw diagnostic(cat(path, ": файл уже существует"), {});
  if(!root || root.value >= db.size() || !db.is<n_module>(root)) throw diagnostic("репозиторий: неверный корень", {});
  repository_state state;
  state.path = path;
  return state;
}

// The container contains journal once. The image is the prefix boundary of this journal,
// so the recovery does not lose future node numbers and the history of other branches.
constexpr std::string_view repository_magic = "VISHREPO04";
constexpr uint64_t repository_limit = 512ull * 1024 * 1024;

// Check links before the playback operation: regular replay trusts its journal. The checksum detects
// damage to bytes, this check is damage to the structure.
void repository_replay(ast_db & db, std::vector<journal_entry> const & entries) {
  uint64_t step = 0;
  std::string_view reason = "ссылка";
  auto bad = [&] { throw diagnostic(cat("репозиторий: повреждён журнал (операция ", step, ", ", reason, ")"), {}); };
  std::vector<uint32_t> entered;
  uint32_t visit = 0;
  auto tree = [&](node_id root) {
    reason = "дерево";
    entered.resize(db.size());
    ++visit;
    std::vector<std::pair<node_id, uint32_t>> stack{{root, 0}};
    while(!stack.empty()) {
      auto [n, depth] = stack.back();
      stack.pop_back();
      if(!n || n.value >= db.size()) bad();
      if(depth > 2048) bad();
      if(entered[n.value] == visit) bad(); // cycle
      entered[n.value] = visit;
      db.for_each_child(n, [&](node_id c) { stack.push_back({c, depth + 1}); });
      if(stack.size() > 1000000) bad();
    }
  };
  auto kind_ok = [&](node_kind const & kind, uint32_t count) {
    reason = "поля узла";
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
      if constexpr(std::is_same_v<std::remove_cvref_t<decltype(k)>, n_extend>)
        if(k.target && k.target >= count) bad();
    }, kind);
  };
  auto place_ok = [&](span w) {
    reason = "позиция узла";
    if(w.source >= db.sources.size()) { if(!db.sources.empty() || w.source) bad(); }
    // Virtual closing brackets/synthetic node incomplete input can have a position
    // behind the end of the import. Replay doesn't name her.
    if(w.begin > w.end) bad();
  };
  for(auto const & e: entries) {
    ++step;
    reason = "ссылка";
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
    if(auto link = std::get_if<j_link>(&e.op)) {
      reason = "обратные связи";
      auto parent = [&](node_id n) {
        if(!n) return node_id{};
        if(n.value >= db.size()) bad();
        return db[n].parent;
      };
      node_id slow(link->root), fast = slow;
      do {
        slow = parent(slow); fast = parent(parent(fast));
        if(slow && slow == fast) bad();
      } while(slow && fast);
    }
  }
}

void repository_validate(repository_file const & f, ast_db const & db) {
  auto bad = [] { throw diagnostic("репозиторий: повреждён каталог или история", {}); };
  if(f.root.value >= db.size() || !db.is<n_module>(f.root)) bad();
  auto modules = [&](auto const & xs) {
    for(auto const & m: xs)
      if(m.node.value >= db.size() || !db.is<n_module>(m.node)) bad();
  };
  modules(f.state.modules);
  std::vector<uint64_t> module_birth(db.size(), ~uint64_t{0});
  for(std::size_t k = 0; k < f.journal.size(); ++k)
    if(auto added = std::get_if<j_add>(&f.journal[k].op); added && std::holds_alternative<n_module>(added->kind) && added->id < db.size()) module_birth[added->id] = k + 1;
  for(auto const & s: f.state.snapshots) {
    if(!s.end || s.end > f.journal.size()) bad();
    modules(s.modules);
    for(auto const & m: s.modules) if(module_birth[m.node.value] > s.end) bad();
  }

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
  os::file_lock lock(state.path + ".lock");
  if(state.disk_checksum) {
    if(repository_hash(os::mapped_file(state.path).text()) != state.disk_checksum)
      throw diagnostic("файл репозитория изменился — сохранение отклонено", {});
  } else if(os::file_exists(state.path)) throw diagnostic("файл репозитория уже существует", {});
  os::write_file_atomic(state.path, out.bytes);
  state.disk_checksum = repository_hash(out.bytes);
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
  f.state.disk_checksum = repository_hash(bytes);
  ast_db checked;
  repository_replay(checked, f.journal);
  repository_validate(f, checked);
  return f;
}

} // namespace vish
