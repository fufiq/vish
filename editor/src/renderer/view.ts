import type {
  Complete,
  Decor,
  Cursor,
  Hint,
  HintLine,
  Hover,
  NodeId,
  Selection,
  Update,
  View,
  ViewNode,
} from '../protocol/messages';
import {
  ASTNode,
  pieceOfText,
  type NodeMarks,
  type PieceText,
} from './nodes/ast_node';
import { RecordNode } from './nodes/record_node';
import { tagOf } from './nodes/index';

// Representation of the module in the window: node elements on the pieces sent, the
// cursor is where the VM holds it. The item is addressed directly to node_id:
// - there's no model at the window.
export class ViewElement {
  readonly byId = new Map<NodeId, ASTNode>();
  readonly caret = document.createElement('div');
  readonly popup = document.createElement('ul');
  // Selection - rectangles on top of the text (the text is also in Shadow DOM node)
  readonly selectionLayer = document.createElement('div');
  // Instructions: guidance (at the pointer) and set (above the cursor)
  readonly tip = document.createElement('div');
  readonly hint = document.createElement('div');
  root: ASTNode | undefined;

  readonly nodes = new Map<NodeId, ViewNode>();

  constructor(
    readonly host: HTMLElement,
    readonly data: View,
  ) {
    for (const n of data.nodes) this.nodes.set(n.id, n);
    this.root = this.#build(data.root);
    this.caret.className = 'vish-caret';
    this.popup.className = 'vish-complete';
    this.popup.hidden = true;
    this.selectionLayer.className = 'vish-selection';
    this.tip.className = 'vish-tip';
    this.tip.hidden = true;
    this.hint.className = 'vish-tip vish-hint';
    this.hint.hidden = true;
    host.replaceChildren(...this.#chrome(this.root));
  }

  #chrome(root: ASTNode): Node[] {
    return [
      this.selectionLayer,
      root,
      this.caret,
      this.popup,
      this.hint,
      this.tip,
    ];
  }

  // Guidance - at the pointer (x, y - coordinates of the window).
  showHover(m: Hover, x: number, y: number, open: (n: NodeId) => void): void {
    this.tip.hidden = m.lines.length === 0;
    if (this.tip.hidden) return;
    renderLines(this.tip, m.lines, open);
    const origin = this.host.getBoundingClientRect();
    this.tip.style.left = `${String(x - origin.left + this.host.scrollLeft)}px`;
    this.tip.style.top = `${String(y - origin.top + this.host.scrollTop + 18)}px`;
  }
  hideHover(): void {
    this.tip.hidden = true;
  }

  // The tip of the set is above the place (the beginning of the word or call name).
  showHint(m: Hint, open: (n: NodeId) => void): void {
    const box = this.#box(m.node, m.piece, m.offset);
    this.hint.hidden = m.lines.length === 0 || !box;
    if (this.hint.hidden || !box) return;
    renderLines(this.hint, m.lines, open);
    this.hint.style.left = `${String(box.left)}px`;
    this.hint.style.top = `${String(box.top)}px`;
  }

  // AST node under the pointer (the deepest).
  static nodeOf(e: Event): ASTNode | undefined {
    return e.composedPath().find((x): x is ASTNode => x instanceof ASTNode);
  }

  #build(id: NodeId): ASTNode {
    const n = this.nodes.get(id);
    if (!n) throw new Error(`узел #${String(id)} не прислан`);
    const el = document.createElement(tagOf(n.kind));
    if (!(el instanceof ASTNode)) {
      throw new Error(`вид ${n.kind} не зарегистрирован`);
    }
    el.nodeId = id;
    el.dataset.kind = n.kind;
    el.dataset.id = String(id);
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
      this.host.replaceChildren(...this.#chrome(fresh));
    } else {
      fresh.slot = old.slot;
      old.replaceWith(fresh);
    }
    // node, which saved id, - with the same labels until the next check (VM saves id
    // node when dialing)
    if (this.#decor) this.showMarks(this.#decor);
  }

  #decor: Decor | undefined;
  #selection: Selection | undefined;

  // Texts of pieces in order projection : (node, piece) → Text.
  #texts(): { node: NodeId; piece: number; text: Text }[] {
    const out: { node: NodeId; piece: number; text: Text }[] = [];
    const walk = (id: NodeId): void => {
      const n = this.nodes.get(id);
      const el = this.byId.get(id);
      if (!n || !el) return;
      n.pieces.forEach((p, i) => {
        if (typeof p !== 'string') {
          walk(p);
          return;
        }
        const t = el.textOf(i);
        if (t) out.push({ node: id, piece: i, text: t });
      });
    };
    if (this.root) walk(this.root.nodeId);
    return out;
  }

  // Selection from second end (VM) to cursor: rectangles of lines of text.
  showSelection(s: Selection): void {
    this.#selection = s;
    this.selectionLayer.replaceChildren();
    const c = this.#cursor;
    const cn = c?.node ?? null;
    if (s.node === null || !c || cn === null) return;
    const texts = this.#texts();
    const at = (node: NodeId, piece: number) =>
      texts.findIndex((t) => t.node === node && t.piece === piece);
    let a = { i: at(s.node, s.piece), off: s.offset };
    let b = { i: at(cn, c.piece), off: c.offset };
    if (a.i < 0 || b.i < 0) return;
    if (a.i > b.i || (a.i === b.i && a.off > b.off)) [a, b] = [b, a];
    const origin = this.host.getBoundingClientRect();
    for (const [i, { text: t }] of texts.entries()) {
      if (i < a.i || i > b.i) continue;
      const range = document.createRange();
      range.setStart(t, i === a.i ? utf16Offset(t.data, a.off) : 0);
      range.setEnd(t, i === b.i ? utf16Offset(t.data, b.off) : t.length);
      for (const r of range.getClientRects()) {
        const box = document.createElement('div');
        box.style.left = `${String(r.left - origin.left + this.host.scrollLeft)}px`;
        box.style.top = `${String(r.top - origin.top + this.host.scrollTop)}px`;
        box.style.width = `${String(Math.max(r.width, 4))}px`;
        box.style.height = `${String(r.height)}px`;
        this.selectionLayer.append(box);
      }
    }
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

  // Cadet VM. The error path is visible in its entirety (step illumination, other muted)
  // when the cursor or pointer on the node path; the step function (entry point) is only
  // visible with its node.
  #pathAt = { cursor: false, pointer: false };
  static onPath(el: Element | undefined): boolean {
    if (el instanceof ASTNode && el.hasAttribute('path')) return true;
    for (let x = el?.parentElement; x instanceof ASTNode; x = x.parentElement)
      if (x.hasAttribute('path') && x.dataset.kind !== 'n_fn') return true;
    return false;
  }
  #showPath(): void {
    this.host.toggleAttribute(
      'path-focus',
      this.#pathAt.cursor || this.#pathAt.pointer,
    );
  }
  pointPath(el: Element | undefined): void {
    this.#pathAt.pointer = ViewElement.onPath(el);
    this.#showPath();
  }

  showCursor(c: Cursor): void {
    this.#cursor = c;
    this.#pathAt.cursor =
      c.node !== null && ViewElement.onPath(this.byId.get(c.node));
    this.#showPath();
    const box = this.#box(c.node, c.piece, c.offset);
    this.caret.hidden = !box;
    if (!box) return;
    this.caret.style.left = `${String(box.left)}px`;
    this.caret.style.top = `${String(box.top)}px`;
    this.caret.style.height = `${String(box.height)}px`;
    this.caret.scrollIntoView({ block: 'nearest', inline: 'nearest' });
    if (this.#selection) this.showSelection(this.#selection);
  }

  #marked = new Set<ASTNode>();
  // Error locations within node (last labels)
  errorRanges: Range[] = [];

  // [from, to] in the code points of node (its chunks-texts) → Range DOM.
  #rangeIn(id: NodeId, from: number, to: number): Range | undefined {
    const el = this.byId.get(id);
    const n = this.nodes.get(id);
    if (!el || !n) return undefined;
    const range = document.createRange();
    let seen = 0;
    let started = false;
    for (const [i, p] of n.pieces.entries()) {
      const t = typeof p === 'string' ? el.textOf(i) : undefined;
      if (!t) continue;
      const len = codePoints(t.data);
      if (!started && from < seen + len) {
        range.setStart(t, utf16Offset(t.data, from - seen));
        started = true;
      }
      if (started && to <= seen + len) {
        range.setEnd(t, utf16Offset(t.data, to - seen));
        return range;
      }
      seen += len;
    }
    return undefined;
  }

  // Check marks: old ones are removed, sent. Returns the first error for the status
  // string.
  showMarks(d: Decor): string | undefined {
    this.#decor = d;
    for (const el of this.#marked) {
      el.setMarks({});
      el.removeAttribute('error-range');
    }
    this.#marked.clear();
    const by = new Map<NodeId, NodeMarks>();
    let first: string | undefined;
    for (const m of d.marks) {
      const e = by.get(m.node) ?? {};
      if (m.kind === 'type') e.type = m.text;
      if (m.kind === 'value') e.value = m.text;
      if (m.kind === 'dead') e.dead = true;
      if (m.kind === 'current') e.current = true;
      if (m.kind === 'prev' || m.kind === 'next') e.near = m.kind;
      if (m.kind === 'path') e.path = m.text;
      if (m.kind === 'overloads') e.overloads = m.text;
      if (m.kind === 'chosen') e.chosen = m.text;
      if (m.kind === 'common' || m.kind === 'own') e.line = m.kind;
      if (m.kind === 'diverged') e.diverged = m.text;
      if (m.kind === 'unknown') e.unknown = true;
      if (m.kind === 'error') {
        e.error = m.text;
        first ??= m.text;
      }
      by.set(m.node, e);
    }
    for (const [id, e] of by) {
      const el = this.byId.get(id);
      if (!el) continue;
      el.setMarks(e);
      this.#marked.add(el);
    }
    // error place inside node (raw block: atom and displacement in it) - text range;
    // backlight - CSS Custom Highlight, without editing DOM node
    this.errorRanges = d.marks.flatMap((m) =>
      m.kind === 'error' && m.to > m.from
        ? (this.#rangeIn(m.node, m.from, m.to) ?? [])
        : [],
    );
    const registry = (globalThis.CSS as typeof CSS | undefined)?.highlights;
    if (registry) {
      registry.set('vish-error', new Highlight(...this.errorRanges));
      for (const m of d.marks)
        if (m.kind === 'error' && m.to > m.from)
          this.byId.get(m.node)?.setAttribute('error-range', '');
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
      .filter((n) => n instanceof RecordNode && n.composite)
      .flatMap((n) => (n.shadowRoot ? [n.shadowRoot] : []));
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

// Lines of hint: view - signature, link - by click (open).
export function renderLines(
  box: HTMLElement,
  lines: readonly HintLine[],
  open: (node: NodeId) => void,
): void {
  box.replaceChildren(
    ...lines.map((l) => {
      const row = document.createElement('div');
      row.className = 'line';
      row.dataset.kind = l.kind;
      const kind = document.createElement('span');
      kind.className = 'kind';
      kind.textContent = l.kind;
      const text = document.createElement('span');
      text.className = 'text';
      if (l.to > l.from) {
        // eslint-disable-next-line @typescript-eslint/no-misused-spread
        const cps = [...l.text];
        const mark = document.createElement('b');
        mark.textContent = cps.slice(l.from, l.to).join('');
        text.append(
          cps.slice(0, l.from).join(''),
          mark,
          cps.slice(l.to).join(''),
        );
      } else text.textContent = l.text;
      row.append(kind, text);
      const target = l.target;
      if (target !== null) {
        row.classList.add('link');
        row.addEventListener('mousedown', (e) => {
          e.preventDefault();
          e.stopPropagation();
          open(target);
        });
      }
      return row;
    }),
  );
}
