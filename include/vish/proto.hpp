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

namespace vish::proto {

constexpr uint32_t version = 8;

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
// Save (Ctrl+S): journal database in .vishdb next to the program (author's decision).
struct c_save {};
// Merger with another copy of the database (§9.4): the path to .vishdb (relative to the program).
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
// Track: Go to step (click on stop in the list).
struct c_trace_step {
  uint32_t step;
};
// AI: Agent-client VM. The name of the client (in the list of customers), the module elements (key and
// text), the suggestion of editing the element by key (the whole text, why) and the decision of the person
// on the proposal.
struct c_name {
  std::string name;
};
struct c_items {};
struct c_propose {
  std::string key;
  std::string text;
  std::string why;
};
struct c_decide {
  uint32_t id;
  bool accept;
};
// The AI receives facts from the VM: the exact node and explicit verification response associated with editing
// the program (the node derivatives and rendering of this counter do not change). revision=0 is current.
struct c_context { uint32_t request; uint32_t view; node_id node; uint64_t revision; };
struct c_check { uint32_t request; uint64_t revision; };
// Copy of the program: create, edit, check, test, offer, drop. All fields are scalar - the same
// protocol is available in .wire scripts; the packet is assembled in VM, not applied in parts.
struct c_workspace {
  uint32_t request;
  uint32_t workspace;
  std::string action;
  uint64_t revision;
  std::string key;
  std::string text;
  std::string args;
  std::string input;
  std::string expected;
};
struct c_repository {
  uint32_t request;
  std::string action; // info/init/module/open/snapshot/diff/restore/fork/checkout/merge/save
  std::string key;
  std::string text;
  uint64_t revision;
};
using client_msg = std::variant<c_hello, c_open_view, c_close_view, c_key, c_pointer, c_goto, c_layer, c_run, c_save, c_trace, c_merge, c_take, c_projection, c_input, c_select, c_hover, c_open, c_inspect, c_apply, c_stop, c_trace_step, c_name, c_items, c_propose, c_decide, c_context, c_check, c_workspace, c_repository>;

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
  std::string projection; // projection - Record Module (§4.4); Empty - by default
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
  std::string result; // Type of function result ("unknown")
  std::string doc;  // a line of documentation (at the selected; frame "draft")
  uint32_t from;    // the part of the name (from, to), the code points
  uint32_t to;
};
struct s_complete {
  uint32_t view;
  node_id node;
  uint32_t piece;
  uint32_t offset;
  uint32_t selected;
  std::vector<s_item> items;
  // select the item "Add" to the transition list - the text to be created (frame "Transition"); otherwise empty
  std::string preview;
  // Basement of the list: the recipient after the point, its type and where the selected
  std::string context;
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
// overload of the presentation function (first layer level, author's solution 2026-10-26): node
// and head of the ad; overload is the number shown (switching - c_open over node).
struct s_overload {
  node_id fn;
  std::string head;
};
struct s_layers {
  uint32_t view;
  std::vector<s_layer> layers;
  std::string selected;
  std::vector<s_overload> overloads;
  uint32_t overload;
};
// Startup output: Print the program and error with the track (ok - no error). Start-up is the process of
// the machine, output as it is executed: more is a continuation of the former, state - "running" (goes),
// "stopped" (stopped)," - is finished. From - on the line text: the node of its print and the signature
// "function: line" (empty - no space); now - what is executed (launched), empty - nothing (frame "Run").
//
struct s_src {
  node_id node;
  std::string label;
};
struct s_output {
  std::string text;
  bool ok;
  bool more;
  std::string state;
  std::vector<s_src> from;
  node_id now;
};
struct s_saved {
  std::string path;
};
// Merger: taken alone (changed only in them) and conflicts are both options for a person: Empty text -
// there is no element in this version. Difference with the base (frame "Merge"): [from, to) - changed in
// the text of the version, code points; note - short that ("3 → 4", "+ comment").
//
//
struct s_conflict {
  std::string key;
  std::string base;
  std::string mine;
  std::string theirs;
  uint32_t mine_from = 0;
  uint32_t mine_to = 0;
  std::string mine_note;
  uint32_t theirs_from = 0;
  uint32_t theirs_to = 0;
  std::string theirs_note;
  std::string why; // what happened
};
// path - fused copy; taken_why - to the self taken (from where, whether visible in the module)
struct s_merge {
  std::string path;
  std::vector<std::string> taken;
  std::vector<std::string> taken_why;
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
// Status banging (on top of the window): mode and most importantly - kind "error" (program error), "trace"
// (track step), "run" (executed), "done" (event: launch is completed, saved), "hide; hint - a hint of the
// mode keys. No bangs at all. Text - VM: . action - the action of the button ("goto signature" - the
// transition list from "add"), label - its inscription (frame "Service states"); empty - no button.
//
//
//
struct s_status {
  std::string kind;
  std::string text;
  std::string hint;
  std::string action;
  std::string label;
};
// Track stops - window around the current step step step number, call depth, function, short text of
// the instruction, its node; current - the current step number.
//
struct s_stop {
  uint32_t step;
  uint32_t depth;
  std::string fn;
  std::string text;
  node_id node;
  std::string value; // the value of the instruction (“= 9.0”) or printed by it (“output 0.0”);
};
// prev, next - adjacent steps of the same depth (ыF10, F10), number + 1 (0 - no); summary - the result of the path
struct s_stops {
  uint32_t current;
  std::vector<s_stop> stops;
  uint32_t prev;
  uint32_t next;
  std::string summary;
};
// Other clients in the view: cursor (node, piece, offset; node is not in the layout - the closest
// ancestor), where it rules the draft (draft - names, draft_node - element). Unimportant data - a separate
// message:
// 
//.
struct s_peer {
  uint32_t client;
  std::string name;
  node_id node;
  uint32_t piece;
  uint32_t offset;
  std::string where;
  std::string draft;
  node_id draft_node;
};
struct s_peers {
  uint32_t view;
  std::vector<s_peer> peers;
  std::string me; // where the cursor of the client (frame "Two clients": line "you")
};
// Elements of the program module: key (signature), text, internal
struct s_element {
  std::string key;
  std::string text;
  bool inner;
};
struct s_items {
  std::vector<s_element> items;
};
// Proposals for edits (AI): number, element key, current text (empty - new element), proposed why, from
// which client; the difference with the current - place and signature, as in a merger.
struct s_proposal {
  uint32_t id;
  std::string key;
  std::string base;
  std::string text;
  std::string why;
  std::string from;
  uint32_t text_from = 0;
  uint32_t text_to = 0;
  std::string note;
  uint64_t revision = 0;
  std::string checks;
};
struct s_proposals {
  std::vector<s_proposal> proposals;
};
struct s_agent {
  uint32_t request;
  uint64_t revision;
  uint32_t workspace;
  uint32_t proposal;
  bool ok;
  std::string text;
};
struct repository_entry {
  std::string kind; // current/module/snapshot/branch
  uint64_t id;
  uint64_t parent;
  node_id node;
  std::string name;
  std::string detail;
  uint64_t head; // second parent of the image or head branch
};
struct s_repository {
  uint32_t request;
  bool ok;
  uint64_t revision;
  std::string path;
  std::string branch;
  uint64_t head;
  bool dirty;
  std::string message;
  std::string diff;
  std::vector<repository_entry> entries;
};
using server_msg = std::variant<s_hello, s_view, s_cursor, s_error, s_update, s_complete, s_decor, s_layers, s_output, s_saved, s_trace, s_merge, s_select, s_clipboard, s_hover, s_hint, s_inspect, s_status, s_stops, s_peers, s_items, s_proposals, s_agent, s_repository>;

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

// --- Meta information for text form (test scripts) ---
//
// Text form of messages (`key 1 End` → c_key, answers VM → `cursor view=1 …`) - in language
// (lib/tools.vish): Here is meta-information by reflection: message names and fields, client message from
// field values, VM message by flat tree. The message name is the name of the unit without the prefix c /s .
//
//

consteval std::string_view short_name(std::meta::info t) {
  std::string_view n = std::meta::identifier_of(std::meta::dealias(t));
  return n.substr(2);
}

// Type of text field: string, node, num
consteval std::string_view field_kind(std::meta::info t) {
  if(std::meta::dealias(t) == std::meta::dealias(^^std::string)) return "string";
  if(std::meta::dealias(t) == std::meta::dealias(^^node_id)) return "node";
  return "num";
}

// Customer Messages: by appearance - name and fields (names and types) in order.
struct client_kind {
  std::string_view name;
  std::vector<std::string_view> fields, kinds;
};
std::vector<client_kind> client_kinds() {
  std::vector<client_kind> out;
  static constexpr auto ks = [] {
    std::array<std::size_t, std::variant_size_v<client_msg>> a{};
    for(std::size_t k = 0; k < a.size(); ++k) a[k] = k;
    return a;
  }();
  template for(constexpr std::size_t k: ks) {
    using M = std::variant_alternative_t<k, client_msg>;
    client_kind x{short_name(^^M), {}, {}};
    template for(constexpr auto f: fields_of<M>()) {
      x.fields.push_back(std::meta::identifier_of(f));
      x.kinds.push_back(field_kind(std::meta::type_of(f)));
    }
    out.push_back(std::move(x));
  }
  return out;
}
// A name client message from the values of the fields in order (lines as is, numbers and node are
// decimal); missing ones are zeros. There's no such thing, empty.
std::optional<client_msg> client_from(std::string_view name, std::vector<std::string> const & values) {
  std::optional<client_msg> out;
  static constexpr auto ks = [] {
    std::array<std::size_t, std::variant_size_v<client_msg>> a{};
    for(std::size_t k = 0; k < a.size(); ++k) a[k] = k;
    return a;
  }();
  template for(constexpr std::size_t k: ks) {
    using M = std::variant_alternative_t<k, client_msg>;
    if(!out && name == short_name(^^M)) {
      M & m = std::get<k>(out.emplace(std::in_place_index<k>));
      std::size_t at = 0;
      template for(constexpr auto f: fields_of<M>()) {
        using F = [:std::meta::type_of(f):];
        if(at < values.size()) {
          std::string const & w = values[at++];
          if constexpr(std::is_same_v<F, std::string>) m.[:f:] = w;
          else {
            uint64_t x = 0;
            std::from_chars(w.data(), w.data() + w.size(), x);
            if constexpr(std::is_same_v<F, node_id>) m.[:f:] = node_id(uint32_t(x));
            else m.[:f:] = F(x);
          }
        }
      }
    }
  }
  return out;
}

// Message VM - flat tree (order of bypass): on the record - view (struct, list, node, num, str), field name
// (the message - his name), text (line; number - as printed; list - structs, if elements aggregates), number
// (node; no node - - -1), children, record number after subtree.
struct flat_tree {
  std::vector<std::string_view> kind;
  std::vector<std::string> name, text;
  std::vector<int64_t> num, count, next;

  std::size_t put(std::string_view k, std::string_view n, std::string t, int64_t x, int64_t c) {
    kind.push_back(k);
    name.emplace_back(n);
    text.push_back(std::move(t));
    num.push_back(x);
    count.push_back(c);
    next.push_back(0);
    return kind.size() - 1;
  }
  void value_of(std::string_view n, auto const & x) {
    using T = std::remove_cvref_t<decltype(x)>;
    if constexpr(std::is_same_v<T, node_id>) put("node", n, "", x ? int64_t(x.value) : -1, 0);
    else if constexpr(std::is_arithmetic_v<T>) put("num", n, cat(x), int64_t(x), 0);
    else if constexpr(std::is_same_v<T, std::string>) put("str", n, x, 0, 0);
    else if constexpr(is_variant<T>::value) std::visit([&](auto const & a) { value_of(n, a); }, x);
    else if constexpr(is_vector<T>::value) {
      using E = typename T::value_type;
      std::size_t at = put("list", n, std::is_aggregate_v<E> && !is_id<E>::value ? "structs" : "values", 0, int64_t(x.size()));
      for(auto const & e: x) value_of("", e);
      next[at] = int64_t(kind.size());
    } else {
      std::size_t at = put("struct", n, "", 0, int64_t(fields_of<T>().size()));
      template for(constexpr auto m: fields_of<T>()) value_of(std::meta::identifier_of(m), x.[:m:]);
      next[at] = int64_t(kind.size());
    }
    if(next.back() == 0) next.back() = int64_t(kind.size());
  }
};
flat_tree flat(server_msg const & msg) {
  flat_tree t;
  std::visit([&](auto const & m) {
    std::size_t at = t.put("struct", short_name(std::meta::remove_cvref(^^decltype(m))), "", 0, int64_t(fields_of<std::remove_cvref_t<decltype(m)>>().size()));
    template for(constexpr auto f: fields_of<std::remove_cvref_t<decltype(m)>>()) t.value_of(std::meta::identifier_of(f), m.[:f:]);
    t.next[at] = int64_t(t.kind.size());
  }, msg);
  return t;
}

} // namespace vish::proto

namespace vish::proto {

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

} // namespace vish::proto
