// vish is the VM of vish. One unit of translation: all code is headers (HO-Build).
//
// The source of truth is AST (D7). Any command accepts either text (.vish - import text projection
// to the database) or the base itself (.vishdb - journal). The launch line is in language
// (lib/tools.vish, cli; cli.hpp are the commands themselves); here is the input, the VM stack and
// the `vish native` bootstrap: it collects primitives, without which the tool module does not exist.
#include "vish/base.hpp"
#include "vish/os.hpp"
#include "vish/lexer.hpp"
#include "vish/ast.hpp"
#include "vish/parser.hpp"
#include "vish/prelude.hpp"
#include "vish/printer.hpp"
#include "vish/dump.hpp"
#include "vish/interp.hpp"
#include "vish/test_runner.hpp"
#include "vish/views.hpp"
#include "vish/store.hpp"
#include "vish/session.hpp"
#include "vish/server.hpp"
#include "vish/editor_lib.hpp"
#include "vish/tools.hpp"
#include "vish/machine.hpp"
#include "vish/modules.hpp"
#include "vish/jit/jit.hpp"
#include "vish/cli.hpp"
// out/native.hpp (vish native, build.sh): native "cpp" prelude bodies and tools modules, editor - after the
// entire VM: their primitives are called server, test runner, versions, commands
#include "native.hpp"

namespace {

using vish::cat;

// Assembling native “cpp” { … } (§3.3): Bodies from prelude and data files into a header that VM
// compiles into itself (build.sh). The file is rewritten only if it has changed: the exit code 3 is
// “reassemble”. "We need a basic language and a bootstrap."
int native(int argc, char ** argv) {
  try {
    vish::ast_db db;
    vish::load_prelude(db);
    for(int i = 3; i < argc; ++i) {
      vish::os::mapped_file f(argv[i]);
      vish::parser p(db, f.text(), argv[i]);
      p.parse_module();
      if(!p.errors().empty()) throw p.errors().front();
    }
    std::string out = "// Сгенерировано `vish native` из native \"cpp\" { … } библиотек — не править.\n#pragma once\n\nnamespace vish {\n\n";
    std::string table;
    uint32_t k = 0;
    for(uint32_t i = 0; i < db.size(); ++i) {
      if(!db.is<vish::n_native>(vish::node_id(i))) continue;
      vish::node_id fn = vish::native_owner(db, vish::node_id(i));
      std::string code = db.as<vish::n_native>(vish::node_id(i)).code;
      auto const & ps = db.as<vish::n_fn>(fn).params;
      for(std::size_t j = 0; j < ps.size(); ++j) { // #x - parameter x
        std::string name = cat("#", db.strings.text(db.as<vish::n_param>(ps[j]).name));
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
      std::string key = vish::native_key(db, fn);
      out += cat("// ", key, "\nvalue native_", k, "(interpreter & vm, std::vector<value> & a, span where) {", code, "}\n\n");
      table += cat("    {\"", key, "\", native_", k, "},\n");
      ++k;
    }
    out += k ? cat("native_entry const native_entries[] = {\n", table, "};\n\nstd::span<native_entry const> native_table() { return native_entries; }\n")
             : std::string("std::span<native_entry const> native_table() { return {}; }\n");
    out += "\n} // namespace vish\n";
    std::string old;
    if(std::filesystem::exists(argv[2])) old = std::string(vish::os::mapped_file(argv[2]).text());
    if(old == out) return 0;
    vish::os::write_file(argv[2], out);
    return 3;
  } catch(vish::diagnostic const & d) {
    vish::put(stderr, cat(d.what(), "\n"));
    return 1;
  }
}

} // namespace

int run(int argc, char ** argv);

// All work is on the VM 256MB stack (os::with_stack): depth is not the limit of the host stack.
int main(int argc, char ** argv) {
  return vish::os::with_stack(size_t(256) << 20, [&] { return run(argc, argv); });
}

int run(int argc, char ** argv) {
  if(argc >= 3 && std::string_view(argv[1]) == "native") return native(argc, argv);
  return vish::run_cli(argc, argv);
}
