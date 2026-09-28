import type { Cursor, NodeId, View } from '../protocol/messages';
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

  constructor(
    readonly host: HTMLElement,
    readonly data: View,
  ) {
    const nodes = new Map(data.nodes.map((n) => [n.id, n]));
    const build = (id: NodeId): ASTNode => {
      const n = nodes.get(id);
      if (!n) throw new Error(`узел #${String(id)} не прислан`);
      const el = document.createElement(tagOf(n.kind));
      if (!(el instanceof ASTNode))
        throw new Error(`вид ${n.kind} не зарегистрирован`);
      el.nodeId = id;
      this.byId.set(id, el);
      el.show(n.pieces, build);
      return el;
    };
    this.root = build(data.root);
    this.caret.className = 'tz-caret';
    host.replaceChildren(this.root, this.caret);
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
