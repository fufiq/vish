// Debugging output of the AST tree is one implementation for all types of node
// through reflection: new types of node are printed without editing this file.
#pragma once

#include "ast.hpp"

namespace vish {

struct dumper {
  explicit dumper(ast_db const & db) : db(db) {}

  std::string dump(node_id n) {
    out.clear();
    visit(n, 0);
    return out;
  }
  void visit(node_id n, uint32_t depth) {
    out.append(depth * 2, ' ');
    if(!n) {
      out += "·\n";
      return;
    }
    out += kind_name(db[n]);
    std::visit([&](auto const & k) { scalars(k); }, db[n].kind);
    out += '\n';
    std::visit([&](auto const & k) { children(k, depth + 1); }, db[n].kind);
  }

  void scalars(auto const & k) {
    template for(constexpr auto m: fields_of<std::remove_cvref_t<decltype(k)>>()) {
      using field_t = [:std::meta::type_of(m):];
      constexpr auto name = std::meta::identifier_of(m);
      if constexpr(std::is_same_v<field_t, symbol>) {
        out += cat(" ", name, "=", db.strings.text(k.[:m:]));
      } else if constexpr(std::is_same_v<field_t, bool>) {
        if(k.[:m:]) out += cat(" ", name);
      } else if constexpr(std::is_same_v<field_t, int64_t> || std::is_same_v<field_t, double>) {
        out += cat(" ", name, "=", k.[:m:]);
      } else if constexpr(std::is_same_v<field_t, std::string> || std::is_same_v<field_t, node_text>) {
        out += cat(" ", name, "=", in_quotes(std::string(std::string_view(k.[:m:]))));
      } else if constexpr(std::is_enum_v<field_t>) {
        out += cat(" ", name, "=", uint32_t(k.[:m:]));
      } else if constexpr(std::is_same_v<field_t, list<symbol>> && std::meta::identifier_of(m) == "words") { // clause labels of n_call.words: without them the dumps of different clauses are equal
        if(!k.[:m:].empty()) {
          out += cat(" ", name, "=[");
          bool first = true;
          for(symbol w: k.[:m:]) {
            if(!first) out += ',';
            first = false;
            out += db.strings.text(w);
          }
          out += ']';
        }
      }
    }
  }

  void children(auto const & k, uint32_t depth) {
    template for(constexpr auto m: fields_of<std::remove_cvref_t<decltype(k)>>()) {
      using field_t = [:std::meta::type_of(m):];
      if constexpr(std::is_same_v<field_t, node_id>) {
        if(k.[:m:]) {
          out.append(depth * 2, ' ');
          out += cat(std::meta::identifier_of(m), ":\n");
          visit(k.[:m:], depth + 1);
        }
      } else if constexpr(std::is_same_v<field_t, list<node_id>>) {
        for(node_id c: k.[:m:]) visit(c, depth);
      }
    }
  }

  ast_db const & db;
  std::string out;
};

} // namespace vish
