// tz - VM king. One unit of translation: all code is headers (HO-Build).
//
// The source of truth is AST (D7). Any command accepts either text (.tz - import text projection
// to the database) or the base itself (.tzdb - journal). The launch line is in language
// (lib/tools.tz, cli; cli.hpp are the commands themselves); here is the input, the VM stack and
// the `tz native` bootstrap: it collects primitives, without which there is no tool module.
#include "tz/base.hpp"
#include "tz/os.hpp"
#include "tz/lexer.hpp"
#include "tz/ast.hpp"
#include "tz/parser.hpp"
#include "tz/prelude.hpp"
#include "tz/printer.hpp"
#include "tz/dump.hpp"
#include "tz/interp.hpp"
#include "tz/test_runner.hpp"
#include "tz/views.hpp"
#include "tz/store.hpp"
#include "tz/session.hpp"
#include "tz/server.hpp"
#include "tz/editor_lib.hpp"
#include "tz/tools.hpp"
#include "tz/machine.hpp"
#include "tz/modules.hpp"
#include "tz/jit/jit.hpp"
#include "tz/cli.hpp"
// out/native.hpp (tz native, build.sh): native "cpp" prelude bodies and tools modules, editor - after the
// entire VM: their primitives are called server, runner tests, versions, commands
#include "native.hpp"

namespace {

using tz::cat;

// Assembling native “cpp” { … } (§3.3): Bodies from prelude and data files into a header that VM
// compiles into itself (build.sh). The file is rewritten only if it has changed: the exit code 3 is
// “reassemble”. "We need a basic language and a bootstrap."
int native(int argc, char ** argv) {
  try {
    tz::ast_db db;
    tz::load_prelude(db);
    for(int i = 3; i < argc; ++i) {
      tz::os::mapped_file f(argv[i]);
      tz::parser p(db, f.text(), argv[i]);
      p.parse_module();
      if(!p.errors().empty()) throw p.errors().front();
    }
    std::string out = "// Сгенерировано `tz native` из native \"cpp\" { … } библиотек — не править.\n#pragma once\n\nnamespace tz {\n\n";
    std::string table;
    uint32_t k = 0;
    for(uint32_t i = 0; i < db.size(); ++i) {
      if(!db.is<tz::n_native>(tz::node_id(i))) continue;
      tz::node_id fn = tz::native_owner(db, tz::node_id(i));
      std::string code = db.as<tz::n_native>(tz::node_id(i)).code;
      auto const & ps = db.as<tz::n_fn>(fn).params;
      for(std::size_t j = 0; j < ps.size(); ++j) { // #x - parameter x
        std::string name = cat("#", db.strings.text(db.as<tz::n_param>(ps[j]).name));
        for(std::size_t at = code.find(name); at != std::string::npos; at = code.find(name, at)) {
          char next = at + name.size() < code.size() ? code[at + name.size()] : ' ';
          if(std::isalnum(static_cast<unsigned char>(next)) || next == '_') {
            at += name.size();
            continue;
          }
          std::string arg = cat("(a[", j, "])");
          code.replace(at, name.size(), arg);
          at += arg.size();
        }
      }
      std::string key = tz::native_key(db, fn);
      out += cat("// ", key, "\nvalue native_", k, "(interpreter & vm, std::vector<value> & a, span where) {", code, "}\n\n");
      table += cat("    {\"", key, "\", native_", k, "},\n");
      ++k;
    }
    out += k ? cat("native_entry const native_entries[] = {\n", table, "};\n\nstd::span<native_entry const> native_table() { return native_entries; }\n")
             : std::string("std::span<native_entry const> native_table() { return {}; }\n");
    out += "\n} // namespace tz\n";
    std::string old;
    if(std::filesystem::exists(argv[2])) old = std::string(tz::os::mapped_file(argv[2]).text());
    if(old == out) return 0;
    tz::os::write_file(argv[2], out);
    return 3;
  } catch(tz::diagnostic const & d) {
    tz::put(stderr, cat(d.what(), "\n"));
    return 1;
  }
}

} // namespace

int run(int argc, char ** argv);

// All work is on the VM 256MB stack (os::with_stack): depth is not the limit of the host stack.
int main(int argc, char ** argv) {
  return tz::os::with_stack(size_t(256) << 20, [&] { return run(argc, argv); });
}

int run(int argc, char ** argv) {
  if(argc >= 3 && std::string_view(argv[1]) == "native") return native(argc, argv);
  return tz::run_cli(argc, argv);
}
