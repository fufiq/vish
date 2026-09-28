import type {
  Cursor,
  NodeId,
  Update,
  View,
  ViewNode,
} from '../protocol/messages';
import { ASTNode, pieceOfText, type PieceText } from './nodes/ast_node';
import { BlockNode } from './nodes/block_node';
import { tagOf } from './nodes/index';

// Representation of the module in the window: node elements on the pieces sent, the
// cursor is where the VM holds it. The item is addressed directly to node_id:
// - there's no model at the window.
export class ViewElement {
  readonly byId = new Map<NodeId, ASTNode>();
  readonly caret = document.createElement('div');
  root: ASTNode | undefined;

  readonly nodes = new Map<NodeId, ViewNode>();

  constructor(
    readonly host: HTMLElement,
    readonly data: View,
  ) {
    for (const n of data.nodes) this.nodes.set(n.id, n);
    this.root = this.#build(data.root);
    this.caret.className = 'tz-caret';
    host.replaceChildren(this.root, this.caret);
  }

  #build(id: NodeId): ASTNode {
    const n = this.nodes.get(id);
    if (!n) throw new Error(`узел #${String(id)} не прислан`);
    const el = document.createElement(tagOf(n.kind));
    if (!(el instanceof ASTNode)) {
      throw new Error(`вид ${n.kind} не зарегистрирован`);
    }
    el.nodeId = id;
    el.note = n.note;
    this.byId.set(id, el);
    el.show(n.pieces, (c) => this.#build(c));
    return el;
  }

  // Edit from VM: the node subtree is reassembled and replaced by the old (the same
  // slot parent); the rest of the DOM is left unmoved.
  update(u: Update): void {
    for (const n of u.nodes) this.nodes.set(n.id, n);
    const old = this.byId.get(u.node);
    const fresh = this.#build(u.node);
    if (!old || old === this.root) {
      this.root = fresh;
      this.host.replaceChildren(fresh, this.caret);
      return;
    }
    fresh.slot = old.slot;
    old.replaceWith(fresh);
  }

  // Cursor VM: (node, piece, offset at code points) → symbol rectangle.
  showCursor(c: Cursor): void {
    const text =
      c.node === null ? undefined : this.byId.get(c.node)?.textOf(c.piece);
    if (!text) {
      this.caret.hidden = true;
      return;
    }
    const at = utf16Offset(text.data, c.offset);
    const range = document.createRange();
    range.setStart(text, at);
    range.setEnd(text, Math.min(at + 1, text.length));
    const box = range.getBoundingClientRect();
    const origin = this.host.getBoundingClientRect();
    this.caret.hidden = false;
    this.caret.style.left = `${String(box.left - origin.left + this.host.scrollLeft)}px`;
    this.caret.style.top = `${String(box.top - origin.top + this.host.scrollTop)}px`;
    this.caret.style.height = `${String(box.height)}px`;
    this.caret.scrollIntoView({ block: 'nearest', inline: 'nearest' });
  }

  // Click point → place in the text node (indicator for VM).
  placeAt(x: number, y: number): (PieceText & { offset: number }) | undefined {
    const shadowRoots = [...this.byId.values()]
      .filter((n) => n instanceof BlockNode)
      .map((n) => n.shadow);
    const pos = document.caretPositionFromPoint(x, y, { shadowRoots });
    if (!pos || !(pos.offsetNode instanceof Text)) return undefined;
    const where = pieceOfText.get(pos.offsetNode);
    if (!where) return undefined;
    return {
      ...where,
      offset: codePoints(pos.offsetNode.data.slice(0, pos.offset)),
    };
  }
}

export function codePoints(s: string): number {
  // Protocol offsets - in code points (as in VM), not in
  // eslint-disable-next-line @typescript-eslint/no-misused-spread graphemes
  return [...s].length;
}

export function utf16Offset(s: string, cp: number): number {
  let at = 0;
  let n = 0;
  for (const ch of s) {
    if (n++ === cp) return at;
    at += ch.length;
  }
  return at;
}
