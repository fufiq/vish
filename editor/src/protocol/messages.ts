// Protocol messages in TS types. The source of truth is the include/vish/proto.hpp
// aggregates; the schema (schema.json) is derived from them by the `vish schema`
// command, these types repeat it to check the editor's code (the messages.test.ts
// test compares the fields with the live schema).
import schemaJson from './schema.json';
import { decode, frame, type Schema, type Value } from './codec';

export const schema = schemaJson as unknown as Schema;

export type NodeId = number;
// projection node: Your text or child's place.
export type Piece = string | NodeId;

export interface Hello {
  type: 'c_hello';
  version: number;
}
export interface OpenView {
  type: 'c_open_view';
  kind: string;
}
export interface CloseView {
  type: 'c_close_view';
  view: number;
}
// Keyboard is like KeyboardEvent.key; mods are Mods bits.
export interface Key {
  type: 'c_key';
  view: number;
  key: string;
  mods: number;
}
// Place in proper text node: chunk and offset in code points.
export interface Pointer {
  type: 'c_pointer';
  view: number;
  node: NodeId;
  piece: number;
  offset: number;
}
// Signature transition (Ctrl+P): "foo(i64)" - VM responds with a list.
export interface Goto {
  type: 'c_goto';
  view: number;
  text: string;
}
// Function representation layer: context key; everything.
export interface LayerPick {
  type: 'c_layer';
  view: number;
  key: string;
}
// Launch (F5): main() of the current base state is a process in the same VM, output is in progress.
export interface Run {
  type: 'c_run';
  stdin_text: string;
  args: string;
}
// Stop launch (Shift+F5).
export interface Stop {
  type: 'c_stop';
}
// Save (Ctrl+S): journal base in .vishdb next to the program.
export interface Save {
  type: 'c_save';
}
// Start with track (F6): steps - keys, they are interpreted by VM.
export interface Trace {
  type: 'c_trace';
  stdin_text: string;
  args: string;
}
// Merge with another copy of the database (the path to .vishdb) and resolve the conflict.
export interface Merge {
  type: 'c_merge';
  path: string;
}
export interface Take {
  type: 'c_take';
  key: string;
  theirs: boolean;
}
// projection Presentation: Syntax entries module by key;' - by default.
export interface ProjectionPick {
  type: 'c_projection';
  view: number;
  key: string;
}
// Enter the whole text: insert from the clipboard, IME - as a type from the keyboard.
export interface Input {
  type: 'c_input';
  view: number;
  text: string;
}
// Selection by pointer: stretch or Shift+click - to the place (clicks 1); double
// click - word, triple - line.
export interface Select {
  type: 'c_select';
  view: number;
  node: NodeId;
  piece: number;
  offset: number;
  clicks: number;
}
// Hover to the node: VM responds with a hint (s_hover).
export interface HoverAsk {
  type: 'c_hover';
  view: number;
  node: NodeId;
}
// Hint link: the function announcement is in the second panel, another is the cursor to it.
export interface Open {
  type: 'c_open';
  view: number;
  node: NodeId;
}
// AST Inspector: The tree of the module element in which the node node (focus) is located.
export interface InspectAsk {
  type: 'c_inspect';
  view: number;
  node: NodeId;
}
// Apply the program editing function (Ctrl+Shift+R): the answer is s_output.
export interface Apply {
  type: 'c_apply';
  fn: string;
}
// Track: proceed to step (click on stop).
export interface TraceStepAsk {
  type: 'c_trace_step';
  step: number;
}
// AI is a VM client: name, module elements, suggestion of editing, human decision.
export interface NameAsk {
  type: 'c_name';
  name: string;
}
export interface ItemsAsk {
  type: 'c_items';
}
export interface ProposeAsk {
  type: 'c_propose';
  key: string;
  text: string;
  why: string;
}
export interface DecideAsk {
  type: 'c_decide';
  id: number;
  accept: boolean;
}
export type ClientMsg =
  | Hello
  | OpenView
  | CloseView
  | Key
  | Pointer
  | Goto
  | LayerPick
  | Run
  | Save
  | Trace
  | Merge
  | Take
  | ProjectionPick
  | Input
  | Select
  | HoverAsk
  | Open
  | InspectAsk
  | Apply
  | Stop
  | TraceStepAsk
  | NameAsk
  | ItemsAsk
  | ProposeAsk
  | DecideAsk;

export interface ServerHello {
  type: 's_hello';
  version: number;
  program: string;
  revision: number;
}
// note - note node (cause of the hole): decoration, not text.
export interface ViewNode {
  type: 's_node';
  id: NodeId;
  kind: string;
  pieces: Piece[];
  note: string;
}
// Edit: node node subtree replaced (id node saved); comes to all customers - state
// one.
export interface Update {
  type: 's_update';
  view: number;
  revision: number;
  node: NodeId;
  nodes: ViewNode[];
}
export interface View {
  type: 's_view';
  view: number;
  root: NodeId;
  revision: number;
  nodes: ViewNode[];
  // Module without internal elements; fn - one function (second panel)
  kind: string;
}
export interface Cursor {
  type: 's_cursor';
  view: number;
  node: NodeId | null;
  piece: number;
  offset: number;
}
export interface ServerError {
  type: 's_error';
  text: string;
}
// Supplement: options for the beginning of the typed word; selected - selected;
// an empty list - hide. List status is in VM.
export interface CompletionItem {
  type: 's_item';
  name: string;
  kind: string;
  detail: string;
  result: string; // Type of function result ('' - unknown)
  doc: string; // Documentation line (selected)
  from: number; // the part of the name (from, to), the code points
  to: number;
}
export interface Complete {
  type: 's_complete';
  view: number;
  node: NodeId | null;
  piece: number;
  offset: number;
  selected: number;
  items: CompletionItem[];
  preview: string; // Selected to add a transition list - what will be created
  context: string; // Basement: Recipient after the point, its type and where it was selected
}
// Verification tags: "type" - types of declared name for all instance, "error" -
// error in place (with a trace). They come in full after the audit. [from, to] is
// the place inside the node text at the code points (parsing error inside the raw
// block: atom and displacement in it); from ===to is the entire node.
export interface Mark {
  type: 's_mark';
  node: NodeId;
  kind: string;
  text: string;
  from: number;
  to: number;
}
export interface Decor {
  type: 's_decor';
  view: number;
  revision: number;
  marks: Mark[];
}
// The layers of a function are the contexts of its use; kind: "compile-time" or "run-time".
export interface Layer {
  type: 's_layer';
  key: string;
  kind: string;
}
// overload functions (first layer level): node and head; overload - shown.
export interface Overload {
  type: 's_overload';
  fn: NodeId | null;
  head: string;
}
export interface Layers {
  type: 's_layers';
  view: number;
  layers: Layer[];
  selected: string;
  overloads: Overload[];
  overload: number;
}
// Launch output: more is a continuation of the former, state - 'running', 'stopped' or '' (finished). From -
// on the line text: the node of its print and the signature "function: line"; now - what is executed.
export interface Output {
  type: 's_output';
  text: string;
  ok: boolean;
  more: boolean;
  state: string;
  from: { type: 's_src'; node: NodeId | null; label: string }[];
  now: NodeId | null;
}
export interface Saved {
  type: 's_saved';
  path: string;
}
// Track stops - a window around the current step: step number, call depth, function,
// short text of the instruction, its node.
export interface Stops {
  type: 's_stops';
  current: number;
  stops: {
    type: 's_stop';
    step: number;
    depth: number;
    fn: string;
    text: string;
    node: NodeId | null;
    value: string; // What gave the instruction: "= 9.0", "output 0.0", "" - nothing
  }[];
  prev: number; // Neighboring steps of the same depth (ыF10, F10): number + 1, 0 - no
  next: number;
  summary: string; // The end of the way: steps, stops, error
}
// Other clients in the view: cursor (node, piece, offset), where it is that rules the draft -
// unimportant data separate message.
export interface Peers {
  type: 's_peers';
  view: number;
  peers: {
    type: 's_peer';
    client: number;
    name: string;
    node: NodeId | null;
    piece: number;
    offset: number;
    where: string;
    draft: string;
    draft_node: NodeId | null;
  }[];
}
// Status bangs: kind - 'error', 'trace', 'run', 'done' (event) or '' (hide); text collects VM,
// the client only draws. action - action of the button (“goto signature”), label - its
// inscription; empty - no button.
export interface Status {
  type: 's_status';
  kind: string;
  text: string;
  hint: string;
  action: string;
  label: string;
}
// Route step: number, total, current instruction, stack (inside out), step output, start
// error; active - track open.
export interface TraceStep {
  type: 's_trace';
  active: boolean;
  step: number;
  total: number;
  node: NodeId | null;
  stack: string[];
  output: string;
  error: string;
}
// Merger: Taken by itself and conflicts (both options are to drain with your hands). Difference of
// variants with base: [from, to] - changed (code points), note - short what.
export interface Conflict {
  type: 's_conflict';
  key: string;
  base: string;
  mine: string;
  theirs: string;
  mine_from: number;
  mine_to: number;
  mine_note: string;
  theirs_from: number;
  theirs_to: number;
  theirs_note: string;
}
export interface MergeResult {
  type: 's_merge';
  taken: string[];
  conflicts: Conflict[];
}
// Exclusion: second end (first - cursor); node null - no allocation.
export interface Selection {
  type: 's_select';
  view: number;
  node: NodeId | null;
  piece: number;
  offset: number;
}
// Text in the clipboard (Ctrl+C, Ctrl+X): buffer - at the window.
export interface Clipboard {
  type: 's_clipboard';
  text: string;
}
// Tip line: view, text, link (an ad node); [from, to] is the selected part of the text
// (current parameter) in the code points.
export interface HintLine {
  type: 's_line';
  kind: string;
  text: string;
  target: NodeId | null;
  from: number;
  to: number;
}
// Guidance on the knot; empty - nothing to show.
export interface Hover {
  type: 's_hover';
  view: number;
  node: NodeId | null;
  lines: HintLine[];
}
// The tip of the set at the place: the shape of the design or signature; empty - to hide.
export interface Hint {
  type: 's_hint';
  view: number;
  node: NodeId | null;
  piece: number;
  offset: number;
  lines: HintLine[];
}
// The node of the inspector’s tree in the order of bypassing: place in the parent (field and
// parameter of recording or construction), view, your text, by whom disassembled, types.
export interface InspectNode {
  type: 's_inode';
  id: NodeId;
  depth: number;
  role: string;
  kind: string;
  text: string;
  rule: string;
  rule_at: NodeId | null;
  types: string;
}
// Inspector: tree, focus (node at the cursor) and focus opening.
export interface Inspect {
  type: 's_inspect';
  view: number;
  root: NodeId | null;
  focus: NodeId | null;
  nodes: InspectNode[];
  expand: HintLine[];
}
export type ServerMsg =
  | ServerHello
  | View
  | Cursor
  | ServerError
  | Update
  | Complete
  | Decor
  | Layers
  | Output
  | Saved
  | TraceStep
  | MergeResult
  | Selection
  | Clipboard
  | Hover
  | Hint
  | Inspect
  | Status
  | Stops
  | Peers
  | Items
  | Proposals;
// Elements of the module: key (signature), text, internal
export interface Items {
  type: 's_items';
  items: { type: 's_element'; key: string; text: string; inner: boolean }[];
}
// Proposals for edits (AI): current text proposed, why, from whom; the difference is place.
export interface Proposal {
  type: 's_proposal';
  id: number;
  key: string;
  base: string;
  text: string;
  why: string;
  from: string;
  text_from: number;
  text_to: number;
  note: string;
}
export interface Proposals {
  type: 's_proposals';
  proposals: Proposal[];
}

export const Mods = { shift: 1, ctrl: 2, alt: 4, meta: 8 } as const;

export function encodeClient(m: ClientMsg): Uint8Array {
  return frame(schema, schema.client, m as unknown as Value);
}

export function decodeServer(body: Uint8Array): ServerMsg {
  return decode(schema, schema.server, body) as unknown as ServerMsg;
}

// The projection text collected from node pieces is the same as `vish fmt`.
export function projectionText(view: View): string {
  const byId = new Map(view.nodes.map((n) => [n.id, n]));
  const text = (id: NodeId): string =>
    (byId.get(id)?.pieces ?? [])
      .map((p) => (typeof p === 'string' ? p : text(p)))
      .join('');
  return text(view.root);
}
