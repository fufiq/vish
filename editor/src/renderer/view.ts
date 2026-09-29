import type {
  Complete,
  Decor,
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
  readonly popup = document.createElement('ul');
  root: ASTNode | undefined;

  readonly nodes = new Map<NodeId, ViewNode>();

  constructor(
    readonly host: HTMLElement,
    readonly data: View,
  ) {
    for (const n of data.nodes) this.nodes.set(n.id, n);
    this.root = this.#build(data.root);
    this.caret.className = 'tz-caret';
    this.popup.className = 'tz-complete';
    this.popup.hidden = true;
    host.replaceChildren(this.root, this.caret, this.popup);
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
      this.host.replaceChildren(fresh, this.caret, this.popup);
      return;
    }
    fresh.slot = old.slot;
    old.replaceWith(fresh);
  }

  // Place VM (node, piece, offset in code points) → rectangle of the symbol in
  // the coordinates of the view window.
  #box(
    node: NodeId | null,
    piece: number,
    offset: number,
  ): { left: number; top: number; height: number } | undefined {
    const text = node === null ? undefined : this.byId.get(node)?.textOf(piece);
    if (!text) return undefined;
    const at = utf16Offset(text.data, offset);
    const range = document.createRange();
    range.setStart(text, at);
    range.setEnd(text, Math.min(at + 1, text.length));
    const box = range.getBoundingClientRect();
    const origin = this.host.getBoundingClientRect();
    return {
      left: box.left - origin.left + this.host.scrollLeft,
      top: box.top - origin.top + this.host.scrollTop,
      height: box.height,
    };
  }

  get listOpen(): boolean {
    return !this.popup.hidden;
  }

  #cursor: Cursor | undefined;
  #completion: Complete | undefined;

  // Cadet VM.
  showCursor(c: Cursor): void {
    this.#cursor = c;
    const box = this.#box(c.node, c.piece, c.offset);
    this.caret.hidden = !box;
    if (!box) return;
    this.caret.style.left = `${String(box.left)}px`;
    this.caret.style.top = `${String(box.top)}px`;
    this.caret.style.height = `${String(box.height)}px`;
    this.caret.scrollIntoView({ block: 'nearest', inline: 'nearest' });
  }

  #marked = new Set<ASTNode>();

  // Check marks: old ones are removed, sent. Returns the first error for the status
  // string.
  showMarks(d: Decor): string | undefined {
    for (const el of this.#marked) el.setMarks(undefined, undefined);
    this.#marked.clear();
    const by = new Map<NodeId, { type?: string; error?: string }>();
    let first: string | undefined;
    for (const m of d.marks) {
      const e = by.get(m.node) ?? {};
      if (m.kind === 'type') e.type = m.text;
      if (m.kind === 'error') {
        e.error = m.text;
        first ??= m.text;
      }
      by.set(m.node, e);
    }
    for (const [id, e] of by) {
      const el = this.byId.get(id);
      if (!el) continue;
      el.setMarks(e.type, e.error);
      this.#marked.add(el);
    }
    // icons shift text - cursor and list are put again
    if (this.#cursor) this.showCursor(this.#cursor);
    if (this.#completion) this.showCompletion(this.#completion);
    return first;
  }

  // The list of additions - under the beginning of the typed word; selected - from VM.
  showCompletion(m: Complete): void {
    this.#completion = m;
    const box = this.#box(m.node, m.piece, m.offset);
    this.popup.hidden = m.items.length === 0 || !box;
    if (this.popup.hidden || !box) return;
    this.popup.replaceChildren(
      ...m.items.map((item, i) => {
        const li = document.createElement('li');
        li.toggleAttribute('selected', i === m.selected);
        const name = document.createElement('span');
        name.className = 'name';
        name.textContent = item.name;
        const detail = document.createElement('span');
        detail.className = 'detail';
        detail.textContent = `${item.detail} ${item.kind}`.trim();
        li.append(name, detail);
        return li;
      }),
    );
    this.popup.style.left = `${String(box.left)}px`;
    this.popup.style.top = `${String(box.top + box.height)}px`;
    this.popup
      .querySelector('[selected]')
      ?.scrollIntoView({ block: 'nearest' });
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
