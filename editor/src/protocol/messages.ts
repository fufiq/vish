// Protocol messages in TS types. The source of truth is the include/tz/proto.hpp
// aggregates; the schema (schema.json) is derived from them by the `tz schema`
// command, these types repeat it to check the editor's code (the
// messages.test.ts test compares the fields with the live schema).
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
export type ClientMsg = Hello | OpenView | CloseView | Key | Pointer;

export interface ServerHello {
  type: 's_hello';
  version: number;
  program: string;
  revision: number;
}
export interface ViewNode {
  type: 's_node';
  id: NodeId;
  kind: string;
  pieces: Piece[];
}
export interface View {
  type: 's_view';
  view: number;
  root: NodeId;
  revision: number;
  nodes: ViewNode[];
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
export type ServerMsg = ServerHello | View | Cursor | ServerError;

export const Mods = { shift: 1, ctrl: 2, alt: 4, meta: 8 } as const;

export function encodeClient(m: ClientMsg): Uint8Array {
  return frame(schema, schema.client, m as unknown as Value);
}

export function decodeServer(body: Uint8Array): ServerMsg {
  return decode(schema, schema.server, body) as unknown as ServerMsg;
}

// The projection text collected from node pieces is the same as `tz fmt`.
export function projectionText(view: View): string {
  const byId = new Map(view.nodes.map((n) => [n.id, n]));
  const text = (id: NodeId): string =>
    (byId.get(id)?.pieces ?? [])
      .map((p) => (typeof p === 'string' ? p : text(p)))
      .join('');
  return text(view.root);
}
