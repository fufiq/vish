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

constexpr uint32_t version = 6;

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
// Signature transition (Ctrl+P): VM responds with a list (s_complete) -
// overload and “add”.
struct c_goto {
  uint32_t view;
  std::string text;
};
// The presentation layer of the function is the context of its use (key instance);
// the blank key is “all”: labels for all contexts.
struct c_layer {
  uint32_t view;
  std::string key;
};
// Launch (F5): Main of the current base state in the same VM
// 
// stdin - the lines of program input.
struct c_run {
  std::string stdin_text;
  std::string args;
};
// Stop Launch (Shift+F5):
//
struct c_stop {};
// Save (Ctrl+S): journal base in .tzdb next to the program (author's decision).
struct c_save {};
// Merger with another copy of the database (§9.4): the path to .tzdb (relative to the program).
struct c_merge {
  std::string path;
};
// Solve the conflict: take their version of the element or leave mine.
struct c_take {
  std::string key;
  bool theirs;
};
// Launch with track (F6): steps on it - keys (F10, F11, F4, Escape), they are interpreted by VM.
struct c_trace {
  std::string stdin_text;
  std::string args;
};
// projection presentation (§4.4, variant C): syntax recording module with key - print
// recording and parsing editing rules of this representation; "" - by default (records by
// scope, prelude - C-like projection, D3).
struct c_projection {
  uint32_t view;
  std::string key;
};
// Enter the whole text: insert from the clipboard, IME - as a type from the keyboard.
struct c_input {
  uint32_t view;
  std::string text;
};
// Selection by pointer: stretch or Shift+click - to the place (clicks 1); double click -
// word, triple - line.
struct c_select {
  uint32_t view;
  node_id node;
  uint32_t piece;
  uint32_t offset;
  uint32_t clicks;
};
// Hover to the node: VM responds with a hint (s_hover).
struct c_hover {
  uint32_t view;
  node_id node;
};
// Hint link: the function announcement is in the second panel, another is the cursor to it.
struct c_open {
  uint32_t view;
  node_id node;
};
// AST Inspector: The tree of the module element in which the node node (focus) is located.
struct c_inspect {
  uint32_t view;
  node_id node;
};
// Apply the editing function of the program (without parameters): its code_rename, code_replace
// ... change the program through the base as a human edit (§7.4). The answer is s_output.
struct c_apply {
  std::string fn;
};
using client_msg = std::variant<c_hello, c_open_view, c_close_view, c_key, c_pointer, c_goto, c_layer, c_run, c_save, c_trace, c_merge, c_take, c_projection, c_input, c_select, c_hover, c_open, c_inspect, c_apply, c_stop>;

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
// kind - view: module (module without internal elements) or fn (one function is open
// by transition, in the second panel).
struct s_view {
  uint32_t view;
  node_id root;
  uint64_t revision;
  std::vector<s_node> nodes;
  std::string kind;
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
// Check (check) on node : kind "type" - types of declared name for all instance (in
// the layer - in its context), "error" - error in the place of use (with a trace),
// "value" - known at compilation value in the layer, "dead" - in the context of the
// layer was not executed. Decoration, not text; come in whole after every revision.
// [from, to] is the place inside the node text at the code points (parsing error
// inside the raw block is an atom and displacement in it, §4.4); from ==to is the
// entire node.
struct s_mark {
  node_id node;
  std::string kind;
  std::string text;
  uint32_t from = 0;
  uint32_t to = 0;
};
struct s_decor {
  uint32_t view;
  uint64_t revision;
  std::vector<s_mark> marks;
};
// Feature representation layers: contexts of use; kind - "compile-time" (call folded when
// compiled in its entirety) or "run-time"; selected - key or" (all).
struct s_layer {
  std::string key;
  std::string kind;
};
struct s_layers {
  uint32_t view;
  std::vector<s_layer> layers;
  std::string selected;
};
// Startup output: Print the program and error with the track (ok - no error). Start-up is the process of
// the machine, output as it is executed: more is a continuation of the former, state - "running" (goes),
// "stopped" (stopped)," - is finished.
struct s_output {
  std::string text;
  bool ok;
  bool more;
  std::string state;
};
struct s_saved {
  std::string path;
};
// Merger: taken alone (changed only in them) and conflicts are both options for a
// person: Empty text - there is no element in this version.
//
struct s_conflict {
  std::string key;
  std::string base;
  std::string mine;
  std::string theirs;
};
struct s_merge {
  std::vector<std::string> taken;
  std::vector<s_conflict> conflicts;
};
// Route step: number of all, current instruction, call stack (from inside out), program
// output to this step, start error; active - track open.
struct s_trace {
  bool active;
  uint32_t step;
  uint32_t total;
  node_id node;
  std::vector<std::string> stack;
  std::string output;
  std::string error;
};
// Selection: second end (first cursor); node is empty - no allocation.
struct s_select {
  uint32_t view;
  node_id node;
  uint32_t piece;
  uint32_t offset;
};
// Text in the clipboard (Ctrl+C, Ctrl+X): buffer - at the window.
struct s_clipboard {
  std::string text;
};
// Hint line: view ("type", "value", "error", "announcement", "comment", "hole", "form",
// "signature"), text, reference (ad node; empty - no); [from, to) - the selected part of the
// text in code points (current parameter).
// 
//.
struct s_line {
  std::string kind;
  std::string text;
  node_id target;
  uint32_t from = 0;
  uint32_t to = 0;
};
// Guidance on the knot; empty - nothing to show.
struct s_hover {
  uint32_t view;
  node_id node;
  std::vector<s_line> lines;
};
// Node, piece, offset: design shape or call signature with the current parameter; hide
// empty.
struct s_hint {
  uint32_t view;
  node_id node;
  uint32_t piece;
  uint32_t offset;
  std::vector<s_line> lines;
};
// Inspector tree node (in order of bypass, depth - depth): place in the parent (field node, and
// the syntax recording parameter or structure, which it became), view, its short text, by whom
// disassembled (record or construction - rule, ad - rule_at), types by instance.
//
struct s_inode {
  node_id id;
  uint32_t depth;
  std::string role;
  std::string kind;
  std::string text;
  std::string rule;
  node_id rule_at;
  std::string types;
};
// Inspector: the tree of the module element, the focus (node at the cursor) and the opening of
// the focus: syntax recording - which piece became what parameter and what it turned into; the
// construction - her body with substituted arguments.
struct s_inspect {
  uint32_t view;
  node_id root;
  node_id focus;
  std::vector<s_inode> nodes;
  std::vector<s_line> expand;
};
using server_msg = std::variant<s_hello, s_view, s_cursor, s_error, s_update, s_complete, s_decor, s_layers, s_output, s_saved, s_trace, s_merge, s_select, s_clipboard, s_hover, s_hint, s_inspect>;

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

// parsing text form: words in the order of fields; missing fields - zeros; in the line `\s` - space
// (space key: `key 1 \s 2` - Ctrl + Space), `\n` - line translation.
void parse_fields(auto & m, std::vector<std::string_view> const & words) {
  std::size_t at = 1;
  template for(constexpr auto f: fields_of<std::remove_cvref_t<decltype(m)>>()) {
    using F = [:std::meta::type_of(f):];
    if(at < words.size()) {
      auto w = words[at++];
      if constexpr(std::is_same_v<F, std::string>) {
        std::string t(w);
        for(std::size_t k; (k = t.find("\\s")) != std::string::npos;) t.replace(k, 2, " ");
        for(std::size_t k; (k = t.find("\\n")) != std::string::npos;) t.replace(k, 2, "\n");
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
