// Protocol editor.VM (spec §13.1, M5).
//
// 
// The client sends control signals - the cursor lives in VM, in AST
// coordinates. VM sends node (view + layout projection) and cursor.
//
//
//
// Frame: u32 body length, body - message recorded by the same writer as the base
// journal (u32 view number, then field). Binary: Message description - only these
// aggregates: the text form (tests) and the client decoder are derived from them by
// reflection.
#pragma once

#include "printer.hpp"
#include "serial.hpp"

#include <array>

namespace tz::proto {

constexpr uint32_t version = 1;

// --- client ---

struct c_hello {
  uint32_t version;
};
// View is a VM object created by the client
// 
// The module is a text projection module.
struct c_open_view {
  std::string kind;
};
struct c_close_view {
  uint32_t view;
};
// Keyboard: KeyboardEvent.key in the browser; mods are shift/ctrl/alt/meta bits.
struct c_key {
  uint32_t view;
  std::string key;
  uint32_t mods;
};
// Pointer: a place in the proper text node (piece, offset in code points).
struct c_pointer {
  uint32_t view;
  node_id node;
  uint32_t piece;
  uint32_t offset;
};
using client_msg = std::variant<c_hello, c_open_view, c_close_view, c_key, c_pointer>;

// --- VM customer ---

struct s_hello {
  uint32_t version;
  std::string program;
  uint64_t revision;
};
// Node projection: The view (name of the aggregate node) and the pieces are the text or
// place of the child. Note node (cause of the hole): Decoration, not text.
struct s_node {
  node_id id;
  std::string kind;
  std::vector<piece> pieces;
  std::string note;
};
struct s_view {
  uint32_t view;
  node_id root;
  uint64_t revision;
  std::vector<s_node> nodes;
};
// node node subtree replaced (id node saved, children new). It is sent to all
// customers - one state.
struct s_update {
  uint32_t view;
  uint64_t revision;
  node_id node;
  std::vector<s_node> nodes;
};
// Cursor: node, number of the piece-text, offset in code points.
struct s_cursor {
  uint32_t view;
  node_id node;
  uint32_t piece;
  uint32_t offset;
};
struct s_error {
  std::string text;
};
// Supplement: options for the place (node, piece, offset - the beginning of the typed
// word); selected - selected. An empty list to hide. The status of the list is in VM,
// just like the cursor: the client only draws.
struct s_item {
  std::string name;
  std::string kind;
  std::string detail;
};
struct s_complete {
  uint32_t view;
  node_id node;
  uint32_t piece;
  uint32_t offset;
  uint32_t selected;
  std::vector<s_item> items;
};
// Check tags on node : kind "type" - types of declared name for all instance ,
// "error" - error in place of use (with a track). Decoration, not text; come in whole
// after every revision.
struct s_mark {
  node_id node;
  std::string kind;
  std::string text;
};
struct s_decor {
  uint32_t view;
  uint64_t revision;
  std::vector<s_mark> marks;
};
using server_msg = std::variant<s_hello, s_view, s_cursor, s_error, s_update, s_complete, s_decor>;

// --- staff ---

std::string frame(auto const & msg) {
  writer w;
  w.put(uint32_t(0));
  w.put(msg);
  uint32_t n = uint32_t(w.bytes.size() - 4);
  std::memcpy(w.bytes.data(), &n, 4);
  return std::move(w.bytes);
}

// Remove entire footage from the buffer.
auto take_frames(std::string & in, auto && on_body) {
  std::size_t at = 0;
  while(in.size() - at >= 4) {
    uint32_t n;
    std::memcpy(&n, in.data() + at, 4);
    if(in.size() - at - 4 < n) break;
    on_body(std::string_view(in).substr(at + 4, n));
    at += 4 + n;
  }
  in.erase(0, at);
}

void decode(std::string_view body, auto & m) {
  reader r(body);
  m = r.get<std::remove_cvref_t<decltype(m)>>();
  if(!r.at_end()) throw diagnostic("протокол: лишние байты в кадре", {});
}

// --- text form (test scripts, debugging) ---
//
// The message name is the name of the aggregate without the prefix c /s ; the field is in order.

consteval std::string_view short_name(std::meta::info t) {
  std::string_view n = std::meta::identifier_of(std::meta::dealias(t));
  return n.substr(2);
}

void describe_fields(std::string & out, auto const & x);

void describe_value(std::string & out, auto const & x) {
  using T = std::remove_cvref_t<decltype(x)>;
  if constexpr(std::is_same_v<T, node_id>) out += x ? cat("#", x.value) : std::string("·");
  else if constexpr(std::is_arithmetic_v<T>) out += cat(x);
  else if constexpr(std::is_same_v<T, std::string>) out += in_quotes(x);
  else if constexpr(is_variant<T>::value) std::visit([&](auto const & a) { describe_value(out, a); }, x);
  else if constexpr(is_vector<T>::value) {
    using E = typename T::value_type;
    if constexpr(std::is_aggregate_v<E> && !is_id<E>::value) {
      for(auto const & e: x) {
        out += "\n  ";
        describe_fields(out, e);
      }
    } else {
      out += '[';
      for(std::size_t i = 0; i < x.size(); ++i) {
        if(i) out += ' ';
        describe_value(out, x[i]);
      }
      out += ']';
    }
  } else describe_fields(out, x);
}

void describe_fields(std::string & out, auto const & x) {
  bool first = true;
  template for(constexpr auto m: fields_of<std::remove_cvref_t<decltype(x)>>()) {
    if(!first) out += ' ';
    first = false;
    out += cat(std::meta::identifier_of(m), "=");
    describe_value(out, x.[:m:]);
  }
}

std::string describe(auto const & msg) {
  std::string out;
  std::visit([&](auto const & m) {
    out += short_name(std::meta::remove_cvref(^^decltype(m)));
    out += ' ';
    describe_fields(out, m);
  },
      msg);
  return out + '\n';
}

// parsing text form: words in the order of fields; missing fields - zeros; in the
// line `\s` - space (space key: `key 1 \s 2` - Ctrl + Space).
void parse_fields(auto & m, std::vector<std::string_view> const & words) {
  std::size_t at = 1;
  template for(constexpr auto f: fields_of<std::remove_cvref_t<decltype(m)>>()) {
    using F = [:std::meta::type_of(f):];
    if(at < words.size()) {
      auto w = words[at++];
      if constexpr(std::is_same_v<F, std::string>) {
        std::string t(w);
        for(std::size_t k; (k = t.find("\\s")) != std::string::npos;) t.replace(k, 2, " ");
        m.[:f:] = t;
      }
      else {
        auto digits = w.substr(std::is_same_v<F, node_id> && w.starts_with('#'));
        uint64_t x = 0;
        auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), x);
        if(ec != std::errc{} || end != digits.data() + digits.size()) throw diagnostic(cat("протокол: не число: ", w), {});
        if constexpr(std::is_same_v<F, node_id>) m.[:f:] = node_id(uint32_t(x));
        else m.[:f:] = F(x);
      }
    }
  }
}

client_msg parse_client(std::string_view line) {
  auto words = words_of(line);
  if(words.empty()) throw diagnostic("протокол: пустое сообщение", {});
  std::optional<client_msg> out;
  static constexpr auto kinds = [] {
    std::array<std::size_t, std::variant_size_v<client_msg>> a{};
    for(std::size_t k = 0; k < a.size(); ++k) a[k] = k;
    return a;
  }();
  template for(constexpr std::size_t k: kinds) {
    constexpr std::string_view name = short_name(^^std::variant_alternative_t<k, client_msg>);
    if(!out && words[0] == name) parse_fields(std::get<k>(out.emplace(std::in_place_index<k>)), words);
  }
  if(!out) throw diagnostic(cat("протокол: неизвестное сообщение ", words[0]), {});
  return *out;
}

} // namespace tz::proto

namespace tz::proto {

// --- scheme ---
//
// Message description as data: The client (JS) decodes the frames with one generalized
// reader in a diagram - like a writer/reader here, without code for each type:
// 
// Types: "bool" "u8" "u32" "u64" "i64" "f64" "string" "node" (u32, ~0 - no node),
// ["vector", T], ["variant", T...], the name of the unit.

struct schema_builder {
  std::string types;
  std::vector<std::string_view> seen;

  void type(std::string & out, auto t) {
    using T = typename decltype(t)::type;
    if constexpr(std::is_same_v<T, bool>) out += "\"bool\"";
    else if constexpr(std::is_enum_v<T>) type(out, std::type_identity<std::underlying_type_t<T>>{});
    else if constexpr(std::is_same_v<T, uint8_t>) out += "\"u8\"";
    else if constexpr(std::is_same_v<T, uint32_t>) out += "\"u32\"";
    else if constexpr(std::is_same_v<T, uint64_t>) out += "\"u64\"";
    else if constexpr(std::is_same_v<T, int64_t>) out += "\"i64\"";
    else if constexpr(std::is_same_v<T, double>) out += "\"f64\"";
    else if constexpr(std::is_same_v<T, node_id>) out += "\"node\"";
    else if constexpr(is_id<T>::value) out += "\"u32\"";
    else if constexpr(std::is_same_v<T, std::string>) out += "\"string\"";
    else if constexpr(is_vector<T>::value) {
      out += "[\"vector\", ";
      type(out, std::type_identity<typename T::value_type>{});
      out += ']';
    } else if constexpr(is_variant<T>::value) {
      out += "[\"variant\"";
      alternatives(out, t, std::make_index_sequence<std::variant_size_v<T>>{});
      out += ']';
    } else {
      static_assert(std::is_aggregate_v<T>, "в схеме — агрегаты");
      constexpr std::string_view name = std::meta::identifier_of(std::meta::dealias(^^T));
      out += cat("\"", name, "\"");
      if(std::ranges::contains(seen, name)) return;
      seen.push_back(name);
      std::string fields;
      template for(constexpr auto m: fields_of<T>()) {
        if(!fields.empty()) fields += ", ";
        fields += cat("[\"", std::meta::identifier_of(m), "\", ");
        type(fields, std::type_identity<typename [:std::meta::type_of(m):]>{});
        fields += ']';
      }
      types += cat(types.empty() ? "" : ",\n", "    \"", name, "\": [", fields, "]");
    }
  }

  void alternatives(std::string & out, auto t, auto seq) {
    using T = typename decltype(t)::type;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      ((out += ", ", type(out, std::type_identity<std::variant_alternative_t<I, T>>{})), ...);
    }(seq);
  }
};

std::string schema() {
  schema_builder b;
  std::string client, server, kinds;
  b.type(client, std::type_identity<client_msg>{});
  b.type(server, std::type_identity<server_msg>{});
  [&]<std::size_t... I>(std::index_sequence<I...>) {
    ((kinds += cat(I ? ", " : "", "\"", std::meta::identifier_of(std::meta::dealias(^^std::variant_alternative_t<I, node_kind>)), "\"")), ...);
  }(std::make_index_sequence<std::variant_size_v<node_kind>>{});
  return cat("{\n  \"version\": ", version, ",\n  \"client\": ", client, ",\n  \"server\": ", server,
      ",\n  \"node_kinds\": [", kinds, "],\n  \"types\": {\n", b.types, "\n  }\n}\n");
}

} // namespace tz::proto
