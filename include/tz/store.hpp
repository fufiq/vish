// Storing an AST database on a disk: binary journal operations (.tzdb).
//
// Format: Magic "TZDB0001", root module number, then journal records, each
// serialized generically through reflection (serial.hpp).
#pragma once

#include "ast.hpp"
#include "os.hpp"
#include "serial.hpp"

namespace tz {

constexpr std::string_view tzdb_magic = "TZDB0021"; // 0021: template tagged - call (n call.tagged, without n template); 0020: try compile - call the kernel (without n try compile); 0019: hide - call the kernel (without n hide); 0018: require - call the kernel (without n require); 0017: 0x-literal (n int.hex); 0016: native (n n native); 0015: syntax (n call.form, n call.form); 0014 (0014:ntricttric:n000), from the base (00:nx:1000:nx:n.000);

void save_db(ast_db const & db, node_id root, std::string const & path) {
  writer w;
  for(char c: tzdb_magic) w.put(c);
  w.put(root.value);
  for(auto const & e: db.journal()) w.put(e);
  os::write_file(path, w.bytes);
}

std::vector<journal_entry> read_journal(std::string const & path, node_id & root) {
  os::mapped_file file(path);
  reader r(file.text());
  for(char c: tzdb_magic)
    if(r.get<char>() != c) throw diagnostic(cat(path, ": не файл базы tz"), {});
  root = node_id(r.get<uint32_t>());
  std::vector<journal_entry> entries;
  while(!r.at_end()) entries.push_back(r.get<journal_entry>());
  return entries;
}

node_id load_db(ast_db & db, std::string const & path) {
  node_id root;
  db.replay(read_journal(path, root));
  return root;
}

// journal in text.
std::string describe_journal(std::vector<journal_entry> const & entries) {
  std::string out;
  for(auto const & e: entries) {
    out += cat("r", pad(cat(e.revision), 5), " ");
    std::visit([&](auto const & op) {
      using T = std::remove_cvref_t<decltype(op)>;
      if constexpr(std::is_same_v<T, j_intern>) out += cat("строка  #", op.id, " ", in_quotes(op.text));
      else if constexpr(std::is_same_v<T, j_add>) {
        out += cat("узел    #", op.id, " ");
        out += std::visit([](auto const & k) -> std::string_view {
          return std::meta::identifier_of(std::meta::remove_cvref(^^decltype(k)));
        },
            op.kind);
        out += cat(" [", op.where.begin, "..", op.where.end, "]");
      } else if constexpr(std::is_same_v<T, j_replace>) out += cat("замена  #", op.id);
      else if constexpr(std::is_same_v<T, j_link>) out += cat("связи   от #", op.root);
      else out += cat("импорт  текста ", op.text.size(), " байт");
    },
        e.op);
    out += '\n';
  }
  return out;
}

} // namespace tz
